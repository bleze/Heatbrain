#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

#include "app/controller.hpp"
#include "app/rooms.hpp"
#include "plugs/simulated.hpp"
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

TEST_CASE("A plug that misses the firmware question is 'unknown', not 'firmware changed'") {
    Fixture f;
    f.controller->setDataDir(f.dir.path());
    saveFirmwareRecord(f.dir.path(), "bedroom", FirmwareRecord{"simulated", true, "", ""});
    f.plug("bedroom").sw->provision(false);
    auto& sim = dynamic_cast<SimulatedSwitch&>(*f.plug("bedroom").sw);
    sim.setFirmware("");
    const auto r = f.controller->tick(f.now, true);
    CHECK_FALSE(f.plugOn("bedroom"));  // not verified this step: not switched
    const auto p = std::ranges::find_if(r.problems, [](const Problem& x) { return x.key == "plug:bedroom:failsafe"; });
    REQUIRE(p != r.problems.end());
    CHECK_THAT(p->message, Catch::Matchers::ContainsSubstring("firmware version could not be read"));
    CHECK_THAT(p->message, !Catch::Matchers::ContainsSubstring("changed to"));
    CHECK(p->debounce == minutes{15});  // one missed request is no alert

    sim.setFirmware("simulated");
    f.now += minutes{5};
    f.controller->tick(f.now, true);
    CHECK(f.plugOn("bedroom"));
}

TEST_CASE("The price table covers all of today, so 'cheapest hours of the day' sees the past hours too") {
    Fixture f;
    f.controller->tick(f.now, true);  // 12:00 local
    const auto& slots = f.controller->prices().slots;
    REQUIRE_FALSE(slots.empty());
    CHECK(slots.front().start == startOfLocalDay(f.tz, f.now));
    CHECK(slots.back().start >= startOfLocalDay(f.tz, f.now + hours{24}) + hours{23});
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

TEST_CASE("A module restarting does not put the room in limp mode straight away") {
    Fixture f;
    f.controller->tick(f.now, true);  // bedroom 18.0, measured 2 min ago
    // The module goes offline: Netatmo reports it unreachable and without dashboard data.
    f.sensors->readings[0].reachable = false;
    f.sensors->readings[0].temperature.reset();
    f.sensors->readings[0].measuredAt.reset();
    f.now += minutes{5};
    auto r = f.controller->tick(f.now, true);
    CHECK(r.rooms[0].decision.reason != Reason::StaleDataLimp);  // last reading (7 min old) still used
    CHECK(r.rooms[0].decision.inputs.temperature == 18.0);
    CHECK(std::ranges::any_of(r.problems, [](const Problem& p) { return p.key.ends_with(":offline"); }));
    CHECK_FALSE(std::ranges::any_of(r.problems, [](const Problem& p) { return p.key == "room:bedroom:limp"; }));

    f.now += minutes{30};  // still offline: the last reading is now too old
    r = f.controller->tick(f.now, true);
    CHECK(r.rooms[0].decision.reason == Reason::StaleDataLimp);
}

TEST_CASE("Wi-Fi off period: schedule loaded beforehand, followed quietly, handed back afterwards") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    auto* sim = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    REQUIRE(sim);
    f.controller->setDataDir(f.dir.path());
    // now = 12:00 local; Wi-Fi off 12:05-14:00, schedules go on 10 minutes before.
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});

    auto r = f.controller->tick(f.now, true);
    REQUIRE(sim->schedule());  // loaded onto the live, verified plug
    CHECK(f.controller->offlineInfo(f.now)->armedPlugs == std::vector<std::string>{"bedroom"});
    CHECK(reasonCode(r.rooms[0].decision.reason) == "OFFLINE_SCHEDULE");

    // In the period: nothing reachable, no readings - and nothing to report.
    f.now += minutes{40};
    sim->setReachable(false);
    f.sensors->fail = true;
    r = f.controller->tick(f.now, true);
    CHECK(reasonCode(r.rooms[0].decision.reason) == "OFFLINE_SCHEDULE");
    CHECK(r.rooms[0].plugs[0].value("schedule", false));
    for (const auto& p : r.problems) {
        INFO(p.key);
        CHECK_FALSE(p.key.starts_with("plug:bedroom"));
        CHECK_FALSE(p.key.starts_with("room:bedroom:limp"));
    }

    // Afterwards: back online, schedule removed, normal control again.
    f.now += hours{2};
    sim->setReachable(true);
    f.sensors->fail = false;
    f.setTemps(18.0, 14.0);
    r = f.controller->tick(f.now, true);
    CHECK_FALSE(sim->schedule());
    CHECK(f.controller->offlineInfo(f.now)->armedPlugs.empty());
    CHECK(reasonCode(r.rooms[0].decision.reason) != "OFFLINE_SCHEDULE");
}

