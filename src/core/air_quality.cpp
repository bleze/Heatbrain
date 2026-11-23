#include "core/air_quality.hpp"

#include <algorithm>
#include <format>

namespace hb {

using namespace std::chrono;

void AirQualityTracker::update(const std::string& roomId, const std::string& roomName, std::optional<int> co2, Instant now,
                               bool quiet, const TimeZone& tz, const AirQualityConfig& cfg, std::vector<Problem>& out) {
    if (!cfg.enabled) {
        open_.erase(roomId);
        return;
    }
    auto it = open_.find(roomId);
    if (co2) {
        if (*co2 >= cfg.highPpm) {
            if (it == open_.end()) it = open_.emplace(roomId, Spell{now, *co2, quiet}).first;
            it->second.peak = std::max(it->second.peak, *co2);
        } else if (it != open_.end() && *co2 < cfg.highPpm - cfg.hysteresis) {
            // Over. One that started and ended in the quiet hours was never told: keep it for the morning.
            if (it->second.startedQuiet && quiet) night_.push_back({roomName, it->second.start, now, it->second.peak});
            open_.erase(it);
            return;
        }
    }
    if (it == open_.end()) return;
    const auto& s = it->second;
    const std::string now_ = co2 ? std::format("{} ppm", *co2) : std::string("high");
    out.push_back({"room:" + roomId + ":co2", Severity::Warning,
                   std::format("{}: CO₂ {} since {} (highest {} ppm) - time to air the room.", roomName, now_,
                               formatLocalHm(tz.toLocal(s.start)), s.peak),
                   cfg.debounce, hours{3}});
}

std::optional<std::pair<std::string, std::string>> AirQualityTracker::morningSummary(bool quietNow, const TimeZone& tz,
                                                                                     const AirQualityConfig& cfg) {
    const bool ended = wasQuiet_ && !quietNow;
    wasQuiet_ = quietNow;
    if (!ended || night_.empty()) return std::nullopt;
    std::string text;
    for (const auto& n : night_) {
        if (!text.empty()) text += "\n";
        text += std::format("{}: CO₂ above {} ppm {}-{}, highest {} ppm.", n.room, cfg.highPpm,
                            formatLocalHm(tz.toLocal(n.start)), formatLocalHm(tz.toLocal(n.end)), n.peak);
    }
    text += "\nA little fresh air at night (a vent or a window ajar) keeps it below 1000.";
    night_.clear();
    return std::pair{std::string("Air quality last night"), text};
}

}  // namespace hb
