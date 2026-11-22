#include "core/offline.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <regex>

namespace hb {

using namespace std::chrono;

OfflinePeriod currentOrNextOfflinePeriod(const DayWindow& window, const TimeZone& tz, Instant now) {
    const int length = ((window.toMinute - window.fromMinute) % 1440 + 1440) % 1440;
    const auto today = floor<days>(tz.toLocal(now));
    // Yesterday's occurrence may still be running (windows usually cross midnight).
    for (int d = -1; d <= 1; ++d) {
        const Instant start = tz.toInstant(today + days{d} + minutes{window.fromMinute});
        const Instant end = start + minutes{length == 0 ? 1440 : length};
        if (end > now) return {start, end};
    }
    const Instant start = tz.toInstant(today + days{2} + minutes{window.fromMinute});
    return {start, start + minutes{length == 0 ? 1440 : length}};
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

enum class Group { Off, On, Heat, Keep };

}  // namespace

std::vector<OnSpan> offlineSchedule(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz,
                                    Instant from, Instant to, const OfflineDuty& duty,
                                    std::optional<AwayMode> away) {
    // Classify every 15-minute slot.
    struct Slot {
        Instant start;
        Group group;
    };
    std::vector<Slot> slots;
    for (Instant t = floorToSlot(from); t < to; t += kSlotLength) {
        PlannerInput in;
        in.now = t;
        in.away = away;
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
        } else if (d.inputs.mode == "off") {
            g = Group::Off;
        } else if (d.inputs.mode == "keep" || d.inputs.mode == "away") {
            g = Group::Keep;
        } else {
            g = Group::Heat;
        }
        slots.push_back({t, g});
    }

    // Heat/keep runs: the room's duty share, in the run's cheapest slots.
    std::vector<bool> on(slots.size(), false);
    for (std::size_t i = 0; i < slots.size();) {
        std::size_t j = i;
        while (j < slots.size() && slots[j].group == slots[i].group) ++j;
        const Group g = slots[i].group;
        if (g == Group::On) {
            for (std::size_t k = i; k < j; ++k) on[k] = true;
        } else if (g == Group::Heat || g == Group::Keep) {
            const double share = std::clamp(g == Group::Heat ? duty.heat : duty.keep, 0.0, 1.0);
            const auto count = static_cast<std::size_t>(std::lround(share * static_cast<double>(j - i)));
            std::vector<std::size_t> idx;
            for (std::size_t k = i; k < j; ++k) idx.push_back(k);
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

std::optional<std::vector<std::string>> tasmotaScheduleRules(const std::vector<OnSpan>& spans,
                                                             minutes plugUtcOffset, std::size_t maxLength,
                                                             int maxRules) {
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
    static const std::regex ours(R"(^(ON Time#Minute=\d{1,4} DO Power1 [01] ENDON ?)*$)");
    return std::regex_match(rules, ours);
}

}  // namespace hb
