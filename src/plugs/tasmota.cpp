#include "plugs/tasmota.hpp"

#include <regex>

#include <chrono>
#include <format>
#include <stdexcept>

#include "core/log.hpp"
#include "core/time.hpp"
#include "core/version.hpp"

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
    const HttpRequest req = tasmotaRequest(host_, url);

    HttpResponse res;
    try {
        res = http_.send(req);
    } catch (const HttpError&) {
        // Transport failure (timeout, reset): one retry. A plug on a weak signal or in WiFi power
        // save answers in 2-3 s now and then; every command we send is a setting or a query, so
        // sending it twice is harmless.
        res = http_.send(req);
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

std::string parseTasmotaModule(const json& reply) {
    if (!reply.contains("Module")) return {};
    const auto& m = reply["Module"];
    if (m.is_object() && !m.empty() && m.begin()->is_string()) return m.begin()->get<std::string>();
    if (m.is_string()) {
        const auto s = m.get<std::string>();
        const auto open = s.find('('), close = s.rfind(')');
        return open != std::string::npos && close > open ? s.substr(open + 1, close - open - 1) : s;
    }
    return {};
}

std::string tasmotaModel(HttpClient& http, const std::string& host) {
    try {
        const auto res = http.send(tasmotaRequest(host, "http://" + host + "/cm?cmnd=Module"));
        if (res.status != 200) return {};
        return parseTasmotaModule(json::parse(res.body));
    } catch (const std::exception&) {
        return {};
    }
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
        try {
            s.model = parseTasmotaModule(command("Module"));  // not part of Status 0
        } catch (const std::exception&) {
            // reachable all the same: just no model
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
        // During a Wi-Fi off period the auto-off is stretched on purpose, next to a schedule in Rule2.
        const std::string rule2 = pulse > expectedPulse ? command("Rule2").at("Rule2").value("Rules", "") : "";
        if (rule2.empty() || !isHeatbrainScheduleRule(rule2)) {
            st.problems.push_back(std::format("PulseTime1 is {} (expected {})", pulse, expectedPulse));
        }
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
    // An earlier heatbrain fail-safe (other timings) is simply replaced; only someone else's rule
    // is protected.
    if (!existing.empty() && existing != ours && !isHeatbrainFailsafeRule(existing) && !overwriteRules) {
        throw std::runtime_error(host_ + ": Rule1 holds a rule that is not heatbrain's: \"" + existing +
                                 "\". Remove it in the plug's console (Rule1 \"), or replace it with "
                                 "'heatbrain plug <id> provision --force' (a backup is kept).");
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

namespace {
std::optional<Instant> parseTasmotaTime(std::string s) {
    // "2026-11-22T00:28:18" (optionally with a trailing Z)
    static const std::regex iso(R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2}))");
    std::smatch m;
    if (!std::regex_search(s, m, iso)) return std::nullopt;
    const auto ymd = std::chrono::year{std::stoi(m[1])} / std::stoi(m[2]) / std::stoi(m[3]);
    if (!ymd.ok()) return std::nullopt;
    return Instant{std::chrono::sys_days{ymd}.time_since_epoch()} + hours{std::stoi(m[4])} +
           minutes{std::stoi(m[5])} + seconds{std::stoi(m[6])};
}
}  // namespace

minutes tasmotaClockOffset(const json& status7, Instant now, minutes tolerance) {
    if (!status7.contains("StatusTIM")) throw std::runtime_error("the plug did not report its clock");
    const auto& t = status7["StatusTIM"];
    const auto utc = parseTasmotaTime(t.value("UTC", ""));
    const auto local = parseTasmotaTime(t.value("Local", ""));
    if (!utc || !local) throw std::runtime_error("the plug's clock could not be read");
    const auto drift = duration_cast<minutes>(*utc > now ? *utc - now : now - *utc);
    if (drift > tolerance) {
        throw std::runtime_error(std::format("the plug's clock is {} minutes off (no time sync?)", drift.count()));
    }
    return round<minutes>(*local - *utc);
}

bool tasmotaCanSwitchWifiOff(std::string_view firmware) {
    return !parseVersion(firmware).empty() && compareVersions(firmware, kTasmotaWifiOffSince) >= 0;
}

std::optional<OfflinePeriod> TasmotaSwitch::supportedRadioOff(std::optional<OfflinePeriod> radioOff) {
    if (!radioOff) return std::nullopt;
    const json fw = command("Status 2");
    const std::string version = fw.contains("StatusFWR") ? fw["StatusFWR"].value("Version", "") : "";
    if (tasmotaCanSwitchWifiOff(version)) return radioOff;
    return std::nullopt;
}

void TasmotaSwitch::loadScheduleRules(const std::vector<OnSpan>& spans, Instant now,
                                      std::optional<OfflinePeriod> radioOff) {
    const minutes offset = tasmotaClockOffset(command("Status 7"), now);
    const auto rules = tasmotaScheduleRules(spans, offset, radioOff);
    if (!rules) throw std::runtime_error("too many switch points for the plug's rule slots");
    // Rule1 is the fail-safe; Rule2/Rule3 carry the schedule - unless the user has rules there.
    for (const char* slot : {"Rule2", "Rule3"}) {
        const std::string existing = command(slot).at(slot).value("Rules", "");
        if (!isHeatbrainScheduleRule(existing)) {
            throw std::runtime_error(std::format("{} on the plug holds your own rule; heatbrain won't overwrite it", slot));
        }
    }
    command(rules->size() > 0 ? "Rule2 " + (*rules)[0] : "Rule2 \"");
    command(rules->size() > 0 ? "Rule2 1" : "Rule2 0");
    command(rules->size() > 1 ? "Rule3 " + (*rules)[1] : "Rule3 \"");
    command(rules->size() > 1 ? "Rule3 1" : "Rule3 0");
}

bool TasmotaSwitch::armOffline(const std::vector<OnSpan>& spans, Instant now, Instant until,
                               std::optional<OfflinePeriod> radioOff) {
    radioOff = supportedRadioOff(radioOff);
    loadScheduleRules(spans, now, radioOff);
    seconds longest{0};
    for (const auto& s : spans) longest = std::max(longest, duration_cast<seconds>(s.to - s.from));
    // Auto-off long enough for the longest planned ON (each still ends with its own OFF);
    // the watchdog only starts limp mode if heatbrain is silent after the period.
    const seconds pulse = std::max(duration_cast<seconds>(failsafe_.pulse), longest + minutes{10});
    const seconds watchdog = duration_cast<seconds>(until - now) + duration_cast<seconds>(failsafe_.watchdog);
    command(std::format("PulseTime1 {}", tasmotaPulseTime(pulse)));
    command(std::format("RuleTimer1 {}", watchdog.count()));
    command("RuleTimer2 0");
    command(scheduleOnAt(spans, now) ? "Power1 On" : "Power1 Off");
    return radioOff.has_value();
}

bool TasmotaSwitch::armRadioOff(const OfflinePeriod& radioOff, Instant now) {
    const auto supported = supportedRadioOff(radioOff);
    if (!supported) return false;
    loadScheduleRules({}, now, supported);
    return true;
}

void TasmotaSwitch::keepAlive(Instant now, std::optional<Instant> until) {
    seconds watchdog = duration_cast<seconds>(failsafe_.watchdog);
    if (until && *until > now) watchdog += duration_cast<seconds>(*until - now);
    command(std::format("RuleTimer1 {}", watchdog.count()));
    command("RuleTimer2 0");  // ends a limp phase; a relay left on still goes off by its auto-off
}

void TasmotaSwitch::disarmOffline() {
    for (const char* slot : {"Rule2", "Rule3"}) {
        const std::string existing = command(slot).at(slot).value("Rules", "");
        if (!existing.empty() && isHeatbrainScheduleRule(existing)) {
            command(std::string(slot) + " \"");
            command(std::string(slot) + " 0");
        }
    }
    command(std::format("PulseTime1 {}", tasmotaPulseTime(duration_cast<seconds>(failsafe_.pulse))));
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
