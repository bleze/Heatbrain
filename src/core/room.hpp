#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
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

// What a room does in a period of its daily program.
enum class ProgramMode {
    Heat,  // heat to the target, price-aware (pre-heat, coast in expensive hours, cheap boost)
    Keep,  // only keep above the target: no pre-heating or boosting, ignores price
    Off,   // off; only frost protection still heats
};
std::string_view modeName(ProgramMode m);  // "heat" | "keep" | "off"

// A switch point: from `atMinute` until the next point, the room does `mode`.
// The last point of the day continues past midnight until the first one.
struct ProgramPoint {
    int atMinute = 0;
    ProgramMode mode = ProgramMode::Heat;
    double target = 20.0;  // unused for Off
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
    std::string thermostat;         // thermostat id instead of plugs (heatbrain sets its setpoint)
    std::string sensor;             // "<source>:<sensor id or name>", empty for price-only
    double minTemp = 15.0;          // in heat periods: heat below this whatever the price
    double maxTemp = 23.0;          // never heat above this
    double frostTemp = 5.0;         // even "off" or a manual OFF heats below this
    std::vector<ProgramPoint> program;  // sorted by time, at least one point
    std::chrono::minutes warmup{180};   // a booking starts heating this long before it begins
    Strategy strategy;
    std::optional<PriceOnlyConfig> priceOnly;
};

// The program period in effect at a local minute of day.
struct ActivePeriod {
    ProgramPoint point;
    int fromMinute = 0;
    int toMinute = 1440;  // start of the next point (may be smaller than from: wraps)
    std::string label;    // "17:00-21:00"
};
std::optional<ActivePeriod> periodAt(const RoomConfig& room, int minuteOfDay);

std::string formatWindow(const DayWindow& w);  // "17:00-21:00"

}  // namespace hb
