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

namespace hb {

// Thermostats regulate by themselves; heatbrain sets their setpoint. Unlike plugs they need no
// fail-safe: when heatbrain is gone they keep the last setpoint.
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

// The setpoint for a thermostat room from the planner's decision: the period's target, lower
// while it is expensive, higher when pre-heating or in the cheapest hours, frost protection when
// off, never above the room's maximum.
double thermostatSetpoint(const RoomConfig& room, const Decision& d);

}  // namespace hb
