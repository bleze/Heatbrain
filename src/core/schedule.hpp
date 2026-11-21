#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "core/time.hpp"

namespace hb {

// A static time-of-day price schedule with optional seasons and weekdays.
// This is the generic way to describe a grid tariff when no API publishes it.
struct ScheduleBand {
    int fromMinute = 0;   // inclusive, minutes after local midnight
    int toMinute = 1440;  // exclusive
    double amount = 0.0;
};

struct ScheduleSeason {
    std::array<bool, 13> months{};    // index 1..12
    std::array<bool, 8> weekdays{};   // index 1..7 (ISO, Monday = 1)
    std::vector<ScheduleBand> bands;
};

struct Schedule {
    std::vector<ScheduleSeason> seasons;

    // First matching season, then first matching band.
    std::optional<double> at(LocalTime t) const;
};

// "HH:MM" -> minutes after midnight. Accepts "24:00" as end of day.
std::optional<int> parseClock(std::string_view s);

}  // namespace hb
