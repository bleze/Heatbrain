#include "core/offline.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <regex>

namespace hb {

using namespace std::chrono;

OfflinePeriod currentOrNextOfflinePeriod(const WifiOffPlan& plan, const TimeZone& tz, Instant now) {
    const auto today = floor<days>(tz.toLocal(now));
    // One occurrence per END date; the window for that date depends on its weekday.
    std::optional<OfflinePeriod> best;
    for (int d = -1; d <= 3; ++d) {
        const local_days endDay = today + days{d};
        const weekday wd{endDay};
        const bool weekend = wd == Saturday || wd == Sunday;
        const DayWindow& w = weekend && plan.weekend ? *plan.weekend : plan.normal;
        const int length = ((w.toMinute - w.fromMinute) % 1440 + 1440) % 1440;
        const Instant end = tz.toInstant(endDay + minutes{w.toMinute});
        const Instant start = end - minutes{length == 0 ? 1440 : length};
        if (end <= now) continue;
        if (!best || start < best->start) best = OfflinePeriod{start, end};
    }
    return *best;
}

std::vector<ObservedGap> findReadingGaps(const std::vector<Instant>& times, Instant now, minutes minGap) {
    std::vector<ObservedGap> gaps;
    for (std::size_t i = 1; i < times.size(); ++i) {
        if (times[i] - times[i - 1] >= minGap) gaps.push_back({times[i - 1], times[i], false});
    }
    if (!times.empty() && now - times.back() >= minGap) gaps.push_back({times.back(), now, true});
    return gaps;
}

bool scheduleOnAt(const std::vector<OnSpan>& spans, Instant t) {
    return std::ranges::any_of(spans, [&](const OnSpan& s) { return t >= s.from && t < s.to; });
}

namespace {

std::optional<double> priceAt(const PriceTable& prices, Instant slot) {
    for (const auto& s : prices.slots) {
        if (s.start == slot) return s.total;
    }
    return std::nullopt;
}

enum class Group { Off, On, Heat, Keep, Warmup };

}  // namespace

std::vector<OnSpan> offlineSchedule(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz,
                                    Instant from, Instant to, const OfflineDuty& duty,
                                    std::optional<AwayMode> away, std::optional<OfflineStart> start,
                                    std::optional<ThermalModel> thermal) {
    // Classify every 15-minute slot.
    struct Slot {
        Instant start;
        Group group;
        std::optional<double> target;
    };
    std::vector<Slot> slots;
    for (Instant t = floorToSlot(from); t < to; t += kSlotLength) {
        PlannerInput in;
        in.now = t;
        in.away = away;
        in.thermal = thermal;
        if (start) { in.outdoorTemp = start->outdoor; in.forecast = start->forecast; }
        const auto period = periodAt(room, minuteOfDay(tz.toLocal(t)));
        // As in planAhead: a room exactly at its target, so only program and price decide.
        double assumed = period && period->point.mode != ProgramMode::Off ? period->point.target : room.minTemp + 1;
        if (away && away->activeAt(t)) assumed = away->keepAbove + 1;
        in.temperature = std::max(assumed, room.minTemp);
        in.measuredAt = t;
        const Decision d = decideRoom(room, in, prices, tz);
        Group g;
        if (room.priceOnly) {
            g = d.on ? Group::On : Group::Off;
        } else if (d.inputs.warmupTo && d.reason != Reason::PeakAvoid) {
            g = Group::Warmup;  // (expensive slots in it coast: a heat run of their own)
        } else if (d.inputs.mode == "off") {
            g = Group::Off;
        } else if (d.inputs.mode == "keep" || d.inputs.mode == "away") {
            g = Group::Keep;
        } else {
            g = Group::Heat;
        }
        slots.push_back({t, g, d.inputs.target});
    }

    // The room's temperature at the end of each slot if it is not heated: from the starting
    // temperature, cooling towards outdoors (forecast, else the outdoor reading, else 5 degrees).
    std::vector<std::optional<double>> unheated(slots.size());
    if (start) {
        double t = start->temperature;
        Instant at = start->at;
        for (std::size_t k = 0; k < slots.size(); ++k) {
            const Instant end = slots[k].start + kSlotLength;
            if (end > at) {
                const double out = (start->forecast ? start->forecast->at(end) : std::nullopt).value_or(start->outdoor.value_or(5.0));
                const double h = duration<double, std::ratio<3600>>(end - at).count();
                t = out + (t - out) * std::exp(-start->coolingRate * h);
                at = end;
            }
            unheated[k] = t;
        }
    }

    // Heat/keep runs: the room's duty share, in the run's cheapest slots.
    std::vector<bool> on(slots.size(), false);
    for (std::size_t i = 0; i < slots.size();) {
        std::size_t j = i;
        const Group g = slots[i].group;
        // A warm-up is one run, its target rising along the curve.
        while (j < slots.size() && slots[j].group == g && (g == Group::Warmup || slots[j].target == slots[i].target)) ++j;
        if (g == Group::On) {
            for (std::size_t k = i; k < j; ++k) on[k] = true;
        } else if (g == Group::Warmup) {
            // The curve assumes full heat from where it starts: on from when the unheated room would
            // be below it, to the end of the warm-up.
            std::size_t first = i;
            if (start) {
                while (first < j && unheated[first] && slots[first].target && *unheated[first] > *slots[first].target) ++first;
            }
            for (std::size_t k = first; k < j; ++k) on[k] = true;
        } else if (g == Group::Heat || g == Group::Keep) {
            // From when the unheated room would be at the target (with the hysteresis): before that,
            // heating is not needed. Without a starting temperature: the whole run.
            std::size_t first = i;
            if (start && slots[i].target) {
                const double need = *slots[i].target + kHeatBand;
                while (first < j && unheated[first] && *unheated[first] > need) ++first;
            }
            const double share = std::clamp(duty.forRun(g == Group::Keep, slots[i].target), 0.0, 1.0);
            const auto count = static_cast<std::size_t>(std::lround(share * static_cast<double>(j - first)));
            std::vector<std::size_t> idx;
            for (std::size_t k = first; k < j; ++k) idx.push_back(k);
            // Cheapest first; unknown prices last; ties keep time order (fewer switch points).
            std::ranges::stable_sort(idx, [&](std::size_t a, std::size_t b) {
                const auto pa = priceAt(prices, slots[a].start), pb = priceAt(prices, slots[b].start);
                if (pa.has_value() != pb.has_value()) return pa.has_value();
                return pa && pb && *pa < *pb;
            });
            for (std::size_t k = 0; k < count && k < idx.size(); ++k) on[idx[k]] = true;
        }
        i = j;
    }

    std::vector<OnSpan> spans;
    for (std::size_t k = 0; k < slots.size(); ++k) {
        if (!on[k]) continue;
        const Instant a = std::max(slots[k].start, from);
        const Instant b = std::min(slots[k].start + kSlotLength, to);
        if (a >= b) continue;
        if (!spans.empty() && spans.back().to == a) spans.back().to = b;
        else spans.push_back({a, b});
    }
    return spans;
}

std::optional<OfflinePeriod> plugRadioOffWindow(const OfflinePeriod& period) {
    if (period.end - period.start < minutes{20}) return std::nullopt;
    return OfflinePeriod{period.start + minutes{1}, period.end - minutes{2}};
}

std::optional<std::vector<std::string>> tasmotaScheduleRules(const std::vector<OnSpan>& spans,
                                                             minutes plugUtcOffset,
                                                             std::optional<OfflinePeriod> radioOff,
                                                             std::size_t maxLength, int maxRules) {
    const auto plugMinute = [&](Instant t) {
        const auto m = duration_cast<minutes>(t.time_since_epoch()) + plugUtcOffset;
        return static_cast<int>(((m.count() % 1440) + 1440) % 1440);
    };
    // One trigger per edge; a later edge in the same minute wins (back-to-back spans merge).
    std::vector<std::pair<Instant, bool>> ordered;
    for (const auto& s : spans) {
        ordered.push_back({s.from, true});
        ordered.push_back({s.to, false});
    }
    std::vector<std::string> parts;
    if (radioOff) {
        // First, so they always fit (the rule set is cut at its end) and land in Rule2.
        parts.push_back(std::format("ON Time#Minute={} DO Wifi 0 ENDON", plugMinute(radioOff->start)));
        parts.push_back(std::format("ON Time#Minute={} DO Wifi 1 ENDON", plugMinute(radioOff->end)));
    }
    std::map<int, std::size_t> seen;
    for (const auto& [t, state] : ordered) {
        const int m = plugMinute(t);
        const std::string part = std::format("ON Time#Minute={} DO Power1 {} ENDON", m, state ? 1 : 0);
        if (auto it = seen.find(m); it != seen.end()) {
            parts[it->second] = part;
        } else {
            seen[m] = parts.size();
            parts.push_back(part);
        }
    }
    std::vector<std::string> rules;
    std::string current;
    for (const auto& p : parts) {
        if (!current.empty() && current.size() + 1 + p.size() > maxLength) {
            rules.push_back(current);
            current.clear();
        }
        current += (current.empty() ? "" : " ") + p;
    }
    if (!current.empty()) rules.push_back(current);
    if (static_cast<int>(rules.size()) > maxRules) return std::nullopt;
    return rules;
}

bool isHeatbrainScheduleRule(const std::string& rules) {
    static const std::regex ours(R"(^(ON Time#Minute=\d{1,4} DO (Power1|Wifi) [01] ENDON ?)*$)");
    return std::regex_match(rules, ours);
}

}  // namespace hb
