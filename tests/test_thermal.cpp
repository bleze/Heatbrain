#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/thermal.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

namespace {

// A room obeying dT/dt = gain*heating - loss*(T - outdoor), sampled every 5 minutes like the
// decision log, with Netatmo's 0.1 degree steps. Heats `onHours`, rests `offHours`, repeatedly.
std::vector<ThermalSample> simulate(double gain, double loss, double outdoor, int onHours, int offHours, int days) {
    std::vector<ThermalSample> out;
    double t = 15;
    const Instant start = sys_days{2026y / 11 / 1};
    const int cycle = (onHours + offHours) * 12;
    for (int i = 0; i < days * 24 * 12; ++i) {
        const bool heating = i % cycle < onHours * 12;
        out.push_back({start + minutes{5 * i}, std::round(t * 10) / 10, outdoor, heating});
        for (int s = 0; s < 5; ++s) t += (gain * heating - loss * (t - outdoor)) / 60.0;  // per minute
    }
    return out;
}

}  // namespace

TEST_CASE("The thermal model learns how fast a room warms and cools") {
    // A radiator room: +1.5 deg/h at full heat, loses 5% of the indoor-outdoor gap per hour.
    const auto m = fitThermalModel(simulate(1.5, 0.05, 2.0, 4, 6, 7));
    REQUIRE(m.usable());
    CHECK(m.loss == Approx(0.05).margin(0.012));
    CHECK(m.gain == Approx(1.5).margin(0.25));
}

TEST_CASE("Too little or no outdoor data: the model is not used") {
    auto samples = simulate(1.5, 0.05, 2.0, 4, 6, 7);
    for (auto& s : samples) s.outdoor.reset();
    CHECK_FALSE(fitThermalModel(samples).usable());
    CHECK_FALSE(fitThermalModel({}).usable());
}

TEST_CASE("Warm-up estimates: radiators take hours, a concrete floor a day, the impossible is capped") {
    // Radiator room from 15 to 21 with 0 outside: equilibrium 30, ln(15/9)/0.05 = 10.2 h -> x1.25 + 30 min
    const auto radiator = estimateWarmup({0.05, 1.5, 9, 9}, 15, 21, 0);
    CHECK(radiator.reachable);
    CHECK(radiator.duration.count() == Approx(10.2 * 1.25 * 60 + 30).margin(10));

    // Concrete floor from 5 to 22: slow gain, small loss: about a day.
    const auto slab = estimateWarmup({0.01, 1.0, 9, 9}, 5, 22, 0);
    CHECK(slab.reachable);
    CHECK(slab.duration > hours{20});
    CHECK(slab.duration < hours{30});

    // Already warm: nothing to do. Out of reach (equilibrium 12 deg): capped and flagged.
    CHECK(estimateWarmup({0.05, 1.5, 9, 9}, 22, 21, 0).duration == minutes{0});
    const auto cold = estimateWarmup({0.1, 1.0, 9, 9}, 5, 22, 2);
    CHECK_FALSE(cold.reachable);
    CHECK(cold.duration == hours{48});
}
