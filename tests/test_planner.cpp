#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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
    CHECK(decide(input(at(12), 18.5), bedroom(), makePrices(flat)).reason == Reason::BelowTarget);
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
    const auto d = decide(input(at(17), 18.4));  // before the warm-up for 21:00 begins
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

TEST_CASE("Colder outside: more pre-heating before the peak") {
    CHECK(outdoorFactor(20, std::nullopt) == 1.0);
    CHECK(outdoorFactor(20, 5.0) == 1.0);
    CHECK(outdoorFactor(20, -10.0) == 1.5);  // capped
    CHECK(outdoorFactor(20, 15.0) == 0.5);   // mild: half

    auto mild = input(at(15), 19.6);
    mild.outdoorTemp = 15.0;  // boost 0.5: ceiling 19.5 - already there
    CHECK_FALSE(decide(mild).on);

    auto cold = input(at(15), 19.6);
    cold.outdoorTemp = -5.0;  // boost 1.5: ceiling 20.5
    const auto d = decide(cold);
    CHECK(d.reason == Reason::PreheatBeforePeak);
    CHECK_THAT(d.detail, ContainsSubstring("20.5°C"));
    CHECK_THAT(d.detail, ContainsSubstring("-5.0°C outside"));
    CHECK(d.inputs.outdoorTemp == -5.0);
}

TEST_CASE("Plan ahead shows the intent per period") {
    RoomConfig r = bedroom();
    r.program = {{7 * 60, ProgramMode::Heat, 19.0}, {17 * 60, ProgramMode::Off, 0}, {21 * 60, ProgramMode::Heat, 20.0},
                 {23 * 60, ProgramMode::Keep, 16.0}};
    const auto plan = planAhead(r, makePrices(), tz(), at(12), hours{14}, std::nullopt, std::nullopt);
    // 12:00 heat 19 -> 14:00 pre-heat (peak at 17 within 3 h) -> 17 off -> 21 heat 20 -> 23 keep 16
    std::vector<std::string> kinds;
    for (const auto& s : plan) kinds.push_back(s.kind);
    CHECK(kinds == std::vector<std::string>{"heat", "preheat", "off", "heat", "keep"});
    CHECK(plan[1].from == at(14));
    CHECK(plan[2].from == at(17));
    CHECK(plan[3].target == 20.0);
    CHECK(plan.back().to == at(26));

    const auto away = planAhead(r, makePrices(), tz(), at(12), hours{4}, AwayMode{12.0, at(13), at(15)}, std::nullopt);
    CHECK(away[0].kind != "away");
    CHECK(away[1].kind == "away");
    CHECK(away[1].from == at(13));
}

TEST_CASE("Price-only rooms pick whole hours, so the relay does not flap every 15 minutes") {
    RoomConfig shed;
    shed.id = "shed";
    shed.priceOnly = PriceOnlyConfig{1, {}};
    PriceTable p = makePrices();
    // Make 12:15 the single cheapest slot of the day, but 03:00-04:00 the cheapest hour.
    for (auto& s : p.slots) {
        if (s.start == at(12, 15)) s.total = s.parts[0] = 0.5;
        if (s.start >= at(3) && s.start < at(4)) s.total = s.parts[0] = 1.0;
    }
    CHECK(decide(input(at(3, 30), std::nullopt), shed, p).reason == Reason::CheapHours);
    CHECK(decide(input(at(12, 15), std::nullopt), shed, p).reason == Reason::NotCheapHours);
}

namespace {
// The pizzeria: kept above 5 deg all day, booked now and then.
RoomConfig pizzeria() {
    RoomConfig r;
    r.id = "pizzeria";
    r.name = "Pizzeria";
    r.thermostat = "pizzeria";
    r.sensor = "devi:pizzeria";
    r.minTemp = 5;
    r.maxTemp = 25;
    r.program = {{0, ProgramMode::Keep, 5.0}};
    r.warmup = hours{3};
    return r;
}
}  // namespace

