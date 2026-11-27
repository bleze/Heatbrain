#include "web/server.hpp"

#include <httplib.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>

#include "core/battery.hpp"
#include "app/power_mix_sync.hpp"
#include "app/config_service.hpp"
#include "app/cost_report.hpp"
#include "app/rooms.hpp"
#include "core/cost.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "notify/notifier.hpp"
#include "plugs/plugs.hpp"
#include "plugs/tasmota.hpp"
#include "storage/history.hpp"
#include "app/transfer.hpp"
#include "app/meter_sync.hpp"
#include "app/thermostats.hpp"
#include "core/offline.hpp"
#include "sensors/velux.hpp"
#include "sensors/push.hpp"
#include "sensors/shelly.hpp"

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

std::optional<int> fixedWebPort() {
    // Set by the Docker image: inside a container the port is part of the port mapping, so the
    // config's web.port must not move it (the address is chosen in docker-compose.yml).
    const char* v = std::getenv("HEATBRAIN_WEB_PORT");
    if (!v || !*v) return std::nullopt;
    const int p = std::atoi(v);
    return p >= 1 && p <= 65535 ? std::optional<int>(p) : std::nullopt;
}

WebConfig parseWebConfig(const json& cfg) {
    WebConfig w;
    if (cfg.contains("web")) {
        const auto& j = cfg["web"];
        w.enabled = j.value("enabled", w.enabled);
        w.bind = j.value("bind", w.bind);
        w.port = j.value("port", w.port);
    }
    if (const auto fixed = fixedWebPort()) w.port = *fixed;
    return w;
}

fs::path locateWebRoot(const std::vector<fs::path>& candidates) {
    for (const auto& c : candidates) {
        if (fs::exists(c / "index.template.html")) return c;
    }
    return {};
}

namespace {

std::string readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The page with every local script/stylesheet URL fingerprinted by its contents
// ("app.js" -> "app.js?v=1a2b3c4d"): a changed file gets a new URL, so no browser can keep
// running an old copy, whatever its cache does.
std::string renderIndex(const fs::path& webRoot) {
    std::string html = readWhole(webRoot / "index.template.html");
    static const std::regex asset(R"re((src|href)="([\w.-]+\.(?:js|css))")re");
    std::string out;
    auto it = html.cbegin();
    for (std::smatch m; std::regex_search(it, html.cend(), m, asset); it = m.suffix().first) {
        out.append(it, m[0].first);
        std::uint32_t h = 2166136261u;  // FNV-1a
        for (unsigned char c : readWhole(webRoot / m[2].str())) {
            h ^= c;
            h *= 16777619u;
        }
        out += std::format(R"({}="{}?v={:08x}")", m[1].str(), m[2].str(), h);
    }
    out.append(it, html.cend());
    return out;
}

}  // namespace

WebServer::WebServer(WebConfig cfg, const StatusBoard& board, WebContext ctx)
    : cfg_(std::move(cfg)), board_(board), ctx_(std::move(ctx)), server_(std::make_unique<httplib::Server>()) {
    routes();
}

WebServer::~WebServer() { stop(); }

void WebServer::setRooms(std::vector<RoomConfig> rooms) {
    std::lock_guard lock(roomsMutex_);
    rooms_ = std::move(rooms);
}

std::vector<RoomConfig> WebServer::rooms() const {
    std::lock_guard lock(roomsMutex_);
    return rooms_;
}

namespace {

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, json{{"error", message}}, status);
}

long long epoch(Instant t) { return t.time_since_epoch().count(); }

// The end of a history window: now, or ?to=<epoch seconds> for an earlier one (never later than now).
// A window in the past gets no prices beyond its end (the live one shows the coming ones too).
struct Window {
    Instant from, to, pricesTo;
};
Window historyWindow(const httplib::Request& req, int hoursBack) {
    const Instant now = floor<seconds>(system_clock::now()) + seconds{1};
    Instant to = now;
    if (req.has_param("to")) {
        if (const long long t = std::atoll(req.get_param_value("to").c_str()); t > 0) to = std::min(now, Instant{seconds{t}});
    }
    return {to - hours{hoursBack}, to, to < now - minutes{5} ? to : to + hours{36}};
}

json readConfigFile(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    return json::parse(in, nullptr, true, /*ignore_comments=*/true);
}

json parseBody(const httplib::Request& req) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) throw std::invalid_argument("request body is not valid JSON");
    return body;
}

// Wi-Fi off for the charts: as seen (gaps in the sensor readings) and as planned (config).
json wifiLane(const History& history, const json& cfg, Instant from, Instant to) {
    const Instant now = floor<seconds>(system_clock::now());
    json seen = json::array(), planned = json::array();
    for (const auto& g : findReadingGaps(history.readingTimes(from - hours{2}, now + minutes{1}), now)) {
        if (g.firstAfter <= from || g.lastBefore >= to) continue;
        seen.push_back({g.lastBefore.time_since_epoch().count(), g.firstAfter.time_since_epoch().count(), g.ongoing});
    }
    try {
        if (const auto plan = parseWifiOff(cfg)) {
            const TimeZone tz(cfg.value("timezone", std::string("Europe/Copenhagen")));
            // Every occurrence overlapping the chart, including the hours ahead.
            for (Instant t = from - hours{24}; t < to + hours{36};) {
                const auto p = currentOrNextOfflinePeriod(*plan, tz, t);
                if (p.start >= to + hours{36}) break;
                if (p.end > from) planned.push_back({p.start.time_since_epoch().count(), p.end.time_since_epoch().count()});
                t = p.end;
            }
        }
    } catch (const std::exception&) {
        // an invalid config shows no planned periods; Settings reports the problem
    }
    if (seen.empty() && planned.empty()) return nullptr;
    return json{{"seen", seen}, {"planned", planned}};
}

json failsafeJson(Plug& plug) {
    try {
        const auto fs = plug.sw->checkFailsafe();
        return json{{"installed", fs.installed()}, {"problems", fs.problems}};
    } catch (const std::exception& e) {
        // Unknown, not "missing": a slow plug must not look like it lost its fail-safe.
        return json{{"installed", false}, {"unknown", true},
                    {"problems", json::array({std::string("could not check: ") + e.what()})}};
    }
}

}  // namespace

