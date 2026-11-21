#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/rooms.hpp"
#include "core/alerts.hpp"
#include "core/planner.hpp"
#include "plugs/plugs.hpp"
#include "providers/price_model.hpp"
#include "sensors/temperature_source.hpp"
#include "storage/history.hpp"

namespace hb {

struct RoomResult {
    const RoomConfig* room = nullptr;
    Decision decision;
    std::vector<PlanSegment> plan;  // next 24 h
    bool changed = false;
    nlohmann::json plugs = nlohmann::json::array();
};

struct TickResult {
    std::vector<RoomResult> rooms;
    std::vector<Problem> problems;  // fed to the AlertManager
};

nlohmann::json toJson(const DecisionInputs& in);

// One control step: read prices and sensors, decide every room, switch live plugs,
// record history. The same code backs `heatbrain plan` (apply = false) and `heatbrain run`.
class Controller {
public:
    Controller(const TimeZone& tz, PriceModel& prices, std::vector<std::unique_ptr<TemperatureSource>>& sources,
               std::vector<Plug>& plugs, std::vector<RoomConfig> rooms, const Overrides* overrides,
               History* history);

    // apply = false: decide only - no plug commands, no history writes.
    TickResult tick(Instant now, bool apply);

    const std::vector<RoomConfig>& rooms() const { return rooms_; }
    const PriceTable& prices() const { return prices_; }
    const std::map<std::string, SensorReading>& readings() const { return readings_; }
    void setDataDir(std::filesystem::path dir) { dataDir_ = std::move(dir); }
    void setAwayStore(const AwayStore* away) { away_ = away; }
    void setOutdoorSensor(std::string ref) { outdoorSensor_ = std::move(ref); }
    // The outdoor reading used in the last tick, if any.
    struct OutdoorUsed {
        std::string key;   // "<source>:<sensor id>" as stored in the history
        std::string name;
        double temperature = 0;
    };
    const std::optional<OutdoorUsed>& outdoor() const { return outdoorUsed_; }

private:
    void refreshPrices(Instant now, TickResult& result);
    void readSensors(TickResult& result);
    const SensorReading* findSensor(const std::string& ref) const;
    nlohmann::json applyToPlugs(const RoomConfig& room, bool on, Instant now, TickResult& result);
    // Empty when the plug may be switched live; otherwise why not.
    std::string failsafeProblem(Plug& plug, Instant now);

    const TimeZone& tz_;
    PriceModel& priceModel_;
    std::vector<std::unique_ptr<TemperatureSource>>& sources_;
    std::vector<Plug>& plugs_;
    std::vector<RoomConfig> rooms_;
    const Overrides* overrides_;
    const AwayStore* away_ = nullptr;
    std::string outdoorSensor_;  // "<source>:<id or name>"; empty = first outdoor sensor
    std::optional<OutdoorUsed> outdoorUsed_;
    History* history_;

    PriceTable prices_;
    std::map<std::string, SensorReading> readings_;  // "<source>:<sensor id>", last good reading
    std::map<std::string, bool> lastOn_;             // room id -> previous decision
    std::map<std::string, std::pair<std::string, Instant>> failsafeChecks_;  // plug id -> (problem, checked at)
    std::filesystem::path dataDir_;  // firmware test records; empty = not checked
    std::optional<Instant> lastPrune_;
};

}  // namespace hb
