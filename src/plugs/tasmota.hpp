#pragma once

#include <chrono>
#include <functional>
#include <string>

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
    HttpClient& http_;
    std::string host_;
    std::string password_;
    FailsafeConfig failsafe_;
};

// Parses a reply to "Power" / "Power1": {"POWER":"ON"} or {"POWER1":"OFF"}.
std::optional<bool> parseTasmotaPower(const nlohmann::json& reply);

}  // namespace hb