void WebServer::routes() {
    auto& s = *server_;

    // --- Dashboard ----------------------------------------------------------------------

    s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, board_.get()); });

    s.Get(R"(/api/rooms/([\w-]+)/history)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto all = rooms();
        const auto room = std::ranges::find_if(all, [&](const RoomConfig& r) { return r.id == id; });
        if (room == all.end()) return sendError(res, 404, "unknown room");
        int hoursBack = 24;
        if (req.has_param("hours")) hoursBack = std::clamp(std::atoi(req.get_param_value("hours").c_str()), 1, 24 * 14);

        // One SQLite connection per request: connections are not shared across threads.
        History history(ctx_.dataDir / "history.db");
        const auto [from, to, pricesTo] = historyWindow(req, hoursBack);

        json temps = json::array();
        json co2 = json::array();
        for (const auto& t : history.temperatures(id, from, to)) {
            temps.push_back({epoch(t.at), t.temperature});
            if (t.co2) co2.push_back({epoch(t.at), *t.co2});
        }
        json decisions = json::array();
        json events = json::array();
        for (const auto& d : history.decisions(id, from, to, false)) {
            // [time, on, reason, target, 1 when the target is on a warm-up curve, 1 when a booking sets it,
            //  an off period's "stay above" floor]
            decisions.push_back({epoch(d.at), d.on ? 1 : 0, d.reason,
                                 d.inputs.contains("target") ? d.inputs["target"] : json(nullptr),
                                 d.inputs.contains("warmupTo") ? 1 : 0, d.inputs.value("mode", "") == "booked" ? 1 : 0,
                                 d.inputs.value("mode", "") == "off" && d.inputs.contains("coastAbove")
                                     ? d.inputs["coastAbove"] : json(nullptr)});
            if (d.changed) {
                events.push_back({{"ts", epoch(d.at)}, {"on", d.on}, {"reason", d.reason}, {"detail", d.detail},
                                  {"inputs", d.inputs}, {"plugs", d.plugs}});
            }
        }
        json prices = json::array();
        for (const auto& p : history.prices(from, pricesTo)) prices.push_back({epoch(p.slot), p.total});

        sendJson(res, json{{"room",
                            {{"id", room->id},
                             {"name", room->name},
                             {"minTemp", room->minTemp},
                             {"maxTemp", room->maxTemp},
                             {"priceOnly", room->priceOnly.has_value()}}},
                           {"from", epoch(from)},
                           {"to", epoch(to)},
                           {"temperatures", temps},
                           {"co2", co2},
                           {"decisions", decisions},
                           {"events", events},
                           {"prices", prices},
                           {"wifi", wifiLane(history, readConfigFile(ctx_.configFile), from, to)}});
    });

    // Temperature history of a single sensor (e.g. the outdoor module): ?key=netatmo:02:00:...&hours=24
    s.Get("/api/sensor-history", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string key = req.get_param_value("key");
        if (key.empty()) return sendError(res, 400, "key is required");
        int hoursBack = 24;
        if (req.has_param("hours")) hoursBack = std::clamp(std::atoi(req.get_param_value("hours").c_str()), 1, 24 * 14);
        History history(ctx_.dataDir / "history.db");
        const auto [from, to, pricesTo] = historyWindow(req, hoursBack);
        json temps = json::array();
        json humidity = json::array();
        json co2 = json::array();
        for (const auto& t : history.sensorTemperatures(key, from, to)) {
            temps.push_back({epoch(t.at), t.temperature});
            if (t.humidity) humidity.push_back({epoch(t.at), *t.humidity});
            if (t.co2) co2.push_back({epoch(t.at), *t.co2});
        }
        json prices = json::array();
        for (const auto& p : history.prices(from, pricesTo)) prices.push_back({epoch(p.slot), p.total});
        sendJson(res, json{{"key", key}, {"from", epoch(from)}, {"to", epoch(to)}, {"temperatures", temps},
                           {"humidity", humidity}, {"co2", co2}, {"prices", prices},
                           {"wifi", wifiLane(history, readConfigFile(ctx_.configFile), from, to)}});
    });

    s.Post(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto all = rooms();
        if (std::ranges::none_of(all, [&](const RoomConfig& r) { return r.id == id; })) {
            return sendError(res, 404, "unknown room");
        }
        const json body = parseBody(req);
        if (!body.is_object() || !body.contains("on") || !body.contains("minutes")) {
            return sendError(res, 400, R"(expected {"on": true|false, "minutes": N})");
        }
        const int minutesFor = body.value("minutes", 0);
        if (minutesFor < 1 || minutesFor > 7 * 24 * 60) return sendError(res, 400, "minutes must be 1..10080");
        const Instant until = floor<seconds>(system_clock::now()) + minutes{minutesFor};
        Overrides(ctx_.dataDir / "overrides.json").set(id, ManualOverride{body.value("on", false), until});
        log::info("Web: manual {} for {} until {}", body.value("on", false) ? "ON" : "OFF", id, formatIsoUtc(until));
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"room", id}, {"on", body.value("on", false)}, {"until", epoch(until)}});
    });

    s.Delete(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        Overrides(ctx_.dataDir / "overrides.json").clear(id);
        log::info("Web: override cleared for {}", id);
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"room", id}, {"cleared", true}});
    });

    // Room bookings: {"from": unix seconds, "until": unix seconds, "target": 22}. The room heats to the
    // target from its warm-up time before "from" until "until", then follows its program again.
    // Checks a booking for a room; the error to answer with, if any.
    const auto checkBooking = [this](const std::string& roomId, const json& body, Booking& out) -> std::pair<int, std::string> {
        const auto all = rooms();
        const auto room = std::ranges::find_if(all, [&](const RoomConfig& r) { return r.id == roomId; });
        if (room == all.end()) return {404, "unknown room"};
        if (room->priceOnly) return {400, "A room without a temperature sensor cannot be booked"};
        const auto now = floor<seconds>(system_clock::now());
        const Instant from{seconds{body.value("from", 0LL)}};
        const Instant until{seconds{body.value("until", 0LL)}};
        const double target = body.value("target", 22.0);
        if (until <= now || from > now + days{90}) return {400, "The booking must end in the future and start within 90 days"};
        if (from >= until) return {400, "The start must be before the end"};
        if (until - from > days{14}) return {400, "A booking can be at most 14 days (use away mode for longer)"};
        if (target < room->frostTemp || target >= room->maxTemp) {
            return {400, std::format("The temperature must be {:.0f}..{:.1f} °C (below the room's maximum; raise it "
                                     "in Settings → Rooms if needed)", room->frostTemp, room->maxTemp - 0.5)};
        }
        out = Booking{target, from, until};
        return {0, ""};
    };
    s.Post(R"(/api/rooms/([\w-]+)/bookings)", [this, checkBooking](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        Booking b;
        if (const auto [status, why] = checkBooking(id, parseBody(req), b); status) return sendError(res, status, why);
        try {
            const std::string bid = BookingStore(ctx_.dataDir / "bookings.json").add(id, b);
            log::info("Web: {} booked {} - {} at {:.1f} °C", id, formatIsoUtc(b.from), formatIsoUtc(b.until), b.target);
            if (ctx_.onOverride) ctx_.onOverride();
            sendJson(res, json{{"id", bid}, {"room", id}, {"from", epoch(b.from)}, {"until", epoch(b.until)}, {"target", b.target}});
        } catch (const std::invalid_argument& e) {
            sendError(res, 409, e.what());
        }
    });
    // Change a booking: {"room", "from", "until", "target"} (the room may change too).
    s.Put(R"(/api/bookings/([\w-]+))", [this, checkBooking](const httplib::Request& req, httplib::Response& res) {
        const std::string bid = req.matches[1];
        const json body = parseBody(req);
        const std::string roomId = body.value("room", std::string());
        Booking b;
        if (const auto [status, why] = checkBooking(roomId, body, b); status) return sendError(res, status, why);
        try {
            if (!BookingStore(ctx_.dataDir / "bookings.json").update(bid, roomId, b)) return sendError(res, 404, "unknown booking");
            log::info("Web: booking {} changed: {} {} - {} at {:.1f} °C", bid, roomId, formatIsoUtc(b.from), formatIsoUtc(b.until), b.target);
            if (ctx_.onOverride) ctx_.onOverride();
            sendJson(res, json{{"id", bid}, {"room", roomId}, {"from", epoch(b.from)}, {"until", epoch(b.until)}, {"target", b.target}});
        } catch (const std::invalid_argument& e) {
            sendError(res, 409, e.what());
        }
    });
    s.Delete(R"(/api/bookings/([\w-]+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        if (!BookingStore(ctx_.dataDir / "bookings.json").remove(id)) return sendError(res, 404, "unknown booking");
        log::info("Web: booking {} removed", id);
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"removed", id}});
    });

    // Away/holiday period: {"from": unix seconds (default now), "until": unix seconds, "keepAbove": 12}
    s.Post("/api/away", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        const auto now = floor<seconds>(system_clock::now());
        const Instant from{seconds{body.value("from", now.time_since_epoch().count())}};
        const Instant until{seconds{body.value("until", 0LL)}};
        const double keepAbove = body.value("keepAbove", 12.0);
        if (until <= now || until > now + days{90}) return sendError(res, 400, "The end must be within the next 90 days");
        if (from >= until) return sendError(res, 400, "The start must be before the end");
        if (keepAbove < 5 || keepAbove > 20) return sendError(res, 400, "Keep above must be 5..20 °C");
        AwayStore(ctx_.dataDir / "away.json").set(AwayMode{keepAbove, from, until});
        log::info("Web: away {} - {} (keep above {:.1f} °C)", formatIsoUtc(from), formatIsoUtc(until), keepAbove);
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"from", epoch(from)}, {"until", epoch(until)}, {"keepAbove", keepAbove}});
    });
    s.Delete("/api/away", [this](const httplib::Request&, httplib::Response& res) {
        AwayStore(ctx_.dataDir / "away.json").clear();
        log::info("Web: away mode ended");
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"away", nullptr}});
    });

    // Energy and cost per room and day, from the decision log, stored prices and each plug's
    // wattage. "saved" compares with paying each day's average price for the same energy.
    s.Get("/api/costs", [this](const httplib::Request& req, httplib::Response& res) {
        const int dayCount = req.has_param("days") ? std::clamp(std::atoi(req.get_param_value("days").c_str()), 1, 90) : 30;
        History history(ctx_.dataDir / "history.db");
        sendJson(res, costReport(readConfigFile(ctx_.configFile), history, floor<seconds>(system_clock::now()), dayCount));
    });

    // --- Settings -----------------------------------------------------------------------

    s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        // Rooms are always presented as daily programs (older configs are converted).
        const json cfg = withPrograms(readConfigFile(ctx_.configFile));
        json out{{"config", maskSecrets(cfg)}, {"errors", validateConfig(cfg, ctx_.dataDir)}};
        if (const auto fixed = fixedWebPort()) out["fixedWebPort"] = *fixed;
        sendJson(res, out);
    });

    // Export / import for moving heatbrain (e.g. to the NAS). POST, so a link can't trigger it.
    s.Post("/api/export", [this](const httplib::Request& req, httplib::Response& res) {
        const bool withHistory = req.has_param("history") && req.get_param_value("history") == "1";
        try {
            const json bundle = buildExport(ctx_.dataDir, ctx_.configFile, withHistory);
            log::info("Web: exported configuration{}", withHistory ? " and history" : "");
            res.set_header("Content-Disposition", "attachment; filename=\"heatbrain-export.json\"");
            sendJson(res, bundle);
        } catch (const std::exception& e) {
            sendError(res, 500, std::string("Export failed: ") + e.what());
        }
    });

    s.Post("/api/import", [this](const httplib::Request& req, httplib::Response& res) {
        json bundle;
        try {
            bundle = json::parse(req.body);
        } catch (const std::exception&) {
            return sendError(res, 400, "This is not a heatbrain export file (not JSON).");
        }
        try {
            const auto r = applyImport(bundle, ctx_.dataDir, ctx_.configFile);
            log::info("Web: imported export from {} ({} files{}) - reloading", r.exportedAt, r.files,
                      r.history ? ", history" : "");
            if (ctx_.onConfigSaved) ctx_.onConfigSaved();
            sendJson(res, json{{"imported", true}, {"files", r.files}, {"history", r.history}, {"exportedAt", r.exportedAt}});
        } catch (const std::invalid_argument& e) {
            sendError(res, 400, e.what());
        } catch (const std::exception& e) {
            log::warn("Web: import failed: {}", e.what());
            sendError(res, 500, std::string("Import failed: ") + e.what());
        }
    });

    // When the sensors were offline at night over the last week (the Wi-Fi off times, as seen).
    s.Get("/api/wifi-gaps", [this](const httplib::Request&, httplib::Response& res) {
        const Instant now = floor<seconds>(system_clock::now());
        History history(ctx_.dataDir / "history.db");
        json gaps = json::array();
        for (const auto& g : findReadingGaps(history.readingTimes(now - days{8}, now + minutes{1}), now)) {
            gaps.push_back({{"from", g.lastBefore.time_since_epoch().count()},
                            {"to", g.firstAfter.time_since_epoch().count()},
                            {"ongoing", g.ongoing}});
        }
        sendJson(res, json{{"gaps", gaps}, {"now", now.time_since_epoch().count()}});
    });

    // Household meter (Eloverblik): status for Settings, "fetch now", and use vs. radiators.
    s.Get("/api/meter/status", [this](const httplib::Request&, httplib::Response& res) {
        json st = loadMeterStatus(ctx_.dataDir);
        bool configured = false;
        try {
            configured = parseMeterConfig(readConfigFile(ctx_.configFile)).has_value();
        } catch (const std::exception&) {
        }
        st["configured"] = configured;
        sendJson(res, st);
    });
    s.Post("/api/meter/sync", [](const httplib::Request&, httplib::Response& res) {
        MeterSync::requestNow();
        sendJson(res, json{{"requested", true}});
    });
    s.Get("/api/usage", [this](const httplib::Request& req, httplib::Response& res) {
        // days=0: everything stored.
        const int dayCount = req.has_param("days") ? std::clamp(std::atoi(req.get_param_value("days").c_str()), 0, 3660) : 7;
        History history(ctx_.dataDir / "history.db");
        // Ending with the newest reading: the meter lags a day behind.
        const auto last = history.lastMeterReading();
        const Instant to = last ? last->start + last->length : floor<seconds>(system_clock::now());
        const auto first = history.firstMeterReading();
        const Instant from = dayCount == 0 ? (first ? first->start : to - days{30}) : to - days{dayCount};
        json out = usageReport(readConfigFile(ctx_.configFile), history, from, to);
        out["from"] = epoch(from);
        out["to"] = epoch(to);
        sendJson(res, out);
    });

    // Hours each room heated per day (days=0: everything kept), with the outdoor sensor's days.
    s.Get("/api/heating-days", [this](const httplib::Request& req, httplib::Response& res) {
        const int dayCount = req.has_param("days") ? std::clamp(std::atoi(req.get_param_value("days").c_str()), 0, 3660) : 90;
        const json cfg = readConfigFile(ctx_.configFile);
        // The outdoor sensor as the controller picks it: the configured one ("<source>:<id or name>"),
        // else the first outdoor module among the last readings.
        std::string outdoorKey, outdoorName;
        try {
            if (std::filesystem::exists(ctx_.dataDir / "readings.json")) {
                const json items = json::parse(std::ifstream(ctx_.dataDir / "readings.json")).value("readings", json::object());
                const std::string ref = cfg.value("outdoorSensor", std::string());
                const std::string source = ref.substr(0, ref.find(':'));
                const std::string id = ref.find(':') == std::string::npos ? ref : ref.substr(ref.find(':') + 1);
                for (const auto& [k, r] : items.items()) {
                    if (!ref.empty() && (k == ref || (k.starts_with(source + ":") && r.value("name", "") == id))) {
                        outdoorKey = k;
                        outdoorName = r.value("name", k);
                    }
                }
                for (const auto& [k, r] : items.items()) {
                    if (outdoorKey.empty() && r.value("kind", "") == "outdoor") {
                        outdoorKey = k;
                        outdoorName = r.value("name", k);
                    }
                }
            }
        } catch (const std::exception& e) {
            log::warn("Heating days: readings.json: {}", e.what());
        }
        History history(ctx_.dataDir / "history.db");
        sendJson(res, heatingDaysReport(cfg, history, floor<seconds>(system_clock::now()), dayCount, outdoorKey, outdoorName));
    });

    // A sensor's last year, a point per day: [[day, minTemp, avgTemp, maxTemp, avgHumidity|null, avgCo2|null, maxCo2|null], ...] - for the
    // charts in Settings -> Sensors (with /api/batteries for the battery).
    s.Get("/api/sensor-days", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("key")) return sendError(res, 400, "key missing");
        History history(ctx_.dataDir / "history.db");
        // Up to now, or ?to=<epoch seconds> for an earlier window.
        const Instant to = historyWindow(req, 0).to + days{1};
        json days = json::array();
        for (const auto& d : history.sensorDays(req.get_param_value("key"), to - std::chrono::days{400}, to)) {
            days.push_back({epoch(d.day), d.minTemp, d.avgTemp, d.maxTemp, d.avgHumidity ? json(*d.avgHumidity) : json(nullptr),
                            d.avgCo2 ? json(*d.avgCo2) : json(nullptr), d.maxCo2 ? json(*d.maxCo2) : json(nullptr)});
        }
        sendJson(res, json{{"days", days}});
    });

    // Battery levels per sensor (one a day, see Controller::checkBatteries) and when each is
    // projected to run out: {"<source>:<id>": {"samples": [[day, percent], ...], "daysLeft": 141.2|null}}.
    s.Get("/api/batteries", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::object();
        try {
            if (std::ifstream in(ctx_.dataDir / "battery.json"); in) {
                const json saved = json::parse(in);
                for (const auto& [key, arr] : saved.items()) {
                    std::vector<BatterySample> samples;
                    for (const auto& p : arr) samples.push_back({Instant{seconds{p.at(0).get<long long>()}}, p.at(1).get<int>()});
                    const auto left = daysUntilEmpty(samples);
                    out[key] = {{"samples", arr}, {"daysLeft", left ? json(*left) : json(nullptr)}};
                }
            }
        } catch (const std::exception& e) {
            return sendError(res, 500, std::string("battery history: ") + e.what());
        }
        sendJson(res, out);
    });

    // The price area's power mix per hour (fetched in the background, see PowerMixSync).
    s.Get("/api/power-mix", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, loadPowerMix(ctx_.dataDir));
    });

    // DEVIreg Smart thermostats: what is paired and how they are doing, and pairing itself.
    s.Get("/api/devi", [this](const httplib::Request&, httplib::Response& res) {
        DeviRegistry::instance().setDataDir(ctx_.dataDir);
        json out{{"paired", false}, {"rooms", json::array()}};
        try {
            const auto id = DeviRegistry::instance().identity();
            if (!id || id->rooms.empty()) return sendJson(res, out);
            std::set<std::string> configured;
            for (const auto& t : readConfigFile(ctx_.configFile).value("thermostats", json::array())) {
                configured.insert(t.value("peerId", ""));
            }
            out["paired"] = true;
            out["houseName"] = id->houseName;
            for (const auto& r : id->rooms) {
                json room{{"name", r.name}, {"peerId", r.peerId}, {"configured", configured.contains(r.peerId)}};
                try {
                    auto th = DeviRegistry::instance().get(r.peerId);
                    if (!th->state().updated) th->refresh();  // first look: connect and ask
                    const DeviState st = th->state();
                    room["connected"] = st.connected;
                    if (!st.error.empty()) room["error"] = st.error;
                    if (st.roomTemp) room["roomTemp"] = *st.roomTemp;
                    if (st.floorTemp) room["floorTemp"] = *st.floorTemp;
                    if (st.manualSetpoint) room["manualSetpoint"] = *st.manualSetpoint;
                    if (st.controlState) room["mode"] = dominion::controlStateName(*st.controlState);
                    if (st.heating) room["heating"] = *st.heating;
                    if (!st.raw.empty()) {
                        json raw = json::object();
                        for (const auto& [code, hex] : st.raw) raw[std::to_string(code)] = hex;
                        room["raw"] = raw;
                    }
                } catch (const std::exception& e) {
                    room["connected"] = false;
                    room["error"] = e.what();
                }
                out["rooms"].push_back(room);
            }
        } catch (const std::exception& e) {
            out["error"] = e.what();
        }
        sendJson(res, out);
    });
    s.Post("/api/devi/pair", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        try {
            const auto id = deviPair(ctx_.dataDir, body.value("code", ""), body.value("phoneName", "heatbrain"));
            DeviRegistry::instance().setDataDir(ctx_.dataDir);
            DeviRegistry::instance().reload();
            json rooms = json::array();
            for (const auto& r : id.rooms) rooms.push_back({{"name", r.name}, {"peerId", r.peerId}});
            sendJson(res, json{{"paired", true}, {"houseName", id.houseName}, {"rooms", rooms}});
        } catch (const std::invalid_argument& e) {
            sendError(res, 400, e.what());
        } catch (const std::exception& e) {
            log::warn("DEVI pairing failed: {}", e.what());
            sendError(res, 502, e.what());
        }
    });

    // A battery sensor reporting by itself (Shelly H&T Gen3 "Actions"/webhooks):
    //   /api/push/shelly?id=shed&name=Shed&temp=${ev.tC}   and   ...&hum=${ev.rh}
    //   (heatbrain's setup adds &battery=${status["devicepower:0"].battery.percent} to both)
    // GET or POST; temp/tC/temperature, hum/rh/humidity, batt/battery are all accepted.
    const auto pushHandler = [](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.get_param_value("id");
        static const std::regex idOk("^[A-Za-z0-9_-]{1,40}$");
        if (!std::regex_match(id, idOk)) return sendError(res, 400, "id must be 1-40 letters, digits, - or _");
        const auto number = [&](std::initializer_list<const char*> names, double lo, double hi) -> std::optional<double> {
            for (const char* n : names) {
                if (!req.has_param(n)) continue;
                const std::string v = req.get_param_value(n);
                if (v.find("${") != std::string::npos) {
                    log::warn("Shelly {}: '{}={}' was not filled in by the device - check the token in its Action URL", id, n, v);
                    return std::nullopt;
                }
                char* end = nullptr;
                const double d = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0' || !(d >= lo && d <= hi)) return std::nullopt;
                return d;
            }
            return std::nullopt;
        };
        const auto temp = number({"temp", "tC", "temperature"}, -40, 80);
        const auto hum = number({"hum", "rh", "humidity"}, 0, 100);
        const auto batt = number({"batt", "battery"}, 0, 100);
        if (!temp && !hum && !batt) return sendError(res, 400, "no temperature, humidity or battery in the request");
        const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        PushRegistry::instance().put(id, req.get_param_value("name"), temp, hum,
                                     batt ? std::optional<int>(static_cast<int>(*batt)) : std::nullopt, now);
        // Set up before heatbrain asked for the battery level: add it to the Shelly's webhooks now, while
        // it is awake waiting for this answer (a battery Shelly sleeps otherwise, so Set up needs its
        // button pressed). Best effort, at most once an hour per sensor; IPv4 senders only.
        if (!req.has_param("battery") && !req.has_param("batt") && req.remote_addr.find(':') == std::string::npos) {
            static std::mutex triedMutex;
            static std::map<std::string, Instant> tried;
            bool go = false;
            {
                std::lock_guard lock(triedMutex);
                const auto it = tried.find(id);
                go = it == tried.end() || now - it->second > hours{1};
                if (go) tried[id] = now;
            }
            if (go) {
                try {
                    HttplibClient http(milliseconds{1000}, milliseconds{2000});
                    if (const int n = addShellyBattery(http, req.remote_addr); n > 0) {
                        log::info("Shelly {} at {}: its reports now include the battery level ({} webhooks updated)", id, req.remote_addr, n);
                    }
                } catch (const std::exception& e) {
                    log::info("Shelly {} at {}: could not add the battery level while it reported ({}) - Set up in Settings does it",
                              id, req.remote_addr, e.what());
                }
            }
        }
        sendJson(res, json{{"ok", true}});
    };
    s.Get("/api/push/shelly", pushHandler);
    s.Post("/api/push/shelly", pushHandler);

    // Finding Shellys on the network and setting one up without the Shelly app: heatbrain installs the
    // two webhooks on the device itself (see sensors/shelly.hpp).
    const auto shellyJson = [](const DiscoveredShelly& d) {
        json j{{"host", d.host}, {"id", d.id}, {"model", d.model}, {"app", d.app}, {"name", d.name},
               {"firmware", d.firmware}, {"gen", d.gen}, {"auth", d.auth}, {"sensor", d.sensor}};
        if (d.temperature) j["temperature"] = *d.temperature;
        if (d.humidity) j["humidity"] = *d.humidity;
        if (d.battery) j["battery"] = *d.battery;
        if (!d.pushId.empty()) j["pushId"] = d.pushId;
        if (!d.pushName.empty()) j["pushName"] = d.pushName;
        return j;
    };
    // One scan for everything heatbrain can find on the network: Tasmota plugs and Shellys together.
    s.Post("/api/discover", [shellyJson](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        const std::string subnet = body.value("subnet", std::string());
        HttplibClient probe(milliseconds{1500}, milliseconds{2000});
        try {
            auto shellys = std::async(std::launch::async, [&] { return discoverShelly(subnet, probe); });
            json tasmota = json::array();
            for (const auto& d : discoverTasmota(subnet, probe)) {
                json j{{"host", d.host}, {"deviceName", d.deviceName}, {"model", d.model}, {"firmware", d.firmware}, {"hardware", d.hardware}};
                if (d.on) j["on"] = *d.on;
                tasmota.push_back(j);
            }
            json shelly = json::array();
            for (const auto& d : shellys.get()) shelly.push_back(shellyJson(d));
            sendJson(res, json{{"tasmota", tasmota}, {"shelly", shelly}});
        } catch (const std::exception& e) {
            sendError(res, 400, e.what());
        }
    });
    s.Post("/api/shelly/setup", [shellyJson](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        const std::string host = body.value("host", "");
        static const std::regex hostOk(R"(^[A-Za-z0-9.-]{1,253}(:\d{1,5})?$)");
        if (!std::regex_match(host, hostOk)) return sendError(res, 400, "host must be an address like 192.168.1.42");
        HttplibClient http(milliseconds{3000}, milliseconds{5000});
        try {
            const auto r = setupShelly(http, host, body.value("base", ""), body.value("name", ""));
            // Its readings right away, rather than at its next change.
            PushRegistry::instance().put(r.device.id, r.device.name, r.device.temperature, r.device.humidity, r.device.battery,
                                         std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
            log::info("Web: set up Shelly {} at {} as '{}' ({} earlier webhooks replaced)", r.device.model, host, r.device.name, r.removed);
            sendJson(res, json{{"device", shellyJson(r.device)}, {"removed", r.removed}});
        } catch (const std::exception& e) {
            log::warn("Web: setting up the Shelly at {} failed: {}", host, e.what());
            sendError(res, 502, e.what());
        }
    });

    // VELUX ACTIVE: log in once with the account (the password is used for this request only, never
    // stored), then list the rooms with climate readings so the page can show what was found.
    s.Post("/api/velux/connect", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        const auto tokenFile = ctx_.dataDir / "velux-tokens.json";
        try {
            HttplibClient internet;
            const auto clock = [] { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); };
            velux::login(internet, body.value("email", ""), body.value("password", ""), tokenFile, clock());
            velux::Source src("velux", internet, velux::Auth(internet, tokenFile, clock), clock);
            json rooms = json::array();
            for (const auto& r : src.read()) {
                json room{{"name", r.name}, {"temperature", *r.temperature}};
                if (r.humidity) room["humidity"] = *r.humidity;
                if (r.co2) room["co2"] = *r.co2;
                rooms.push_back(room);
            }
            sendJson(res, json{{"connected", true}, {"rooms", rooms}});
        } catch (const std::invalid_argument& e) {
            sendError(res, 400, e.what());
        } catch (const SourceAuthError& e) {
            sendError(res, 401, e.what());
        } catch (const std::exception& e) {
            log::warn("VELUX login failed: {}", e.what());
            sendError(res, 502, e.what());
        }
    });
    s.Post("/api/velux/disconnect", [this](const httplib::Request&, httplib::Response& res) {
        std::error_code ec;
        std::filesystem::remove(ctx_.dataDir / "velux-tokens.json", ec);
        sendJson(res, json{{"connected", false}});
    });

    s.Post("/api/config/validate", [this](const httplib::Request& req, httplib::Response& res) {
        const json incoming = unmaskSecrets(parseBody(req), readConfigFile(ctx_.configFile));
        sendJson(res, json{{"errors", validateConfig(incoming, ctx_.dataDir)}});
    });

    s.Put("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        const json stored = readConfigFile(ctx_.configFile);
        const json incoming = keepLearned(unmaskSecrets(parseBody(req), stored), stored);
        const auto errors = validateConfig(incoming, ctx_.dataDir);
        if (!errors.empty()) return sendJson(res, json{{"errors", errors}}, 400);
        saveConfig(ctx_.configFile, incoming);
        log::info("Web: configuration saved - reloading");
        // When: a status from before this is from the old settings (the dashboard waits for a newer one).
        const Instant at = floor<seconds>(system_clock::now());
        if (ctx_.onConfigSaved) ctx_.onConfigSaved();
        sendJson(res, json{{"saved", true}, {"at", epoch(at)}});
    });

    s.Get("/api/plugs", [this](const httplib::Request&, httplib::Response& res) {
        // Own plug objects from the saved config: never shares state with the control loop.
        const json cfg = readConfigFile(ctx_.configFile);
        HttplibClient lan(kPlugConnectTimeout, kPlugReadTimeout);
        auto plugs = loadPlugs(cfg, lan);
        json latest;
        try {
            latest = latestFirmware();
        } catch (const std::exception& e) {
            hb::log::debug("Latest firmware lookup failed: {}", e.what());
        }
        std::vector<std::future<json>> futures;
        for (auto& p : plugs) {
            futures.push_back(std::async(std::launch::async, [&p, &latest, this] {
                const auto st = p.sw->status();
                json j{{"id", p.id}, {"reachable", st.reachable}, {"firmware", st.firmware}, {"hardware", st.hardware},
                       {"deviceName", st.deviceName}, {"model", st.model}, {"error", st.error}};
                if (st.on) j["on"] = *st.on;
                if (st.rssi) j["rssi"] = *st.rssi;
                j["failsafe"] = st.reachable ? failsafeJson(p) : json{{"installed", false}, {"problems", json::array()}};
                if (st.reachable && latest.is_object() && !latest.value("version", "").empty()) {
                    j["latest"] = latest;
                    j["updateAvailable"] = compareVersions(st.firmware, latest.value("version", "")) < 0;
                }
                // What "Remove fail-safe" would put back, and from when ("backup-20261121T203903Z.json").
                try {
                    const std::string name = originalBackup(ctx_.dataDir, p).filename().string();
                    json b{{"file", name}};
                    if (name.size() >= 23) {
                        const std::string t = name.substr(7, 16);  // 20261121T203903Z
                        b["takenAt"] = std::format("{}-{}-{}T{}:{}:{}Z", t.substr(0, 4), t.substr(4, 2), t.substr(6, 2),
                                                   t.substr(9, 2), t.substr(11, 2), t.substr(13, 2));
                    }
                    j["backup"] = b;
                } catch (const std::exception&) {
                    // no backup yet
                }
                if (const auto rec = loadFirmwareRecord(ctx_.dataDir, p.id)) {
                    j["test"] = {{"version", rec->version}, {"verified", rec->verified}, {"checkedAt", rec->checkedAt},
                                 {"detail", rec->detail},
                                 {"matchesFirmware", st.reachable && compareVersions(st.firmware, rec->version) == 0}};
                }
                return j;
            }));
        }
        json out = json::array();
        std::map<std::string, std::string> models;
        for (auto& f : futures) {
            out.push_back(f.get());
            if (!out.back().value("model", std::string()).empty()) models[out.back()["id"]] = out.back()["model"];
        }
        // A plug's model, once it reports one, goes into the settings: next time the page shows the
        // plug as what it is (its Sonoff mark) before any plug has answered. Not a settings change to
        // act on, so no reload.
        try {
            static std::mutex learning;
            std::lock_guard lock(learning);
            if (const auto updated = withLearnedPlugModels(readConfigFile(ctx_.configFile), models)) {
                saveConfig(ctx_.configFile, *updated);
                log::info("Saved the model of plugs that reported one");
            }
        } catch (const std::exception& e) {
            log::warn("Could not save the plugs' models: {}", e.what());
        }
        sendJson(res, out);
    });

    s.Post(R"(/api/plugs/([\w-]+)/(provision|restore))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const std::string action = req.matches[2];
        const json cfg = readConfigFile(ctx_.configFile);
        HttplibClient lan(kPlugConnectTimeout, kPlugReadTimeout);
        auto plugs = loadPlugs(cfg, lan);
        auto it = std::ranges::find_if(plugs, [&](const Plug& p) { return p.id == id; });
        if (it == plugs.end()) return sendError(res, 404, "unknown plug (save the configuration first)");
        Plug& plug = *it;
        try {
            if (action == "provision") {
                const json body = req.body.empty() ? json::object() : parseBody(req);
                const auto backupFile = saveBackup(ctx_.dataDir, plug, plug.sw->backup());
                plug.sw->provision(body.value("force", false));
                log::info("Web: provisioned plug {} (backup {})", id, backupFile.string());
                sendJson(res, json{{"failsafe", failsafeJson(plug)}, {"backup", backupFile.filename().string()}});
            } else {
                if (plug.mode == PlugMode::Live) {
                    return sendError(res, 409, "Set the plug to dry-run and save before restoring: a live plug "
                                               "without its fail-safe must not be switched.");
                }
                const auto file = originalBackup(ctx_.dataDir, plug);
                std::ifstream in(file);
                plug.sw->restore(json::parse(in));
                log::info("Web: restored plug {} from {}", id, file.string());
                sendJson(res, json{{"restored", file.filename().string()}, {"failsafe", failsafeJson(plug)}});
            }
        } catch (const std::exception& e) {
            log::warn("Web: {} of plug {} failed: {}", action, id, e.what());
            sendError(res, 502, e.what());
        }
    });


    s.Post("/api/notify/test", [this](const httplib::Request&, httplib::Response& res) {
        HttplibClient http;
        auto nc = loadNotifications(readConfigFile(ctx_.configFile), http);
        if (nc.channels.empty()) return sendError(res, 400, "No notification channel configured (save first)");
        json results = json::array();
        for (auto& ch : nc.channels) {
            try {
                ch->sendText("heatbrain test", "If you can read this, notifications work. 🔥");
                results.push_back({{"channel", ch->name()}, {"ok", true}});
            } catch (const std::exception& e) {
                results.push_back({{"channel", ch->name()}, {"ok", false}, {"error", e.what()}});
            }
        }
        sendJson(res, results);
    });

    s.Get("/api/firmware/latest", [this](const httplib::Request&, httplib::Response& res) {
        try {
            sendJson(res, latestFirmware());
        } catch (const std::exception& e) {
            sendError(res, 502, std::string("Could not check for Tasmota releases: ") + e.what());
        }
    });

    s.Get("/api/jobs", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::object();
        std::lock_guard lock(jobsMutex_);
        for (const auto& [id, j] : jobs_) {
            json log = json::array();
            for (const auto& [t, m] : j.log) log.push_back({t, m});
            out[id] = {{"kind", j.kind}, {"state", j.state}, {"started", j.started}, {"finished", j.finished}, {"log", log}};
        }
        sendJson(res, out);
    });

    s.Post(R"(/api/plugs/([\w-]+)/(upgrade|test|restart))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const std::string action = req.matches[2];
        const json cfg = readConfigFile(ctx_.configFile);
        // Each job builds its own client and plug objects: nothing is shared with the control loop.
        auto lan = std::make_shared<HttplibClient>(kPlugConnectTimeout, kPlugReadTimeout);
        auto plugs = std::make_shared<std::vector<Plug>>(loadPlugs(cfg, *lan));
        auto it = std::ranges::find_if(*plugs, [&](const Plug& p) { return p.id == id; });
        if (it == plugs->end()) return sendError(res, 404, "unknown plug (save the configuration first)");
        auto* tasmota = dynamic_cast<TasmotaSwitch*>(it->sw.get());
        if (!tasmota) return sendError(res, 400, "only Tasmota plugs support this");

        if (action == "restart") {
            try {
                tasmota->restart();
            } catch (const std::exception& e) {
                return sendError(res, 502, e.what());
            }
            return sendJson(res, json{{"restarting", true}});
        }
        if (it->mode == PlugMode::Live) {
            return sendError(res, 409, "Switch the plug to dry-run and save first: heatbrain never tests or upgrades a plug it is controlling.");
        }
        const auto status = it->sw->status();
        if (!status.reachable) return sendError(res, 502, "plug unreachable: " + status.error);

        const fs::path dataDir = ctx_.dataDir;
        const auto record = [dataDir, id](const std::string& version, bool ok, const std::string& detail) {
            saveFirmwareRecord(dataDir, id, FirmwareRecord{version, ok, formatIsoUtc(floor<seconds>(system_clock::now())), detail});
        };

        bool started = false;
        if (action == "test") {
            started = startJob(id, "test", [this, lan, plugs, tasmota, status, record](const auto& log) {
                log("Fail-safe test on Tasmota " + status.firmware + " (the relay is on for about 30 seconds)");
                const bool ok = tasmota->testPulse(log, [this](milliseconds d) { jobSleep(d); });
                record(status.firmware, ok, ok ? "auto-off and refresh verified" : "fail-safe test failed");
                log(ok ? "Fail-safe works on this firmware" : "Fail-safe test FAILED: this plug will not be switched live");
                return ok;
            });
        } else {
            json latest;
            try {
                latest = latestFirmware();
            } catch (const std::exception& e) {
                return sendError(res, 502, std::string("Could not check for Tasmota releases: ") + e.what());
            }
            const std::string target = latest.value("version", "");
            if (compareVersions(status.firmware, target) >= 0) {
                return sendError(res, 409, "Already on the latest version (" + status.firmware + ")");
            }
            Plug* plug = &*it;
            started = startJob(id, "upgrade", [this, lan, plugs, plug, tasmota, status, target, record, dataDir](const auto& log) {
                log("Upgrading from " + status.firmware + " to " + target + ". Do not unplug the plug.");
                const auto backup = saveBackup(dataDir, *plug, tasmota->backup());
                log("Settings backed up to " + backup.filename().string());
                const std::string url = tasmotaOtaUrl(status.firmware, status.hardware);
                tasmota->startUpgrade(url);
                log("Downloading " + url + " - the plug restarts by itself (twice on 1 MB devices)");

                // Wait for the real target build: 1 MB devices run "minimal" in between.
                std::string seen = status.firmware;
                bool wentOffline = false;
                int stable = 0;
                const auto deadline = std::chrono::steady_clock::now() + minutes{10};
                while (std::chrono::steady_clock::now() < deadline) {
                    jobSleep(seconds{5});
                    const auto st = tasmota->status();
                    if (!st.reachable) {
                        if (!wentOffline) log("Plug is restarting...");
                        wentOffline = true;
                        stable = 0;
                        continue;
                    }
                    if (st.firmware != seen) {
                        log("Plug is back running " + st.firmware);
                        seen = st.firmware;
                    }
                    const bool minimal = st.firmware.find("minimal") != std::string::npos;
                    stable = (!minimal && compareVersions(st.firmware, target) == 0) ? stable + 1 : 0;
                    if (stable >= 2) break;
                }
                if (stable < 2) {
                    log("FAILED: the plug did not come back on " + target + " within 10 minutes (last seen: " + seen + ")");
                    record(seen, false, "upgrade did not complete");
                    return false;
                }
                log("Upgrade complete: now on " + seen);

                const auto fsCheck = tasmota->checkFailsafe();
                if (fsCheck.installed()) {
                    log("Fail-safe settings survived the upgrade");
                } else {
                    for (const auto& p : fsCheck.problems) log("Note: " + p);
                }

                log("Running the fail-safe test (the relay is on for about 30 seconds)");
                const bool ok = tasmota->testPulse(log, [this](milliseconds d) { jobSleep(d); });
                record(seen, ok, ok ? "verified after upgrade" : "fail-safe test failed after upgrade");
                log(ok ? "All good: fail-safe verified on " + seen
                       : "Fail-safe test FAILED on " + seen + ": this plug will not be switched live");
                return ok;
            });
        }
        if (!started) return sendError(res, 409, "Another plug operation is running - wait for it to finish");
        sendJson(res, json{{"started", action}});
    });

    // The page itself is rendered (asset fingerprints); everything else is served as files.
    const auto index = [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(renderIndex(ctx_.webRoot), "text/html; charset=utf-8");
    };
    if (!ctx_.webRoot.empty()) {
        s.Get("/", index);
        s.Get("/index.html", index);
        s.set_mount_point("/", ctx_.webRoot.string());
        s.set_file_extension_and_mimetype_mapping("webmanifest", "application/manifest+json");
    }

    // Never let browsers keep UI files: with heuristic caching (and even with no-cache plus
    // 304 revalidation) Chrome kept running an old settings.js against a newer page. The
    // files are small and travel over the LAN only, so always sending them is cheap.
    s.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!req.path.starts_with("/api/")) {
            res.set_header("Cache-Control", "no-store");
            res.headers.erase("ETag");
            res.headers.erase("Last-Modified");
        }
    });

    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::invalid_argument& e) {
            sendError(res, 400, e.what());
        } catch (const std::exception& e) {
            sendError(res, 500, e.what());
        }
    });
}

