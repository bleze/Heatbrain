#include "app/config.hpp"

#include <fstream>
#include <stdexcept>

#include "core/log.hpp"

namespace hb {

namespace fs = std::filesystem;

AppConfig loadConfig(const fs::path& dataDir, const std::vector<fs::path>& exampleCandidates) {
    AppConfig cfg;
    cfg.dataDir = dataDir;
    cfg.configFile = dataDir / "heatbrain.json";
    fs::create_directories(dataDir);

    if (!fs::exists(cfg.configFile)) {
        bool copied = false;
        for (const auto& candidate : exampleCandidates) {
            if (fs::exists(candidate)) {
                fs::copy_file(candidate, cfg.configFile);
                log::info("Created {} from {}", cfg.configFile.string(), candidate.string());
                copied = true;
                break;
            }
        }
        if (!copied) {
            throw std::runtime_error("No config at " + cfg.configFile.string() +
                                     " and no example config found to copy");
        }
    }

    std::ifstream in(cfg.configFile);
    try {
        cfg.raw = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(cfg.configFile.string() + ": " + e.what());
    }
    cfg.timezone = cfg.raw.value("timezone", std::string("Europe/Copenhagen"));
    cfg.currency = cfg.raw.value("currency", std::string("DKK"));
    return cfg;
}

}  // namespace hb
