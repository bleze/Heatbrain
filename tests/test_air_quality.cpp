#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sqlite3.h>

#include "core/air_quality.hpp"
#include "storage/history.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Matchers::ContainsSubstring;

namespace {
const TimeZone kTz{"Europe/Copenhagen"};
const Instant kDay = sys_days{2026y / 11 / 24} - 1h;  // midnight local (CET = UTC+1)
Instant local(int h, int m = 0) { return kDay + hours{h} + minutes{m}; }

std::vector<Problem> step(AirQualityTracker& t, int co2, Instant at, bool quiet, const AirQualityConfig& cfg = {}) {
    std::vector<Problem> p;
    t.update("bedroom", "Bedroom", co2, at, quiet, kTz, cfg, p);
    return p;
}
}  // namespace

TEST_CASE("CO2: a problem while it is high, gone once clearly back down") {
    AirQualityTracker t;
    CHECK(step(t, 900, local(14), false).empty());
    auto p = step(t, 1450, local(14, 5), false);
    REQUIRE(p.size() == 1);
    CHECK(p[0].key == "room:bedroom:co2");
    CHECK(p[0].debounce == minutes{20});  // a short spike is no alert
    p = step(t, 1600, local(14, 10), false);
    CHECK_THAT(p[0].message, ContainsSubstring("1600 ppm since 14:05 (highest 1600 ppm)"));
    CHECK(step(t, 1300, local(14, 15), false).size() == 1);  // just under the limit: not over yet
    CHECK(step(t, 1150, local(14, 20), false).empty());      // below 1200: over
}

TEST_CASE("CO2: a spell within the quiet hours comes as one message when they end") {
    AirQualityTracker t;
    const AirQualityConfig cfg;
    CHECK_FALSE(t.morningSummary(true, kTz, cfg));
    step(t, 1500, local(2, 10), true);
    step(t, 1720, local(4), true);
    CHECK(step(t, 1100, local(6, 40), true).empty());
    CHECK_FALSE(t.morningSummary(true, kTz, cfg));  // still quiet
    const auto m = t.morningSummary(false, kTz, cfg);
    REQUIRE(m);
    CHECK_THAT(m->second, ContainsSubstring("Bedroom: CO₂ above 1400 ppm 02:10-06:40, highest 1720 ppm."));
    CHECK_FALSE(t.morningSummary(false, kTz, cfg));  // once
}

TEST_CASE("CO2: still high when the quiet hours end - the normal alert, not the morning message") {
    AirQualityTracker t;
    const AirQualityConfig cfg;
    t.morningSummary(true, kTz, cfg);
    step(t, 1500, local(5), true);
    CHECK(step(t, 1500, local(7), false).size() == 1);
    CHECK_FALSE(t.morningSummary(false, kTz, cfg));
}

TEST_CASE("CO2: switched off, or a missing reading") {
    AirQualityTracker t;
    AirQualityConfig off;
    off.enabled = false;
    CHECK(step(t, 2000, local(14), false, off).empty());
    step(t, 1500, local(14), false);
    std::vector<Problem> p;
    t.update("bedroom", "Bedroom", std::nullopt, local(14, 5), false, kTz, {}, p);
    CHECK(p.size() == 1);  // no reading this step: the spell stays
}

TEST_CASE("History keeps CO2 per reading and per day; older databases get the columns") {
    test::TempDir dir;
    const auto file = dir.path() / "history.db";
    {
        // A database from before CO2.
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
        sqlite3_exec(db,
                     "CREATE TABLE temperatures (ts INTEGER NOT NULL, sensor_key TEXT NOT NULL, room_id TEXT NOT NULL,"
                     " temperature REAL NOT NULL, humidity REAL, PRIMARY KEY (sensor_key, ts));"
                     "INSERT INTO temperatures VALUES (100, 'netatmo:a', 'bedroom', 19.5, 50);",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    History h(file);
    const Instant day = sys_days{2026y / 11 / 23};
    h.recordTemperature({day + 1h, "netatmo:a", "bedroom", 19.0, 50.0, day, 800.0});
    h.recordTemperature({day + 2h, "netatmo:a", "bedroom", 19.2, 51.0, day, 1600.0});
    h.recordTemperature({day + 3h, "netatmo:a", "bedroom", 19.4, 52.0, day, std::nullopt});
    const auto t = h.sensorTemperatures("netatmo:a", Instant{}, day + 24h);
    REQUIRE(t.size() == 4);
    CHECK_FALSE(t[0].co2);  // the old reading
    CHECK(*t[2].co2 == 1600);
    const auto d = h.sensorDays("netatmo:a", day, day + 24h);
    REQUIRE(d.size() == 1);
    CHECK(*d[0].avgCo2 == 1200);
    CHECK(*d[0].maxCo2 == 1600);
}