void WebServer::start() {
    if (!server_->bind_to_port(cfg_.bind, cfg_.port)) {
        throw std::runtime_error(std::format("Web UI: cannot listen on {}:{} (port in use?)", cfg_.bind, cfg_.port));
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    log::info("Web UI on http://{}:{}/{}", cfg_.bind == "0.0.0.0" ? "localhost" : cfg_.bind, cfg_.port,
              ctx_.webRoot.empty() ? " (API only: web files not found)" : "");
}

void WebServer::stop() {
    stopping_ = true;
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

void WebServer::jobSleep(std::chrono::milliseconds d) const {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
        if (stopping_) throw std::runtime_error("heatbrain is stopping");
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
}

bool WebServer::startJob(const std::string& plugId, const std::string& kind,
                         std::function<bool(const std::function<void(const std::string&)>&)> fn) {
    const auto nowEpoch = [] { return static_cast<long long>(system_clock::now().time_since_epoch() / seconds{1}); };
    {
        std::lock_guard lock(jobsMutex_);
        // One device operation at a time: upgrades are risky enough one by one.
        for (const auto& [id, j] : jobs_) {
            if (j.state == "running") return false;
        }
        Job job;
        job.plugId = plugId;
        job.kind = kind;
        job.started = nowEpoch();
        jobs_[plugId] = job;
    }
    workers_.emplace_back([this, plugId, kind, fn = std::move(fn), nowEpoch] {
        const auto log = [&](const std::string& msg) {
            hb::log::info("Plug {} {}: {}", plugId, kind, msg);
            std::lock_guard lock(jobsMutex_);
            jobs_[plugId].log.emplace_back(nowEpoch(), msg);
        };
        bool ok = false;
        try {
            ok = fn(log);
        } catch (const std::exception& e) {
            log(std::string("FAILED: ") + e.what());
        }
        std::lock_guard lock(jobsMutex_);
        jobs_[plugId].state = ok ? "done" : "failed";
        jobs_[plugId].finished = nowEpoch();
    });
    return true;
}

json WebServer::latestFirmware() {
    // GitHub allows 60 unauthenticated requests per hour: cache for 12 h.
    HttplibClient internet;
    CachingHttpClient cache(internet, ctx_.dataDir / "cache");
    const json j = json::parse(cache.get("https://api.github.com/repos/arendst/Tasmota/releases/latest", hours{12}).body);
    return json{{"version", j.value("tag_name", "")},
                {"name", j.value("name", "")},
                {"publishedAt", j.value("published_at", "")},
                {"url", j.value("html_url", "")}};
}

}  // namespace hb