TEST_CASE("Wi-Fi off period: a plug that can't take the schedule is reported") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    auto* sim = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});
    sim->setReachable(false);  // Wi-Fi went off early
    const auto r = f.controller->tick(f.now, true);
    CHECK(std::ranges::any_of(r.problems, [](const Problem& p) { return p.key == "plug:bedroom:night-schedule"; }));
    CHECK(f.controller->offlineInfo(f.now)->errors.size() == 1);
}

TEST_CASE("Wi-Fi off period: a restart during the night keeps following the loaded schedule") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    f.controller->setDataDir(f.dir.path());
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});
    f.controller->tick(f.now, true);  // loads and saves the schedule

    // A new controller (heatbrain restarted) in the middle of the period.
    Controller again(f.tz, f.prices, f.sources, f.plugs, parseRooms(f.cfg, f.plugs), &f.overrides, &f.history);
    again.setDataDir(f.dir.path());
    again.setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});
    const auto info = again.offlineInfo(f.now + minutes{30});
    REQUIRE(info);
    CHECK(info->armedPlugs == std::vector<std::string>{"bedroom"});
    const auto r = again.tick(f.now + minutes{30}, true);
    CHECK(reasonCode(r.rooms[0].decision.reason) == "OFFLINE_SCHEDULE");
}

TEST_CASE("Wi-Fi off period: back early - heatbrain takes over as soon as readings arrive") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    auto* sim = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});
    f.controller->tick(f.now, true);
    REQUIRE(sim->schedule());
    // 12:40: the Wi-Fi is already back and the sensors report again.
    f.now += minutes{40};
    f.setTemps(18.0, 14.0);
    const auto r = f.controller->tick(f.now, true);
    CHECK_FALSE(sim->schedule());
    CHECK(reasonCode(r.rooms[0].decision.reason) != "OFFLINE_SCHEDULE");
}

TEST_CASE("Wi-Fi off period: back late - plugs keep their schedule, one quiet note") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    auto* sim = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});
    f.controller->tick(f.now, true);
    sim->setReachable(false);
    f.sensors->fail = true;
    // 14:40 local: 40 minutes past the planned end, still nothing reachable.
    f.now += hours{2} + minutes{40};
    const auto r = f.controller->tick(f.now, true);
    CHECK(sim->schedule());  // still loaded on the device (it could not be removed)
    CHECK(reasonCode(r.rooms[0].decision.reason) == "OFFLINE_SCHEDULE");
    CHECK(std::ranges::any_of(r.problems, [](const Problem& p) { return p.key == "wifi:late"; }));
    for (const auto& p : r.problems) CHECK_FALSE(p.key.starts_with("plug:bedroom:unreachable"));
    CHECK(f.controller->offlineInfo(f.now)->late);
}

TEST_CASE("Wi-Fi off period: the plugs switch their own Wi-Fi off too, dry-run ones included") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    f.plug("spare").sw->provision(false);  // dry-run, with the fail-safe
    auto* live = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    auto* spare = dynamic_cast<SimulatedSwitch*>(f.plug("spare").sw.get());
    f.controller->setDataDir(f.dir.path());
    f.controller->setOfflineWindow(DayWindow{12 * 60 + 5, 14 * 60}, minutes{10});

    f.controller->tick(f.now, true);
    REQUIRE(live->radioOff());
    CHECK(live->radioOff()->start == f.now + minutes{6});              // 12:06
    CHECK(live->radioOff()->end == f.now + hours{2} - minutes{2});     // 13:58
    REQUIRE(spare->radioOff());
    CHECK_FALSE(spare->schedule());                                    // dry-run: no schedule
    CHECK(f.controller->offlineInfo(f.now)->radioOff == std::vector<std::string>{"bedroom", "spare"});
    CHECK(f.controller->offlineInfo(f.now)->armedPlugs == std::vector<std::string>{"bedroom"});

    // Morning: both handed back.
    f.now += hours{2} + minutes{5};
    f.setTemps(18.0, 14.0);
    f.controller->tick(f.now, true);
    CHECK_FALSE(live->radioOff());
    CHECK_FALSE(spare->radioOff());
    CHECK(f.controller->offlineInfo(f.now)->radioOff.empty());
}

TEST_CASE("Wi-Fi off period: the plugs' own Wi-Fi can be left on") {
    Fixture f;
    f.plug("bedroom").sw->provision(false);
    auto* live = dynamic_cast<SimulatedSwitch*>(f.plug("bedroom").sw.get());
    WifiOffPlan plan{DayWindow{12 * 60 + 5, 14 * 60}, std::nullopt};
    plan.plugRadio = false;
    f.controller->setOfflineWindow(plan, minutes{10});
    f.controller->tick(f.now, true);
    REQUIRE(live->schedule());
    CHECK_FALSE(live->radioOff());
}

TEST_CASE("Dry-run plugs with a fail-safe get their watchdog fed, never switched") {
    Fixture f;
    auto* spare = dynamic_cast<SimulatedSwitch*>(f.plug("spare").sw.get());  // dry-run
    f.controller->tick(f.now, true);
    CHECK(spare->keepAlives() == 0);  // no fail-safe: not touched
    f.plug("spare").sw->provision(false);
    f.now += minutes{31};             // the fail-safe check is repeated every 30 minutes
    const auto r = f.controller->tick(f.now, true);
    CHECK(spare->keepAlives() == 1);
    CHECK_FALSE(spare->keptUntil());
    CHECK_FALSE(f.plugOn("spare"));
    CHECK(r.rooms[1].plugs[0].value("watchdog", false));
}

