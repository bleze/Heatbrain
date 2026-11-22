#include "app/power_mix_sync.hpp"

#include <fstream>

#include "core/log.hpp"
#include "net/http.hpp"
#include "providers/power_mix.hpp"

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

namespace {
constexpr minutes kEvery{15};
}  // namespace

PowerMixSync::PowerMixSync(std::string area, fs::path dataDir)
    : area_(std::move(area)), dataDir_(std::move(dataDir)), thread_([this] { run(); }) {}

PowerMixSync::~PowerMixSync() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void PowerMixSync::run() {
    while (true) {
        syncOnce();
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, kEvery, [this] { return stop_; })) return;
    }
}

void PowerMixSync::syncOnce() {
    try {
        HttplibClient internet(seconds{10}, seconds{60});
        CachingHttpClient http(internet, dataDir_ / "cache");
        // Energi Data Service allows only a few requests a minute (shared with the prices): cached
        // ten minutes, two requests per round.
        const auto prodex = json::parse(http.get(prodexUrl(area_), minutes{10}).body);
        const auto co2 = json::parse(http.get(co2Url(area_), minutes{10}).body);
        json hours = json::array();
        for (const auto& h : parsePowerMix(prodex, co2)) {
            hours.push_back({h.hour.time_since_epoch().count(), h.offshoreWind, h.onshoreWind, h.solar, h.plants,
                             h.imports, h.co2 ? json(*h.co2) : json(nullptr)});
        }
        const json out{{"area", area_},
                       {"updated", floor<seconds>(system_clock::now()).time_since_epoch().count()},
                       {"hours", hours}};
        const fs::path file = dataDir_ / "power-mix.json", tmp = dataDir_ / "power-mix.json.tmp";
        std::ofstream(tmp) << out.dump();
        fs::rename(tmp, file);
    } catch (const std::exception& e) {
        log::warn("Power mix: {}", e.what());
    }
}

json loadPowerMix(const fs::path& dataDir) {
    try {
        std::ifstream in(dataDir / "power-mix.json");
        if (in) return json::parse(in);
    } catch (const std::exception&) {
    }
    return json::object();
}

std::string spotArea(const json& cfg) {
    if (!cfg.contains("price") || !cfg["price"].is_object()) return {};
    for (const auto& c : cfg["price"].value("components", json::array())) {
        if (c.value("type", "") == "energidataservice.spot") return c.value("area", "");
    }
    return {};
}

}  // namespace hb
