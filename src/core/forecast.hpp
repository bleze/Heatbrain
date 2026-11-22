#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "core/time.hpp"

namespace hb {

// The outdoor temperature over the coming hours (a weather forecast), corrected towards the
// outdoor sensor: the forecast is for the area, the sensor for the house. The difference measured
// now fades out over six hours.
struct OutdoorForecast {
    std::vector<std::pair<Instant, double>> hourly;  // sorted by time
    double offset = 0;    // measured - forecast at offsetAt
    Instant offsetAt{};

    bool empty() const { return hourly.empty(); }
    // Interpolated; nullopt outside the forecast.
    std::optional<double> at(Instant t) const;
    // Mean over [from, to) (15-minute steps); nullopt if any part is outside the forecast.
    std::optional<double> meanOver(Instant from, Instant to) const;
    // The same forecast, corrected to `measured` at `now`.
    OutdoorForecast correctedTo(double measured, Instant now) const;
};

}  // namespace hb
