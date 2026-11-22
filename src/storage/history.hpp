#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/price.hpp"
#include "providers/eloverblik.hpp"
#include "core/time.hpp"

struct sqlite3;

namespace hb {

struct DecisionRecord {
    Instant at;
    std::string roomId;
    bool on = false;
    bool changed = false;   // differs from the room's previous decision
    std::string reason;     // reasonCode()
    std::string detail;
    nlohmann::json inputs;  // DecisionInputs snapshot
    nlohmann::json plugs;   // per plug: mode, applied state, errors
};

struct TemperatureRecord {
    Instant at;             // measurement time reported by the sensor
    std::string sensorKey;  // "<source>:<sensor id>"
    std::string roomId;     // empty if the sensor is not mapped to a room
    double temperature = 0;
    std::optional<double> humidity;
};

struct PricePoint {
    Instant slot;
    double total = 0;
};

// SQLite store for temperatures, decisions and prices (WAL mode, so the CLI can read
// while `heatbrain run` writes).
class History {
public:
    explicit History(const std::filesystem::path& dbFile);
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    void recordDecision(const DecisionRecord& d);
    void recordTemperature(const TemperatureRecord& t);  // duplicates (same sensor+time) ignored
    void recordPrices(const PriceTable& table);

    std::vector<DecisionRecord> decisions(const std::string& roomId, Instant from, Instant to,
                                          bool changesOnly) const;
    std::optional<DecisionRecord> lastDecision(const std::string& roomId) const;
    std::vector<TemperatureRecord> temperatures(const std::string& roomId, Instant from, Instant to) const;
    std::vector<TemperatureRecord> sensorTemperatures(const std::string& sensorKey, Instant from, Instant to) const;
    std::vector<PricePoint> prices(Instant from, Instant to) const;
    // Household meter readings (Eloverblik), replaced when fetched again.
    void recordMeter(const std::vector<MeterReading>& readings);
    std::vector<MeterReading> meterReadings(Instant from, Instant to) const;
    std::optional<MeterReading> lastMeterReading() const;
    std::optional<MeterReading> firstMeterReading() const;
    // Times of all stored readings (any sensor), sorted: shows when the sensors were online.
    std::vector<Instant> readingTimes(Instant from, Instant to) const;

    void prune(Instant olderThan);

    // Consistent copy of the whole database into a new file (VACUUM INTO), safe while writing.
    void snapshotTo(const std::filesystem::path& file) const;
    // Replaces all rows with those of another heatbrain history database, in one transaction.
    void replaceFrom(const std::filesystem::path& file);

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
};

}  // namespace hb
