#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/thermal.hpp"

namespace hb {

// How well a room keeps its heat, from its learned heating model: the heat that leaks out per degree
// of indoor-outdoor difference, in watts. The model gives the cooling rate (loss, per hour) and the
// warming rate at full power (gain, deg/h); with the heater's power P the room's heat capacity is
// P / gain, so the heat loss is loss x P / gain.
std::optional<double> heatLossWattsPerK(const ThermalModel& m, double heaterWatts);

// The energy and money to keep a room at `insideAvg` with `outsideAvg` outdoors for 30 days.
struct MonthlyHeat {
    double kWh = 0;
    double cost = 0;
};
MonthlyHeat monthlyHeat(double wattsPerK, double insideAvg, double outsideAvg, double pricePerKWh);

// "Poor insulation": a room whose heat loss per m2 floor is clearly high - at least 2 W/K per m2 and
// twice the median of the other rooms (or 3 W/K per m2 without others to compare with).
bool poorInsulation(double perM2, const std::vector<double>& otherRoomsPerM2);

// "Heat loss has gone up": the last week's cooling rate clearly above the weeks before (+50 % and at
// least 0.01/h), each fitted on enough cooling stretches.
bool heatLossRose(const ThermalModel& recent, const ThermalModel& before);

}  // namespace hb
