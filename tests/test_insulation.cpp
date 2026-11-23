#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/insulation.hpp"

using namespace hb;

namespace {
ThermalModel model(double loss, double gain, int cooling = 6, int heating = 6) {
    ThermalModel m;
    m.loss = loss;
    m.gain = gain;
    m.coolingStretches = cooling;
    m.heatingStretches = heating;
    return m;
}
}  // namespace

TEST_CASE("Heat loss in W/K from the learned model and the heater's power") {
    // The bedroom: loss 0.042/h, gain 1.09 deg/h at 1500 W -> heat capacity ~1376 Wh/K -> ~58 W/K.
    CHECK(*heatLossWattsPerK(model(0.042, 1.09), 1500) == Catch::Approx(57.8).margin(0.2));
    CHECK_FALSE(heatLossWattsPerK(model(0.042, 1.09, 1, 1), 1500));  // not learned yet
    CHECK_FALSE(heatLossWattsPerK(model(0.042, 1.09), 0));           // power unknown
}

TEST_CASE("Monthly heat: W/K x degrees x 720 h") {
    const auto h = monthlyHeat(58, 19, 2, 2.4);
    CHECK(h.kWh == Catch::Approx(58 * 17 * 720 / 1000.0));
    CHECK(h.cost == Catch::Approx(h.kWh * 2.4));
    CHECK(monthlyHeat(58, 10, 15, 2.4).kWh == 0);  // warmer outside: none
}

TEST_CASE("Poor insulation: clearly high per m2, and far above the other rooms") {
    CHECK(poorInsulation(4.1, {1.2, 1.6}));
    CHECK_FALSE(poorInsulation(2.5, {1.6, 2.0}));  // high, but so are the others
    CHECK_FALSE(poorInsulation(1.8, {0.5}));       // below 2 W/K per m2
    CHECK(poorInsulation(3.2, {}));
    CHECK_FALSE(poorInsulation(2.6, {}));
}

TEST_CASE("Heat loss rose: the last week clearly faster than the weeks before") {
    CHECK(heatLossRose(model(0.07, 1, 4), model(0.04, 1, 10)));
    CHECK_FALSE(heatLossRose(model(0.05, 1, 4), model(0.04, 1, 10)));   // +25 %
    CHECK_FALSE(heatLossRose(model(0.015, 1, 4), model(0.008, 1, 10))); // +88 %, but tiny
    CHECK_FALSE(heatLossRose(model(0.07, 1, 2), model(0.04, 1, 10)));   // too few stretches this week
}