TEST_CASE("A booking warms the room up early, holds the target through the peak, then hands back") {
    const auto room = pizzeria();
    const Booking b{22.0, at(16), at(23), room.warmup};  // 16-23, so warming from 13:00

    auto in = input(at(12), 6.0);
    in.booking = bookingAt({b}, in.now);
    CHECK_FALSE(in.booking);  // too early: the program (keep above 5) applies
    CHECK_FALSE(decide(in, room).on);

    in = input(at(13, 30), 8.0);
    in.booking = bookingAt({b}, in.now);
    auto d = decide(in, room);
    CHECK(d.on);
    CHECK(d.reason == Reason::WarmUp);
    CHECK(d.inputs.mode == "booked");
    REQUIRE(d.inputs.target);
    CHECK(*d.inputs.target > 8.0);    // on the warm-up curve from 5 (keep above) to 22
    CHECK(*d.inputs.target < 22.0);
    CHECK(d.inputs.warmupTo == 22.0);
    CHECK_THAT(d.inputs.targetLabel, ContainsSubstring("warming up"));
    CHECK_THAT(d.detail, ContainsSubstring("the booking"));

    // Warmer than the curve: waits, even in the cheap hours.
    in = input(at(13, 30), 15.0);
    in.booking = bookingAt({b}, in.now);
    CHECK(decide(in, room).reason == Reason::WarmUpOnTrack);

    // 18:00 is in the expensive peak; a heat period would coast, the booking does not.
    in = input(at(18), 21.0);
    in.booking = bookingAt({b}, in.now);
    d = decide(in, room);
    CHECK(d.on);
    CHECK(d.reason != Reason::PeakAvoid);
    CHECK_THAT(d.inputs.targetLabel, ContainsSubstring("booked until 23:00"));

    in = input(at(23, 15), 21.0);
    in.booking = bookingAt({b}, in.now);
    d = decide(in, room);
    CHECK_FALSE(d.on);
    CHECK(d.inputs.mode == "keep");
}

TEST_CASE("A booking wins over away mode, and the plan shows it") {
    const auto room = pizzeria();
    const Booking b{22.0, at(16), at(20), room.warmup};
    auto in = input(at(17), 15.0);
    in.away = AwayMode{12.0, at(0), at(23)};
    in.booking = bookingAt({b}, in.now);
    CHECK(decide(in, room).inputs.mode == "booked");

    const auto plan = planAhead(room, makePrices(), tz(), at(12), hours{10}, std::nullopt, std::nullopt, {b});
    const auto booked = std::ranges::find_if(plan, [](const PlanSegment& s) { return s.kind == "booked"; });
    REQUIRE(booked != plan.end());
    CHECK(booked->from == at(13));
    CHECK(booked->warmup);
    CHECK(booked->booked);
    double last = 0;
    for (auto it = booked; it != plan.end() && it->kind == "booked"; ++it) {  // up the curve, then flat
        REQUIRE(it->target);
        CHECK(*it->target >= last);
        last = *it->target;
        if (it->from >= at(16)) CHECK_FALSE(it->warmup);
        if (it->to == at(20)) CHECK(*it->target == 22.0);
    }
    CHECK(plan.back().kind == "keep");
    CHECK_FALSE(plan.back().booked);
}

TEST_CASE("Plan: a warm room in a keep-above period shows as idle until it would cool to the floor") {
    const auto room = pizzeria();  // keep above 5 all day
    // At 16 deg with 5 outside, cooling 5 %/h: still well above 5 deg after 24 h -> idle all day.
    const auto warm = planAhead(room, makePrices(), tz(), at(12), hours{24}, std::nullopt, 5.0, {}, 16.0, 0.05);
    CHECK(std::ranges::all_of(warm, [](const PlanSegment& s) { return s.kind == "idle"; }));

    // Cooling fast (50 %/h) from 8 deg with 0 outside: idle first, then holding the floor ("keep").
    const auto cold = planAhead(room, makePrices(), tz(), at(12), hours{24}, std::nullopt, 0.0, {}, 8.0, 0.5);
    REQUIRE(cold.size() >= 2);
    CHECK(cold.front().kind == "idle");
    CHECK(cold.back().kind == "keep");

    // Without a current temperature the plan is as before.
    const auto unknown = planAhead(room, makePrices(), tz(), at(12), hours{24}, std::nullopt, 5.0);
    CHECK(unknown.front().kind == "keep");
}

TEST_CASE("Plan: a heat period below the room's temperature is idle too") {
    auto room = pizzeria();
    room.program = {{0, ProgramMode::Heat, 5.0}};
    room.minTemp = 5;
    const auto plan = planAhead(room, makePrices(), tz(), at(12), hours{24}, std::nullopt, 5.0, {}, 16.0, 0.05);
    CHECK(std::ranges::all_of(plan, [](const PlanSegment& s) { return s.kind == "idle"; }));
}

