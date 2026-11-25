#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <utility>

namespace hb {

// Share of time a room heats when it can't measure: per program mode.
struct OfflineDuty {
    double heat = 0.25;  // "heat to" periods
    double keep = 0.25;  // "keep above" periods (and away)
    // Learned per target: a 16-degree night needs far less than a 20-degree day.
    // Key: (keep ? 1 : 0, target in tenths of a degree).
    std::map<std::pair<int, int>, double> byTarget;
    bool learned = false;  // from the room's own history (else the shares above are guesses)
    static std::pair<int, int> key(bool keep, double target) {
        return {keep ? 1 : 0, static_cast<int>(std::lround(target * 10))};
    }
    // The share for a run: learned for this target if known, else the mode's share - but no more
    // than 15 min per hour, since an unseen target is usually a lower night setting.
    double forRun(bool keep, std::optional<double> target) const {
        if (target) {
            if (auto it = byTarget.find(key(keep, *target)); it != byTarget.end()) return it->second;
        }
        return std::min(keep ? this->keep : heat, 0.25);
    }
};

}  // namespace hb
