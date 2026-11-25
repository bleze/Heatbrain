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

TEST_CASE("Thermal model: the hour after a gap in the readings is left out (sensors settle)") {
    // Cooling 0.3 deg/h, steady; a 6 h gap, then a sensor that reads 2 deg low and catches up within
    // 15 min (as a thermostat's built-in sensor does after a Wi-Fi-off night).
    std::vector<ThermalSample> s;
    const Instant t0{std::chrono::seconds{1'790'000'000}};
    using std::chrono::minutes;
    double temp = 18.0;
    for (int m = 0; m <= 6 * 60; m += 5) { s.push_back({t0 + minutes{m}, temp, 5.0, false}); temp -= 0.3 / 12; }
    const Instant back = t0 + minutes{12 * 60};
    temp -= 1.8;  // cooled during the gap
    for (int m = 0; m <= 4 * 60; m += 5) {
        const double artefact = m < 15 ? -2.0 * (15 - m) / 15.0 : 0.0;
        s.push_back({back + minutes{m}, temp + artefact, 5.0, false});
        temp -= 0.3 / 12;
    }
    const auto m = fitThermalModel(s);
    // Every cooling window says the same: about 0.3 / (indoor - outdoor); none sees the jump.
    CHECK(m.coolingStretches >= 3);
    CHECK(m.loss > 0.02);
}

TEST_CASE("Thermal model: a room that warms without heating (sun, PCs, people) does not count as not cooling") {
    auto samples = simulate(1.5, 0.05, 2.0, 4, 6, 7);
    const auto before = fitThermalModel(samples);
    // After a break in the readings, a gaming evening: 5 hours unheated while it creeps up 20.6 -> 21.5.
    const Instant evening = samples.back().at + hours{3};
    for (int i = 0; i <= 60; ++i) samples.push_back({evening + minutes{5 * i}, std::round((20.6 + 0.015 * i) * 10) / 10, 2.0, false});
    const auto after = fitThermalModel(samples);
    CHECK(after.coolingStretches == before.coolingStretches);
    CHECK(after.loss == Approx(before.loss));
}

TEST_CASE("A quiet sensor: the temperature is estimated from the last reading, the heating since and the rates") {
    ThermalModel m;
    m.loss = 0.1;  // per hour
    m.gain = 1.0;  // °C per hour at full heat
    const Instant t0 = sys_days{2026y / 11 / 25} + hours{8};
    // Not heated for an hour: Newton's cooling towards 0 °C, 20 * e^-0.1.
    CHECK(estimateTemperature(m, 20, t0, t0 + hours{1}, {}, 0) == Approx(20 * std::exp(-0.1)).margin(0.02));
    // Heated the whole hour: towards gain/loss = 10 °C above outdoors, 10 + 10 * e^-0.1.
    CHECK(estimateTemperature(m, 20, t0, t0 + hours{1}, {{t0 - minutes{30}, t0 + hours{2}}}, 0) ==
          Approx(10 + 10 * std::exp(-0.1)).margin(0.02));
    // Heated for the last half hour only: in between.
    const double half = estimateTemperature(m, 20, t0, t0 + hours{1}, {{t0 + minutes{30}, t0 + hours{1}}}, 0);
    CHECK(half > 18.2);
    CHECK(half < 19.0);
    // No time passed: the reading itself.
    CHECK(estimateTemperature(m, 17.7, t0, t0, {}, 3) == 17.7);
}
