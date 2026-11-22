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
    bool usable() const { return coolingStretches >= 3 && heatingStretches >= 3 && gain > 0.05; }
};

// Fits the model on samples (sorted by time, any spacing up to 20 min; gaps split stretches).
// Each stretch is 90 minutes or more of steady heating or not-heating with a known outdoor temperature.
ThermalModel fitThermalModel(const std::vector<ThermalSample>& samples);

struct WarmupEstimate {
    std::chrono::minutes duration{0};
    bool reachable = true;  // false: the model says the target is (almost) out of reach; capped
};

// How long heating at full power takes from `from` to `target` °C with `outdoor` outside,
// with a margin (x1.25 + 30 min), capped at 48 h.
WarmupEstimate estimateWarmup(const ThermalModel& m, double from, double target, double outdoor);

}  // namespace hb
