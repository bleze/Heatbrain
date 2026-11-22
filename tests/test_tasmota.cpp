#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    std::map<std::string, std::string> extraRules;  // Rule2 / Rule3
    std::string clockUtc = "2026-11-23T22:00:00", clockLocal = "2026-11-23T23:00:00";
    // Simulated time for PulseTime (seconds), advanced by the test's sleeper.
    double clock = 0, onAt = 0;
    bool brokenRefresh = false;  // a firmware where re-sending ON does not restart the countdown
    double pulseSeconds() const { return pulseTime > 111 ? pulseTime - 100 : pulseTime / 10.0; }
    void tickPulse() {
        if (power && pulseTime > 0 && clock - onAt >= pulseSeconds()) power = false;
    }

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
            tickPulse();
            if (arg == "On") {
                if (!power || !brokenRefresh) onAt = clock;
                power = true;
            }
            if (arg == "Off") power = false;
            r = {{"POWER", power ? "ON" : "OFF"}};
        } else if (name == "PulseTime1") {
            if (!arg.empty()) pulseTime = std::stoi(arg);
            tickPulse();
            const int remaining = power ? static_cast<int>(100 + pulseSeconds() - (clock - onAt)) : 192000;
            r = {{"PulseTime1", {{"Set", pulseTime}, {"Remaining", remaining}}}};
        } else if (name == "Rule1") {
            if (arg == "1") ruleOn = true;
            else if (arg == "0") ruleOn = false;
            else if (arg == "\"") rules.clear();
            else if (!arg.empty()) rules = arg;
            r = {{"Rule1", {{"State", ruleOn ? "ON" : "OFF"}, {"Rules", rules}}}};
        } else if (name == "Rule2" || name == "Rule3") {
            auto& text = extraRules[name];
            if (arg == "\"") text.clear();
            else if (!arg.empty() && arg != "1" && arg != "0") text = arg;
            r = {{name, {{"State", "ON"}, {"Rules", text}}}};
        } else if (name == "Status" && arg == "7") {
            r = {{"StatusTIM", {{"UTC", clockUtc}, {"Local", clockLocal}}}};
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

TEST_CASE("Fail-safe function test passes on a correct device and restores its settings") {
    Fixture f;
    f.device.pulseTime = 1300;  // provisioned
    f.device.power = true;      // was on before the test
    std::vector<std::string> lines;
    const bool ok = f.sw.testPulse([&](const std::string& l) { lines.push_back(l); },
                                   [&](std::chrono::milliseconds d) { f.device.clock += d.count() / 1000.0; });
    CHECK(ok);
    CHECK(f.device.pulseTime == 1300);  // restored
    CHECK(f.device.power);              // restored
    CHECK(std::ranges::any_of(lines, [](const std::string& l) { return l.find("switched itself off") != std::string::npos; }));
}

TEST_CASE("Fail-safe function test catches firmware where a refresh does not restart the countdown") {
    Fixture f;
    f.device.brokenRefresh = true;
    std::vector<std::string> lines;
    const bool ok = f.sw.testPulse([&](const std::string& l) { lines.push_back(l); },
                                   [&](std::chrono::milliseconds d) { f.device.clock += d.count() / 1000.0; });
    CHECK_FALSE(ok);
    CHECK(std::ranges::any_of(lines, [](const std::string& l) { return l.find("did not restart") != std::string::npos; }));
    CHECK_FALSE(f.device.power);
    CHECK(f.device.pulseTime == 0);
}

TEST_CASE("Starting an upgrade sets the OTA URL first") {
    Fixture f;
    f.http.handler = [&](const std::string& url) {
        if (url.find("Upgrade") != std::string::npos) return HttpResponse{200, R"({"Upgrade":"Version 16.0.0 from http://ota.tasmota.com/tasmota/release/tasmota.bin.gz"})"};
        if (url.find("OtaUrl") != std::string::npos) return HttpResponse{200, R"({"OtaUrl":"http://ota.tasmota.com/tasmota/release/tasmota.bin.gz"})"};
        return f.device.handle(url);
    };
    f.sw.startUpgrade("http://ota.tasmota.com/tasmota/release/tasmota.bin.gz");
    CHECK(f.http.requests.size() == 2);
    CHECK(f.http.requests[0].url.find("OtaUrl") != std::string::npos);
}

TEST_CASE("Tasmota command retries once after a transport failure") {
    Fixture f;
    int failures = 1;
    f.http.handler = [&](const std::string& url) {
        if (failures-- > 0) throw HttpError("GET http://192.168.1.38 failed: Connection timed out");
        return f.device.handle(url);
    };
    CHECK(f.sw.status().reachable);  // first attempt timed out, the retry answered

    failures = 2;  // two in a row: reported as unreachable
    CHECK_FALSE(f.sw.status().reachable);
}

TEST_CASE("Remove fail-safe restores the newest backup from before the fail-safe") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "heatbrain-test-backups";
    fs::remove_all(dir);
    hb::Plug plug;
    plug.id = "bedroom";
    fs::create_directories(dir / "plugs" / "bedroom");
    const auto write = [&](const char* name, const std::string& rules) {
        std::ofstream(dir / "plugs" / "bedroom" / name)
            << nlohmann::json{{"rule1", {{"state", "ON"}, {"rules", rules}}}, {"pulseTime1", 0}}.dump();
    };
    const std::string ours = hb::tasmotaFailsafeRule(hb::FailsafeConfig{});
    write("backup-20261121T190000Z.json", "");                     // before install: the original
    write("backup-20261121T200000Z.json", "ON Power1#State DO x");  // the user's own rule, still original
    write("backup-20261122T080000Z.json", ours);                   // before an upgrade: has the fail-safe
    CHECK(hb::originalBackup(dir, plug).filename() == "backup-20261121T200000Z.json");
    CHECK(hb::backupHasFailsafe(nlohmann::json{{"rule1", {{"rules", ours}}}}));
    fs::remove_all(dir);
}

TEST_CASE("Wi-Fi off: the night schedule goes onto the plug with a stretched fail-safe") {
    using namespace std::chrono;
    Fixture f;
    f.sw.provision(false);  // the normal fail-safe first
    const Instant now = Instant{sys_days{2026y / 11 / 23}.time_since_epoch()} + hours{22};  // 23:00 local
    const Instant until = now + hours{7};
    // ON 00:00-02:00 local (23:00-01:00 UTC)
    const std::vector<OnSpan> spans{{now + hours{1}, now + hours{3}}};
    f.sw.armOffline(spans, now, until);
    CHECK(f.device.extraRules["Rule2"] == "ON Time#Minute=0 DO Power1 1 ENDON ON Time#Minute=120 DO Power1 0 ENDON");
    CHECK(f.device.pulseTime == 100 + 2 * 3600 + 600);       // longest ON + 10 min
    CHECK(f.device.ruleTimer1 == 7 * 3600 + 30 * 60);        // until the end + the normal watchdog
    CHECK_FALSE(f.device.power);                             // off now: the first ON is at midnight
    CHECK(f.sw.checkFailsafe().installed());                 // the stretched auto-off is expected

    f.sw.disarmOffline();
    CHECK(f.device.extraRules["Rule2"].empty());
    CHECK(f.device.pulseTime == 1300);
    CHECK(f.device.ruleTimer1 == 30 * 60);
}

TEST_CASE("Wi-Fi off: refused when the plug's clock is wrong or its rule slots hold the user's rules") {
    using namespace std::chrono;
    const Instant now = Instant{sys_days{2026y / 11 / 23}.time_since_epoch()} + hours{22};
    {
        Fixture f;
        f.device.clockUtc = "1970-01-01T00:10:00";  // never synced
        CHECK_THROWS_WITH(f.sw.armOffline({}, now, now + hours{7}), Catch::Matchers::ContainsSubstring("clock"));
    }
    {
        Fixture f;
        f.device.extraRules["Rule2"] = "ON Button1#State DO Power1 2 ENDON";
        CHECK_THROWS_WITH(f.sw.armOffline({}, now, now + hours{7}), Catch::Matchers::ContainsSubstring("your own rule"));
        CHECK(f.device.extraRules["Rule2"] == "ON Button1#State DO Power1 2 ENDON");
    }
}

TEST_CASE("Reinstalling the fail-safe replaces an earlier heatbrain rule, not someone else's") {
    Fixture f;
    f.sw.provision(false);  // limp 10 / 50
    const std::string old = f.device.rules;
    CHECK(isHeatbrainFailsafeRule(old));
    FailsafeConfig quiet;
    quiet.limpOn = std::chrono::minutes{0};  // "stay off"
    TasmotaSwitch again{f.http, "192.168.1.38", "", quiet};
    again.provision(false);
    CHECK(f.device.rules == tasmotaFailsafeRule(quiet));
    CHECK(isHeatbrainFailsafeRule(f.device.rules));

    f.device.rules = "ON Button1#State DO Power1 2 ENDON";
    CHECK_THROWS_WITH(again.provision(false), Catch::Matchers::ContainsSubstring("not heatbrain's"));
}
