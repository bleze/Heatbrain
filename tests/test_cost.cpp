#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/cost.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}
Instant at(int day, int hour, int minute = 0) {
    return tz().toInstant(local_days{2026y / 11 / day} + hours{hour} + minutes{minute});
}
std::map<Instant, double> flatPrices(int day, double cheap, double peak) {
    std::map<Instant, double> p;
    for (int h = 0; h < 24; ++h) {
        for (int q = 0; q < 4; ++q) p[at(day, h, 15 * q)] = (h >= 17 && h < 21) ? peak : cheap;
    }
    return p;
}

}  // namespace

TEST_CASE("Cost = watts x on-time x the price of each slot") {
    // 1000 W on from 16:30 to 17:30: half an hour cheap (2.0), half an hour peak (3.4).
    const std::vector<DecisionPoint> d{{at(21, 16, 30), true, true}, {at(21, 17, 30), false, true}};
    const auto days = dailyCost(d, flatPrices(21, 2.0, 3.4), 1000, hours{2}, at(21, 0), at(22, 0), tz());
    REQUIRE(days.size() == 1);
    CHECK(days[0].date == "2026-11-21");
    CHECK(days[0].kWh == Approx(1.0));
    CHECK(days[0].cost == Approx(0.5 * 2.0 + 0.5 * 3.4));
}

TEST_CASE("Intervals follow the decision log, capped where the controller was down") {
    // Decisions every 5 min while on; cap at 15 min.
    std::vector<DecisionPoint> d;
    for (int m = 0; m < 60; m += 5) d.push_back({at(21, 16, 30) + minutes{m}, true, true});
    d.push_back({at(21, 17, 30), false, true});
    const auto days = dailyCost(d, flatPrices(21, 2.0, 3.4), 1000, minutes{15}, at(21, 0), at(22, 0), tz());
    REQUIRE(days.size() == 1);
    CHECK(days[0].kWh == Approx(1.0));
    CHECK(days[0].onHours == Approx(1.0));
    CHECK(days[0].cost == Approx(0.5 * 2.0 + 0.5 * 3.4));
    CHECK(days[0].liveKWh == Approx(1.0));

    // The controller was down for two hours after a single ON decision: only maxGap counts.
    const std::vector<DecisionPoint> gap{{at(21, 10), true, false}, {at(21, 12), false, false}};
    const auto g = dailyCost(gap, flatPrices(21, 2.0, 3.4), 2000, minutes{15}, at(21, 0), at(22, 0), tz());
    CHECK(g[0].kWh == Approx(0.5));      // 2 kW x 15 min
    CHECK(g[0].liveKWh == Approx(0.0));  // dry-run: "would have cost"
}

TEST_CASE("Energy across midnight is split by local day; unknown prices are reported") {
    std::vector<DecisionPoint> d;
    for (int m = 0; m <= 60; m += 5) d.push_back({at(21, 23, 30) + minutes{m}, m < 60, true});
    std::map<Instant, double> prices = flatPrices(21, 2.0, 3.4);  // no prices for the 22nd
    const auto days = dailyCost(d, prices, 1000, minutes{15}, at(21, 0), at(23, 0), tz());
    REQUIRE(days.size() == 2);
    CHECK(days[0].date == "2026-11-21");
    CHECK(days[0].kWh == Approx(0.5));
    CHECK(days[0].cost == Approx(1.0));
    CHECK(days[1].date == "2026-11-22");
    CHECK(days[1].kWh == Approx(0.5));
    CHECK(days[1].kWhWithoutPrice == Approx(0.5));
    CHECK(days[1].cost == Approx(0.0));
}

TEST_CASE("Daily average market price") {
    const auto avg = dailyAveragePrice(flatPrices(21, 2.0, 3.4), tz());
    CHECK(avg.at("2026-11-21") == Approx((20 * 2.0 + 4 * 3.4) / 24));
}

TEST_CASE("Thermostat energy from its heating-time counter: exact, priced where it happened") {
    // A 2975 W floor: 15 min of heating in the 16:45-17:00 interval (cheap), 30 min across the next
    // hour (peak), nothing after; the counter is lifetime seconds.
    const std::vector<CounterSample> s{{at(21, 16, 45), 1000, true}, {at(21, 17, 0), 1000 + 900, true},
                                       {at(21, 18, 0), 1900 + 1800, true}, {at(21, 18, 5), 3700, true}};
    const auto days = dailyCostFromCounter(s, flatPrices(21, 2.0, 3.4), 2975, hours{2}, at(21, 0), at(22, 0), tz());
    REQUIRE(days.size() == 1);
    CHECK(days[0].onHours == Approx(0.75));
    CHECK(days[0].kWh == Approx(2.975 * 0.75));
    CHECK(days[0].cost == Approx(2.975 * (0.25 * 2.0 + 0.5 * 3.4)));
    CHECK(days[0].liveKWh == Approx(days[0].kWh));
}

TEST_CASE("Thermostat counter: gaps and resets are skipped, not invented") {
    const std::vector<CounterSample> s{{at(21, 10), 5000, true}, {at(21, 14), 9000, true},   // 4 h gap: skipped
                                       {at(21, 14, 5), 9100, false}, {at(21, 14, 10), 50, false}};  // reset
    const auto days = dailyCostFromCounter(s, flatPrices(21, 2.0, 3.4), 1000, minutes{15}, at(21, 0), at(22, 0), tz());
    REQUIRE(days.size() == 1);
    CHECK(days[0].onHours == Approx(100.0 / 3600));
    CHECK(days[0].liveKWh == Approx(0.0));  // dry-run interval
}
