#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "net/http.hpp"
#include "plugs/switch.hpp"

namespace hb {

// Tasmota over its HTTP command API (http://<host>/cm?cmnd=...).
class TasmotaSwitch final : public Switch {
public:
    TasmotaSwitch(HttpClient& http, std::string host, std::string password, FailsafeConfig failsafe);

    PlugStatus status() override;
    bool set(bool on) override;
    FailsafeStatus checkFailsafe() override;
    nlohmann::json backup() override;
    void provision(bool overwriteRules) override;
    void restore(const nlohmann::json& backup) override;
    bool armOffline(const std::vector<OnSpan>& spans, Instant now, Instant until,
                    std::optional<OfflinePeriod> radioOff = {}) override;
    bool armRadioOff(const OfflinePeriod& radioOff, Instant now) override;
    void disarmOffline() override;
    void keepAlive(Instant now, std::optional<Instant> until) override;

    // Sends one command and returns the parsed JSON reply. Throws on failure.
    nlohmann::json command(const std::string& cmnd);

    // Starts an over-the-air upgrade from otaUrl. The device downloads, flashes and
    // restarts by itself (1 MB devices go through the minimal build: two restarts).
    void startUpgrade(const std::string& otaUrl);

    void restart();

    using Log = std::function<void(const std::string&)>;
    using Sleeper = std::function<void(std::chrono::milliseconds)>;

    // Function test of the dead-man switch on the real device: ON with a 20 s PulseTime,
    // refresh after ~8 s (the countdown must restart), then the relay must switch itself
    // off. Restores the previous PulseTime and relay state afterwards. Returns success;
    // details go to log. The relay is on for roughly 30 seconds.
    bool testPulse(const Log& log, const Sleeper& sleep);

private:
    // radioOff kept only when the firmware can switch Wi-Fi off safely.
    std::optional<OfflinePeriod> supportedRadioOff(std::optional<OfflinePeriod> radioOff);
    // Writes the schedule into Rule2/Rule3, refusing when they hold the user's own rules.
    void loadScheduleRules(const std::vector<OnSpan>& spans, Instant now, std::optional<OfflinePeriod> radioOff);

    HttpClient& http_;
    std::string host_;
    std::string password_;
    FailsafeConfig failsafe_;
};

// The plug's local time minus UTC from a "Status 7" reply, provided its UTC clock is within
// `tolerance` of `now`. Throws std::runtime_error with the reason otherwise.
std::chrono::minutes tasmotaClockOffset(const nlohmann::json& status7, Instant now,
                                        std::chrono::minutes tolerance = std::chrono::minutes{5});

// "Wifi 0" crashed Tasmota when traffic arrived as it ran until 15.4.0 (arendst/Tasmota#24536).
inline constexpr const char* kTasmotaWifiOffSince = "15.4.0";
bool tasmotaCanSwitchWifiOff(std::string_view firmware);

// Parses a reply to "Power" / "Power1": {"POWER":"ON"} or {"POWER1":"OFF"}.
std::optional<bool> parseTasmotaPower(const nlohmann::json& reply);

}  // namespace hb
