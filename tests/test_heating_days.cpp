#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/cost_report.hpp"
#include "storage/history.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;
using Catch::Approx;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}
Instant at(int day, int hour, int minute = 0) {
    return tz().toInstant(local_days{2026y / 11 / day} + hours{hour} + minutes{minute});
}

// Decisions every 5 minutes over [from, to): on from 06:00 to 08:00.
void decide(History& h, const std::string& room, Instant from, Instant to) {
    for (Instant t = from; t < to; t += minutes{5}) {
        const int hour = hourOfDay(tz().toLocal(t));
        const bool on = hour >= 6 && hour < 8;
        h.recordDecision({t, room, on, false, on ? "BELOW_TARGET" : "TARGET_REACHED", "", json::object(),
                          json::array({{{"id", room}, {"mode", "live"}, {"state", on}}})});
    }
}

const json kCfg = json::parse(R"({"timezone":"Europe/Copenhagen","control":{"tickSeconds":300},
    "plugs":[{"id":"bedroom","watts":1000}],
    "rooms":[{"id":"bedroom","name":"Bedroom","plugs":["bedroom"]},
             {"id":"hall","name":"Hall","plugs":[],"priceOnly":{"maxPrice":2}}]})");

}  // namespace

TEST_CASE("Heating days: hours on per local day, finished days kept for good") {
    test::TempDir dir;
    History h(dir.path() / "history.db");
    // From the 20th on; nothing from noon on the 21st until midnight (heatbrain was down).
    decide(h, "bedroom", at(20, 0), at(21, 12));
    decide(h, "bedroom", at(22, 0), at(22, 12));
    const Instant now = at(22, 12);

    const json r = heatingDaysReport(kCfg, h, now, 0, "", "");
    CHECK(r["from"] == "2026-11-20");
    CHECK(r["today"] == "2026-11-22");
    CHECK(r["excluded"] == json::array({"Hall"}));
    REQUIRE(r["rooms"].size() == 1);
    const json& days = r["rooms"][0]["days"];
    REQUIRE(days.size() == 3);
    CHECK(days[0][0] == "2026-11-20");
    CHECK(days[0][1].get<double>() == Approx(2.0));  // 06:00-08:00
    CHECK(days[0][2].get<double>() == Approx(2.0));  // x 1 kW
    CHECK(days[0][3].get<double>() == Approx(24.0));
    CHECK(days[1][1].get<double>() == Approx(2.0));
    CHECK(days[1][3].get<double>() == Approx(12.0 + 10.0 / 60));  // the last decision counts 15 min at most
    CHECK(days[2][0] == "2026-11-22");                              // today, so far
    CHECK(days[2][3].get<double>() == Approx(12.0));

    // Only the finished days are kept; they stay when the detailed decisions are pruned.
    const auto kept = h.roomDays("bedroom", "2026-01-01", "2026-12-31");
    REQUIRE(kept.size() == 2);
    CHECK(kept[1].date == "2026-11-21");
    h.prune(at(22, 0));
    const json later = heatingDaysReport(kCfg, h, now, 0, "", "");
    REQUIRE(later["rooms"][0]["days"].size() == 3);
    CHECK(later["from"] == "2026-11-20");
    CHECK(later["rooms"][0]["days"][1][1].get<double>() == Approx(2.0));

    // A shorter range: from its first day only.
    const json two = heatingDaysReport(kCfg, h, now, 2, "", "");
    CHECK(two["from"] == "2026-11-21");
    CHECK(two["rooms"][0]["days"].size() == 2);
}

TEST_CASE("Heating days: the outdoor sensor's daily temperatures alongside") {
    test::TempDir dir;
    History h(dir.path() / "history.db");
    decide(h, "bedroom", at(21, 0), at(21, 1));
    for (const double temp : {1.0, 5.0}) {
        h.recordTemperature({at(21, temp == 1.0 ? 6 : 14), "netatmo:out", "", temp, std::nullopt, at(21, 0)});
    }
    const json r = heatingDaysReport(kCfg, h, at(22, 12), 7, "netatmo:out", "Garden");
    REQUIRE(r["outdoor"].is_object());
    CHECK(r["outdoor"]["name"] == "Garden");
    REQUIRE(r["outdoor"]["days"].size() == 1);
    CHECK(r["outdoor"]["days"][0][0] == "2026-11-21");
    CHECK(r["outdoor"]["days"][0][1].get<double>() == Approx(3.0));
    CHECK(r["outdoor"]["days"][0][2].get<double>() == Approx(1.0));
    CHECK(r["outdoor"]["days"][0][3].get<double>() == Approx(5.0));
}

TEST_CASE("Forgetting a room's early decisions: they no longer count, the rest is summed up again") {
    test::TempDir dir;
    History h(dir.path() / "history.db");
    decide(h, "bedroom", at(20, 0), at(22, 12));
    decide(h, "office", at(20, 0), at(22, 12));
    const Instant now = at(22, 12);
    REQUIRE(heatingDaysReport(kCfg, h, now, 0, "", "")["rooms"][0]["days"].size() == 3);  // 20th and 21st kept

    // Before 07:00 on the 21st: the 20th goes, the 21st keeps 07:00-08:00 of its heating.
    CHECK(h.forgetDecisions("bedroom", at(21, 7), "2026-11-21") == 12 * 24 + 7 * 12);
    const json r = heatingDaysReport(kCfg, h, now, 0, "", "");
    const json& days = r["rooms"][0]["days"];
    REQUIRE(days.size() == 2);
    CHECK(days[0][0] == "2026-11-21");
    CHECK(days[0][1].get<double>() == Approx(1.0));
    CHECK(days[0][3].get<double>() == Approx(17.0));
    CHECK(h.decisions("office", at(20, 0), now, false).size() == 12 * 60);  // other rooms untouched
}
