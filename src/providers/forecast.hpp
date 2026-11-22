#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/forecast.hpp"
#include "net/http.hpp"

namespace hb {

// Hourly outdoor temperature for the next two days from Open-Meteo (free, no key). Only the
// position, rounded to about a kilometre, is sent.
std::string openMeteoUrl(double latitude, double longitude);
OutdoorForecast parseOpenMeteo(const nlohmann::json& j);
// Cached for an hour; throws when there is neither a fresh nor a cached answer.
OutdoorForecast fetchForecast(CachingHttpClient& http, double latitude, double longitude);

}  // namespace hb
