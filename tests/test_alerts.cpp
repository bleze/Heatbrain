#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "core/alerts.hpp"
#include "notify/notifier.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}

Instant at(int hour, int minute = 0) { return tz().toInstant(local_days{2026y / 11 / 23} + hours{hour} + minutes{minute}); }

Problem plugDown(minutes debounce = minutes{10}) {
    return {"plug:bedroom:unreachable", Severity::Warning, "Plug 'bedroom' is unreachable", debounce, std::nullopt};
}

struct Recorder {
    std::vector<Alert> got;
    bool accept = true;
    AlertManager::Deliver fn() {
        return [this](const Alert& a) {
            if (accept) got.push_back(a);
            return accept;
        };
    }
};

}  // namespace

TEST_CASE("Problems alert only after the debounce, then resolve") {
    AlertManager m({}, tz());
    Recorder r;
    m.update(at(12, 0), {plugDown()}, r.fn());
    m.update(at(12, 5), {plugDown()}, r.fn());
    CHECK(r.got.empty());  // a missed poll or two is not worth a notification

    m.update(at(12, 10), {plugDown()}, r.fn());
    REQUIRE(r.got.size() == 1);
    CHECK(r.got[0].kind == Alert::Kind::Raised);
    CHECK(r.got[0].since == at(12, 0));

    m.update(at(12, 15), {plugDown()}, r.fn());
    CHECK(r.got.size() == 1);  // no repeat every tick

    m.update(at(12, 20), {}, r.fn());
    REQUIRE(r.got.size() == 2);
    CHECK(r.got[1].kind == Alert::Kind::Resolved);
}

TEST_CASE("Blips shorter than the debounce produce no alert and no 'resolved'") {
    AlertManager m({}, tz());
    Recorder r;
    m.update(at(12, 0), {plugDown()}, r.fn());
    m.update(at(12, 5), {}, r.fn());
    CHECK(r.got.empty());
}

TEST_CASE("Ongoing problems are repeated at the policy interval") {
    AlertManager m(AlertPolicy{hours{6}, std::nullopt}, tz());
    Recorder r;
    m.update(at(8), {plugDown(minutes{0})}, r.fn());
    m.update(at(13, 55), {plugDown(minutes{0})}, r.fn());
    m.update(at(14), {plugDown(minutes{0})}, r.fn());
    REQUIRE(r.got.size() == 2);
    CHECK(r.got[1].kind == Alert::Kind::Reminder);
}

TEST_CASE("Quiet hours hold back non-critical alerts but not critical ones") {
    AlertManager m(AlertPolicy{hours{6}, DayWindow{23 * 60, 7 * 60}}, tz());
    Recorder r;
    const Problem auth{"source:netatmo:auth", Severity::Critical, "re-login needed", minutes{0}, std::nullopt};
    m.update(at(2), {plugDown(minutes{0}), auth}, r.fn());
    REQUIRE(r.got.size() == 1);
    CHECK(r.got[0].key == "source:netatmo:auth");

    m.update(at(7), {plugDown(minutes{0}), auth}, r.fn());  // morning: the plug alert goes out
    REQUIRE(r.got.size() == 2);
    CHECK(r.got[1].key == "plug:bedroom:unreachable");
    CHECK(r.got[1].since == at(2));
}

TEST_CASE("Failed deliveries are retried on the next tick") {
    AlertManager m({}, tz());
    Recorder r;
    r.accept = false;
    m.update(at(12), {plugDown(minutes{0})}, r.fn());
    r.accept = true;
    m.update(at(12, 5), {plugDown(minutes{0})}, r.fn());
    REQUIRE(r.got.size() == 1);
    CHECK(r.got[0].kind == Alert::Kind::Raised);
}

TEST_CASE("Active problems are listed for the UI") {
    AlertManager m({}, tz());
    Recorder r;
    m.update(at(12), {plugDown()}, r.fn());
    REQUIRE(m.activeProblems().size() == 1);  // listed even before alerting
}

TEST_CASE("Discord payload") {
    const Alert a{Alert::Kind::Raised, "source:netatmo:auth", Severity::Critical, "Netatmo needs a new token", at(12)};
    const auto j = discordPayload(a);
    CHECK(j["username"] == "heatbrain");
    CHECK(j["embeds"][0]["description"] == "Netatmo needs a new token");
    CHECK(j["embeds"][0]["color"] == 0xE74C3C);

    test::FakeHttp http;
    http.requestHandler = [](const HttpRequest& req) {
        CHECK(req.method == "POST");
        CHECK(req.contentType == "application/json");
        return HttpResponse{204, ""};
    };
    DiscordNotifier d(http, "https://discord.com/api/webhooks/1/abc");
    d.send(a);
    CHECK(http.calls == 1);

    http.requestHandler = [](const HttpRequest&) { return HttpResponse{429, R"({"retry_after":1.5})"}; };
    CHECK_THROWS_AS(d.send(a), HttpError);
}
