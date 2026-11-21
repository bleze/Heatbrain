#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>

#include "core/planner.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Matchers::ContainsSubstring;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}

// A winter Monday shaped like the real data: cheap night, peak 17-21.
constexpr std::array<double, 24> kHourly = {
    2.0, 2.0, 2.0, 2.0, 2.0, 2.0,  // 00-06
    2.3, 2.3, 2.3, 2.3, 2.3, 2.3, 2.3, 2.3, 2.3, 2.3,  // 06-16
    2.5,                       // 16
    3.4, 3.4, 3.4, 3.4,        // 17-21 peak
    2.4, 2.4, 2.4};            // 21-24

const auto kDay = local_days{2026y / 11 / 23};

PriceTable makePrices(const std::array<double, 24>& hourly = kHourly) {
    PriceTable t;
    t.currency = "DKK";
    // Two days, so look-ahead works late in the evening too.
    for (int d = 0; d < 2; ++d) {
        for (int h = 0; h < 24; ++h) {
            for (int q = 0; q < 4; ++q) {
                const Instant start = tz().toInstant(kDay + days{d} + hours{h} + minutes{15 * q});
                t.slots.push_back({start, {hourly[static_cast<std::size_t>(h)]}, hourly[static_cast<std::size_t>(h)]});
            }
        }
    }
    return t;
}

RoomConfig bedroom() {
    RoomConfig r;
    r.id = "bedroom";
    r.name = "Bedroom";
    r.plugIds = {"bedroom"};
    r.sensor = "netatmo:Bedroom";
    r.minTemp = 16;
    r.maxTemp = 22;
    r.program = {{7 * 60, ProgramMode::Heat, 19.0}, {21 * 60, ProgramMode::Heat, 20.0}, {23 * 60, ProgramMode::Heat, 16.0}};
    return r;
}

Instant at(int hour, int minute = 0) { return tz().toInstant(kDay + hours{hour} + minutes{minute}); }

PlannerInput input(Instant now, std::optional<double> temp, bool on = false) {
    PlannerInput in;
    in.now = now;
    in.temperature = temp;
    in.measuredAt = now - minutes{3};
    in.currentlyOn = on;
    return in;
}

Decision decide(const PlannerInput& in, const RoomConfig& room = bedroom(), const PriceTable& p = makePrices()) {
    return decideRoom(room, in, p, tz());
}

}  // namespace

TEST_CASE("Price outlook classifies the peak as expensive and the night as cheapest") {
    const auto p = makePrices();
    const auto peak = priceOutlook(p, at(18), minutes{180});
    CHECK(peak.expensive);
    CHECK_FALSE(peak.cheapest);

    const auto night = priceOutlook(p, at(2), minutes{180});
    CHECK(night.cheapest);
    CHECK_FALSE(night.expensive);

    const auto afternoon = priceOutlook(p, at(15), minutes{180});
    REQUIRE(afternoon.nextExpensiveAt);
    CHECK(*afternoon.nextExpensiveAt == at(17));
}

TEST_CASE("Flat prices: nothing is expensive or cheap, plain thermostat") {
    std::array<double, 24> flat{};
    flat.fill(2.0);
    const auto o = priceOutlook(makePrices(flat), at(18), minutes{180});
    CHECK_FALSE(o.expensive);
    CHECK_FALSE(o.cheapest);
    CHECK(decide(input(at(18), 18.5), bedroom(), makePrices(flat)).reason == Reason::BelowTarget);
}

TEST_CASE("Hard limits win") {
    SECTION("above max: off, even with manual ON") {
        auto in = input(at(12), 22.1);
        in.override = ManualOverride{true, at(13)};
        const auto d = decide(in);
        CHECK_FALSE(d.on);
        CHECK(d.reason == Reason::AboveMax);
    }
    SECTION("below min: on, even in the peak") {
        const auto d = decide(input(at(18), 15.8));
        CHECK(d.on);
        CHECK(d.reason == Reason::BelowMin);
    }
}

TEST_CASE("Stale or missing temperature: bounded limp cycle, never continuous") {
    auto in = input(at(12, 5), 18.0);
    in.measuredAt = at(11);  // 65 min old
    const auto d = decide(in);
    CHECK(d.reason == Reason::StaleDataLimp);
    CHECK_THAT(d.detail, ContainsSubstring("65 min old"));

    int onMinutes = 0;
    for (int m = 0; m < 60; ++m) {
        auto i = input(at(12, m), std::nullopt);
        i.measuredAt.reset();
        if (decide(i).on) ++onMinutes;
    }
    CHECK(onMinutes == 10);  // 10 min ON / 50 min OFF
}

TEST_CASE("Manual overrides") {
    auto in = input(at(12), 18.0);
    in.override = ManualOverride{false, at(14)};
    CHECK(decide(in).reason == Reason::ManualOff);

    in.temperature = 4.0;  // pipes freezing
    CHECK(decide(in).reason == Reason::FrostProtection);

    in.override = ManualOverride{true, at(11)};  // expired
    in.temperature = 19.0;
    CHECK(decide(in).reason != Reason::ManualOn);
}

