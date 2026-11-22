#include "providers/forecast.hpp"

#include <format>
#include <stdexcept>

namespace hb {

using namespace std::chrono;

std::string openMeteoUrl(double latitude, double longitude) {
    return std::format(
        "https://api.open-meteo.com/v1/forecast?latitude={:.2f}&longitude={:.2f}&hourly=temperature_2m"
        "&past_days=1&forecast_days=2&timeformat=unixtime&timezone=GMT",
        latitude, longitude);
}

OutdoorForecast parseOpenMeteo(const nlohmann::json& j) {
    OutdoorForecast f;
    const auto& h = j.at("hourly");
    const auto& times = h.at("time");
    const auto& temps = h.at("temperature_2m");
    for (std::size_t i = 0; i < times.size() && i < temps.size(); ++i) {
        if (!temps[i].is_number()) continue;  // null for hours the model has no value for
        f.hourly.emplace_back(Instant{seconds{times[i].get<long long>()}}, temps[i].get<double>());
    }
    if (f.hourly.empty()) throw std::runtime_error("Open-Meteo: no temperatures in the answer");
    return f;
}

OutdoorForecast fetchForecast(CachingHttpClient& http, double latitude, double longitude) {
    const auto body = http.get(openMeteoUrl(latitude, longitude), hours{1});
    return parseOpenMeteo(nlohmann::json::parse(body.body));
}

}  // namespace hb
