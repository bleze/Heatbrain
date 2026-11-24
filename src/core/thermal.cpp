#include "core/thermal.hpp"

#include <algorithm>
#include <cmath>

namespace hb {

using namespace std::chrono;

namespace {

constexpr auto kMaxGap = minutes{20};
// After a gap in the readings (a Wi-Fi-off night) a sensor needs a while to settle: a thermostat's
// built-in sensor reads low while its radio was idle and catches up within minutes - which would
// look like the room warming by itself. The first hour after a gap is left out.
constexpr auto kSettle = minutes{60};
constexpr auto kWindow = minutes{90};
constexpr double kMinIndoorOutdoorGap = 3.0;  // below this, cooling says little about the loss
constexpr auto kCap = hours{48};

double median(std::vector<double> v) {
    std::ranges::sort(v);
    const auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

struct Window {
    double rate;     // °C per hour
    double indoor;   // mean
    double outdoor;  // mean
    bool heating;
};

// Back-to-back windows of at least kWindow inside runs of the same heating state, with no gaps and
// a known outdoor temperature. Temperatures come in 0.1 degree steps, so windows must not be short.
std::vector<Window> windows(const std::vector<ThermalSample>& s) {
    std::vector<Window> out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (!s[i].outdoor) { ++i; continue; }
        std::size_t j = i;
        while (j + 1 < s.size() && s[j + 1].heating == s[i].heating && s[j + 1].at - s[j].at <= kMaxGap &&
               s[j + 1].outdoor && s[j].at - s[i].at < kWindow) {
            ++j;
        }
        if (s[j].at - s[i].at < kWindow) { i = j + 1; continue; }
        double indoor = 0, outdoor = 0;
        for (std::size_t k = i; k <= j; ++k) { indoor += s[k].temperature; outdoor += *s[k].outdoor; }
        const double n = static_cast<double>(j - i + 1);
        const double h = duration<double, std::ratio<3600>>(s[j].at - s[i].at).count();
        out.push_back({(s[j].temperature - s[i].temperature) / h, indoor / n, outdoor / n, s[i].heating});
        i = j;  // the next window starts where this one ended
    }
    return out;
}

}  // namespace

ThermalModel fitThermalModel(const std::vector<ThermalSample>& all) {
    ThermalModel m;
    std::vector<ThermalSample> samples;
    std::optional<Instant> settledAt;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i > 0 && all[i].at - all[i - 1].at > kMaxGap) settledAt = all[i].at + kSettle;
        if (settledAt && all[i].at < *settledAt) continue;
        samples.push_back(all[i]);
    }
    const auto ws = windows(samples);
    std::vector<double> losses;
    for (const auto& w : ws) {
        const double gap = w.indoor - w.outdoor;
        // A room that warmed without heating had heat from elsewhere (sun, PCs, people, a warm room
        // next door): it tells nothing about how fast the room loses heat, so it is left out rather
        // than counted as "does not cool" (which would store too little heat for an off period).
        if (!w.heating && gap >= kMinIndoorOutdoorGap && w.rate < 0) losses.push_back(-w.rate / gap);
    }
    m.coolingStretches = static_cast<int>(losses.size());
    m.loss = losses.empty() ? 0.0 : median(losses);
    std::vector<double> gains;
    for (const auto& w : ws) {
        if (w.heating) gains.push_back(w.rate + m.loss * (w.indoor - w.outdoor));
    }
    m.heatingStretches = static_cast<int>(gains.size());
    m.gain = gains.empty() ? 0.0 : median(gains);
    return m;
}

WarmupEstimate estimateWarmup(const ThermalModel& m, double from, double target, double outdoor) {
    if (from >= target) return {minutes{0}, true};
    double hoursNeeded;
    bool reachable = true;
    if (m.loss <= 1e-6) {
        hoursNeeded = (target - from) / m.gain;
    } else {
        const double equilibrium = outdoor + m.gain / m.loss;  // where full heat levels off
        if (equilibrium <= target + 0.3) {
            reachable = false;
            hoursNeeded = duration<double, std::ratio<3600>>(kCap).count();
        } else {
            hoursNeeded = std::log((equilibrium - from) / (equilibrium - target)) / m.loss;
        }
    }
    auto d = duration_cast<minutes>(duration<double, std::ratio<3600>>(hoursNeeded * 1.25)) + minutes{30};
    if (d > kCap) {
        d = kCap;
        reachable = reachable && hoursNeeded * 1.25 <= 48;
    }
    return {d, reachable};
}

}  // namespace hb
