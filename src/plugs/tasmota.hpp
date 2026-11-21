#pragma once

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

private:
    HttpClient& http_;
    std::string host_;
    std::string password_;
    FailsafeConfig failsafe_;
};

// Parses a reply to "Power" / "Power1": {"POWER":"ON"} or {"POWER1":"OFF"}.
std::optional<bool> parseTasmotaPower(const nlohmann::json& reply);

}  // namespace hb