namespace {
ThermalModel learnedRoom(double loss = 0.05, double gain = 3.0) {
    ThermalModel m;
    m.loss = loss;
    m.gain = gain;
    m.coolingStretches = 5;
    m.heatingStretches = 5;
    return m;
}
std::shared_ptr<const OutdoorForecast> flatForecast(double temperature) {
    auto f = std::make_shared<OutdoorForecast>();
    for (int h = 0; h < 48; ++h) f->hourly.emplace_back(tz().toInstant(kDay + hours{h}), temperature);
    return f;
}
}  // namespace

TEST_CASE("Price outlook knows when the expensive stretch ends") {
    const auto o = priceOutlook(makePrices(), at(15), minutes{180});
    REQUIRE(o.nextExpensiveAt);
    CHECK(*o.nextExpensiveAt == at(17));
    REQUIRE(o.nextExpensiveUntil);
    CHECK(*o.nextExpensiveUntil == at(21));
}

TEST_CASE("Needed pre-heat: enough to coast through the stretch, none when the room holds anyway") {
    const auto m = learnedRoom();
    // floor 18, 5 deg outside, 4 h at loss 0.05: must start at 5 + 13 e^0.2 = 20.88
    CHECK(neededPreheat(m, 19, 1, 5, at(17), at(21)) == Catch::Approx(1.878).epsilon(0.01));
    CHECK(neededPreheat(m, 19, 1, 15, at(17), at(21)) == 0.0);    // mild: holds
    CHECK(neededPreheat(m, 19, 1, 5, at(17), at(17, 30)) == 0.0); // short peak: holds
}

TEST_CASE("Learned rooms pre-heat just enough, and only as early as needed") {
    auto in = input(at(15), 19.2);
    in.outdoorTemp = 5.0;
    in.thermal = learnedRoom();
    CHECK(decide(in).reason != Reason::PreheatBeforePeak);  // 2 h ahead: too early, the heat would leak away

    in.now = at(15, 45);
    in.measuredAt = in.now;
    const auto d = decide(in);
    CHECK(d.reason == Reason::PreheatBeforePeak);
    CHECK_THAT(d.detail, ContainsSubstring("20.9°C"));
    CHECK_THAT(d.detail, ContainsSubstring("to last until 21:00"));
    REQUIRE(d.inputs.ceiling);
    CHECK(*d.inputs.ceiling == Catch::Approx(20.88).epsilon(0.01));

    // Mild evening by the forecast: the room holds through the peak by itself, no pre-heat.
    in.forecast = flatForecast(15.0);
    in.now = at(16, 30);
    in.measuredAt = in.now;
    CHECK(decide(in).reason != Reason::PreheatBeforePeak);
}

TEST_CASE("The forecast for the peak sets the pre-heat, not the temperature right now") {
    auto in = input(at(15), 19.6);
    in.outdoorTemp = 15.0;              // mild now: boost 0.5, ceiling 19.5 - already there
    in.forecast = flatForecast(-5.0);   // but freezing during the peak: boost 1.5
    const auto d = decide(in);
    CHECK(d.reason == Reason::PreheatBeforePeak);
    CHECK_THAT(d.detail, ContainsSubstring("20.5°C"));
}

TEST_CASE("Window open: heating paused, frost protection and manual ON still win") {
    auto in = input(at(10), 17.0);
    in.windowOpen = OpenWindowEvent{at(9, 55), at(10, 25), 1.4, minutes{15}};
    const auto d = decide(in);
    CHECK_FALSE(d.on);
    CHECK(d.reason == Reason::WindowOpen);
    CHECK_THAT(d.detail, ContainsSubstring("fell 1.4° in 15 min"));
    CHECK_THAT(d.detail, ContainsSubstring("10:25"));

    auto cold = input(at(10), 4.0);
    cold.windowOpen = in.windowOpen;
    CHECK(decide(cold).reason == Reason::FrostProtection);

    auto manual = in;
    manual.override = ManualOverride{true, at(11)};
    CHECK(decide(manual).reason == Reason::ManualOn);

    auto later = in;
    later.now = at(10, 30);
    later.measuredAt = later.now;
    CHECK(decide(later).reason != Reason::WindowOpen);
}