TEST_CASE("Peak: coast down to target minus the allowed drop") {
    const auto coasting = decide(input(at(18), 18.4));  // target 19, allowed down to 18
    CHECK_FALSE(coasting.on);
    CHECK(coasting.reason == Reason::PeakAvoid);
    CHECK_THAT(coasting.detail, ContainsSubstring("3.40"));

    const auto tooCold = decide(input(at(18), 17.9));
    CHECK(tooCold.on);
    CHECK(tooCold.reason == Reason::BelowComfortDuringPeak);

    // Hysteresis: once heating in the peak, continue a little past the floor.
    CHECK(decide(input(at(18), 18.1, /*on=*/true)).on);
    CHECK_FALSE(decide(input(at(18), 18.4, /*on=*/true)).on);
}

TEST_CASE("Pre-heat before the peak when it's clearly cheaper now") {
    const auto d = decide(input(at(15), 19.2));
    CHECK(d.on);
    CHECK(d.reason == Reason::PreheatBeforePeak);
    CHECK_THAT(d.detail, ContainsSubstring("17:00"));
    CHECK_THAT(d.detail, ContainsSubstring("20.0°C"));  // target 19 + boost 1

    const auto done = decide(input(at(15), 20.1, /*on=*/true));
    CHECK_FALSE(done.on);
    CHECK(done.reason == Reason::TargetReached);

    // Outside the 3 h look-ahead: normal thermostat.
    CHECK(decide(input(at(10), 19.2)).reason == Reason::HoldOff);
}

TEST_CASE("Cheapest slots: heat a little extra") {
    RoomConfig r = bedroom();
    r.program = {{0, ProgramMode::Heat, 19.0}};
    const auto d = decide(input(at(3), 19.0), r);
    CHECK(d.on);
    CHECK(d.reason == Reason::CheapBoost);
    CHECK_FALSE(decide(input(at(3), 19.5, true), r).on);  // reached target + 0.5
}

TEST_CASE("Thermostat with hysteresis and the night band across midnight") {
    CHECK(decide(input(at(12), 18.6)).reason == Reason::BelowTarget);
    CHECK(decide(input(at(12), 19.4)).reason == Reason::TargetReached);
    CHECK(decide(input(at(12), 19.0, true)).reason == Reason::HoldOn);
    CHECK(decide(input(at(12), 19.0, false)).reason == Reason::HoldOff);

    const auto night = decide(input(at(2), 17.0));  // night target 16: cool bedroom
    CHECK_FALSE(night.on);
    CHECK(night.inputs.target == 16.0);
    CHECK(night.inputs.targetLabel == "23:00-07:00");
}

TEST_CASE("Price-only rooms run the cheapest hours and respect no-heat windows") {
    RoomConfig shed;
    shed.id = "shed";
    shed.plugIds = {"shed"};
    shed.priceOnly = PriceOnlyConfig{6, {DayWindow{17 * 60, 21 * 60}}};

    CHECK(decide(input(at(3), std::nullopt), shed).reason == Reason::CheapHours);      // night = cheapest
    CHECK(decide(input(at(12), std::nullopt), shed).reason == Reason::NotCheapHours);
    CHECK(decide(input(at(18), std::nullopt), shed).reason == Reason::AvoidWindow);

    PriceTable none;
    none.currency = "DKK";
    CHECK(decide(input(at(12), std::nullopt), shed, none).reason == Reason::NoPricesFallback);
    CHECK_FALSE(decide(input(at(18), std::nullopt), shed, none).on);  // still avoids the window
}

TEST_CASE("Decision inputs are captured for the log") {
    const auto d = decide(input(at(18), 18.4));
    CHECK(d.inputs.temperature == 18.4);
    CHECK(d.inputs.temperatureAgeMinutes == 3);
    CHECK(d.inputs.target == 19.0);
    CHECK(d.inputs.price == 3.4);
    CHECK(d.inputs.priceExpensive);
}

