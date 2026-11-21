#include "plugs/tasmota.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

#include "core/log.hpp"
#include "core/time.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

TasmotaSwitch::TasmotaSwitch(HttpClient& http, std::string host, std::string password,
                             FailsafeConfig failsafe)
    : http_(http), host_(std::move(host)), password_(std::move(password)), failsafe_(failsafe) {
    validate(failsafe_);
}

json TasmotaSwitch::command(const std::string& cmnd) {
    std::string url = "http://" + host_ + "/cm?";
    if (!password_.empty()) url += "user=admin&password=" + urlEncode(password_) + "&";
    url += "cmnd=" + urlEncode(cmnd);

    HttpResponse res;
    try {
        res = http_.get(url);
    } catch (const HttpError&) {
        // Transport failure (timeout, reset): one retry. A plug on a weak signal or in WiFi power
        // save answers in 2-3 s now and then; every command we send is a setting or a query, so
        // sending it twice is harmless.
        res = http_.get(url);
    }
    if (res.status != 200) throw HttpError(std::format("{}: HTTP {} for '{}'", host_, res.status, cmnd));
    json j;
    try {
        j = json::parse(res.body);
    } catch (const json::parse_error&) {
        throw HttpError(std::format("{}: not a Tasmota JSON reply for '{}'", host_, cmnd));
    }
    if (j.contains("WARNING")) {
        throw HttpError(std::format("{}: {} (set the plug's web password in config)", host_,
                                    j["WARNING"].get<std::string>()));
    }
    if (j.contains("Command") && j["Command"].is_string()) {
        throw HttpError(std::format("{}: command '{}' rejected: {}", host_, cmnd,
                                    j["Command"].get<std::string>()));
    }
    return j;
}

std::optional<bool> parseTasmotaPower(const json& reply) {
    for (const char* key : {"POWER", "POWER1"}) {
        if (reply.contains(key) && reply[key].is_string()) return reply[key].get<std::string>() == "ON";
    }
    return std::nullopt;
}

PlugStatus TasmotaSwitch::status() {
    PlugStatus s;
    try {
        const json j = command("Status 0");
        s.reachable = true;
        if (j.contains("StatusSTS")) {
            const auto& sts = j["StatusSTS"];
            s.on = parseTasmotaPower(sts);
            s.uptime = sts.value("Uptime", "");
            if (sts.contains("Wifi")) s.rssi = sts["Wifi"].value("RSSI", 0);
        }
        if (j.contains("Status")) s.deviceName = j["Status"].value("DeviceName", "");
        if (j.contains("StatusFWR")) {
            s.firmware = j["StatusFWR"].value("Version", "");
            s.hardware = j["StatusFWR"].value("Hardware", "");
        }
    } catch (const std::exception& e) {
        s.reachable = false;
        s.error = e.what();
    }
    return s;
}

bool TasmotaSwitch::set(bool on) {
    // Re-arm the watchdog and cancel any limp-mode phase before switching. Separate
    // commands (not Backlog) so every step is confirmed synchronously.
    const auto wd = duration_cast<seconds>(failsafe_.watchdog).count();
    command(std::format("RuleTimer1 {}", wd));
    command("RuleTimer2 0");
    // Power ON also restarts the PulseTime countdown (verified on Tasmota 14.6).
    const auto state = parseTasmotaPower(command(on ? "Power1 On" : "Power1 Off"));
    if (!state) throw HttpError(host_ + ": no power state in reply");
    return *state;
}

FailsafeStatus TasmotaSwitch::checkFailsafe() {
    FailsafeStatus st;
    const int pulse = command("PulseTime1").at("PulseTime1").value("Set", -1);
    const int expectedPulse = tasmotaPulseTime(duration_cast<seconds>(failsafe_.pulse));
    if (pulse != expectedPulse) {
        st.problems.push_back(std::format("PulseTime1 is {} (expected {})", pulse, expectedPulse));
    }
    const json rule = command("Rule1").at("Rule1");
    if (rule.value("Rules", "") != tasmotaFailsafeRule(failsafe_)) {
        st.problems.push_back("Rule1 does not contain the heatbrain watchdog");
    } else if (rule.value("State", "") != "ON") {
        st.problems.push_back("Rule1 is disabled");
    }
    if (command("Timers").value("Timers", "") != "OFF") {
        st.problems.push_back("Device timers are enabled (they would fight heatbrain)");
    }
    if (command("PowerOnState").value("PowerOnState", -1) != 0) {
        st.problems.push_back("PowerOnState is not 0 (relay may come back ON after a power cut)");
    }
    return st;
}

