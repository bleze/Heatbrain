#include "app/config_service.hpp"

#include <algorithm>
#include <format>
#include <fstream>

#include "app/rooms.hpp"
#include "core/time.hpp"
#include "notify/notifier.hpp"
#include "plugs/plugs.hpp"
#include "providers/price_model.hpp"
#include "sensors/sensors.hpp"
#include "web/server.hpp"

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace {

void maskField(json& obj, const char* key) {
    if (obj.is_object() && obj.contains(key) && obj[key].is_string() && !obj[key].get<std::string>().empty()) {
        obj[key] = kSecretMask;
    }
}

void restoreField(json& incoming, const json* stored, const char* key) {
    if (!incoming.is_object() || !incoming.contains(key) || incoming[key] != kSecretMask) return;
    if (stored && stored->contains(key)) incoming[key] = (*stored)[key];
    else incoming[key] = "";  // nothing to restore: never store the mask itself
}

const json* findById(const json& stored, const char* arrayKey, const json& item) {
    if (!stored.contains(arrayKey) || !item.contains("id")) return nullptr;
    for (const auto& s : stored[arrayKey]) {
        if (s.value("id", json()) == item["id"]) return &s;
    }
    return nullptr;
}

// Netatmo sources may lack an explicit id: it defaults to the type.
json withSourceIds(json arr) {
    for (auto& s : arr) {
        if (!s.contains("id")) s["id"] = s.value("type", std::string("source"));
    }
    return arr;
}

// HTTP client that never touches the network (validation must not have side effects).
class NoNetwork final : public HttpClient {
public:
    HttpResponse send(const HttpRequest&) override { throw HttpError("no network during validation"); }
};

}  // namespace

json maskSecrets(const json& cfg) {
    json out = cfg;
    if (out.contains("temperatureSources")) {
        for (auto& s : out["temperatureSources"]) {
            maskField(s, "clientSecret");
            maskField(s, "refreshToken");
        }
    }
    if (out.contains("plugs")) {
        for (auto& p : out["plugs"]) maskField(p, "password");
    }
    if (out.contains("notifications") && out["notifications"].contains("channels")) {
        for (auto& c : out["notifications"]["channels"]) {
            maskField(c, "webhookUrl");
            maskField(c, "token");
        }
    }
    return out;
}

json unmaskSecrets(json incoming, const json& stored) {
    if (incoming.contains("temperatureSources")) {
        incoming["temperatureSources"] = withSourceIds(incoming["temperatureSources"]);
        json storedSources = stored;
        if (stored.contains("temperatureSources")) storedSources["temperatureSources"] = withSourceIds(stored["temperatureSources"]);
        for (auto& s : incoming["temperatureSources"]) {
            const json* old = findById(storedSources, "temperatureSources", s);
            restoreField(s, old, "clientSecret");
            restoreField(s, old, "refreshToken");
        }
    }
    if (incoming.contains("plugs")) {
        for (auto& p : incoming["plugs"]) restoreField(p, findById(stored, "plugs", p), "password");
    }
    if (incoming.contains("notifications") && incoming["notifications"].contains("channels")) {
        auto& channels = incoming["notifications"]["channels"];
        const json* storedChannels = stored.contains("notifications") && stored["notifications"].contains("channels")
                                         ? &stored["notifications"]["channels"]
                                         : nullptr;
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const json* old = storedChannels && i < storedChannels->size() ? &(*storedChannels)[i] : nullptr;
            restoreField(channels[i], old, "webhookUrl");
            restoreField(channels[i], old, "token");
        }
    }
    return incoming;
}

std::vector<std::string> validateConfig(const json& cfg, const fs::path& dataDir) {
    std::vector<std::string> errors;
    const auto check = [&](const char* part, auto&& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            errors.push_back(std::format("{}: {}", part, e.what()));
        }
    };
    if (!cfg.is_object()) return {"Config must be a JSON object"};

    NoNetwork offline;
    std::unique_ptr<TimeZone> tz;
    check("Time zone", [&] { tz = std::make_unique<TimeZone>(cfg.value("timezone", std::string("Europe/Copenhagen"))); });
    if (tz) {
        check("Prices", [&] {
            const fs::path scratch = fs::temp_directory_path() / "heatbrain-validate-cache";
            CachingHttpClient cache(offline, scratch);
            ProviderContext ctx{cache, *tz, cfg.value("currency", std::string("DKK")), [] { return Instant{}; }};
            PriceModel model(cfg.at("price"), ctx);
        });
    }
    std::vector<Plug> plugs;
    check("Plugs", [&] { plugs = loadPlugs(cfg, offline); });
    check("Rooms", [&] { parseRooms(cfg, plugs); });
    check("Temperature sources", [&] { loadTemperatureSources(cfg, offline, dataDir, [] { return Instant{}; }); });
    check("Notifications", [&] { loadNotifications(cfg, offline); });
    check("Web", [&] {
        const auto w = parseWebConfig(cfg);
        if (w.port < 1 || w.port > 65535) throw std::runtime_error("port must be 1..65535");
    });
    check("Control", [&] {
        const int tick = cfg.contains("control") ? cfg["control"].value("tickSeconds", 300) : 300;
        if (tick < 60) throw std::runtime_error("tickSeconds must be at least 60");
        for (const auto& p : plugs) {
            if (p.mode == PlugMode::Live && seconds{tick} * 2 > duration_cast<seconds>(p.failsafe.pulse)) {
                throw std::runtime_error(std::format("plug {}: tick ({} s) must be at most half its fail-safe pulse ({} min)",
                                                     p.id, tick, p.failsafe.pulse.count()));
            }
        }
    });
    return errors;
}

void saveConfig(const fs::path& configFile, const json& cfg) {
    const fs::path backups = configFile.parent_path() / "config-backups";
    fs::create_directories(backups);
    if (fs::exists(configFile)) {
        const auto now = floor<seconds>(system_clock::now());
        fs::copy_file(configFile, backups / std::format("heatbrain-{:%Y%m%dT%H%M%SZ}.json", now),
                      fs::copy_options::overwrite_existing);
        std::vector<fs::path> all;
        for (const auto& e : fs::directory_iterator(backups)) all.push_back(e.path());
        std::ranges::sort(all);
        for (std::size_t i = 0; i + 20 < all.size(); ++i) fs::remove(all[i]);
    }
    fs::path tmp = configFile;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << cfg.dump(2) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, configFile);
}

}  // namespace hb
