#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "app/meter_sync.hpp"
#include "providers/eloverblik.hpp"
#include "storage/history.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

// Shape of a gettimeseries reply (Eloverblik customer API), two quarters.
const char* kSeries = R"({"result":[{"MyEnergyData_MarketDocument":{"TimeSeries":[{"mRID":"571313100000000000",
  "Period":[{"resolution":"PT15M","timeInterval":{"start":"2026-11-20T23:00:00Z","end":"2026-11-21T23:00:00Z"},
  "Point":[{"position":"1","out_Quantity.quantity":"0.125","out_Quantity.quality":"A04"},
           {"position":"2","out_Quantity.quantity":"0.5","out_Quantity.quality":"A04"}]}]}]},
  "success":true,"errorCode":10000,"errorText":"NoError"}]})";

const char* kPoints = R"({"result":[{"meteringPointId":"571313100000000000","typeOfMP":"E17","streetName":"Testvej",
  "buildingNumber":"1","postcode":"7000","cityName":"Fredericia","balanceSupplierName":"EWII"}]})";

}  // namespace

TEST_CASE("Eloverblik time series: quarters with their start times") {
    const auto r = parseEloverblikTimeSeries(json::parse(kSeries));
    REQUIRE(r.size() == 2);
    const Instant start = sys_days{2026y / 11 / 20} + 23h;
    CHECK(r[0] == MeterReading{start, minutes{15}, 0.125});
    CHECK(r[1] == MeterReading{start + minutes{15}, minutes{15}, 0.5});
    CHECK_THROWS_WITH(parseEloverblikTimeSeries(json::parse(R"({"result":[{"success":false,"errorText":"WrongNumberOfDays"}]})")),
                      ContainsSubstring("WrongNumberOfDays"));
}

TEST_CASE("Eloverblik: the access token is reused, and a refused token says what to do") {
    test::TempDir dir;
    test::FakeHttp http;
    int tokenCalls = 0;
    std::string lastAuth;
    http.requestHandler = [&](const HttpRequest& req) -> HttpResponse {
        for (const auto& [k, v] : req.headers) if (k == "Authorization") lastAuth = v;
        if (req.url.ends_with("api/token")) { ++tokenCalls; return {200, R"({"result":"access-1"})"}; }
        if (req.url.find("meteringpoints") != std::string::npos) return {200, kPoints};
        if (req.url.find("gettimeseries/2026-11-20/2026-11-22/Actual") != std::string::npos) {
            CHECK(json::parse(req.body)["meteringPoints"]["meteringPoint"][0] == "571313100000000000");
            return {200, kSeries};
        }
        return {404, ""};
    };
    Instant now = sys_days{2026y / 11 / 22} + 10h;
    EloverblikClient c(http, dir.path() / "token.json", "refresh-token-xyz", [&] { return now; }, "https://x/");
    const auto points = c.meteringPoints();
    REQUIRE(points.size() == 1);
    CHECK(points[0].type == "E17");
    CHECK(points[0].address == "Testvej 1, 7000 Fredericia");
    CHECK(c.readings(points[0].id, "2026-11-20", "2026-11-22").size() == 2);
    CHECK(tokenCalls == 1);  // one login for both calls
    CHECK(lastAuth == "Bearer access-1");

    // A new client (restart) within 20 hours reuses the saved access token.
    EloverblikClient again(http, dir.path() / "token.json", "refresh-token-xyz", [&] { return now; }, "https://x/");
    again.meteringPoints();
    CHECK(tokenCalls == 1);

    // A refused refresh token: a clear message.
    test::FakeHttp refuses;
    refuses.handler = [](const std::string&) { return HttpResponse{401, ""}; };
    EloverblikClient bad(refuses, dir.path() / "token2.json", "expired", [&] { return now; }, "https://x/");
    CHECK_THROWS_WITH(bad.meteringPoints(), ContainsSubstring("create a new one on eloverblik.dk"));
}

TEST_CASE("Usage: the radiators' share of the measured use, per meter interval") {
    test::TempDir dir;
    History h(dir.path() / "history.db");
    const Instant t0 = sys_days{2026y / 11 / 21} + 10h;
    h.recordMeter({{t0, minutes{60}, 1.2}, {t0 + hours{1}, minutes{60}, 0.3}});
    // A live 1000 W plug on from 10:00 to 10:30 (decisions every 5 minutes), then off.
    for (int m = 0; m < 60; m += 5) {
        const bool on = m < 30;
        h.recordDecision({t0 + minutes{m}, "bedroom", on, false, on ? "BELOW_TARGET" : "TARGET_REACHED", "",
                          json::object(), json::array({{{"id", "bedroom"}, {"mode", "live"}, {"state", on}}})});
    }
    const json cfg = json::parse(R"({"plugs":[{"id":"bedroom","watts":1000}],"rooms":[{"id":"bedroom"}]})");
    const json u = usageReport(cfg, h, t0, t0 + hours{2});
    REQUIRE(u["meter"].size() == 2);
    CHECK(u["meter"][0][2] == 1.2);
    CHECK(u["heating"][0][2].get<double>() == Catch::Approx(0.5));  // 30 min x 1 kW
    CHECK(u["heating"][1][2].get<double>() == Catch::Approx(0.0));
}
