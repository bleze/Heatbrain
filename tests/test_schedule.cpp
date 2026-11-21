#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "core/schedule.hpp"
#include "providers/basic_components.hpp"

using namespace hb;
using namespace std::chrono;

namespace {

const char* kSchedule = R"({
  "seasons": [
    { "months": [10,11,12,1,2,3], "weekdays": ["mon","tue","wed","thu","fri"],
      "bands": [ {"from":"00:00","to":"06:00","amount":0.1},
                 {"from":"06:00","to":"17:00","amount":0.3},
                 {"from":"17:00","to":"21:00","amount":0.9},
                 {"from":"21:00","to":"24:00","amount":0.3} ] },
    { "months": [10,11,12,1,2,3],
      "bands": [ {"from":"00:00","to":"24:00","amount":0.2} ] }
  ]
})";

}  // namespace

TEST_CASE("Schedule picks season, weekday and band") {
    const Schedule s = parseSchedule(nlohmann::json::parse(kSchedule));
    const auto friday = local_days{2026y / 11 / 20};
    const auto saturday = local_days{2026y / 11 / 21};

    CHECK(*s.at(friday + 3h) == 0.1);
    CHECK(*s.at(friday + 17h) == 0.9);
    CHECK(*s.at(friday + 20h + 59min) == 0.9);
    CHECK(*s.at(friday + 21h) == 0.3);
    CHECK(*s.at(friday + 23h + 59min) == 0.3);
    CHECK(*s.at(saturday + 18h) == 0.2);              // weekend falls through to season 2
    CHECK_FALSE(s.at(local_days{2026y / 7 / 1} + 12h));  // summer not described
}

TEST_CASE("parseClock") {
    CHECK(parseClock("00:00") == 0);
    CHECK(parseClock("17:30") == 17 * 60 + 30);
    CHECK(parseClock("24:00") == 1440);
    CHECK_FALSE(parseClock("24:01"));
    CHECK_FALSE(parseClock("7:30"));
    CHECK_FALSE(parseClock("07:60"));
}

TEST_CASE("Invalid schedules are rejected") {
    CHECK_THROWS(parseSchedule(nlohmann::json::parse(
        R"({"seasons":[{"bands":[{"from":"10:00","to":"09:00","amount":1}]}]})")));
    CHECK_THROWS(parseSchedule(nlohmann::json::parse(
        R"({"seasons":[{"months":[13],"bands":[]}]})")));
    CHECK_THROWS(parseSchedule(nlohmann::json::parse(
        R"({"seasons":[{"weekdays":["funday"],"bands":[]}]})")));
}
