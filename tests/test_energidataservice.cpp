#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "providers/energidataservice.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

namespace {

struct Fixture {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache{http, dir.path()};
    TimeZone tz{"Europe/Copenhagen"};
    Instant now = sys_days{2026y / 11 / 21} + 10h;
    ProviderContext ctx{cache, tz, "DKK", [this] { return now; }};
};

}  // namespace

TEST_CASE("parseDayAheadPrices converts per-MWh to per-kWh and skips nulls") {
    const auto prices = eds::parseDayAheadPrices(test::readFixture("dayahead_dk2.json"), "DKK");
    REQUIRE(prices.size() == 4);
    const Instant t0 = sys_days{2026y / 11 / 21} + 12h;
    CHECK(prices.at(t0) == Approx(1.0));
    CHECK(prices.at(t0 + 45min) == Approx(-0.5));  // negative prices happen

    const auto eur = eds::parseDayAheadPrices(test::readFixture("dayahead_dk2.json"), "eur");
    CHECK(eur.at(t0) == Approx(0.1338));
}

TEST_CASE("Rate-limit responses become errors, not empty data") {
    CHECK_THROWS(eds::parseDayAheadPrices(
        R"({ "statusCode": 429, "message": "Rate limit is exceeded." })", "DKK"));
}

TEST_CASE("SpotComponent maps 15-minute prices onto slots") {
    Fixture f;
    f.http.handler = [](const std::string&) {
        return HttpResponse{200, test::readFixture("dayahead_dk2.json")};
    };
    eds::SpotComponent spot("Spot", "DK2", "https://example.test", f.ctx);

    const Instant t0 = sys_days{2026y / 11 / 21} + 12h;
    const auto v = spot.values(slotsBetween(t0, t0 + 75min));
    REQUIRE(v.size() == 5);
    CHECK(*v[0] == Approx(1.0));
    CHECK(*v[1] == Approx(2.0));
    CHECK(*v[3] == Approx(-0.5));
    CHECK_FALSE(v[4]);  // not published

    CHECK(f.http.lastUrl.find("DayAheadPrices") != std::string::npos);
    CHECK(f.http.lastUrl.find("DK2") != std::string::npos);
}

TEST_CASE("SpotComponent refetches missing tomorrow prices only after publication time") {
    Fixture f;  // 10:00 UTC = 11:00 local, before tomorrow's prices are published
    f.http.handler = [](const std::string&) {
        return HttpResponse{200, test::readFixture("dayahead_dk2.json")};
    };
    eds::SpotComponent spot("Spot", "DK2", "https://example.test", f.ctx);
    const Instant tomorrow = sys_days{2026y / 11 / 22} + 12h;
    auto ageCache = [&](minutes age) {
        for (const auto& e : std::filesystem::directory_iterator(f.dir.path())) {
            std::filesystem::last_write_time(e.path(), std::filesystem::file_time_type::clock::now() - age);
        }
    };

    spot.values(slotsBetween(tomorrow, tomorrow + 1h));
    CHECK(f.http.calls == 1);

    ageCache(20min);  // older than the 15 min retry interval, younger than 6 h
    spot.values(slotsBetween(tomorrow, tomorrow + 1h));
    CHECK(f.http.calls == 1);  // 11:00 local: no point asking yet

    f.now = sys_days{2026y / 11 / 21} + 12h + 30min;  // 13:30 local
    spot.values(slotsBetween(tomorrow, tomorrow + 1h));
    CHECK(f.http.calls == 2);  // prices should be out now - retry

    spot.values(slotsBetween(tomorrow, tomorrow + 1h));
    CHECK(f.http.calls == 2);  // but at most every 15 minutes
}

TEST_CASE("SpotComponent fills hourly data across four slots") {
    Fixture f;
    f.http.handler = [](const std::string&) {
        return HttpResponse{200, R"({"records":[
            {"TimeUTC":"2026-11-21T12:00:00","DayAheadPriceDKK":1000.0},
            {"TimeUTC":"2026-11-21T13:00:00","DayAheadPriceDKK":2000.0}]})"};
    };
    eds::SpotComponent spot("Spot", "DK2", "https://example.test", f.ctx);
    const Instant t0 = sys_days{2026y / 11 / 21} + 12h;
    const auto v = spot.values(slotsBetween(t0, t0 + 2h + 15min));
    REQUIRE(v.size() == 9);
    CHECK(*v[3] == Approx(1.0));
    CHECK(*v[4] == Approx(2.0));
    CHECK(*v[7] == Approx(2.0));
    CHECK_FALSE(v[8]);
}

TEST_CASE("DatahubTariffComponent sums several charge codes") {
    Fixture f;
    f.http.handler = [](const std::string&) {
        return HttpResponse{200, R"({"records":[
            {"ChargeTypeCode":"40000","ValidFrom":"2026-01-01T00:00:00","ValidTo":null,"Price1":0.05},
            {"ChargeTypeCode":"41000","ValidFrom":"2026-01-01T00:00:00","ValidTo":"2027-01-01T00:00:00","Price1":0.04}
        ]})"};
    };
    eds::DatahubTariffComponent tariff("Energinet", "5790000432752", {"40000", "41000"}, "D03",
                                       "https://example.test", f.ctx);
    const Instant dec = sys_days{2026y / 12 / 31} + 22h;  // 23:00 local, last hour of 2026
    const auto v = tariff.values({dec, dec + 1h});
    CHECK(*v[0] == Approx(0.09));
    CHECK_FALSE(v[1]);  // 41000 has no row for 2027
}