TEST_CASE("A restart during a Netatmo outage keeps using the last readings for a while") {
    Fixture f;
    f.controller->setDataDir(f.dir.path());
    f.controller->tick(f.now, true);  // a good reading, saved

    // Restart five minutes later; Netatmo is down.
    f.sensors->fail = true;
    Controller again(f.tz, f.prices, f.sources, f.plugs, parseRooms(f.cfg, f.plugs), &f.overrides, &f.history);
    again.setDataDir(f.dir.path());
    const auto r = again.tick(f.now + minutes{5}, true);
    CHECK(reasonCode(r.rooms[0].decision.reason) != "STALE_DATA_LIMP");
    for (const auto& p : r.problems) CHECK_FALSE(p.key.ends_with(":sensor"));  // not "sensor not found"
}

TEST_CASE("Limp alerts wait 15 minutes") {
    Fixture f;
    f.sensors->fail = true;
    const auto r = f.controller->tick(f.now, true);
    const auto limp = std::ranges::find_if(r.problems, [](const Problem& p) { return p.key == "room:bedroom:limp"; });
    REQUIRE(limp != r.problems.end());
    CHECK(limp->debounce == minutes{15});
}

TEST_CASE("A booking can be changed in place; overlaps are refused, a moved start warms up afresh") {
    test::TempDir dir;
    const BookingStore store{dir.path() / "bookings.json"};
    const Instant base = floor<seconds>(system_clock::now()) + hours{24};
    const std::string a = store.add("pizzeria", Booking{22, base, base + hours{6}});
    store.add("pizzeria", Booking{22, base + hours{24}, base + hours{30}});
    store.markWarming(a, base - hours{3});

    // Same start, other end and temperature: still warming.
    REQUIRE(store.update(a, "pizzeria", Booking{21, base, base + hours{8}}));
    auto all = store.load();
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == a);
    CHECK(all[0].booking.target == 21);
    CHECK(all[0].booking.until == base + hours{8});
    CHECK(all[0].warmingSince);

    // Into the other booking: refused, nothing changed.
    CHECK_THROWS_AS(store.update(a, "pizzeria", Booking{21, base + hours{20}, base + hours{26}}), std::invalid_argument);
    CHECK(store.load()[0].booking.from == base);

    // Moved: works out its warm-up afresh. Another room: no overlap with this room's bookings.
    REQUIRE(store.update(a, "bedroom", Booking{20, base + hours{25}, base + hours{27}}));
    all = store.load();
    const auto moved = std::ranges::find_if(all, [&](const StoredBooking& s) { return s.id == a; });
    REQUIRE(moved != all.end());
    CHECK(moved->room == "bedroom");
    CHECK_FALSE(moved->warmingSince);
    CHECK_FALSE(store.update("nope", "bedroom", Booking{20, base, base + hours{1}}));
}

TEST_CASE("Sensor days: each new reading counts once in its day; filled once from stored readings") {
    test::TempDir dir;
    History h{dir.path() / "history.db"};
    const Instant day0{seconds{1'795'000'000}};
    const Instant d = day0 - (day0.time_since_epoch() % days{1});
    h.recordTemperature({d + hours{1}, "netatmo:a", "", 18.0, 50.0, d});
    h.recordTemperature({d + hours{2}, "netatmo:a", "", 20.0, 60.0, d});
    h.recordTemperature({d + hours{2}, "netatmo:a", "", 20.0, 60.0, d});  // the same reading again: ignored
    auto days = h.sensorDays("netatmo:a", d, d + std::chrono::days{1});
    REQUIRE(days.size() == 1);
    CHECK(days[0].readings == 2);
    CHECK(days[0].minTemp == 18.0);
    CHECK(days[0].maxTemp == 20.0);
    CHECK(days[0].avgTemp == 19.0);
    REQUIRE(days[0].avgHumidity);
    CHECK(*days[0].avgHumidity == 55.0);

    // An older database: readings without days, summarised once.
    History old{dir.path() / "old.db"};
    old.recordTemperature({d + hours{1}, "netatmo:b", "", 10.0, std::nullopt});
    old.recordTemperature({d + hours{30}, "netatmo:b", "", 12.0, std::nullopt});
    old.fillSensorDays([&](Instant t) { return t - (t.time_since_epoch() % std::chrono::days{1}); });
    old.fillSensorDays([&](Instant t) { return t - (t.time_since_epoch() % std::chrono::days{1}); });  // no double count
    days = old.sensorDays("netatmo:b", d, d + std::chrono::days{3});
    REQUIRE(days.size() == 2);
    CHECK(days[0].readings == 1);
    CHECK_FALSE(days[0].avgHumidity);
}
