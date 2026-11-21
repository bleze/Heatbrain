#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hb {

struct AppConfig {
    std::filesystem::path dataDir;
    std::filesystem::path configFile;
    std::string timezone;
    std::string currency;
    nlohmann::json raw;
};

// Loads <dataDir>/heatbrain.json. If it does not exist, the first existing file in
// exampleCandidates is copied there first.
AppConfig loadConfig(const std::filesystem::path& dataDir,
                     const std::vector<std::filesystem::path>& exampleCandidates);

}  // namespace hb
