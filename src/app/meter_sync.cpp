#include "app/meter_sync.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

#include "core/log.hpp"
#include "net/http.hpp"
#include "providers/eloverblik.hpp"

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

namespace {
std::atomic<bool> g_meterNow{false};
constexpr hours kSyncEvery{6};
constexpr days kFirstFetch{30};
constexpr days kBackfillChunk{365};     // older history: a year per fetch...
constexpr days kBackfillLimit{365 * 10};  // ...until the account has nothing older (safety stop)
Instant nowSec() { return floor<seconds>(system_clock::now()); }
}  // namespace

std::optional<MeterConfig> parseMeterConfig(const json& cfg) {
    if (!cfg.contains("meter") || cfg["meter"].is_null()) return std::nullopt;
    const auto& m = cfg["meter"];
    const std::string type = m.value("type", "eloverblik");
    if (type != "eloverblik") throw std::runtime_error("meter: unknown type '" + type + "' (known: eloverblik)");
    MeterConfig c{m.value("refreshToken", ""), m.value("meteringPoint", "")};
    if (c.refreshToken.empty()) return std::nullopt;  // section present but no token yet
    return c;
}

MeterSync::MeterSync(MeterConfig cfg, fs::path dataDir, const TimeZone& tz)
    : cfg_(std::move(cfg)), dataDir_(std::move(dataDir)), tz_(tz), thread_([this] { run(); }) {}

MeterSync::~MeterSync() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void MeterSync::requestNow() { g_meterNow = true; }

void MeterSync::run() {
    while (true) {
        syncOnce();
        std::unique_lock lock(mutex_);
        const auto until = steady_clock::now() + kSyncEvery;
        // Wake every second to notice "Fetch now" and shutdown.
        while (!stop_ && !g_meterNow && steady_clock::now() < until) cv_.wait_for(lock, seconds{1});
        if (stop_) return;
        g_meterNow = false;
    }
}

void MeterSync::syncOnce() {
    json status = loadMeterStatus(dataDir_);
    status["lastAttempt"] = nowSec().time_since_epoch().count();
    try {
        HttplibClient http(seconds{10}, seconds{90});
        EloverblikClient client(http, dataDir_ / "eloverblik-token.json", cfg_.refreshToken, nowSec);
        const auto points = client.meteringPoints();
        if (points.empty()) throw std::runtime_error("no metering points on this Eloverblik account");
        const MeteringPoint* chosen = nullptr;
        for (const auto& p : points) {
            if (!cfg_.meteringPoint.empty() ? p.id == cfg_.meteringPoint : p.type == "E17") { chosen = &p; break; }
        }
        if (!chosen) {
            if (!cfg_.meteringPoint.empty()) throw std::runtime_error("metering point " + cfg_.meteringPoint + " not found");
            chosen = &points.front();
        }

        History history(dataDir_ / "history.db");
        const auto last = history.lastMeterReading();
        // Re-fetch the last day: Eloverblik fills in late values.
        const Instant fromT = last ? last->start - days{1} : nowSec() - kFirstFetch;
        const std::string from = formatLocalDate(tz_.toLocal(fromT));
        const std::string to = formatLocalDate(tz_.toLocal(nowSec() + days{1}));
        auto readings = client.readings(chosen->id, from, to);
        history.recordMeter(readings);

        // Older history, a chunk per fetch, until a year is in.
        const bool complete = status.value("historyComplete", false);
        if (const auto first = history.firstMeterReading(); !complete && first && first->start > nowSec() - kBackfillLimit) {
            const Instant olderTo = first->start;
            const Instant olderFrom = olderTo - kBackfillChunk;
            const auto older = client.readings(chosen->id, formatLocalDate(tz_.toLocal(olderFrom)),
                                               formatLocalDate(tz_.toLocal(olderTo)));
            history.recordMeter(older);
            log::info("Meter: {} older readings ({} - {})", older.size(), formatLocalDate(tz_.toLocal(olderFrom)),
                      formatLocalDate(tz_.toLocal(olderTo)));
            if (older.empty()) status["historyComplete"] = true;  // nothing further back on the account
            readings.insert(readings.end(), older.begin(), older.end());
        }

        json pts = json::array();
        for (const auto& p : points) pts.push_back({{"id", p.id}, {"type", p.type}, {"address", p.address}, {"supplier", p.supplier}});
        status["points"] = pts;
        status["meteringPoint"] = chosen->id;
        status["address"] = chosen->address;
        status["supplier"] = chosen->supplier;
        status["lastSync"] = nowSec().time_since_epoch().count();
        status["fetched"] = readings.size();
        if (const auto oldest = history.firstMeterReading()) status["firstReading"] = oldest->start.time_since_epoch().count();
        if (const auto newest = history.lastMeterReading()) {
            status["lastReading"] = (newest->start + newest->length).time_since_epoch().count();
            status["resolutionMinutes"] = newest->length.count();
        }
        status["error"] = nullptr;
        log::info("Meter: {} readings from Eloverblik ({} - {})", readings.size(), from, to);
    } catch (const std::exception& e) {
        status["error"] = e.what();
        log::warn("Meter: {}", e.what());
    }
    std::ofstream(dataDir_ / "meter-status.json") << status.dump(2);
}

