#include <catch2/catch_test_macros.hpp>

#include "core/time.hpp"

using namespace hb;
using namespace std::chrono;

TEST_CASE("parseIsoLocal accepts the formats used by the APIs") {
    CHECK(parseIsoLocal("2026-11-22T22:45:00") == local_days{2026y / 11 / 22} + 22h + 45min);
    CHECK(parseIsoLocal("2026-11-22T22:45") == local_days{2026y / 11 / 22} + 22h + 45min);
    CHECK(parseIsoLocal("2026-11-22") == local_days{2026y / 11 / 22});
    CHECK(parseIsoLocal("2026-11-22T22:45:00.000Z") == local_days{2026y / 11 / 22} + 22h + 45min);
}

TEST_CASE("parseIsoLocal rejects garbage") {
    CHECK_FALSE(parseIsoLocal(""));
    CHECK_FALSE(parseIsoLocal("2026-13-01"));
    CHECK_FALSE(parseIsoLocal("2026-02-30"));
    CHECK_FALSE(parseIsoLocal("2026-11-22X10:00"));
    CHECK_FALSE(parseIsoLocal("2026-11-22T25:00"));
    CHECK_FALSE(parseIsoLocal("not a date"));
}

TEST_CASE("Copenhagen local time follows CET/CEST") {
    const TimeZone tz("Europe/Copenhagen");
    const Instant winter = sys_days{2026y / 11 / 21} + 12h;
    const Instant summer = sys_days{2026y / 7 / 1} + 12h;
    CHECK(hourOfDay(tz.toLocal(winter)) == 13);
    CHECK(hourOfDay(tz.toLocal(summer)) == 14);
    CHECK(tz.toInstant(tz.toLocal(winter)) == winter);
}

TEST_CASE("Days have 92/96/100 slots around DST changes") {
    const TimeZone tz("Europe/Copenhagen");
    auto slotsInDay = [&](sys_days day) {
        const Instant start = startOfLocalDay(tz, day + 12h);
        const Instant end = startOfLocalDay(tz, start + 36h);
        return slotsBetween(start, end).size();
    };
    CHECK(slotsInDay(2026y / 3 / 29) == 92);   // spring forward
    CHECK(slotsInDay(2026y / 11 / 21) == 96);
    CHECK(slotsInDay(2026y / 10 / 25) == 100); // fall back
}

TEST_CASE("floorToSlot rounds down to 15 minutes") {
    const Instant t = sys_days{2026y / 11 / 21} + 12h + 29min + 59s;
    CHECK(floorToSlot(t) == sys_days{2026y / 11 / 21} + 12h + 15min);
}

TEST_CASE("Local time helpers") {
    const LocalTime t = local_days{2026y / 11 / 21} + 17h + 30min;  // a Saturday
    CHECK(minuteOfDay(t) == 17 * 60 + 30);
    CHECK(monthOf(t) == 11);
    CHECK(isoWeekday(t) == 6);
    CHECK(formatLocalDate(t) == "2026-11-21");
    CHECK(formatLocalHm(t) == "17:30");
}
