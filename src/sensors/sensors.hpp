#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "net/http.hpp"
#include "sensors/temperature_source.hpp"

namespace hb {

// Builds sources from the config's "temperatureSources" array. Token/state files are
// kept in dataDir. Types: "netatmo", "velux".
std::vector<std::unique_ptr<TemperatureSource>> loadTemperatureSources(
    const nlohmann::json& cfg, HttpClient& http, const std::filesystem::path& dataDir,
    std::function<Instant()> now);

}  // namespace hb
