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

    const HttpResponse res = http_.get(url);
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
