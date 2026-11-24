#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "net/http.hpp"
#include "plugs/switch.hpp"

namespace hb {

// HTTP timeouts for plugs on the LAN. Generous on purpose: ESP8266 plugs with a weak signal
// or WiFi power save regularly take 2-3 s to accept a connection.
inline constexpr std::chrono::milliseconds kPlugConnectTimeout{6000};
inline constexpr std::chrono::milliseconds kPlugReadTimeout{8000};

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

// Result of the last fail-safe function test, per plug (<dataDir>/plugs/<id>/firmware.json).
// A live plug whose firmware differs from a recorded test - or whose test failed - is not
// switched until the test passes on the current firmware.
struct FirmwareRecord {
    std::string version;
    bool verified = false;
    std::string checkedAt;
    std::string detail;
};
// The backup to restore "original" settings from: the newest one taken while heatbrain's fail-safe
// was NOT on the plug (backups are also taken before upgrades, when it usually is).
std::filesystem::path originalBackup(const std::filesystem::path& dataDir, const Plug& plug);
bool backupHasFailsafe(const nlohmann::json& backup);

std::optional<FirmwareRecord> loadFirmwareRecord(const std::filesystem::path& dataDir, const std::string& plugId);
void saveFirmwareRecord(const std::filesystem::path& dataDir, const std::string& plugId, const FirmwareRecord& r);

struct DiscoveredPlug {
    std::string host;
    std::string deviceName;
    std::string firmware;
    std::string hardware;
    std::string model;  // "Sonoff Basic" (Tasmota's Module), when known
    std::optional<bool> on;
};

// Probes every address of a /24 network ("192.168.1.0/24" or "192.168.1") for Tasmota.
std::vector<DiscoveredPlug> discoverTasmota(std::string_view subnet, HttpClient& http);

}  // namespace hb
