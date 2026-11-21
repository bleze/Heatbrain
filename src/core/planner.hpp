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

struct PlannerInput {
    Instant now;
    std::optional<double> temperature;
    std::optional<Instant> measuredAt;
    bool currentlyOn = false;  // the planner's previous decision (hysteresis)
    std::optional<ManualOverride> override;
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
};

struct Decision {
    bool on = false;
    Reason reason = Reason::HoldOff;
    std::string detail;  // plain-language explanation
    DecisionInputs inputs;
};

Decision decideRoom(const RoomConfig& room, const PlannerInput& in, const PriceTable& prices,
                    const TimeZone& tz);

}  // namespace hb
