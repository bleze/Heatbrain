#include "sensors/sensors.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <stdexcept>

#include "sensors/netatmo.hpp"

namespace hb {

std::vector<std::unique_ptr<TemperatureSource>> loadTemperatureSources(
    const nlohmann::json& cfg, HttpClient& http, const std::filesystem::path& dataDir,
    std::function<Instant()> now) {
    std::vector<std::unique_ptr<TemperatureSource>> sources;
    if (!cfg.contains("temperatureSources")) return sources;
    std::set<std::string> ids;
    for (const auto& sj : cfg.at("temperatureSources")) {
        const std::string id = sj.value("id", sj.value("type", std::string("source")));
        if (!ids.insert(id).second) throw std::runtime_error("Duplicate temperature source id: " + id);
        const std::string type = sj.at("type").get<std::string>();
        if (type == "netatmo") {
            if (sj.value("clientId", "").empty()) continue;  // not set up yet (e.g. fresh install)
            netatmo::Auth auth(http, sj.value("clientId", ""), sj.value("clientSecret", ""),
                               sj.value("refreshToken", ""), dataDir / (id + "-tokens.json"), now);
            sources.push_back(std::make_unique<netatmo::Source>(id, http, std::move(auth)));
        } else {
            throw std::runtime_error("Temperature source " + id + ": unknown type '" + type +
                                     "' (known: netatmo)");
        }
    }
    return sources;
}

}  // namespace hb
