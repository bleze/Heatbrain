#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>

#include "core/offline.hpp"

using namespace hb;
using namespace std::chrono;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}

const auto kDay = local_days{2026y / 11 / 23};
Instant at(int day, int hour, int minute = 0) { return tz().toInstant(kDay + days{day} + hours{hour} + minutes{minute}); }

// Night prices with a clear cheap stretch 02:00-03:00.
PriceTable nightPrices() {
    PriceTable t;
    t.currency = "DKK";
    for (int d = -1; d < 2; ++d) {
        for (int h = 0; h < 24; ++h) {
            for (int q = 0; q < 4; ++q) {
                const double p = h == 2 ? 1.0 : (h < 6 ? 2.0 : 2.5);
                t.slots.push_back({tz().toInstant(kDay + days{d} + hours{h} + minutes{15 * q}), {p}, p});
            }
        }
    }
    return t;
}

RoomConfig room(ProgramMode nightMode) {
    RoomConfig r;
    r.id = "bedroom";
    r.plugIds = {"bedroom"};
    r.minTemp = 10;
    r.maxTemp = 24;
    r.program = {{7 * 60, ProgramMode::Heat, 20.0}, {23 * 60, nightMode, 16.0}};
    r.warmup = minutes{0};  // no early start for 07:00 (tested on its own below)
    return r;
}

const DayWindow kNight{23 * 60 + 30, 6 * 60 + 30};

}  // namespace

TEST_CASE("Wi-Fi off period: the running occurrence, else the next") {
    // Before it: tonight's.
    auto p = currentOrNextOfflinePeriod(kNight, tz(), at(0, 22));
    CHECK(p.start == at(0, 23, 30));
    CHECK(p.end == at(1, 6, 30));
    CHECK_FALSE(p.contains(at(0, 22)));
    // In it, after midnight: the one that started yesterday.
    p = currentOrNextOfflinePeriod(kNight, tz(), at(1, 2));
    CHECK(p.start == at(0, 23, 30));
    CHECK(p.contains(at(1, 2)));
    // After it: tonight's again.
    p = currentOrNextOfflinePeriod(kNight, tz(), at(1, 7));
    CHECK(p.start == at(1, 23, 30));
}

TEST_CASE("Night schedule: keep/heat periods heat their learned share in the cheapest slots") {
    // 23:30-06:30 in a keep-above period, 25 % duty: 7 h -> 1 h 45 min, the cheapest first (02-03).
    const auto spans = offlineSchedule(room(ProgramMode::Keep), nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30),
                                       OfflineDuty{0.25, 0.25}, std::nullopt);
    minutes total{0};
    for (const auto& s : spans) total += duration_cast<minutes>(s.to - s.from);
    CHECK(total == minutes{105});
    CHECK(scheduleOnAt(spans, at(1, 2, 30)));  // the cheap hour is used
    CHECK_FALSE(scheduleOnAt(spans, at(1, 6)));
}

TEST_CASE("Night schedule: an off period stays off, price-only rooms take their cheapest hours") {
    CHECK(offlineSchedule(room(ProgramMode::Off), nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30),
                          OfflineDuty{0.5, 0.5}, std::nullopt)
              .empty());

    RoomConfig shed;
    shed.id = "shed";
    shed.priceOnly = PriceOnlyConfig{1, {}};  // the single cheapest hour of the day
    shed.program = {{0, ProgramMode::Heat, 10.0}};
    const auto spans = offlineSchedule(shed, nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30), OfflineDuty{}, std::nullopt);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].from == at(1, 2));
    CHECK(spans[0].to == at(1, 3));
}

