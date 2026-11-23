#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/alerts.hpp"
#include "core/time.hpp"

namespace hb {

// Air quality from the CO2 the room sensors measure (Netatmo indoor modules, VELUX): below 1000 ppm
// is fine, above 1400 the air is poor - time to air the room.
struct AirQualityConfig {
    bool enabled = true;
    int highPpm = 1400;                    // notify above this ...
    std::chrono::minutes debounce{20};     // ... once it has lasted this long
    int hysteresis = 200;                  // back to normal below highPpm - hysteresis
};

// Per room: a "CO2 high" problem while it lasts (the alert engine debounces it, holds it during quiet
// hours and says when it is over). A spell that starts and ends within the quiet hours would never
// be told, so those are collected for one message when the quiet hours end.
class AirQualityTracker {
public:
    // One room's latest CO2 (nullopt: no reading this step - an ongoing spell stays as it is).
    void update(const std::string& roomId, const std::string& roomName, std::optional<int> co2, Instant now, bool quiet,
                const TimeZone& tz, const AirQualityConfig& cfg, std::vector<Problem>& out);
    // Once, on the first step after the quiet hours: what happened in them. Call every step.
    std::optional<std::pair<std::string, std::string>> morningSummary(bool quietNow, const TimeZone& tz,
                                                                      const AirQualityConfig& cfg);

private:
    struct Spell {
        Instant start;
        int peak = 0;
        bool startedQuiet = false;
    };
    struct NightSpell {
        std::string room;
        Instant start, end;
        int peak = 0;
    };
    std::map<std::string, Spell> open_;
    std::vector<NightSpell> night_;
    bool wasQuiet_ = false;
};

}  // namespace hb
