#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/failsafe.hpp"
#include "core/offline.hpp"

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
    std::string model;        // "Sonoff Basic R4" (Tasmota's Module), when known
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

    // Wi-Fi off period: load `spans` onto the device so it switches on its own clock until
    // `until`, with the fail-safe stretched to match (auto-off covers the longest planned ON,
    // the watchdog runs until the period is over). Throws with a readable reason when the
    // device can't do it (clock not synced, its rule slots in use, too many switch points).
    // radioOff: the plug also switches its own Wi-Fi off in that window, if its firmware can;
    // returns whether it will.
    virtual bool armOffline(const std::vector<OnSpan>& spans, Instant now, Instant until,
                            std::optional<OfflinePeriod> radioOff = {}) {
        (void)spans, (void)now, (void)until, (void)radioOff;
        return false;
    }
    // Dry-run plug: only its own Wi-Fi off and on again in the window - no schedule, the relay
    // untouched. Returns whether the plug will (false: its firmware can't). Undone by disarmOffline.
    virtual bool armRadioOff(const OfflinePeriod& radioOff, Instant now) {
        (void)radioOff, (void)now;
        return false;
    }
    // Back on the network: remove the schedule (and the Wi-Fi off) and restore the normal fail-safe timers.
    virtual void disarmOffline() {}

    // Dry-run plug with a fail-safe: restart the watchdog without switching anything, so limp mode
    // only ever starts when heatbrain itself is gone. `until`: keep it quiet until then (Wi-Fi off).
    virtual void keepAlive(Instant now, std::optional<Instant> until) { (void)now, (void)until; }
};

}  // namespace hb
