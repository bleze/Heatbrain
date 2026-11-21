#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <fstream>

#include "sensors/netatmo.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;

TEST_CASE("parseStationsData returns base station, indoor and outdoor modules") {
    const auto r = netatmo::parseStationsData(json::parse(test::readFixture("netatmo_stationsdata.json")));
    REQUIRE(r.size() == 4);  // rain gauge skipped

    CHECK(r[0].name == "Living room");
    CHECK(r[0].kind == "indoor");
    CHECK(r[0].temperature == 20.7);
    CHECK(r[0].co2 == 725);
    CHECK_FALSE(r[0].batteryPercent);  // mains powered
    CHECK(r[0].measuredAt == Instant{seconds{1795287095}});

    CHECK(r[1].name == "Carport");
    CHECK(r[1].kind == "outdoor");

    CHECK(r[2].sensorId == "03:00:00:00:00:03");
    CHECK(r[2].temperature == 18.9);
    CHECK(r[2].batteryPercent == 48);

    CHECK(r[3].name == "Basement");
    CHECK_FALSE(r[3].reachable);
    CHECK_FALSE(r[3].temperature);  // offline modules have no dashboard data
}

namespace {

// Fake Netatmo token endpoint that rotates refresh tokens like the real one.
struct FakeNetatmo {
    std::string validRefresh = "rt-config";
    int issued = 0;
    std::vector<std::string> refreshTokensUsed;
    bool expireAccessOnce = false;

    HttpResponse handle(const HttpRequest& req) {
        if (req.url.ends_with("/oauth2/token")) {
            CHECK(req.method == "POST");
            const auto start = req.body.find("refresh_token=") + 14;
            const std::string used = req.body.substr(start, req.body.find('&', start) - start);
            refreshTokensUsed.push_back(used);
            if (used != validRefresh) return {400, R"({"error":"invalid_grant"})"};
            ++issued;
            validRefresh = "rt-" + std::to_string(issued);  // rotate: the old one is now dead
            return {200, json{{"access_token", "at-" + std::to_string(issued)},
                              {"refresh_token", validRefresh},
                              {"expires_in", 10800}}
                             .dump()};
        }
        if (req.url.ends_with("/api/getstationsdata")) {
            const std::string auth = req.headers.at(0).second;
            if (expireAccessOnce) {
                expireAccessOnce = false;
                return {403, R"({"error":{"code":3,"message":"Access token expired"}})"};
            }
            CHECK(auth == "Bearer at-" + std::to_string(issued));
            return {200, test::readFixture("netatmo_stationsdata.json")};
        }
        return {404, "{}"};
    }
};

struct Fixture {
    test::TempDir dir;
    FakeNetatmo server;
    test::FakeHttp http;
    Instant now = sys_days{2026y / 11 / 21} + 12h;
    Fixture() {
        http.requestHandler = [this](const HttpRequest& r) { return server.handle(r); };
    }
    netatmo::Auth auth(const std::string& configToken = "rt-config") {
        return netatmo::Auth(http, "cid", "secret", configToken, dir.path() / "netatmo-tokens.json",
                             [this] { return now; }, "https://netatmo.test");
    }
};

}  // namespace

TEST_CASE("First use bootstraps from the config token and persists the rotated one") {
    Fixture f;
    auto auth = f.auth();
    CHECK(auth.accessToken() == "at-1");
    CHECK(f.server.refreshTokensUsed == std::vector<std::string>{"rt-config"});

    std::ifstream in(f.dir.path() / "netatmo-tokens.json");
    const json stored = json::parse(in);
    CHECK(stored["refresh_token"] == "rt-1");

    CHECK(auth.accessToken() == "at-1");  // still valid: no new refresh
    CHECK(f.server.issued == 1);
}

TEST_CASE("After a restart the stored rotated token is used, not the consumed config token") {
    Fixture f;
    f.auth().accessToken();

    auto restarted = f.auth();
    CHECK(restarted.accessToken() == "at-1");  // stored access token still valid
    CHECK(f.server.issued == 1);

    f.now += hours{3};  // access token expired
    CHECK(restarted.accessToken() == "at-2");
    CHECK(f.server.refreshTokensUsed.back() == "rt-1");
}

TEST_CASE("A new refresh token pasted into the config takes over") {
    Fixture f;
    f.auth().accessToken();
    f.server.validRefresh = "rt-new-from-user";  // user generated a fresh token
    auto auth = f.auth("rt-new-from-user");
    CHECK(auth.accessToken() == "at-2");
    CHECK(f.server.refreshTokensUsed.back() == "rt-new-from-user");
}

TEST_CASE("A rejected refresh token asks for human action") {
    Fixture f;
    f.server.validRefresh = "something-else";
    auto auth = f.auth();
    CHECK_THROWS_AS(auth.accessToken(), netatmo::AuthError);
    CHECK_THROWS_WITH(auth.accessToken(), Catch::Matchers::ContainsSubstring("dev.netatmo.com"));
}

TEST_CASE("Source refreshes once when the API reports an expired access token") {
    Fixture f;
    netatmo::Source src("netatmo", f.http, f.auth(), "https://netatmo.test");
    CHECK(src.read().size() == 4);
    f.server.expireAccessOnce = true;
    CHECK(src.read().size() == 4);
    CHECK(f.server.issued == 2);
}
