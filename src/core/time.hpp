#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hb {

using Instant = std::chrono::sys_seconds;          // a point in time (UTC)
using LocalTime = std::chrono::local_seconds;      // wall-clock time in some zone

// Prices and plans are computed in fixed slots of this length.
inline constexpr std::chrono::minutes kSlotLength{15};

// Parses "YYYY-MM-DD", "YYYY-MM-DDTHH:MM" or "YYYY-MM-DDTHH:MM:SS" (fraction and zone
// suffix are ignored). The result is a naive wall-clock time; the caller decides the zone.
std::optional<LocalTime> parseIsoLocal(std::string_view s);

// Same format, interpreted as UTC.
std::optional<Instant> parseIsoUtc(std::string_view s);

std::string formatIsoUtc(Instant t);        // "2026-11-21T12:00:00Z"
std::string formatLocalDate(LocalTime t);   // "2026-11-21"
std::string formatLocalHm(LocalTime t);     // "13:45"

class TimeZone {
public:
    explicit TimeZone(std::string name);  // throws std::runtime_error if unknown

    LocalTime toLocal(Instant t) const;
    // Ambiguous times (DST fall-back) resolve to the earliest instant; times inside a
    // DST gap resolve to the transition instant.
    Instant toInstant(LocalTime t) const;

    const std::string& name() const { return name_; }

private:
    std::string name_;
    const std::chrono::time_zone* tz_;
};

Instant floorToSlot(Instant t);

// Slot start times covering [from, to).
std::vector<Instant> slotsBetween(Instant from, Instant to);

// Instant of local midnight for the local day containing t.
Instant startOfLocalDay(const TimeZone& tz, Instant t);

// Helpers on wall-clock time.
int minuteOfDay(LocalTime t);         // 0..1439
int hourOfDay(LocalTime t);           // 0..23
unsigned monthOf(LocalTime t);        // 1..12
unsigned isoWeekday(LocalTime t);     // 1 = Monday .. 7 = Sunday

}  // namespace hb
