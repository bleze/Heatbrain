#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "providers/forecast.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Matchers::ContainsSubstring;

namespace {
Instant h(int hour) { return Instant{hours{491'000 + hour}}; }
}  // namespace

TEST_CASE("Open-Meteo answer is parsed, nulls skipped") {
    const auto j = nlohmann::json::parse(std::format(
        R"({{"hourly":{{"time":[{},{},{}],"temperature_2m":[2.5,null,-1.0]}}}})",
        h(0).time_since_epoch().count(), h(1).time_since_epoch().count(), h(2).time_since_epoch().count()));
    const auto f = parseOpenMeteo(j);
    REQUIRE(f.hourly.size() == 2);
    CHECK(f.hourly[1].second == -1.0);
    CHECK_THROWS(parseOpenMeteo(nlohmann::json::parse(R"({"hourly":{"time":[],"temperature_2m":[]}})")));
    CHECK_THAT(openMeteoUrl(55.67891, 12.56789), ContainsSubstring("latitude=55.68&longitude=12.57"));
}

TEST_CASE("Forecast interpolates, averages and is corrected towards the outdoor sensor") {
    OutdoorForecast f;
    f.hourly = {{h(0), 0.0}, {h(1), 2.0}, {h(2), 4.0}, {h(12), 4.0}};
    CHECK(*f.at(h(0) + minutes{30}) == Catch::Approx(1.0));
    CHECK_FALSE(f.at(h(13)));
    CHECK(*f.meanOver(h(0), h(1)) == Catch::Approx(0.75));  // 0, 0.5, 1, 1.5
    CHECK_FALSE(f.meanOver(h(11), h(14)));

    // The sensor reads 3 degrees warmer than the forecast now: fully at first, faded out after 6 h.
    const auto c = f.correctedTo(3.0, h(0));
    CHECK(*c.at(h(0)) == Catch::Approx(3.0));
    CHECK(*c.at(h(3)) == Catch::Approx(4.0 + 1.5));
    CHECK(*c.at(h(8)) == Catch::Approx(4.0));
    // An implausible difference is capped.
    CHECK(*f.correctedTo(20.0, h(0)).at(h(0)) == Catch::Approx(6.0));
}
