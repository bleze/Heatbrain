#include "core/overshoot.hpp"

#include <algorithm>

namespace hb {

using namespace std::chrono;

std::optional<double> learnOvershoot(const std::vector<HeatSwitch>& switches,
                                     const std::vector<std::pair<Instant, double>>& temperatures) {
    std::vector<double> rises;
    for (std::size_t i = 1; i < switches.size(); ++i) {
        const auto& off = switches[i];
        const auto& on = switches[i - 1];
        if (off.on || !on.on || !off.usable || !on.usable || off.at - on.at < minutes{10}) continue;
        Instant until = off.at + minutes{90};
        if (i + 1 < switches.size()) {
            if (switches[i + 1].at - off.at < minutes{45}) continue;  // heated again: the rise is cut short
            until = std::min(until, switches[i + 1].at);
        }
        // The reading at the switch-off (the last one before it), then the highest after it.
        std::optional<double> start, peak;
        for (const auto& [at, t] : temperatures) {
            if (at <= off.at) {
                if (off.at - at <= minutes{15}) start = t;
            } else if (at <= until) {
                peak = peak ? std::max(*peak, t) : t;
            }
        }
        if (!start || !peak) continue;
        rises.push_back(std::max(0.0, *peak - *start));
    }
    if (rises.size() < 3) return std::nullopt;
    std::sort(rises.begin(), rises.end());
    const std::size_t n = rises.size();
    const double median = n % 2 ? rises[n / 2] : (rises[n / 2 - 1] + rises[n / 2]) / 2;
    return std::clamp(median, 0.0, 1.5);
}

}  // namespace hb
