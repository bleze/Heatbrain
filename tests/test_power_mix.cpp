#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>

#include "app/power_mix_sync.hpp"
#include "providers/power_mix.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Power mix: 5-minute records averaged per hour, net imports only, CO2 alongside") {
    const auto prodex = nlohmann::json::parse(R"({"records":[
        {"Minutes5UTC":"2026-11-22T20:00:00","OffshoreWindPower":3000,"OnshoreWindPower":1000,"SolarPower":0,
         "ProductionGe100MW":500,"ProductionLt100MW":300,"ExchangeGermany":-400,"ExchangeSweden":600,"BornholmSE4":null},
        {"Minutes5UTC":"2026-11-22T20:55:00","OffshoreWindPower":1000,"OnshoreWindPower":1000,"SolarPower":10,
         "ProductionGe100MW":700,"ProductionLt100MW":300,"ExchangeGermany":-900,"ExchangeSweden":100},
        {"Minutes5UTC":"2026-11-22T21:00:00","OffshoreWindPower":2000,"OnshoreWindPower":0,"SolarPower":0,
         "ProductionGe100MW":0,"ProductionLt100MW":0}
    ]})");
    const auto co2 = nlohmann::json::parse(R"({"records":[
        {"Minutes5UTC":"2026-11-22T20:00:00","CO2Emission":40},{"Minutes5UTC":"2026-11-22T20:30:00","CO2Emission":60},
        {"Minutes5UTC":"2026-11-23T05:00:00","CO2Emission":99}
    ]})");
    const auto mix = parsePowerMix(prodex, co2);
    REQUIRE(mix.size() == 2);  // the CO2-only hour is left out
    CHECK(mix[0].hour == Instant{sys_days{2026y / 11 / 22} + hours{20}});
    CHECK(mix[0].offshoreWind == Catch::Approx(2000));
    CHECK(mix[0].onshoreWind == Catch::Approx(1000));
    CHECK(mix[0].solar == Catch::Approx(5));
    CHECK(mix[0].plants == Catch::Approx(900));
    CHECK(mix[0].imports == Catch::Approx(100));  // +200 net, then -800 net (an export, not a source)
    REQUIRE(mix[0].co2);
    CHECK(*mix[0].co2 == Catch::Approx(50));
    CHECK_FALSE(mix[1].co2);
}

TEST_CASE("Power mix: the area comes from the spot price, URLs ask for it and the last 8 days") {
    const auto cfg = nlohmann::json::parse(
        R"({"price":{"components":[{"type":"fixed"},{"type":"energidataservice.spot","area":"DK2"}]}})");
    CHECK(spotArea(cfg) == "DK2");
    CHECK(spotArea(nlohmann::json::object()).empty());
    CHECK_THAT(prodexUrl("DK2"), ContainsSubstring("ElectricityProdex5MinRealtime?start=now-P8D"));
    CHECK_THAT(prodexUrl("DK2"), ContainsSubstring("DK2"));
    CHECK_THAT(co2Url("DK1"), ContainsSubstring("CO2Emis?start=now-P8D"));
}

TEST_CASE("Power mix: the constant night floor of the solar estimate is taken off") {
    nlohmann::json recs = nlohmann::json::array();
    // 24.7 MW all night, 120 MW at noon (one record per hour is enough).
    for (int h = 0; h < 24; ++h) {
        const double solar = h == 12 ? 120.0 : 24.7;
        recs.push_back({{"Minutes5UTC", std::format("2026-11-22T{:02}:00:00", h)}, {"SolarPower", solar}, {"OffshoreWindPower", 1000}});
    }
    const auto mix = parsePowerMix(nlohmann::json{{"records", recs}}, nlohmann::json::object());
    REQUIRE(mix.size() == 24);
    CHECK(mix[2].solar == Catch::Approx(0));
    CHECK(mix[12].solar == Catch::Approx(95.3));
}
