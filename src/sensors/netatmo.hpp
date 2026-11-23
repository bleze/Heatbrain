#pragma once

// Netatmo Weather Station (indoor/outdoor modules) via https://api.netatmo.com.
// OAuth2: access tokens live ~3 h; refresh tokens ROTATE on every refresh, so the new one
// is persisted before it is used. Losing it means generating a new token by hand.

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "net/http.hpp"
#include "sensors/temperature_source.hpp"

namespace hb::netatmo {

inline constexpr const char* kApiBase = "https://api.netatmo.com";

// The refresh token was rejected: a human has to generate a new one.
class AuthError : public SourceAuthError {
public:
    using SourceAuthError::SourceAuthError;
};

class Auth {
public:
    Auth(HttpClient& http, std::string clientId, std::string clientSecret,
         std::string configRefreshToken, std::filesystem::path tokenFile,
         std::function<Instant()> now, std::string apiBase = kApiBase);

    // A valid access token, refreshing (and persisting the rotated refresh token) if needed.
    std::string accessToken();

    // Force a refresh on the next accessToken() call (e.g. after the API said "expired").
    void invalidate();

private:
    struct Tokens {
        std::string access;
        std::string refresh;
        std::string bootstrap;  // the config refreshToken these tokens descend from
        Instant expiresAt{};
    };

    std::optional<Tokens> load() const;
    void save(const Tokens& t) const;
    void refresh();

    HttpClient& http_;
    std::string clientId_, clientSecret_, configRefreshToken_;
    std::filesystem::path tokenFile_;
    std::function<Instant()> now_;
    std::string apiBase_;
    std::optional<Tokens> tokens_;
};

// Parses a getstationsdata response into readings (main indoor unit, indoor and outdoor
// modules; rain/wind modules have no temperature and are skipped).
std::vector<SensorReading> parseStationsData(const nlohmann::json& body);

class Source final : public TemperatureSource {
public:
    Source(std::string id, HttpClient& http, Auth auth, std::string apiBase = kApiBase);
    const std::string& id() const override { return id_; }
    std::vector<SensorReading> read() override;

private:
    std::string id_;
    HttpClient& http_;
    Auth auth_;
    std::string apiBase_;
};

}  // namespace hb::netatmo
