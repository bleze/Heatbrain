#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "sensors/velux.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;

namespace {

const Instant kNow = sys_days{2026y / 11 / 23} + 18h;

json homesData() {
    return json::parse(R"({"body":{"homes":[{"id":"h1","name":"Granvaenget",
      "rooms":[{"id":"r1","name":"Pizzeria"},{"id":"r2","name":"Hall"}],
      "modules":[{"id":"g1","type":"NXG"},{"id":"d1","type":"NXD","room_id":"r1"},{"id":"w1","type":"NXO","room_id":"r1"},
                 {"id":"w2","type":"NXO","room_id":"r2"}]}]}})");
}

json homeStatus(bool switchReachable = true, bool gatewayReachable = true) {
    json j = json::parse(R"({"body":{"home":{"id":"h1",
      "rooms":[{"id":"r1","temperature":103,"humidity":61,"co2":540},{"id":"r2"}],
      "modules":[{"id":"g1","type":"NXG"},{"id":"d1","type":"NXD","battery_percent":80},{"id":"w1","type":"NXO"},{"id":"w2","type":"NXO"}]}}})");
    j["body"]["home"]["modules"][0]["reachable"] = gatewayReachable;
    j["body"]["home"]["modules"][1]["reachable"] = switchReachable;
    j["body"]["home"]["modules"][1]["last_seen"] = duration_cast<seconds>((kNow - minutes{3}).time_since_epoch()).count();
    return j;
}

}  // namespace

TEST_CASE("VELUX: one reading per room with climate data, temperature in tenths") {
    const auto r = velux::parseHome(homesData(), homeStatus(), "h1", kNow);
    REQUIRE(r.size() == 1);  // the hall has only a window: no reading
    CHECK(r[0].sensorId == "r1");
    CHECK(r[0].name == "Pizzeria");
    CHECK(r[0].station == "Granvaenget");
    CHECK(r[0].kind == "indoor");
    CHECK(*r[0].temperature == Catch::Approx(10.3));
    CHECK(*r[0].humidity == 61);
    CHECK(*r[0].co2 == 540);
    CHECK(*r[0].batteryPercent == 80);
    CHECK(*r[0].measuredAt == kNow - minutes{3});  // the wall switch's last contact
    CHECK(r[0].reachable);
}

TEST_CASE("VELUX: an unreachable wall switch or gateway makes the room's reading unreachable") {
    CHECK_FALSE(velux::parseHome(homesData(), homeStatus(false, true), "h1", kNow)[0].reachable);
    CHECK_FALSE(velux::parseHome(homesData(), homeStatus(true, false), "h1", kNow)[0].reachable);
}

TEST_CASE("VELUX: login keeps only tokens; reads refresh an expired token and retry") {
    test::TempDir dir;
    test::FakeHttp http;
    Instant now = kNow;
    int tokenCalls = 0;
    http.requestHandler = [&](const HttpRequest& req) -> HttpResponse {
        if (req.url.ends_with("/oauth2/token")) {
            ++tokenCalls;
            if (req.body.find("grant_type=password") != std::string::npos) {
                if (req.body.find("password=right") == std::string::npos) return {400, R"({"error":"invalid_grant"})"};
                return {200, R"({"access_token":"a1","refresh_token":"r1","expires_in":10800})"};
            }
            CHECK(req.body.find("refresh_token=r1") != std::string::npos);
            return {200, R"({"access_token":"a2","refresh_token":"r2","expires_in":10800})"};
        }
        if (req.url.ends_with("/api/homesdata")) return {200, homesData().dump()};
        if (req.url.ends_with("/syncapi/v1/homestatus")) {
            const bool fresh = std::ranges::any_of(req.headers, [](const auto& h) { return h.second == "Bearer a2"; });
            if (!fresh) return {403, R"({"error":{"code":3,"message":"Access token expired"}})"};
            CHECK(json::parse(req.body)["home_id"] == "h1");
            return {200, homeStatus().dump()};
        }
        return {404, "{}"};
    };
    const auto file = dir.path() / "velux-tokens.json";

    CHECK_THROWS_AS(velux::login(http, "me@example.com", "wrong", file, now), SourceAuthError);
    CHECK_FALSE(std::filesystem::exists(file));

    velux::login(http, "me@example.com", "right", file, now);
    std::ifstream in(file);
    const std::string saved((std::istreambuf_iterator<char>(in)), {});
    CHECK(saved.find("right") == std::string::npos);  // the password is not kept
    in.close();

    velux::Source src("velux", http, velux::Auth(http, file, [&] { return now; }), [&] { return now; });
    const auto r = src.read();  // homestatus says "expired" -> refresh -> retry
    REQUIRE(r.size() == 1);
    CHECK(r[0].name == "Pizzeria");
    CHECK(tokenCalls == 3);
}

TEST_CASE("VELUX: not connected is a login problem, not a network error") {
    test::TempDir dir;
    test::FakeHttp http;
    http.handler = [](const std::string&) { return HttpResponse{500, "{}"}; };
    velux::Source src("velux", http, velux::Auth(http, dir.path() / "velux-tokens.json", [] { return kNow; }), [] { return kNow; });
    CHECK_THROWS_AS(src.read(), SourceAuthError);
}
