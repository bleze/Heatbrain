#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/forecast.hpp"
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

// The configured Wi-Fi off times: every night, optionally different at the weekend - the nights
// that END on a Saturday or Sunday morning (sleeping in is about the end).
struct WifiOffPlan {
    DayWindow normal;
    std::optional<DayWindow> weekend;
};

// The occurrence that is running at `now`, or else the next one.
OfflinePeriod currentOrNextOfflinePeriod(const WifiOffPlan& plan, const TimeZone& tz, Instant now);
inline OfflinePeriod currentOrNextOfflinePeriod(const DayWindow& window, const TimeZone& tz, Instant now) {
    return currentOrNextOfflinePeriod(WifiOffPlan{window, std::nullopt}, tz, now);
}

// Gaps of at least minGap between consecutive reading times (sorted): when the network was off,
// as seen by the sensors. A gap still running at `now` is included, ending at `now`.
struct ObservedGap {
    Instant lastBefore;  // last reading before the gap
    Instant firstAfter;  // first reading after it (or now, if still running)
    bool ongoing = false;
};
std::vector<ObservedGap> findReadingGaps(const std::vector<Instant>& times, Instant now,
                                         std::chrono::minutes minGap = std::chrono::minutes{30});

struct OnSpan {
    Instant from;
    Instant to;
    bool operator==(const OnSpan&) const = default;
};

// Share of time a room heats when it can't measure: per program mode.
struct OfflineDuty {
    double heat = 0.25;  // "heat to" periods
    double keep = 0.25;  // "keep above" periods (and away)
    // Learned per target: a 16-degree night needs far less than a 20-degree day.
    // Key: (keep ? 1 : 0, target in tenths of a degree).
    std::map<std::pair<int, int>, double> byTarget;
    static std::pair<int, int> key(bool keep, double target) {
        return {keep ? 1 : 0, static_cast<int>(std::lround(target * 10))};
    }
    // The share for a run: learned for this target if known, else the mode's share - but no more
    // than 15 min per hour, since an unseen target is usually a lower night setting.
    double forRun(bool keep, std::optional<double> target) const {
        if (target) {
            if (auto it = byTarget.find(key(keep, *target)); it != byTarget.end()) return it->second;
        }
        return std::min(keep ? this->keep : heat, 0.25);
    }
};

// The room's temperature just before the Wi-Fi goes off and how fast it cools: the night schedule
// then only heats from when the room would actually get down to what the period asks.
struct OfflineStart {
    Instant at;
    double temperature = 0;
    double coolingRate = 0.05;  // per hour towards outdoors: learned, else cautious
    std::optional<double> outdoor;
    std::shared_ptr<const OutdoorForecast> forecast;
};

// What the room should do in [from, to) without temperature readings:
//  - price-only rooms: exactly what the planner would do (cheapest hours);
//  - "off" periods: off;
//  - heat / keep periods: duty x length of each run, placed in its cheapest 15-minute slots - with a
//    known starting temperature only the part of the run from when the room (cooling unheated) would
//    be below the target, so a warm room in a "keep above" night stays silent.
// Spans are merged and sorted.
std::vector<OnSpan> offlineSchedule(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz,
                                    Instant from, Instant to, const OfflineDuty& duty,
                                    std::optional<AwayMode> away, std::optional<OfflineStart> start = {});

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
