#include <catch2/catch_test_macros.hpp>

#include "sensors/push.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;

namespace {
const Instant kNow = sys_days{2026y / 11 / 24} + 12h;
}

TEST_CASE("Pushed sensor: temperature and humidity reports merge; no news means no change for 3 hours") {
    auto& reg = PushRegistry::instance();
    reg.clear();
    reg.put("shed", "Shed", std::nullopt, 61.0, std::nullopt, kNow - 10min);
    CHECK(pushReadings(reg.snapshot(), kNow).empty());  // only humidity so far
    reg.put("shed", "", 7.5, std::nullopt, 90, kNow - 5min);
    auto r = pushReadings(reg.snapshot(), kNow);
    REQUIRE(r.size() == 1);
    CHECK(r[0].sensorId == "shed");
    CHECK(r[0].name == "Shed");  // kept from the first report
    CHECK(*r[0].temperature == 7.5);
    CHECK(*r[0].humidity == 61.0);
    CHECK(*r[0].batteryPercent == 90);
    CHECK(*r[0].measuredAt == kNow);  // current: nothing changed enough to report
    CHECK(r[0].reachable);

    r = pushReadings(reg.snapshot(), kNow + 4h);
    CHECK(*r[0].measuredAt == kNow - 5min);  // too long without a word: stale
    CHECK_FALSE(r[0].reachable);
    reg.clear();
}

TEST_CASE("Pushed sensors survive a restart") {
    test::TempDir dir;
    auto& reg = PushRegistry::instance();
    reg.clear();
    reg.setFile(dir.path() / "pushed-sensors.json");
    reg.put("shed", "Shed", 8.0, 60.0, std::nullopt, kNow);
    reg.clear();
    reg.setFile(dir.path() / "pushed-sensors.json");
    const auto s = reg.snapshot();
    REQUIRE(s.contains("shed"));
    CHECK(*s.at("shed").temperature == 8.0);
    CHECK(s.at("shed").lastReport == kNow);
    reg.setFile({});
    reg.clear();
}
