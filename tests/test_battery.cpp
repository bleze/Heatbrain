#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>

#include "core/battery.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Matchers::ContainsSubstring;

namespace {
const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}
Instant onDay(int d) { return tz().toInstant(local_days{2026y / 11 / 1} + days{d}); }
// One sample a day, falling `perDay` percent a day, ending at `last` percent on day `n - 1`.
std::vector<BatterySample> falling(int n, double perDay, int last) {
    std::vector<BatterySample> v;
    for (int d = 0; d < n; ++d) v.push_back({onDay(d), static_cast<int>(std::lround(last + perDay * (n - 1 - d)))});
    return v;
}
}  // namespace

TEST_CASE("Battery: days until empty from the daily levels") {
    CHECK_FALSE(daysUntilEmpty(falling(5, 0.5, 40)));             // too little history
    CHECK_FALSE(daysUntilEmpty(falling(30, 0.0, 40)));            // not falling
    CHECK(*daysUntilEmpty(falling(30, 0.5, 40)) == Catch::Approx(70).margin(2));  // (40 - 5) / 0.5
}

TEST_CASE("Battery: warned about three weeks ahead, not at 20 %") {
    const Instant now = onDay(40);
    // 20 % but falling slowly (0.2/day): 75 days left - no warning.
    CHECK_FALSE(batteryWarning("Bedroom", 20, falling(41, 0.2, 20), now, std::nullopt, tz()));
    // 12 %, 0.5/day: 14 days left - warned.
    const auto w = batteryWarning("Bedroom", 12, falling(41, 0.5, 12), now, std::nullopt, tz());
    REQUIRE(w);
    CHECK_THAT(w->message, ContainsSubstring("about 2 weeks left"));
    // Unknown drain: only from 10 %; at 5 % always.
    CHECK_FALSE(batteryWarning("Bedroom", 15, {}, now, std::nullopt, tz()));
    CHECK(batteryWarning("Bedroom", 9, {}, now, std::nullopt, tz()));
    CHECK_THAT(batteryWarning("Bedroom", 4, falling(41, 0.1, 4), now, std::nullopt, tz())->message, ContainsSubstring("now"));
}

TEST_CASE("Battery: warned before leaving if it would run out during the away period") {
    const Instant now = onDay(40);
    const auto log = falling(41, 0.5, 25);  // 40 days left
    CHECK_FALSE(batteryWarning("Carport", 25, log, now, std::nullopt, tz()));
    const AwayMode trip{12, now + days{20}, now + days{45}};  // back in 45 days: it runs out before
    const auto w = batteryWarning("Carport", 25, log, now, trip, tz());
    REQUIRE(w);
    CHECK_THAT(w->message, ContainsSubstring("while you're away"));
    const AwayMode shortTrip{12, now + days{2}, now + days{9}};
    CHECK_FALSE(batteryWarning("Carport", 25, log, now, shortTrip, tz()));
}
