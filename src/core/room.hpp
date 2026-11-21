#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hb {

// A time-of-day window in local time; from > to wraps past midnight (e.g. 23:00-07:00).
struct DayWindow {
    int fromMinute = 0;
    int toMinute = 1440;
    bool contains(int minuteOfDay) const {
        return fromMinute <= toMinute ? (minuteOfDay >= fromMinute && minuteOfDay < toMinute)
                                      : (minuteOfDay >= fromMinute || minuteOfDay < toMinute);
    }
};

struct TargetBand {
    DayWindow window;
    double target = 20.0;
    std::string label;  // "07:00-21:00", for explanations
};

// How eagerly the planner trades comfort for price. Defaults are deliberately mild.
struct Strategy {
    double hysteresis = 0.3;        // ±°C around target before switching
    double peakAllowedDrop = 1.0;   // during expensive slots, coast down to target - this
    double preheatBoost = 1.0;      // before expensive slots, heat up to target + this
    std::chrono::minutes preheatWindow{180};  // look this far ahead for expensive slots
    double cheapBoost = 0.5;        // in the cheapest slots, heat up to target + this
    std::chrono::minutes staleAfter{30};      // older temperature readings are not trusted
    std::chrono::minutes limpOn{10};          // duty cycle when the temperature is unknown
    std::chrono::minutes limpOff{50};
};

// Rooms without a temperature sensor: run the cheapest hours of each day.
struct PriceOnlyConfig {
    int hoursPerDay = 12;
    std::vector<DayWindow> avoid;   // never heat in these windows
};

struct RoomConfig {
    std::string id;
    std::string name;
    std::vector<std::string> plugIds;
    std::string sensor;             // "<source>:<sensor id or name>", empty for price-only
    double minTemp = 15.0;          // always heat below this, whatever the price
    double maxTemp = 23.0;          // never heat above this
    double frostTemp = 5.0;         // even a manual OFF heats below this
    std::vector<TargetBand> schedule;
    Strategy strategy;
    std::optional<PriceOnlyConfig> priceOnly;
};

// Target band in effect at a local minute of day (first match), or nullptr.
const TargetBand* targetAt(const RoomConfig& room, int minuteOfDay);

std::string formatWindow(const DayWindow& w);  // "17:00-21:00"

}  // namespace hb
