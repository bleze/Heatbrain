#pragma once

// VELUX ACTIVE with Netatmo: the indoor climate readings (temperature, humidity, CO2) that the
// VELUX app shows per room, measured by the wall switch (NXD) and climate sensors (NXS).
// Read only, from the cloud the VELUX app uses (app.velux-active.com, the app's own client id;
// unofficial, so VELUX may change it). Login once with the VELUX account: only the tokens are
// kept (velux-tokens.json in the data folder), never the password. Refresh tokens rotate.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "net/http.hpp"
#include "sensors/temperature_source.hpp"

namespace hb::velux {

inline constexpr const char* kApiBase = "https://app.velux-active.com";

struct Tokens {
    std::string access;
    std::string refresh;
    Instant expiresAt{};
};

// Logs in with the VELUX account (password grant) and saves the tokens to tokenFile.
// Throws SourceAuthError when the account is rejected.
Tokens login(HttpClient& http, const std::string& email, const std::string& password,
             const std::filesystem::path& tokenFile, Instant now, const std::string& apiBase = kApiBase);

class Auth {
public:
    Auth(HttpClient& http, std::filesystem::path tokenFile, std::function<Instant()> now,
         std::string apiBase = kApiBase);
    // A valid access token, refreshing (and saving the rotated refresh token) when needed.
    std::string accessToken();
    void invalidate();

private:
    void refresh();
    HttpClient& http_;
    std::filesystem::path tokenFile_;
    std::function<Instant()> now_;
    std::string apiBase_;
    std::optional<Tokens> tokens_;
};

// homesdata + homestatus -> one reading per room that reports a temperature. The room's id is the
// sensor id, its name the sensor name. The time is the room's sensor modules' last contact (or
// `now` when the cloud gives none); a room whose sensors (or the gateway) are unreachable is
// reported unreachable.
std::vector<SensorReading> parseHome(const nlohmann::json& homesData, const nlohmann::json& homeStatus,
                                     const std::string& homeId, Instant now);

class Source final : public TemperatureSource {
public:
    Source(std::string id, HttpClient& http, Auth auth, std::function<Instant()> now,
           std::string apiBase = kApiBase);
    const std::string& id() const override { return id_; }
    std::vector<SensorReading> read() override;

private:
    nlohmann::json post(const std::string& what, HttpRequest req, bool retried = false);
    std::string id_;
    HttpClient& http_;
    Auth auth_;
    std::function<Instant()> now_;
    std::string apiBase_;
    std::optional<std::pair<Instant, std::string>> homesData_;  // fetched when, raw body (room names)
};

}  // namespace hb::velux
