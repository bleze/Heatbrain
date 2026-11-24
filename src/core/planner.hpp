#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/price.hpp"
#include "core/forecast.hpp"
#include "core/room.hpp"
#include "core/thermal.hpp"
#include "core/time.hpp"
#include "core/window.hpp"

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
    WindowOpen,             // the temperature fell fast: window open? heating paused
    WarmUp,                 // a warmer program period is coming: below the warm-up curve, heating
    WarmUpOnTrack,          // a warmer program period is coming: on or above the curve, off
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
    std::optional<Instant> nextExpensiveUntil;  // end of that expensive stretch (within 24 h)
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
// While warming up, the target follows the warm-up curve (see warmupCurve) from what the program
// (or away) would have, regardless of price.
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

// A step up in a room's program: from a heat or keep period to a heat or keep period at least half a
// degree warmer. The room starts warming before it, so it is warm when the period begins.
// Never out of an "off" period (often the peak tariff: heating there would defeat it).
struct ProgramStep {
    Instant at;        // when the warmer period begins
    double from = 0;   // the target before it
    double target = 0; // the target from then on
    ProgramMode mode = ProgramMode::Heat;  // of the warmer period: keep-above warms up regardless of price
};

// The next program switch after `now`, if it is a step up within `within` (12 h).
std::optional<ProgramStep> nextStepUp(const RoomConfig& room, const TimeZone& tz, Instant now,
                                      std::chrono::minutes within = std::chrono::hours{12});

// The warm-up curve: where the room should be at `t` to reach the step's target in time. Learned
// rooms follow their full-heat curve backwards from the step (gain x 0.8, reaching the target 30
// min early); until learned, a straight ramp over the room's warm-up setting. Flat at the earlier
// target, eased in and out at both ends, so it rises as an S. A warm-up setting of 0 turns it off.
// `ramp`: the length of the straight ramp instead of the room's warm-up setting (a booking's warm-up).
double warmupCurve(const RoomConfig& room, const ProgramStep& step, Instant t, const std::optional<ThermalModel>& m,
                   double outdoor, std::optional<std::chrono::minutes> ramp = {});

// An off period ahead with a "stay above" floor: the room must hold `level` when it begins to coast
// through it [at, until) without heating and still be above `floor` at the end.
struct CoastStep {
    Instant at;
    Instant until;
    double floor = 0;
    double level = 0;   // needed at `at` (at most max - hysteresis)
    double outdoor = 0; // assumed during the off period
    double atEnd = 0;   // where the room is expected at `until`, from `level`: below `floor` when max caps it
    bool fallsShort() const { return atEnd < floor - 0.05; }
};
// The next program switch after `now`, if it is an off period with a floor within `within`. The room
// cools at `loss` (its learned cooling rate, per hour), else a cautious 5 %/h, towards the forecast
// (else `outdoor`, else 5).
std::optional<CoastStep> nextCoast(const RoomConfig& room, const TimeZone& tz, Instant now, std::optional<double> loss,
                                   const OutdoorForecast* forecast, std::optional<double> outdoor,
                                   std::chrono::minutes within = std::chrono::hours{12});

struct PlannerInput {
    Instant now;
    std::optional<double> temperature;
    std::optional<Instant> measuredAt;
    bool currentlyOn = false;  // the planner's previous decision (hysteresis)
    std::optional<ManualOverride> override;
    std::optional<AwayMode> away;  // whole-house away/holiday mode
    std::optional<Booking> booking;  // this room booked (in effect now)
    std::optional<double> outdoorTemp;  // °C, recent reading of an outdoor sensor
    std::optional<OpenWindowEvent> windowOpen;  // heating paused: the temperature fell fast
    // How fast this room warms and cools, once learned well enough: pre-heating is then sized to
    // carry the room through the expensive hours, and starts only as early as needed.
    std::optional<ThermalModel> thermal;
    // How fast it cools (per hour), as soon as that alone is learned (before the whole model is):
    // sizes the heat stored for an off period with a floor, as the plan's expected coast does.
    std::optional<double> coolingRate;
    std::shared_ptr<const OutdoorForecast> forecast;  // outdoor temperature ahead, if known
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
    std::optional<double> ceiling;  // pre-heat / cheap boost: heating up to this
    std::optional<double> warmupTo;  // warming up: the target of the period ahead (`target` is the curve)
    std::optional<double> coastAbove;  // an off period with a floor (or storing heat for one: then `warmupTo` is the level)
    bool storing = false;  // the warm-up is heat stored for an off period, not a warmer period
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
//   "boost" (cheapest hours), "keep" (keep above), "away", "booked" (incl. warm-up), "off",
//   "warmup" (warming up for a warmer program period ahead: the target is the curve), and
//   "idle": a keep-above or heat period in which the room is predicted to stay warm enough by itself
//   (from its current temperature, cooling towards outdoors at `coolingRate` per hour).
struct PlanSegment {
    Instant from;
    Instant to;
    std::string kind;
    std::optional<double> target;
    bool warmup = false;  // the target is a point on a warm-up curve (also when "idle" or overlaid by the night)
    bool booked = false;  // a booking (or its warm-up) sets the target, also when overlaid by the night
    // Off: the room's expected temperature at `from` and `to` as it coasts (only when the current
    // temperature is known), and the period's "stay above" floor if it has one.
    std::optional<double> coastFrom, coastTo, coastAbove;
};
// Whether `next` extends `prev` (same kind and target; off slots with an expected coast never merge).
bool continuesSegment(const PlanSegment& prev, const PlanSegment& next);
std::vector<PlanSegment> planAhead(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz, Instant now,
                                   std::chrono::hours horizon, std::optional<AwayMode> away,
                                   std::optional<double> outdoorTemp, const std::vector<Booking>& bookings = {},
                                   std::optional<double> currentTemp = {}, std::optional<double> coolingRate = {},
                                   std::optional<ThermalModel> thermal = {},
                                   std::shared_ptr<const OutdoorForecast> forecast = {});

// The pre-heat a room needs before an expensive stretch [from, until): how far above `target` it
// must be when the stretch starts to coast through it without falling below target - allowedDrop,
// cooling towards `outdoor` at the learned rate. 0 when it holds anyway.
double neededPreheat(const ThermalModel& m, double target, double allowedDrop, double outdoor, Instant from,
                     Instant until);

}  // namespace hb
