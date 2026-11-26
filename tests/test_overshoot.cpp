#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/overshoot.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

namespace {
const Instant kStart = sys_days{2026y / 11 / 20} + hours{8};

// A day of heating cycles like the office: on for 40 min, then off; the room keeps rising `rise`
// after each switch-off, reported every 10 minutes.
void cycle(std::vector<HeatSwitch>& sw, std::vector<std::pair<Instant, double>>& temps, Instant at, double rise,
           bool usable = true) {
    sw.push_back({at, true, usable});
    for (int m = 0; m < 40; m += 10) temps.emplace_back(at + minutes{m}, 21.2 + 0.15 * m / 10);
    const Instant off = at + minutes{40};
    sw.push_back({off, false, usable});
    temps.emplace_back(off - minutes{2}, 21.8);
    temps.emplace_back(off + minutes{10}, 21.8 + rise * 0.7);
    temps.emplace_back(off + minutes{20}, 21.8 + rise);
    for (int m = 30; m <= 120; m += 10) temps.emplace_back(off + minutes{m}, 21.8 + rise - 0.1 * (m - 20) / 10);
}
}  // namespace

TEST_CASE("Overshoot: the median rise after the heat goes off") {
    std::vector<HeatSwitch> sw;
    std::vector<std::pair<Instant, double>> temps;
    cycle(sw, temps, kStart, 0.3);
    cycle(sw, temps, kStart + hours{3}, 0.2);
    CHECK_FALSE(learnOvershoot(sw, temps));  // two are not enough
    cycle(sw, temps, kStart + hours{6}, 0.4);
    REQUIRE(learnOvershoot(sw, temps));
    CHECK(*learnOvershoot(sw, temps) == Approx(0.3));
}

TEST_CASE("Overshoot: only the latest cycles count") {
    std::vector<HeatSwitch> sw;
    std::vector<std::pair<Instant, double>> temps;
    for (int i = 0; i < 6; ++i) cycle(sw, temps, kStart + hours{3 * i}, 0.2);  // last week's radiator knob
    for (int i = 6; i < 10; ++i) cycle(sw, temps, kStart + hours{3 * i}, 0.5);  // turned up since
    REQUIRE(learnOvershoot(sw, temps));
    CHECK(*learnOvershoot(sw, temps) == Approx(0.5));  // 4 of the latest 7 rose 0.5
}

TEST_CASE("Overshoot: switch-offs that can't tell are left out") {
    std::vector<HeatSwitch> sw;
    std::vector<std::pair<Instant, double>> temps;
    cycle(sw, temps, kStart, 0.3);
    cycle(sw, temps, kStart + hours{3}, 0.3);
    cycle(sw, temps, kStart + hours{6}, 2.0, false);  // limp mode or a night schedule: not heatbrain's thermostat
    CHECK_FALSE(learnOvershoot(sw, temps));

    // Heated again 20 minutes after: the rise was cut short.
    sw.push_back({kStart + hours{9}, true, true});
    sw.push_back({kStart + hours{9} + minutes{30}, false, true});
    sw.push_back({kStart + hours{9} + minutes{50}, true, true});
    sw.push_back({kStart + hours{10} + minutes{30}, false, true});
    temps.emplace_back(kStart + hours{9} + minutes{29}, 21.0);
    temps.emplace_back(kStart + hours{9} + minutes{45}, 22.5);
    CHECK_FALSE(learnOvershoot(sw, temps));
}
