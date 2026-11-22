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
#include "core/offline.hpp"
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

    // Daily Wi-Fi off period (nullopt = none). The night schedule goes onto the plugs `lead`
    // before it starts - at least one control interval, so a tick is sure to fall in between.
    // Call after setDataDir (the loaded schedules survive a restart in <dataDir>/offline.json).
    void setOfflineWindow(std::optional<WifiOffPlan> plan, std::chrono::minutes lead);
    void setOfflineWindow(DayWindow window, std::chrono::minutes lead) {
        setOfflineWindow(WifiOffPlan{window, std::nullopt}, lead);
    }
    struct OfflineInfo {
        OfflinePeriod period;
        bool active = false;                  // inside the period now
        std::vector<std::string> armedPlugs;  // plugs following a loaded schedule
        std::vector<std::string> errors;      // plugs whose schedule could not be loaded
        bool late = false;                    // past the planned end and the network isn't back
        std::optional<Instant> scheduleUntil; // how far the loaded schedule reaches
    };
    std::optional<OfflineInfo> offlineInfo(Instant now) const;
    // Share of time a room heated in heat / keep periods on recent days (decision log).
    OfflineDuty learnedDuty(const std::string& roomId, Instant now) const;

private:
    void refreshPrices(Instant now, TickResult& result);
    void readSensors(TickResult& result);
    const SensorReading* findSensor(const std::string& ref) const;
    nlohmann::json applyToPlugs(const RoomConfig& room, bool on, Instant now, TickResult& result);
    // Empty when the plug may be switched live; otherwise why not.
    std::string failsafeProblem(Plug& plug, Instant now);
    void armOffline(const OfflinePeriod& period, Instant now, std::optional<AwayMode> away);
    void disarmOffline(Instant now, bool networkBack);
    bool networkBackSince(Instant t) const;  // a sensor reading newer than t arrived
    void loadOfflineState();
    void saveOfflineState() const;

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

    std::optional<WifiOffPlan> offlineWindow_;
    std::chrono::minutes offlineLead_{10};
    struct ArmedRoom {
        Instant start;  // when the Wi-Fi goes off
        Instant until;
        std::vector<OnSpan> spans;
        Instant loadedAt;
    };
    std::map<std::string, ArmedRoom> armedRooms_;          // room id -> schedule followed until
    std::map<std::string, Instant> armedPlugs_;            // plug id -> schedule on the device until
    std::optional<OfflinePeriod> armedPeriod_;             // the planned period the schedules are for
    std::optional<Instant> lastDisarmTry_;
    std::optional<Instant> handledPeriodEnd_;  // schedules were loaded for the period ending then
    std::map<std::string, std::pair<std::string, Instant>> offlineArmErrors_;  // plug id -> (why, period end)
};

}  // namespace hb
