#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/failsafe.hpp"

namespace hb {

struct PlugStatus {
    bool reachable = false;
    std::optional<bool> on;
    std::string deviceName;
    std::string firmware;
    std::string hardware;
    std::optional<int> rssi;  // WiFi signal, percent
    std::string uptime;
    std::string error;        // when unreachable
};

struct FailsafeStatus {
    std::vector<std::string> problems;  // empty = installed and correct
    bool installed() const { return problems.empty(); }
};

// A controllable relay. Implementations are selected by the plug's "type" in config.
class Switch {
public:
    virtual ~Switch() = default;

    virtual PlugStatus status() = 0;

    // Commands the relay and refreshes the on-plug fail-safe timers.
    // Returns the state the device confirmed. Throws on communication failure.
    virtual bool set(bool on) = 0;

    virtual FailsafeStatus checkFailsafe() = 0;

    // Snapshot of every device setting provision() changes, for restore().
    virtual nlohmann::json backup() = 0;

    // Installs the fail-safe and disables device-local schedules that would fight heatbrain.
    // Refuses to overwrite existing device rules unless overwriteRules is set.
    virtual void provision(bool overwriteRules) = 0;

    virtual void restore(const nlohmann::json& backup) = 0;
};

}  // namespace hb
