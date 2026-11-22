#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/time.hpp"
#include "net/http.hpp"

namespace hb {

// Eloverblik (Energinet's DataHub): the customer API that gives anyone in Denmark their own
// meter readings, whatever the supplier. Readings arrive with a delay (usually the next day),
// so this is for looking back, never for control.
//
// Auth: the user creates a refresh token on eloverblik.dk (MitID). It is exchanged for an access
// token valid 24 h; heatbrain caches that one (<dataDir>/eloverblik-token.json) because the token
// endpoint is strictly rate limited.

struct MeteringPoint {
    std::string id;        // 18 digits
    std::string type;      // "E17" consumption, "E18" production, ...
    std::string address;
    std::string supplier;
};

struct MeterReading {
    Instant start;
    std::chrono::minutes length{60};
    double kWh = 0;
    bool operator==(const MeterReading&) const = default;
};

class EloverblikClient {
public:
    EloverblikClient(HttpClient& http, std::filesystem::path tokenFile, std::string refreshToken,
                     std::function<Instant()> now, std::string baseUrl = "https://api.eloverblik.dk/customerapi/");

    std::vector<MeteringPoint> meteringPoints();

    // Readings for [from, toExclusive) as local Danish dates ("2026-11-20"), finest resolution.
    std::vector<MeterReading> readings(const std::string& meteringPoint, const std::string& from,
                                       const std::string& toExclusive);

private:
    std::string accessToken(bool renew);
    HttpResponse call(HttpRequest req);  // with the access token; renews it once on 401

    HttpClient& http_;
    std::filesystem::path tokenFile_;
    std::string refreshToken_;
    std::function<Instant()> now_;
    std::string base_;
};

// The readings in a gettimeseries reply (any resolution: PT15M, PT1H, ...).
std::vector<MeterReading> parseEloverblikTimeSeries(const nlohmann::json& reply);

}  // namespace hb
