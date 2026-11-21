#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

#include "app/controller.hpp"
#include "app/rooms.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

class FakeSensors final : public TemperatureSource {
public:
    std::vector<SensorReading> readings;
    bool fail = false;
    const std::string& id() const override { return id_; }
    std::vector<SensorReading> read() override {
        if (fail) throw std::runtime_error("cloud down");
        return readings;
    }

private:
    std::string id_ = "netatmo";
};

const char* kConfig = R"({
  "price": { "components": [ { "type": "fixed", "name": "Flat", "amount": 2.0 } ] },
  "plugs": [
    { "id": "bedroom", "type": "simulated", "mode": "live" },
    { "id": "spare",   "type": "simulated", "mode": "dryrun" }
  ],
  "rooms": [
    { "id": "bedroom", "plugs": ["bedroom"], "sensor": "netatmo:Bedroom", "minTemp": 16, "maxTemp": 22,
      "schedule": [ { "from": "00:00", "to": "24:00", "target": 19 } ] },
    { "id": "spare", "plugs": ["spare"], "sensor": "netatmo:03:00:00:00:00:09", "minTemp": 10, "maxTemp": 22,
      "schedule": [ { "from": "00:00", "to": "24:00", "target": 15 } ] }
  ]
})";

struct Fixture {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache{http, dir.path() / "cache"};
    TimeZone tz{"Europe/Copenhagen"};
    Instant now = sys_days{2026y / 11 / 23} + 11h;
    ProviderContext ctx{cache, tz, "DKK", [this] { return now; }};
    json cfg = json::parse(kConfig);
    PriceModel prices{cfg.at("price"), ctx};
    std::vector<std::unique_ptr<TemperatureSource>> sources;
    FakeSensors* sensors = nullptr;
    std::vector<Plug> plugs = loadPlugs(cfg, http);
    Overrides overrides{dir.path() / "overrides.json"};
    History history{dir.path() / "history.db"};
    std::unique_ptr<Controller> controller;

    Fixture() {
        auto s = std::make_unique<FakeSensors>();
        sensors = s.get();
        sources.push_back(std::move(s));
        setTemps(18.0, 14.0);
        controller = std::make_unique<Controller>(tz, prices, sources, plugs, parseRooms(cfg, plugs), &overrides,
                                                  &history);
    }
    void setTemps(double bedroom, double spare) {
        sensors->readings = {
            {"03:00:00:00:00:01", "Bedroom", "Home", "indoor", bedroom, 50.0, std::nullopt, now - minutes{2}, true, 80},
            {"03:00:00:00:00:09", "Spare", "Home", "indoor", spare, 50.0, std::nullopt, now - minutes{2}, true, 80}};
    }
    bool plugOn(const std::string& id) {
        return *std::ranges::find_if(plugs, [&](const Plug& p) { return p.id == id; })->sw->status().on;
    }
    Plug& plug(const std::string& id) { return *std::ranges::find_if(plugs, [&](const Plug& p) { return p.id == id; }); }
};

}  // namespace

TEST_CASE("Live plugs are never switched without a verified fail-safe") {
    Fixture f;
    const auto r = f.controller->tick(f.now, true);
    REQUIRE(r.rooms.size() == 2);
    CHECK(r.rooms[0].decision.on);  // 18.0 < 19: wants heat
    CHECK_FALSE(f.plugOn("bedroom"));
    CHECK(r.rooms[0].plugs[0].contains("error"));
    CHECK(r.problems.front().key == "plug:bedroom:failsafe");
    CHECK(r.problems.front().severity == Severity::Critical);

    f.plug("bedroom").sw->provision(false);
    f.now += minutes{5};
    f.controller->tick(f.now, true);
    CHECK(f.plugOn("bedroom"));
}

TEST_CASE("Dry-run plugs are decided and logged but never touched") {
    Fixture f;
    const auto r = f.controller->tick(f.now, true);
    CHECK(r.rooms[1].decision.on);  // spare 14 < 15
    CHECK_FALSE(f.plugOn("spare"));
    CHECK(r.rooms[1].plugs[0]["mode"] == "dryrun");
    CHECK_FALSE(r.rooms[1].plugs[0].contains("state"));
}