TEST_CASE("Night schedule as Tasmota rules on the plug's local clock") {
    // 00:00-01:00 and 02:30-03:00 Copenhagen time; the plug runs on local time (+60 min in winter).
    const std::vector<OnSpan> spans{{at(1, 0), at(1, 1)}, {at(1, 2, 30), at(1, 3)}};
    const auto rules = tasmotaScheduleRules(spans, minutes{60});
    REQUIRE(rules);
    REQUIRE(rules->size() == 1);
    CHECK((*rules)[0] == "ON Time#Minute=0 DO Power1 1 ENDON ON Time#Minute=60 DO Power1 0 ENDON "
                         "ON Time#Minute=150 DO Power1 1 ENDON ON Time#Minute=180 DO Power1 0 ENDON");
    CHECK(isHeatbrainScheduleRule((*rules)[0]));
    CHECK(isHeatbrainScheduleRule(""));
    CHECK_FALSE(isHeatbrainScheduleRule("ON Power1#State DO Publish x ENDON"));

    // Many short spans: split over two rule sets, and refused beyond that.
    std::vector<OnSpan> many;
    for (int i = 0; i < 12; ++i) many.push_back({at(1, 0, 0) + minutes{30 * i}, at(1, 0, 15) + minutes{30 * i}});
    const auto two = tasmotaScheduleRules(many, minutes{60});
    REQUIRE(two);
    CHECK(two->size() == 2);
    for (const auto& r : *two) CHECK(r.size() <= 500);
    for (int i = 12; i < 40; ++i) many.push_back({at(1, 0, 0) + minutes{30 * i}, at(1, 0, 15) + minutes{30 * i}});
    CHECK_FALSE(tasmotaScheduleRules(many, minutes{60}));
}

TEST_CASE("The plug's own Wi-Fi goes off just after the network and comes back just before it") {
    const OfflinePeriod night{at(0, 23, 30), at(1, 6, 30)};
    const auto radio = plugRadioOffWindow(night);
    REQUIRE(radio);
    CHECK(radio->start == at(0, 23, 31));
    CHECK(radio->end == at(1, 6, 28));
    CHECK_FALSE(plugRadioOffWindow({at(0, 23, 30), at(0, 23, 45)}));  // too short to bother

    // Wi-Fi triggers first, on the plug's local clock (+60 min), then the relay's.
    const std::vector<OnSpan> spans{{at(1, 0), at(1, 1)}};
    const auto rules = tasmotaScheduleRules(spans, minutes{60}, radio);
    REQUIRE(rules);
    REQUIRE(rules->size() == 1);
    CHECK((*rules)[0] == "ON Time#Minute=1411 DO Wifi 0 ENDON ON Time#Minute=388 DO Wifi 1 ENDON "
                         "ON Time#Minute=0 DO Power1 1 ENDON ON Time#Minute=60 DO Power1 0 ENDON");
    CHECK(isHeatbrainScheduleRule((*rules)[0]));
    CHECK_FALSE(isHeatbrainScheduleRule("ON Time#Minute=10 DO Wifi 0 ENDON ON Button1#State DO Power1 2 ENDON"));

    // Only the Wi-Fi (a dry-run plug): two triggers, no relay.
    const auto only = tasmotaScheduleRules({}, minutes{60}, radio);
    REQUIRE(only);
    CHECK((*only)[0] == "ON Time#Minute=1411 DO Wifi 0 ENDON ON Time#Minute=388 DO Wifi 1 ENDON");
}

TEST_CASE("Weekend Wi-Fi off times apply to the nights ending Saturday and Sunday") {
    // 2026-11-23 is a Monday; kDay + 4 = Friday 27th, + 5 = Saturday, + 6 = Sunday.
    const WifiOffPlan plan{DayWindow{30, 6 * 60 + 30}, DayWindow{30, 9 * 60}};
    // Thursday evening: the night to Friday is a weekday night.
    auto p = currentOrNextOfflinePeriod(plan, tz(), at(3, 22));
    CHECK(p.start == at(4, 0, 30));
    CHECK(p.end == at(4, 6, 30));
    // Friday evening: the night to Saturday is a weekend night (sleep in until 09:00).
    p = currentOrNextOfflinePeriod(plan, tz(), at(4, 22));
    CHECK(p.start == at(5, 0, 30));
    CHECK(p.end == at(5, 9));
    // Saturday 08:00: still in it.
    CHECK(currentOrNextOfflinePeriod(plan, tz(), at(5, 8)).contains(at(5, 8)));
    // Sunday evening: the night to Monday is a weekday night again.
    p = currentOrNextOfflinePeriod(plan, tz(), at(6, 22));
    CHECK(p.end == at(7, 6, 30));
}

