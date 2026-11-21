#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "net/http.hpp"
#include "plugs/switch.hpp"

namespace hb {

enum class PlugMode {
    Live,    // heatbrain switches the plug
    DryRun,  // heatbrain decides and logs, but never touches the plug
};

struct Plug {
    std::string id;
    std::string name;
    std::string type;
    std::string host;
    PlugMode mode = PlugMode::DryRun;
    FailsafeConfig failsafe;
    std::unique_ptr<Switch> sw;
};

FailsafeConfig parseFailsafe(const nlohmann::json& cfg);

// Builds plugs from the config's "plugs" array. lanHttp is used for device commands.
std::vector<Plug> loadPlugs(const nlohmann::json& cfg, HttpClient& lanHttp);

// Backups of device settings live in <dataDir>/plugs/<id>/.
std::filesystem::path saveBackup(const std::filesystem::path& dataDir, const Plug& plug,
                                 const nlohmann::json& backup);
std::filesystem::path latestBackup(const std::filesystem::path& dataDir, const Plug& plug);

struct DiscoveredPlug {
    std::string host;
    std::string deviceName;
    std::string firmware;
    std::string hardware;
    std::optional<bool> on;
};

// Probes every address of a /24 network ("192.168.1.0/24" or "192.168.1") for Tasmota.
std::vector<DiscoveredPlug> discoverTasmota(std::string_view subnet, HttpClient& http);

}  // namespace hb