TEST_CASE("Decisions and temperatures are recorded; 'changed' marks switch events") {
    Fixture f;
    f.controller->tick(f.now, true);
    f.now += minutes{5};
    f.controller->tick(f.now, true);  // still ON: not a change
    f.setTemps(19.5, 14.0);
    f.now += minutes{5};
    f.controller->tick(f.now, true);  // target reached: OFF

    const auto all = f.history.decisions("bedroom", f.now - hours{1}, f.now + seconds{1}, false);
    REQUIRE(all.size() == 3);
    CHECK(all[0].changed);
    CHECK_FALSE(all[1].changed);
    CHECK(all[2].changed);
    CHECK(all[2].reason == "TARGET_REACHED");
    CHECK(all[2].inputs["temperature"] == 19.5);

    CHECK(f.history.decisions("bedroom", f.now - hours{1}, f.now + seconds{1}, true).size() == 2);
    // Ticks 1 and 2 saw the same measurement (same timestamp): stored once.
    const auto temps = f.history.temperatures("bedroom", f.now - hours{1}, f.now + seconds{1});
    CHECK(temps.size() == 2);
    CHECK(f.history.prices(f.now - hours{1}, f.now + hours{1}).size() > 0);
}

TEST_CASE("plan (apply = false) writes nothing and switches nothing") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    f.controller->tick(f.now, false);
    CHECK_FALSE(f.plugOn("bedroom"));
    CHECK_FALSE(f.history.lastDecision("bedroom"));
}

TEST_CASE("Sensor outage: readings age out and the room goes to limp mode") {
    Fixture f;
    f.controller->tick(f.now, true);
    f.sensors->fail = true;
    f.now += minutes{40};
    const auto r = f.controller->tick(f.now, true);
    CHECK(r.rooms[0].decision.reason == Reason::StaleDataLimp);
    CHECK(std::ranges::any_of(r.problems, [](const Problem& p) {
        return p.key == "source:netatmo:error" && p.message.find("cloud down") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(r.problems, [](const Problem& p) { return p.key == "room:bedroom:limp"; }));
}

TEST_CASE("A restarted controller continues from the last recorded decision") {
    Fixture f;
    f.controller->tick(f.now, true);
    Controller restarted(f.tz, f.prices, f.sources, f.plugs, parseRooms(f.cfg, f.plugs), &f.overrides, &f.history);
    f.now += minutes{5};
    const auto r = restarted.tick(f.now, true);
    CHECK_FALSE(r.rooms[0].changed);
}

TEST_CASE("Room config validation") {
    test::FakeHttp http;
    const auto plugs = loadPlugs(json::parse(R"({"plugs":[{"id":"a","type":"simulated"},{"id":"b","type":"simulated"}]})"), http);
    auto rooms = [&](const char* text) { return parseRooms(json::parse(text), plugs); };

    CHECK_THROWS_WITH(rooms(R"({"rooms":[{"id":"r","plugs":["x"],"priceOnly":{}}]})"), ContainsSubstring("unknown plug"));
    CHECK_THROWS_WITH(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"priceOnly":{}},{"id":"s","plugs":["a"],"priceOnly":{}}]})"),
                      ContainsSubstring("more than one room"));
    CHECK_THROWS_WITH(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"sensor":"netatmo:X",
                       "program":[{"at":"07:00","mode":"heat","target":19},{"at":"07:00","mode":"off"}]}]})"),
                      ContainsSubstring("two switch points"));
    CHECK_THROWS_WITH(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"sensor":"netatmo:X",
                       "program":[{"at":"07:00","mode":"warm"}]}]})"),
                      ContainsSubstring("heat, keep or off"));
    CHECK_THROWS_WITH(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"sensor":"netatmo:X","maxTemp":22,
                       "schedule":[{"from":"00:00","to":"24:00","target":25}]}]})"),
                      ContainsSubstring("must be between 15 and 22"));
    CHECK_NOTHROW(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"sensor":"netatmo:X",
                    "schedule":[{"from":"07:00","to":"23:00","target":19},{"from":"23:00","to":"07:00","target":16}]}]})"));
    CHECK_NOTHROW(rooms(R"({"rooms":[{"id":"r","plugs":["a"],"sensor":"netatmo:X",
                    "program":[{"at":"23:00","mode":"keep","target":15},{"at":"09:00","mode":"heat","target":19}]}]})"));
}

TEST_CASE("Old schedules with no-heat windows become an equivalent program") {
    const auto room = json::parse(R"({"id":"bedroom","plugs":["a"],"sensor":"netatmo:X",
        "schedule":[{"from":"07:00","to":"21:00","target":19},{"from":"21:00","to":"23:00","target":20},
                    {"from":"23:00","to":"07:00","target":16}],
        "noHeat":[{"from":"17:00","to":"21:00"}]})");
    const auto converted = roomWithProgram(room);
    CHECK_FALSE(converted.contains("schedule"));
    CHECK_FALSE(converted.contains("noHeat"));
    CHECK(converted["program"] == json::parse(R"([
        {"at":"07:00","mode":"heat","target":19.0},
        {"at":"17:00","mode":"off"},
        {"at":"21:00","mode":"heat","target":20.0},
        {"at":"23:00","mode":"heat","target":16.0}])"));
    CHECK(roomWithProgram(converted) == converted);  // already converted: unchanged
}
