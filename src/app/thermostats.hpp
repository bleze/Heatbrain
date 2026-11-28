#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/planner.hpp"
#include "core/room.hpp"
#include "devices/devi.hpp"
#include "sensors/temperature_source.hpp"
#include "storage/history.hpp"

namespace hb {

// Thermostats regulate by themselves; heatbrain sets their setpoint. Unlike plugs they need no
// fail-safe: when heatbrain is gone they keep the last setpoint. They regulate on their own sensor,
// though, not the room's: the setpoint is steered so that the relay does what heatbrain decided.
//
// "thermostats": [ { "id": "pizzeria", "type": "devi", "name": "Pizzeria",
//                    "peerId": "<64 hex>", "mode": "dryrun" | "live", "watts": 1200 } ]
struct ThermostatConfig {
    std::string id;
    std::string type = "devi";
    std::string name;
    std::string peerId;
    bool live = false;
    double watts = 0;  // for cost estimates
};
std::vector<ThermostatConfig> parseThermostats(const nlohmann::json& cfg);

// The DEVI connections, shared by the control loop and the web server (a thermostat accepts only
// a couple of connections at a time). Lazily created per thermostat.
class DeviRegistry {
public:
    static DeviRegistry& instance();
    void setDataDir(const std::filesystem::path& dir);
    std::shared_ptr<DeviThermostat> get(const std::string& peerId);  // throws without identity
    std::optional<DeviIdentity> identity();
    void reload();  // after pairing: read the identity again

private:
    std::mutex mutex_;
    std::filesystem::path dataDir_;
    std::optional<DeviIdentity> identity_;
    std::map<std::string, std::shared_ptr<DeviThermostat>> thermostats_;
};

// The thermostats' own sensors as a temperature source "devi": "<id>" is the wall (room) sensor,
// "<id>-floor" the floor probe.
class DeviSource final : public TemperatureSource {
public:
    explicit DeviSource(std::vector<ThermostatConfig> thermostats);
    const std::string& id() const override { return id_; }
    std::vector<SensorReading> read() override;

private:
    std::string id_ = "devi";
    std::vector<ThermostatConfig> thermostats_;
};

// What the thermostat itself reads (its own sensor, where heatbrain decides on the room's) and
// the manual setpoint it has now.
struct DeviReading {
    std::optional<double> temperature;
    std::optional<double> setpoint;
};

// The setpoint for a thermostat room from the planner's decision: the period's target, lower
// while it is expensive, higher when pre-heating or in the cheapest hours, frost protection when
// off, never above the room's maximum. With the thermostat's own reading it is steered so that the
// relay follows the decision: heating means at least kSteerMargin above that reading, off at least
// kSteerMargin below it (no lower than frost protection, or the minimum in a heat period - the
// thermostat's own guard when heatbrain is gone). A setpoint that already does so is kept.
inline constexpr double kSteerMargin = 0.5;
double thermostatSetpoint(const RoomConfig& room, const Decision& d, const DeviReading& devi = {});

// Thermostat rooms: each decision's `on` (what heatbrain wanted) becomes what the relay did until
// the next decision - on for at least half of it, by the thermostat's heating-time counter, or
// else its relay state at the time - and `changed` follows. Decisions without a thermostat are
// left alone. Learning how the room warms and cools needs what really happened.
void useRelayTime(std::vector<DecisionRecord>& decisions, std::chrono::seconds maxGap = std::chrono::minutes{15});
// The thermostat entry of a decision's plugs, if it has one.
const nlohmann::json* thermostatEntry(const DecisionRecord& d);

}  // namespace hb
