#include "core/insulation.hpp"

#include <algorithm>

namespace hb {

std::optional<double> heatLossWattsPerK(const ThermalModel& m, double heaterWatts) {
    if (!m.usable() || heaterWatts <= 0 || m.loss <= 0) return std::nullopt;
    return m.loss * heaterWatts / m.gain;
}

MonthlyHeat monthlyHeat(double wattsPerK, double insideAvg, double outsideAvg, double pricePerKWh) {
    MonthlyHeat h;
    h.kWh = wattsPerK * std::max(0.0, insideAvg - outsideAvg) * 24 * 30 / 1000.0;
    h.cost = h.kWh * pricePerKWh;
    return h;
}

bool poorInsulation(double perM2, const std::vector<double>& others) {
    if (perM2 < 2.0) return false;
    if (others.empty()) return perM2 >= 3.0;
    std::vector<double> v = others;
    std::ranges::sort(v);
    const double median = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
    return perM2 >= 2 * median;
}

bool heatLossRose(const ThermalModel& recent, const ThermalModel& before) {
    if (recent.coolingStretches < 3 || before.coolingStretches < 5 || before.loss <= 0) return false;
    return recent.loss >= before.loss * 1.5 && recent.loss - before.loss >= 0.01;
}

}  // namespace hb