TEST_CASE("Gaps in the sensor readings show when the Wi-Fi was off") {
    std::vector<Instant> t;
    for (int m = 0; m <= 60; m += 10) t.push_back(at(0, 23) + minutes{m});        // 23:00-00:00
    for (int m = 0; m <= 60; m += 10) t.push_back(at(1, 6, 35) + minutes{m});     // 06:35-07:35
    auto gaps = findReadingGaps(t, at(1, 8));
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0].lastBefore == at(1, 0));
    CHECK(gaps[0].firstAfter == at(1, 6, 35));
    CHECK_FALSE(gaps[0].ongoing);
    // Still off now: an ongoing gap.
    gaps = findReadingGaps(t, at(1, 9));
    REQUIRE(gaps.size() == 2);
    CHECK(gaps[1].ongoing);
}

TEST_CASE("Night schedule: the share is learned per target, so a cool night stays off") {
    // The room heats a lot at 20 degrees in the day, but never needed heat at 16 at night.
    OfflineDuty duty{0.6, 0.25, {}};
    duty.byTarget[OfflineDuty::key(false, 16.0)] = 0.0;
    RoomConfig r = room(ProgramMode::Heat);  // night: "heat to 16" from 23:00
    const auto spans = offlineSchedule(r, nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30), duty, std::nullopt);
    CHECK(spans.empty());
    // Unknown target: at most 15 min per hour, not the daytime share.
    const auto unseen = offlineSchedule(r, nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30), OfflineDuty{0.6, 0.25, {}},
                                        std::nullopt);
    minutes total{0};
    for (const auto& s : unseen) total += duration_cast<minutes>(s.to - s.from);
    CHECK(total == minutes{105});
}

TEST_CASE("Night schedule: a warm room in a keep-above night stays silent until it would cool to the floor") {
    // 23:30-06:30, keep above 16. At 19 degrees with 5 outside, cooling 0.05/h, the room is at
    // 16.1 (floor + the band) after ~3.5 h: only the run from about 03:00 is eligible.
    const OfflineStart warm{at(0, 23, 30), 19.0, 0.05, 5.0, nullptr};
    const auto spans = offlineSchedule(room(ProgramMode::Keep), nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30),
                                       OfflineDuty{0.25, 0.25}, std::nullopt, warm);
    REQUIRE_FALSE(spans.empty());
    CHECK(spans.front().from >= at(1, 2, 45));
    CHECK_FALSE(scheduleOnAt(spans, at(1, 2, 30)));  // the cheap hour, but the room is still warm then

    // Slow cooling (well insulated): nothing all night.
    const OfflineStart snug{at(0, 23, 30), 19.0, 0.01, 5.0, nullptr};
    CHECK(offlineSchedule(room(ProgramMode::Keep), nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30),
                          OfflineDuty{0.25, 0.25}, std::nullopt, snug)
              .empty());

    // Already at the floor: the whole night as before.
    const OfflineStart cold{at(0, 23, 30), 16.0, 0.05, 5.0, nullptr};
    const auto all = offlineSchedule(room(ProgramMode::Keep), nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30),
                                     OfflineDuty{0.25, 0.25}, std::nullopt, cold);
    CHECK(scheduleOnAt(all, at(1, 2, 30)));
}

TEST_CASE("Night schedule: a warm-up for the morning heats at full power") {
    // Keep above 16 at night, 20 at 07:00, Wi-Fi back at 06:30: the 3 h ramp starts about 04:00.
    RoomConfig r = room(ProgramMode::Keep);
    r.warmup = minutes{180};
    const OfflineStart cold{at(0, 23, 30), 16.0, 0.05, 5.0, nullptr};
    const auto spans = offlineSchedule(r, nightPrices(), tz(), at(0, 23, 30), at(1, 6, 30), OfflineDuty{0.25, 0.25},
                                       std::nullopt, cold);
    CHECK(scheduleOnAt(spans, at(1, 2, 30)));        // the keep-above share, in the cheap hour
    CHECK_FALSE(scheduleOnAt(spans, at(1, 3, 30)));
    for (int m = 15; m < 150; m += 15) CHECK(scheduleOnAt(spans, at(1, 4) + minutes{m}));  // 04:15-06:30 solid
}
