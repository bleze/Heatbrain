#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/planner.hpp"
#include "core/time.hpp"

namespace hb {

// Battery warnings timed by when a sensor's battery will actually run out, not at a fixed level:
// replacing batteries at 20 % is wasteful. From the daily levels the drain per day is estimated;
// the warning comes about three weeks ahead - or at once, if it would run out during a planned
// away period.
struct BatterySample {
    Instant day;     // start of the local day
    int percent = 0;
};

constexpr int kBatteryEmptyPercent = 5;       // Netatmo modules stop around here
constexpr int kBatteryFallbackPercent = 10;   // warn here while the drain is unknown

// Days until the battery reaches kBatteryEmptyPercent, from a least-squares line through the last
// 60 days; nullopt while there are fewer than 10 days of history or no decline.
std::optional<double> daysUntilEmpty(const std::vector<BatterySample>& samples);

struct BatteryWarning {
    std::string message;
};
// What to tell about one sensor now, if anything.
std::optional<BatteryWarning> batteryWarning(const std::string& name, int percent,
                                             const std::vector<BatterySample>& samples, Instant now,
                                             const std::optional<AwayMode>& away, const TimeZone& tz);

}  // namespace hb