TEST_CASE("Warm-up: the next step up in the program, never out of an off period") {
    const auto night = nextStepUp(bedroom(), tz(), at(3));  // heat to 16 -> 19 at 07:00
    REQUIRE(night);
    CHECK(night->at == at(7));
    CHECK(night->from == 16.0);
    CHECK(night->target == 19.0);
    CHECK(nextStepUp(bedroom(), tz(), at(12))->at == at(21));  // 19 -> 20
    CHECK_FALSE(nextStepUp(bedroom(), tz(), at(22)));          // next: down to 16 at 23:00
    CHECK_FALSE(nextStepUp(bedroom(), tz(), at(7)));           // 07:00 itself is under way: next is 21:00, 14 h off

    RoomConfig r = bedroom();
    r.program = {{7 * 60, ProgramMode::Heat, 19.0}, {17 * 60, ProgramMode::Off, 0}, {21 * 60, ProgramMode::Heat, 20.0},
                 {23 * 60, ProgramMode::Keep, 16.0}};
    CHECK_FALSE(nextStepUp(r, tz(), at(20)));  // off until 21:00: the peak stays off
    CHECK(nextStepUp(r, tz(), at(3))->from == 16.0);  // keep above 16 -> 19
    CHECK(decide(input(at(20, 30), 15.5), r).reason == Reason::ScheduledOff);
}

TEST_CASE("Warm-up curve: flat, then an S up to the target half an hour early") {
    const RoomConfig r = bedroom();  // not learned: a ramp over the 3 h warm-up setting
    const ProgramStep step{at(7), 16.0, 19.0};
    CHECK(warmupCurve(r, step, at(3), std::nullopt, 5.0) == 16.0);
    CHECK(warmupCurve(r, step, at(6, 55), std::nullopt, 5.0) == 19.0);
    double prev = 16.0;
    for (int m = 0; m <= 240; m += 5) {
        const double v = warmupCurve(r, step, at(3) + minutes{m}, std::nullopt, 5.0);
        CHECK(v >= prev - 1e-9);  // never falls
        CHECK(v - prev < 0.3);     // no jumps
        prev = v;
    }
    CHECK(warmupCurve(r, step, at(5), std::nullopt, 5.0) == Catch::Approx(19.0 - 3.0 * 1.5 / 2.5));  // mid-ramp

    // Learned: the room's own full-heat curve, so a slow room starts much earlier.
    const ThermalModel slow{0.05, 1.5, 9, 9};
    const double learned = warmupCurve(r, step, at(3), slow, 0.0);
    CHECK(learned > 17.5);
    CHECK(learned < 18.5);
    RoomConfig off = r;
    off.warmup = minutes{0};
    CHECK(decide(input(at(6, 15), 16.2), off).reason != Reason::WarmUp);
}

TEST_CASE("Warm-up: heats below the curve, rests above it, targets the curve") {
    const auto d = decide(input(at(6, 15), 16.2));  // a normal-priced slot
    CHECK(d.on);
    CHECK(d.reason == Reason::WarmUp);
    REQUIRE(d.inputs.target);
    CHECK(*d.inputs.target > 17.0);
    CHECK(*d.inputs.target < 19.0);
    CHECK(d.inputs.warmupTo == 19.0);
    CHECK(d.inputs.mode == "heat");
    CHECK_THAT(d.inputs.targetLabel, ContainsSubstring("19.0°C at 07:00"));

    const auto ahead = decide(input(at(6, 15), 18.9));
    CHECK_FALSE(ahead.on);
    CHECK(ahead.reason == Reason::WarmUpOnTrack);

    // The plan: warm-up segments, their targets rising to 19 at 07:00.
    const auto plan = planAhead(bedroom(), makePrices(), tz(), at(3), hours{5}, std::nullopt, 5.0);
    double last = 0;
    int warm = 0;
    for (const auto& s : plan) {
        if (!s.warmup) continue;
        ++warm;
        REQUIRE(s.target);
        CHECK(*s.target >= last);
        CHECK(s.to <= at(7));
        last = *s.target;
    }
    CHECK(warm > 4);
    CHECK(plan.back().target == 19.0);
}

TEST_CASE("Warm-up towards a keep-above period ignores price, like the period itself") {
    RoomConfig r = bedroom();
    r.program = {{9 * 60, ProgramMode::Keep, 21.5}, {17 * 60, ProgramMode::Off, 0}, {23 * 60, ProgramMode::Keep, 16.0}};
    std::array<double, 24> morningPeak = kHourly;
    for (int h = 6; h < 9; ++h) morningPeak[static_cast<std::size_t>(h)] = 3.4;  // expensive while warming
    const auto d = decide(input(at(8), 17.0), r, makePrices(morningPeak));
    CHECK(d.on);
    CHECK(d.reason == Reason::WarmUp);
    CHECK(d.inputs.warmupTo == 21.5);

    // Towards a heat period the same slot coasts (down to the curve minus the allowed drop).
    r.program[0].mode = ProgramMode::Heat;
    CHECK(decide(input(at(8), 20.5), r, makePrices(morningPeak)).reason == Reason::PeakAvoid);
}
