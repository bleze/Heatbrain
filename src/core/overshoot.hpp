#pragma once

// How far a room keeps warming after its heat is switched off: a radiator stays hot for a while and
// a sensor reports with a delay, so a room heated "to 21.8" ends up at 22.1. heatbrain learns this
// per room and switches off that much earlier, so the line runs flatter without switching more.

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "core/time.hpp"

namespace hb {

struct HeatSwitch {
    Instant at;
    bool on = false;
    bool usable = true;  // decided by heatbrain on a real temperature (not limp mode, a night schedule, manual, ...)
};

// Only the latest switch-offs count (the after-heat drifts with the weather and the radiator's knob).
inline constexpr std::size_t kRecentCycles = 7;

// The median rise after a switch-off, over the latest kRecentCycles switch-offs that tell it: heated
// at least 10 minutes before, not heated again for at least 45 minutes after, a reading at most 15
// minutes before the switch-off and some after it. Looks up to 90 minutes past each. Nullopt with
// fewer than 3 of them.
std::optional<double> learnOvershoot(const std::vector<HeatSwitch>& switches,
                                     const std::vector<std::pair<Instant, double>>& temperatures);

}  // namespace hb
