#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"

namespace hb {

// How fast a room warms and cools, learned from its own history, so a booking can start heating
// early enough - hours for a radiator, a day or more for floor heating in concrete.
//
// The model is Newton's law of cooling plus a heater at full power:
//     dT/dt = gain * heating - loss * (T - Toutdoor)          [°C per hour]
// `loss` is learned from stretches where the room was not heated (it cools towards outdoors),
// `gain` from stretches where it was. Thermal lag (a slab that keeps warming after the heat is
// off, or takes hours to start) shows up as slower rates, so the estimate leans long.

struct ThermalSample {
    Instant at;
    double temperature = 0;
    std::optional<double> outdoor;
    bool heating = false;
};

struct ThermalModel {
    double loss = 0;   // per hour
    double gain = 0;   // °C per hour at full heat
    int coolingStretches = 0;
    int heatingStretches = 0;
    // How fast it cools in the first hours after the heat goes off (per hour, like `loss`): usually
    // faster than `loss`, which is mostly learned from long, settled stretches (nights) - the air and
    // the radiator's surroundings give their heat off first. Learned from each switch-off followed by
    // at least kCoastMin without heat, over up to kCoastSpan of it.
    double coastLoss = 0;
    int coasts = 0;
    bool usable() const { return coolingStretches >= 3 && heatingStretches >= 3 && gain > 0.05; }
};

// The span after a switch-off that `coastLoss` describes, and the least of it a coast must last to count.
inline constexpr std::chrono::hours kCoastSpan{4};
inline constexpr std::chrono::hours kCoastMin{2};

// What is left of the room's excess over outdoors after `hours` without heat: cooling at `early` for
// the first kCoastSpan after the heat went off (`since` hours before the start, 0: just now), at `loss` after.
double coastFactor(double hours, double loss, std::optional<double> early, double since = 0);

// Fits the model on samples (sorted by time, any spacing up to 20 min; gaps split stretches).
// Each stretch is 90 minutes or more of steady heating or not-heating with a known outdoor temperature.
ThermalModel fitThermalModel(const std::vector<ThermalSample>& samples);

struct WarmupEstimate {
    std::chrono::minutes duration{0};
    bool reachable = true;  // false: the model says the target is (almost) out of reach; capped
};

// The room's temperature at `now`, estimated from its last reading `last` at `lastAt`: heating at full
// power during `heating` (spans [from, to)), and cooling towards `outdoor` all the time, minute by
// minute with the model's rates. For a sensor that went quiet for a while.
double estimateTemperature(const ThermalModel& m, double last, Instant lastAt, Instant now,
                           const std::vector<std::pair<Instant, Instant>>& heating, double outdoor);

// How long heating at full power takes from `from` to `target` °C with `outdoor` outside,
// with a margin (x1.25 + 30 min), capped at 48 h.
WarmupEstimate estimateWarmup(const ThermalModel& m, double from, double target, double outdoor);

}  // namespace hb
