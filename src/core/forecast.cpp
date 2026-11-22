#include "core/forecast.hpp"

#include <algorithm>
#include <chrono>

namespace hb {

using namespace std::chrono;

namespace {
constexpr double kFadeHours = 6.0;
constexpr double kMaxOffset = 6.0;  // a sensor in the sun or a wrong forecast: don't trust more than this
}  // namespace

std::optional<double> OutdoorForecast::at(Instant t) const {
    if (hourly.empty() || t < hourly.front().first || t > hourly.back().first) return std::nullopt;
    const auto hi = std::ranges::lower_bound(hourly, t, {}, [](const auto& p) { return p.first; });
    double v;
    if (hi->first == t || hi == hourly.begin()) {
        v = hi->second;
    } else {
        const auto lo = hi - 1;
        const double f = duration<double>(t - lo->first).count() / duration<double>(hi->first - lo->first).count();
        v = lo->second + (hi->second - lo->second) * f;
    }
    if (offset != 0) {
        const double h = duration<double, std::ratio<3600>>(t - offsetAt).count();
        if (h < kFadeHours) v += offset * (1.0 - std::max(0.0, h) / kFadeHours);
    }
    return v;
}

std::optional<double> OutdoorForecast::meanOver(Instant from, Instant to) const {
    if (to <= from) return at(from);
    double sum = 0;
    int n = 0;
    for (Instant t = from; t < to; t += minutes{15}) {
        const auto v = at(t);
        if (!v) return std::nullopt;
        sum += *v;
        ++n;
    }
    return sum / n;
}

OutdoorForecast OutdoorForecast::correctedTo(double measured, Instant now) const {
    OutdoorForecast f = *this;
    f.offset = 0;
    if (const auto v = f.at(now)) {
        f.offset = std::clamp(measured - *v, -kMaxOffset, kMaxOffset);
        f.offsetAt = now;
    }
    return f;
}

}  // namespace hb
