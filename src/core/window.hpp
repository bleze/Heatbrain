#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <utility>

#include "core/time.hpp"

namespace hb {

// Open-window detection: a room whose temperature falls fast (1 degree within half an hour) has a
// window or door open. Heating is then paused for a while instead of heating the street.
struct OpenWindowConfig {
    bool enabled = true;
    double drop = 1.0;                        // °C ...
    std::chrono::minutes within{30};          // ... within this long
    std::chrono::minutes pause{30};           // heating off this long after a fall
};

struct OpenWindowEvent {
    Instant since;     // when the fall was seen
    Instant until;     // heating paused until
    double drop = 0;   // how far it fell
    std::chrono::minutes over{0};  // over how long
};

class OpenWindowDetector {
public:
    // Feeds a room's reading (repeated readings with the same time are ignored). Returns the new
    // event when this reading shows a fast fall.
    std::optional<OpenWindowEvent> update(Instant measuredAt, double temperature, const OpenWindowConfig& cfg);
    // The pause in effect at `now`, if any.
    std::optional<OpenWindowEvent> pausedAt(Instant now) const;

private:
    std::deque<std::pair<Instant, double>> recent_;
    std::optional<OpenWindowEvent> event_;
};

}  // namespace hb
