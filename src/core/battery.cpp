#include "core/battery.hpp"

#include <cmath>
#include <format>

namespace hb {

using namespace std::chrono;

namespace {
constexpr int kWarnDaysAhead = 21;
constexpr int kAwayMarginDays = 2;
constexpr auto kFitWindow = days{60};
constexpr auto kMinHistory = days{10};
}  // namespace

std::optional<double> daysUntilEmpty(const std::vector<BatterySample>& samples) {
    if (samples.size() < 2) return std::nullopt;
    const Instant last = samples.back().day;
    std::vector<std::pair<double, double>> pts;  // (days before last, percent)
    for (const auto& s : samples) {
        if (last - s.day > kFitWindow) continue;
        pts.emplace_back(duration<double, std::ratio<86400>>(s.day - last).count(), s.percent);
    }
    const double spanDays = -pts.front().first;  // oldest sample used, in days before the newest
    if (pts.size() < 2 || spanDays < duration<double, std::ratio<86400>>(kMinHistory).count()) return std::nullopt;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& [x, y] : pts) { sx += x; sy += y; sxx += x * x; sxy += x * y; }
    const double n = static_cast<double>(pts.size());
    const double den = n * sxx - sx * sx;
    if (den <= 0) return std::nullopt;
    const double slope = (n * sxy - sx * sy) / den;  // percent per day
    if (slope >= -1e-3) return std::nullopt;         // not falling (yet)
    const double now = (sy - slope * sx) / n;        // the fitted level today
    return std::max(0.0, (now - kBatteryEmptyPercent) / -slope);
}

std::optional<BatteryWarning> batteryWarning(const std::string& name, int percent,
                                             const std::vector<BatterySample>& samples, Instant now,
                                             const std::optional<AwayMode>& away, const TimeZone& tz) {
    if (percent <= kBatteryEmptyPercent) {
        return BatteryWarning{std::format("Sensor '{}' battery is at {}% - replace it now", name, percent)};
    }
    const auto left = daysUntilEmpty(samples);
    if (left) {
        const Instant empty = now + duration_cast<seconds>(duration<double, std::ratio<86400>>(*left));
        if (away && away->until > now && empty < away->until + days{kAwayMarginDays}) {
            return BatteryWarning{std::format(
                "Sensor '{}' battery ({}%) will likely run out while you're away (back {}) - replace it before leaving",
                name, percent, formatLocalDate(tz.toLocal(away->until)))};
        }
        if (*left <= kWarnDaysAhead) {
            const int d = static_cast<int>(std::lround(*left));
            const std::string when = d >= 14 ? std::format("about {} weeks", (d + 3) / 7) : std::format("about {} days", std::max(1, d));
            return BatteryWarning{std::format("Sensor '{}' battery is at {}% - {} left", name, percent, when)};
        }
        return std::nullopt;
    }
    if (percent <= kBatteryFallbackPercent) {
        return BatteryWarning{std::format("Sensor '{}' battery is at {}% - replace soon", name, percent)};
    }
    return std::nullopt;
}

}  // namespace hb