json TasmotaSwitch::backup() {
    const auto st = status();
    if (!st.reachable) throw HttpError(st.error);
    const json rule = command("Rule1").at("Rule1");
    return json{
        {"host", host_},
        {"takenAt", formatIsoUtc(floor<seconds>(system_clock::now()))},
        {"firmware", st.firmware},
        {"timers", command("Timers")},
        {"pulseTime1", command("PulseTime1").at("PulseTime1").value("Set", 0)},
        {"powerOnState", command("PowerOnState").value("PowerOnState", 3)},
        {"rule1", {{"state", rule.value("State", "OFF")}, {"rules", rule.value("Rules", "")}}},
    };
}

void TasmotaSwitch::provision(bool overwriteRules) {
    const std::string ours = tasmotaFailsafeRule(failsafe_);
    const std::string existing = command("Rule1").at("Rule1").value("Rules", "");
    if (!existing.empty() && existing != ours && !overwriteRules) {
        throw std::runtime_error(host_ + ": Rule1 is already in use: \"" + existing +
                                 "\". Re-run with --force to replace it (a backup is kept).");
    }
    command("Timers 0");
    command("PowerOnState 0");
    command(std::format("PulseTime1 {}", tasmotaPulseTime(duration_cast<seconds>(failsafe_.pulse))));
    command("Rule1 " + ours);
    command("Rule1 1");
    command(std::format("RuleTimer1 {}", duration_cast<seconds>(failsafe_.watchdog).count()));
    command("RuleTimer2 0");
}

void TasmotaSwitch::startUpgrade(const std::string& otaUrl) {
    command("OtaUrl " + otaUrl);
    const json r = command("Upgrade 1");
    // {"Upgrade":"Version 16.0.0 from http://..."} on success; anything else is a refusal.
    const std::string msg = r.value("Upgrade", "");
    if (msg.find("Version") == std::string::npos && msg.find("from") == std::string::npos) {
        throw HttpError(host_ + ": upgrade not started: " + r.dump());
    }
}

void TasmotaSwitch::restart() { command("Restart 1"); }

bool TasmotaSwitch::testPulse(const Log& log, const Sleeper& sleep) {
    using namespace std::chrono_literals;
    const auto pulseState = [&] {
        const json p = command("PulseTime1").at("PulseTime1");
        return std::pair{p.value("Set", 0), p.value("Remaining", 0)};
    };
    const auto powerOn = [&] { return parseTasmotaPower(command("Power1")).value_or(false); };

    const auto [prevSet, prevRemaining] = pulseState();
    (void)prevRemaining;
    const bool prevPower = powerOn();
    constexpr int kTestPulse = 120;  // Tasmota encoding: 100 + 20 seconds
    bool ok = true;

    try {
        command(std::format("PulseTime1 {}", kTestPulse));
        log("Relay ON with a 20 s auto-off");
        command("Power1 On");
        sleep(8000ms);
        const int before = pulseState().second;
        log(std::format("After 8 s: {} s left", before - 100));
        command("Power1 On");  // heatbrain's refresh
        const int after = pulseState().second;
        log(std::format("Refreshed: {} s left", after - 100));
        if (after <= before + 3) {
            log("FAIL: refreshing did not restart the countdown");
            ok = false;
        }
        // The relay must now switch itself off ~20 s after the refresh.
        bool off = false;
        for (int i = 0; i < 30 && !off; ++i) {
            sleep(1000ms);
            off = !powerOn();
            if (off) log(std::format("Relay switched itself off after {} s", i + 1));
        }
        if (!off) {
            log("FAIL: the relay did not switch itself off");
            ok = false;
        }
    } catch (const std::exception& e) {
        log(std::string("FAIL: ") + e.what());
        ok = false;
    }

    // Always put things back, even after a failure.
    try {
        command("Power1 Off");
        command(std::format("PulseTime1 {}", prevSet));
        if (prevPower) command("Power1 On");
        log(std::format("Restored PulseTime {} and relay {}", prevSet, prevPower ? "ON" : "OFF"));
    } catch (const std::exception& e) {
        log(std::string("WARNING: could not restore settings: ") + e.what());
        ok = false;
    }
    return ok;
}

void TasmotaSwitch::restore(const json& b) {
    command("RuleTimer1 0");
    command("RuleTimer2 0");
    const std::string rules = b.at("rule1").value("rules", "");
    command(rules.empty() ? "Rule1 \"" : "Rule1 " + rules);
    command(b.at("rule1").value("state", "OFF") == "ON" ? "Rule1 1" : "Rule1 0");
    command(std::format("PulseTime1 {}", b.at("pulseTime1").get<int>()));
    command(std::format("PowerOnState {}", b.at("powerOnState").get<int>()));
    command(b.at("timers").value("Timers", "OFF") == "ON" ? "Timers 1" : "Timers 0");
}

}  // namespace hb