json loadMeterStatus(const fs::path& dataDir) {
    try {
        if (fs::exists(dataDir / "meter-status.json")) return json::parse(std::ifstream(dataDir / "meter-status.json"));
    } catch (const std::exception&) {
    }
    return json::object();
}

json usageReport(const json& cfg, const History& history, Instant from, Instant to) {
    std::map<std::string, double> watts;
    for (const auto& p : cfg.value("plugs", json::array())) watts[p.value("id", "")] = p.value("watts", 0.0);
    for (const auto& t : cfg.value("thermostats", json::array())) watts[t.value("id", "")] = t.value("watts", 0.0);

    // When each live plug was on: from one decision to the next (capped: a gap means no control).
    struct On { Instant a, b; double w; };
    std::vector<On> on;
    constexpr minutes kCap{15};
    for (const auto& room : cfg.value("rooms", json::array())) {
        const auto ds = history.decisions(room.value("id", ""), from - hours{1}, to, false);
        for (std::size_t i = 0; i < ds.size(); ++i) {
            const Instant a = ds[i].at;
            const Instant b = std::min(i + 1 < ds.size() ? ds[i + 1].at : a + kCap, a + kCap);
            if (!ds[i].plugs.is_array()) continue;
            for (const auto& p : ds[i].plugs) {
                if (p.value("mode", "") != "live" || !p.value("state", false)) continue;
                if (const double w = watts[p.value("id", "")]; w > 0) on.push_back({a, b, w});
            }
        }
    }

    // Years of hourly readings: sort the on-intervals once (each is at most kCap long) and only
    // look at those that can overlap each reading.
    std::ranges::sort(on, {}, &On::a);
    json meter = json::array(), heating = json::array();
    for (const auto& r : history.meterReadings(from, to)) {
        const Instant s = r.start, e = r.start + r.length;
        double kWh = 0;
        auto it = std::ranges::lower_bound(on, s - kCap, {}, &On::a);
        for (; it != on.end() && it->a < e; ++it) {
            const Instant a = std::max(it->a, s), b = std::min(it->b, e);
            if (b > a) kWh += it->w / 1000.0 * duration<double, std::ratio<3600>>(b - a).count();
        }
        const auto secs = duration_cast<seconds>(r.length).count();
        meter.push_back({s.time_since_epoch().count(), secs, r.kWh});
        heating.push_back({s.time_since_epoch().count(), secs, kWh});
    }
    return json{{"meter", meter}, {"heating", heating}};
}

}  // namespace hb
