#include "core/window.hpp"

#include <algorithm>
#include <chrono>

namespace hb {

using namespace std::chrono;

std::optional<OpenWindowEvent> OpenWindowDetector::update(Instant measuredAt, double temperature,
                                                          const OpenWindowConfig& cfg) {
    if (!recent_.empty() && measuredAt <= recent_.back().first) return std::nullopt;
    while (!recent_.empty() && measuredAt - recent_.front().first > cfg.within) recent_.pop_front();
    recent_.emplace_back(measuredAt, temperature);
    if (!cfg.enabled || (event_ && measuredAt < event_->until)) return std::nullopt;

    const auto peak = std::ranges::max_element(recent_, {}, [](const auto& r) { return r.second; });
    const double drop = peak->second - temperature;
    if (drop < cfg.drop - 1e-9) return std::nullopt;
    event_ = OpenWindowEvent{measuredAt, measuredAt + cfg.pause, drop, duration_cast<minutes>(measuredAt - peak->first)};
    // Start afresh: after the pause only a new fall counts, not the one already seen.
    recent_.clear();
    recent_.emplace_back(measuredAt, temperature);
    return event_;
}

std::optional<OpenWindowEvent> OpenWindowDetector::pausedAt(Instant now) const {
    if (event_ && now >= event_->since && now < event_->until) return event_;
    return std::nullopt;
}

}  // namespace hb
