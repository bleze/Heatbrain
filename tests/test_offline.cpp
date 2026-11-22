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
