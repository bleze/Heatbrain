#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"

namespace hb {

struct SensorReading {
    std::string sensorId;   // stable id, used to map sensors to rooms
    std::string name;       // user-visible name from the source
    std::string station;    // grouping (e.g. Netatmo station / home)
    std::string kind;       // "indoor" or "outdoor"
    std::optional<double> temperature;  // °C
    std::optional<double> humidity;     // %
    std::optional<int> co2;             // ppm
    std::optional<Instant> measuredAt;
    bool reachable = true;
    std::optional<int> batteryPercent;
    std::optional<std::pair<double, double>> location;  // latitude, longitude of the station, if known
};

// Something that reports room temperatures. Selected by "type" in temperatureSources.
class TemperatureSource {
public:
    virtual ~TemperatureSource() = default;
    virtual const std::string& id() const = 0;
    // Current readings of all sensors. Throws on failure.
    virtual std::vector<SensorReading> read() = 0;
};

}  // namespace hb