TEST_CASE("Off periods: off even below minimum, but never below frost protection") {
    RoomConfig r = bedroom();
    r.program = {{7 * 60, ProgramMode::Heat, 19.0}, {17 * 60, ProgramMode::Off, 0}, {21 * 60, ProgramMode::Heat, 20.0},
                 {23 * 60, ProgramMode::Heat, 16.0}};
    const auto peak = decide(input(at(18), 15.0), r);  // below minTemp 16
    CHECK_FALSE(peak.on);
    CHECK(peak.reason == Reason::ScheduledOff);
    CHECK_FALSE(peak.inputs.target);
    CHECK(peak.inputs.mode == "off");
    CHECK_THAT(peak.detail, ContainsSubstring("17:00-21:00"));

    const auto frost = decide(input(at(18), 4.5), r);
    CHECK(frost.on);
    CHECK(frost.reason == Reason::FrostProtection);

    auto stale = input(at(18), std::nullopt);  // no data: limp mode must not heat in the window
    stale.measuredAt.reset();
    for (int m = 0; m < 60; m += 5) {
        stale.now = at(18, m);
        CHECK_FALSE(decide(stale, r).on);
    }

    auto manual = input(at(18), 18.0);  // an explicit manual ON still wins
    manual.override = ManualOverride{true, at(19)};
    CHECK(decide(manual, r).reason == Reason::ManualOn);

    CHECK(decide(input(at(21), 18.0), r).reason == Reason::BelowTarget);  // window ended
    CHECK(decide(input(at(15), 19.2), r).reason == Reason::PreheatBeforePeak);  // pre-heat still works
}

TEST_CASE("Program periods: switch points run until the next one and wrap past midnight") {
    RoomConfig r = bedroom();
    r.program = {{9 * 60, ProgramMode::Heat, 19.0}, {17 * 60, ProgramMode::Off, 0}, {21 * 60, ProgramMode::Heat, 20.0},
                 {23 * 60, ProgramMode::Off, 0}};
    CHECK(periodAt(r, 2 * 60)->point.mode == ProgramMode::Off);  // 23:00 -> 09:00 wraps
    CHECK(periodAt(r, 2 * 60)->label == "23:00-09:00");
    CHECK(periodAt(r, 9 * 60)->point.target == 19.0);
    CHECK(periodAt(r, 16 * 60 + 59)->label == "09:00-17:00");
    CHECK(periodAt(r, 22 * 60)->point.target == 20.0);

    RoomConfig single = bedroom();
    single.program = {{0, ProgramMode::Heat, 19.0}};
    CHECK(periodAt(single, 12 * 60)->label == "all day");
}

TEST_CASE("Keep above: a floor only, ignores price, no pre-heat or boost") {
    RoomConfig r = bedroom();
    r.program = {{0, ProgramMode::Keep, 15.0}};
    CHECK(decide(input(at(18), 14.5), r).reason == Reason::KeepAbove);       // peak price, still heats
    CHECK(decide(input(at(18), 15.5), r).reason == Reason::KeepSatisfied);
    CHECK(decide(input(at(3), 15.5), r).reason == Reason::KeepSatisfied);    // cheapest hours: no boost
    CHECK(decide(input(at(15), 15.5), r).reason == Reason::KeepSatisfied);   // before the peak: no pre-heat
    CHECK(decide(input(at(12), 15.1, /*on=*/true), r).on);                   // hysteresis while heating
    CHECK_FALSE(decide(input(at(12), 15.3, /*on=*/true), r).on);
    CHECK(decide(input(at(12), 15.0), r).inputs.mode == "keep");
}

TEST_CASE("Away mode: keep above the away level, overriding the program incl. off periods") {
    RoomConfig r = bedroom();
    r.program = {{0, ProgramMode::Heat, 20.0}, {17 * 60, ProgramMode::Off, 0}, {21 * 60, ProgramMode::Heat, 20.0}};
    auto in = input(at(12), 15.0);  // would heat (target 20)
    in.away = AwayMode{12.0, at(11), at(23)};
    auto d = decide(in, r);
    CHECK_FALSE(d.on);
    CHECK(d.reason == Reason::KeepSatisfied);
    CHECK(d.inputs.mode == "away");
    CHECK_THAT(d.inputs.targetLabel, ContainsSubstring("away until"));

    auto peak = input(at(18), 11.0);  // inside the program's off period: away keeps the house above 12
    peak.away = in.away;
    CHECK(decide(peak, r).reason == Reason::KeepAbove);

    auto back = input(at(23, 30), 17.0);  // away ended: back to the program
    back.away = in.away;
    CHECK(decide(back, r).reason == Reason::BelowTarget);

    RoomConfig shed;
    shed.id = "shed";
    shed.priceOnly = PriceOnlyConfig{24, {}};
    auto s = input(at(3), std::nullopt);
    s.away = AwayMode{12.0, at(1), at(23)};
    CHECK(decide(s, shed).reason == Reason::AwayOff);
}

TEST_CASE("Away mode can be planned ahead: nothing changes before it starts") {
    RoomConfig r = bedroom();
    r.program = {{0, ProgramMode::Heat, 20.0}};
    auto in = input(at(3), 18.0);
    in.away = AwayMode{12.0, at(4), at(23)};  // e.g. set the evening before a 04:00 flight
    CHECK(decide(in, r).on);  // 03:00: still at home, heating normally (cheap hours)
    CHECK(decide(in, r).inputs.mode == "heat");
    auto later = input(at(5), 18.0);
    later.away = in.away;
    CHECK(decide(later, r).reason == Reason::KeepSatisfied);  // 05:00: away
}
