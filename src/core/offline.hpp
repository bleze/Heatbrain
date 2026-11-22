#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/planner.hpp"
#include "core/price.hpp"
#include "core/room.hpp"
#include "core/time.hpp"

namespace hb {

// "Wi-Fi off" periods: every day the network is switched off (e.g. 23:30-06:30). Plugs and
// sensors can't be reached then, so before it starts heatbrain loads a schedule onto each
// live plug and the plug switches on its own clock. No temperatures arrive either (Netatmo
// uses Wi-Fi too), so heat/keep periods get a duty cycle learned from earlier nights.

struct OfflinePeriod {
    Instant start;
    Instant end;
    bool contains(Instant t) const { return t >= start && t < end; }
};

// The occurrence of the daily window that is running at `now`, or else the next one.
OfflinePeriod currentOrNextOfflinePeriod(const DayWindow& window, const TimeZone& tz, Instant now);

struct OnSpan {
    Instant from;
    Instant to;
    bool operator==(const OnSpan&) const = default;
};

// Share of time a room heats when it can't measure: per program mode.
struct OfflineDuty {
    double heat = 0.25;  // "heat to" periods
    double keep = 0.25;  // "keep above" periods (and away)
};

// What the room should do in [from, to) without temperature readings:
//  - price-only rooms: exactly what the planner would do (cheapest hours);
//  - "off" periods: off;
//  - heat / keep periods: duty x length of each run, placed in its cheapest 15-minute slots.
// Spans are merged and sorted.
std::vector<OnSpan> offlineSchedule(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz,
                                    Instant from, Instant to, const OfflineDuty& duty,
                                    std::optional<AwayMode> away);

bool scheduleOnAt(const std::vector<OnSpan>& spans, Instant t);

// Tasmota rules that switch the relay at the spans' edges, using the plug's own clock:
// "ON Time#Minute=1410 DO Power1 1 ENDON ON Time#Minute=60 DO Power1 0 ENDON ...".
// plugUtcOffset: the plug's local time minus UTC (its Time#Minute counts local minutes).
// Split over at most maxRules rule sets of at most maxLength characters; nullopt if it
// doesn't fit (too many switch points).
std::optional<std::vector<std::string>> tasmotaScheduleRules(const std::vector<OnSpan>& spans,
                                                             std::chrono::minutes plugUtcOffset,
                                                             std::size_t maxLength = 500, int maxRules = 2);

// True for an empty rule set or one tasmotaScheduleRules wrote (so it may be replaced).
bool isHeatbrainScheduleRule(const std::string& rules);

}  // namespace hb
