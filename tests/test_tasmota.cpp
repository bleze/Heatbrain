#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <map>

#include "plugs/plugs.hpp"
#include "plugs/tasmota.hpp"
#include "test_support.hpp"

using namespace hb;
using nlohmann::json;

namespace {

std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

// Minimal model of a Tasmota 14 device's HTTP command API, as observed on a Sonoff Basic R2.
struct FakeTasmota {
    bool power = false;
    int pulseTime = 0;
    std::string rules;
    bool ruleOn = false;
    bool timers = true;
    int powerOnState = 3;
    int ruleTimer1 = 0, ruleTimer2 = 0;
    std::vector<std::string> log;

    HttpResponse handle(const std::string& url) {
        const std::string cmnd = urlDecode(url.substr(url.find("cmnd=") + 5));
        log.push_back(cmnd);
        const auto sp = cmnd.find(' ');
        const std::string name = cmnd.substr(0, sp);
        const std::string arg = sp == std::string::npos ? "" : cmnd.substr(sp + 1);
        json r;
        if (name == "Status" && arg == "0") {
            r = {{"Status", {{"DeviceName", "Sonoff Bedroom"}}},
                 {"StatusFWR", {{"Version", "14.6.0(release-tasmota)"}, {"Hardware", "ESP8285N08"}}},
                 {"StatusSTS", {{"POWER", power ? "ON" : "OFF"}, {"Uptime", "1T02:03:04"}, {"Wifi", {{"RSSI", 78}}}}}};
        } else if (name == "Power1" || name == "Power") {
            if (arg == "On") power = true;
            if (arg == "Off") power = false;
            r = {{"POWER", power ? "ON" : "OFF"}};
        } else if (name == "PulseTime1") {
            if (!arg.empty()) pulseTime = std::stoi(arg);
            r = {{"PulseTime1", {{"Set", pulseTime}, {"Remaining", pulseTime}}}};
        } else if (name == "Rule1") {
            if (arg == "1") ruleOn = true;
            else if (arg == "0") ruleOn = false;
            else if (arg == "\"") rules.clear();
            else if (!arg.empty()) rules = arg;
            r = {{"Rule1", {{"State", ruleOn ? "ON" : "OFF"}, {"Rules", rules}}}};
        } else if (name == "Timers") {
            if (arg == "0") timers = false;
            if (arg == "1") timers = true;
            r = {{"Timers", timers ? "ON" : "OFF"}};
        } else if (name == "PowerOnState") {
            if (!arg.empty()) powerOnState = std::stoi(arg);
            r = {{"PowerOnState", powerOnState}};
        } else if (name == "RuleTimer1" || name == "RuleTimer2") {
            (name == "RuleTimer1" ? ruleTimer1 : ruleTimer2) = std::stoi(arg);
            r = {{"T1", ruleTimer1}, {"T2", ruleTimer2}};
        } else {
            r = {{"Command", "Unknown"}};
        }
        return {200, r.dump()};
    }
};

struct Fixture {
    FakeTasmota device;
    test::FakeHttp http;
    TasmotaSwitch sw{http, "192.168.1.38", "", FailsafeConfig{}};
    Fixture() {
        http.handler = [this](const std::string& url) { return device.handle(url); };
    }
};

}  // namespace

TEST_CASE("Tasmota status") {
    Fixture f;
    f.device.power = true;
    const auto s = f.sw.status();
    CHECK(s.reachable);
    CHECK(s.on == true);
    CHECK(s.firmware == "14.6.0(release-tasmota)");
    CHECK(s.rssi == 78);
}

TEST_CASE("Unreachable plug is reported, not thrown") {
    Fixture f;
    f.http.handler = [](const std::string&) -> HttpResponse { throw HttpError("timeout"); };
    const auto s = f.sw.status();
    CHECK_FALSE(s.reachable);
    CHECK(s.error == "timeout");
}

TEST_CASE("Every switch command re-arms the watchdog and cancels limp mode first") {
    Fixture f;
    f.device.ruleTimer2 = 120;  // limp phase running
    CHECK(f.sw.set(true));
    REQUIRE(f.device.log.size() == 3);
    CHECK(f.device.log[0] == "RuleTimer1 1800");
    CHECK(f.device.log[1] == "RuleTimer2 0");
    CHECK(f.device.log[2] == "Power1 On");
    CHECK(f.device.ruleTimer2 == 0);
    CHECK_FALSE(f.sw.set(false));
}

TEST_CASE("Provision installs the fail-safe; restore puts everything back") {
    Fixture f;
    CHECK_FALSE(f.sw.checkFailsafe().installed());
    CHECK(f.sw.checkFailsafe().problems.size() == 4);

    const json backup = f.sw.backup();
    f.sw.provision(false);
    CHECK(f.sw.checkFailsafe().installed());
    CHECK(f.device.pulseTime == 1300);
    CHECK_FALSE(f.device.timers);
    CHECK(f.device.powerOnState == 0);
    CHECK(f.device.ruleOn);
    CHECK(f.device.ruleTimer1 == 1800);

    f.sw.restore(backup);
    CHECK(f.device.pulseTime == 0);
    CHECK(f.device.timers);
    CHECK(f.device.powerOnState == 3);
    CHECK(f.device.rules.empty());
    CHECK_FALSE(f.device.ruleOn);
}

TEST_CASE("Provision refuses to overwrite someone else's rule without force") {
    Fixture f;
    f.device.rules = "ON Button1#State DO Power TOGGLE ENDON";
    CHECK_THROWS_WITH(f.sw.provision(false), Catch::Matchers::ContainsSubstring("--force"));
    CHECK(f.device.timers);  // nothing changed
    f.sw.provision(true);
    CHECK(f.sw.checkFailsafe().installed());
}

TEST_CASE("Password-protected plugs give a helpful error") {
    Fixture f;
    f.http.handler = [](const std::string&) {
        return HttpResponse{200, R"({"WARNING":"Need user=<username>&password=<password>"})"};
    };
    CHECK_THROWS_WITH(f.sw.set(true), Catch::Matchers::ContainsSubstring("password"));
}

TEST_CASE("Plugs load from config with safe defaults") {
    test::FakeHttp http;
    const auto cfg = json::parse(R"({"plugs":[
        {"id":"bedroom","host":"192.168.1.38"},
        {"id":"sim","type":"simulated","mode":"live","failsafe":{"limpOnMinutes":0}}]})");
    const auto plugs = loadPlugs(cfg, http);
    REQUIRE(plugs.size() == 2);
    CHECK(plugs[0].mode == PlugMode::DryRun);  // never live unless asked for
    CHECK(plugs[0].type == "tasmota");
    CHECK(plugs[1].mode == PlugMode::Live);
    CHECK(plugs[1].failsafe.limpOn.count() == 0);

    CHECK_THROWS(loadPlugs(json::parse(R"({"plugs":[{"id":"a","host":"x"},{"id":"a","host":"y"}]})"), http));
    CHECK_THROWS(loadPlugs(json::parse(R"({"plugs":[{"id":"a"}]})"), http));
    CHECK_THROWS(loadPlugs(json::parse(R"({"plugs":[{"id":"a","host":"x","mode":"yolo"}]})"), http));
}
