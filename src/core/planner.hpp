#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/price.hpp"
#include "core/room.hpp"
#include "core/time.hpp"

namespace hb {

enum class Reason {
    AboveMax,               // at or above the room's hard maximum
    StaleDataLimp,          // temperature unknown/old: bounded duty cycle
    ManualOn,
    ManualOff,
    FrostProtection,        // manual OFF overridden: below frost temperature
    BelowMin,               // below the hard minimum: heat whatever the price
    BelowComfortDuringPeak, // expensive now, but too cold to keep coasting
    PeakAvoid,              // expensive now: coasting
    PreheatBeforePeak,      // cheap now, expensive soon: store some heat
    CheapBoost,             // among the cheapest slots: heat a little extra
    BelowTarget,
    TargetReached,
    HoldOn,                 // inside the hysteresis band, keep heating
    HoldOff,                // inside the hysteresis band, keep off
    CheapHours,             // price-only room: among today's cheapest hours
    NotCheapHours,
    AvoidWindow,            // price-only room: in a configured no-heat window
    ScheduledOff,           // the room's program says off (frost protection only)
    KeepAbove,              // "keep above" period: below the floor, heating
    KeepSatisfied,          // "keep above" period: above the floor, off
    NoPricesFallback,       // price-only room without prices: heat outside avoid windows
    AwayOff,                // price-only room while away
    OfflineSchedule,        // Wi-Fi off period: the plug follows the schedule loaded onto it
};

std::string_view reasonCode(Reason r);  // "PREHEAT_BEFORE_PEAK", stable for storage/UI

// Where the current slot stands among known prices for the next 24 h.
struct PriceOutlook {
    std::optional<double> current;
    std::optional<double> median;
    bool expensive = false;   // top quarter and clearly above the median
    bool cheapest = false;    // bottom quarter and clearly below the median
    std::optional<Instant> nextExpensiveAt;  // within the look-ahead window
    std::optional<double> nextExpensivePrice;
};

PriceOutlook priceOutlook(const PriceTable& prices, Instant now, std::chrono::minutes lookAhead);

struct ManualOverride {
    bool on = false;
    Instant until;
};

// Away/holiday, planned as a period: from `from` until `until` every temperature room only
// keeps above `keepAbove`; price-only rooms stay off.
struct AwayMode {
    double keepAbove = 12.0;
    Instant from;
    Instant until;
    bool activeAt(Instant t) const { return t >= from && t < until; }
};

// A room booked for a while ("the pizzeria at 22 deg from Saturday 16:00 until 23:00"): from
// `warmup` before `from` until `until` the room heats to `target` like a heat period, but never
// coasts in expensive hours - someone is (about to be) there. Wins over the program and away.
struct Booking {
    double target = 22.0;
    Instant from;
    Instant until;
    std::chrono::minutes warmup{180};
    bool activeAt(Instant t) const { return t >= from && t < until; }
    bool warmingAt(Instant t) const { return t >= from - warmup && t < from; }
    bool coversAt(Instant t) const { return warmingAt(t) || activeAt(t); }
};

// The booking in effect at `t` (warming up or under way), if any.
std::optional<Booking> bookingAt(const std::vector<Booking>& bookings, Instant t);

struct PlannerInput {
    Instant now;
    std::optional<double> temperature;
    std::optional<Instant> measuredAt;
    bool currentlyOn = false;  // the planner's previous decision (hysteresis)
    std::optional<ManualOverride> override;
    std::optional<AwayMode> away;  // whole-house away/holiday mode
    std::optional<Booking> booking;  // this room booked (in effect now)
    std::optional<double> outdoorTemp;  // °C, recent reading of an outdoor sensor
};

// Everything a decision was based on, for the decision log.
struct DecisionInputs {
    std::optional<double> temperature;
    std::optional<int> temperatureAgeMinutes;
    std::optional<double> target;
    std::string targetLabel;   // program period, "17:00-21:00"
    std::string mode;          // "heat" | "keep" | "off"
    double minTemp = 0, maxTemp = 0;
    std::optional<double> price;
    std::optional<double> medianPrice;
    bool priceExpensive = false;
    bool priceCheapest = false;
    std::optional<double> outdoorTemp;
};

struct Decision {
    bool on = false;
    Reason reason = Reason::HoldOff;
    std::string detail;  // plain-language explanation
    DecisionInputs inputs;
};

// How much to scale the pre-heat boost for the outdoor temperature: colder outside means
// the room cools faster while off, so store more heat. 1.0 when unknown.
double outdoorFactor(double target, std::optional<double> outdoor);

Decision decideRoom(const RoomConfig& room, const PlannerInput& in, const PriceTable& prices,
                    const TimeZone& tz);

// What a room is set to do over the coming hours, for display. Each slot is decided by the
// real planner assuming the room sits at its target, which yields the intent per period:
//   "heat" (to target, as needed), "preheat", "coast" (expensive: lets it cool a little),
//   "boost" (cheapest hours), "keep" (keep above), "away", "booked" (incl. warm-up), "off", and
//   "idle": a keep-above or heat period in which the room is predicted to stay warm enough by itself
//   (from its current temperature, cooling towards outdoors at `coolingRate` per hour).
struct PlanSegment {
    Instant from;
    Instant to;
    std::string kind;
    std::optional<double> target;
};
std::vector<PlanSegment> planAhead(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz, Instant now,
                                   std::chrono::hours horizon, std::optional<AwayMode> away,
                                   std::optional<double> outdoorTemp, const std::vector<Booking>& bookings = {},
                                   std::optional<double> currentTemp = {}, std::optional<double> coolingRate = {});

}  // namespace hb
