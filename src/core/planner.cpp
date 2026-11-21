#include "core/planner.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace hb {

using namespace std::chrono;

namespace {

// Price classification needs a clear spread; on flat days nothing is "expensive".
constexpr double kExpensiveVsMedian = 1.15;
constexpr double kCheapVsMedian = 0.90;
constexpr double kPreheatMaxRatio = 0.85;  // only pre-heat if now is clearly cheaper than the peak
constexpr std::size_t kMinSlotsForRanking = 8;

std::string temp(double t) { return std::format("{:.1f}°C", t); }

}  // namespace

std::string_view modeName(ProgramMode m) {
    switch (m) {
        case ProgramMode::Heat: return "heat";
        case ProgramMode::Keep: return "keep";
        case ProgramMode::Off: return "off";
    }
    return "?";
}

std::optional<ActivePeriod> periodAt(const RoomConfig& room, int minute) {
    const auto& p = room.program;
    if (p.empty()) return std::nullopt;
    std::size_t idx = p.size() - 1;  // before the first point: the last one is still running
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i].atMinute <= minute) idx = i;
    }
    ActivePeriod a;
    a.point = p[idx];
    a.fromMinute = p[idx].atMinute;
    a.toMinute = p[(idx + 1) % p.size()].atMinute;
    a.label = p.size() == 1 ? "all day" : formatWindow(DayWindow{a.fromMinute, a.toMinute});
    return a;
}

std::string formatWindow(const DayWindow& w) {
    return std::format("{:02}:{:02}-{:02}:{:02}", w.fromMinute / 60, w.fromMinute % 60, w.toMinute / 60,
                       w.toMinute % 60);
}

std::string_view reasonCode(Reason r) {
    switch (r) {
        case Reason::AboveMax: return "ABOVE_MAX";
        case Reason::StaleDataLimp: return "STALE_DATA_LIMP";
        case Reason::ManualOn: return "MANUAL_ON";
        case Reason::ManualOff: return "MANUAL_OFF";
        case Reason::FrostProtection: return "FROST_PROTECTION";
        case Reason::BelowMin: return "BELOW_MIN";
        case Reason::BelowComfortDuringPeak: return "BELOW_COMFORT_DURING_PEAK";
        case Reason::PeakAvoid: return "PEAK_AVOID";
        case Reason::PreheatBeforePeak: return "PREHEAT_BEFORE_PEAK";
        case Reason::CheapBoost: return "CHEAP_BOOST";
        case Reason::BelowTarget: return "BELOW_TARGET";
        case Reason::TargetReached: return "TARGET_REACHED";
        case Reason::HoldOn: return "HOLD_ON";
        case Reason::HoldOff: return "HOLD_OFF";
        case Reason::CheapHours: return "CHEAP_HOURS";
        case Reason::NotCheapHours: return "NOT_CHEAP_HOURS";
        case Reason::AvoidWindow: return "AVOID_WINDOW";
        case Reason::ScheduledOff: return "SCHEDULED_OFF";
        case Reason::KeepAbove: return "KEEP_ABOVE";
        case Reason::KeepSatisfied: return "KEEP_SATISFIED";
        case Reason::AwayOff: return "AWAY_OFF";
        case Reason::NoPricesFallback: return "NO_PRICES_FALLBACK";
    }
    return "UNKNOWN";
}

PriceOutlook priceOutlook(const PriceTable& prices, Instant now, minutes lookAhead) {
    PriceOutlook o;
    const Instant slotNow = floorToSlot(now);
    std::vector<double> window;
    for (const auto& s : prices.slots) {
        if (s.start == slotNow) o.current = s.total;
        if (s.start >= slotNow && s.start < slotNow + hours{24} && s.total) window.push_back(*s.total);
    }
    if (!o.current || window.size() < kMinSlotsForRanking) return o;

    std::ranges::sort(window);
    const double p25 = window[window.size() / 4];
    const double p75 = window[window.size() * 3 / 4];
    o.median = window[window.size() / 2];
    const auto isExpensive = [&](double p) { return p >= p75 && p >= *o.median * kExpensiveVsMedian; };
    o.expensive = isExpensive(*o.current);
    o.cheapest = *o.current <= p25 && *o.current <= *o.median * kCheapVsMedian;

    if (!o.expensive) {
        for (const auto& s : prices.slots) {
            if (s.start <= slotNow || s.start > now + lookAhead || !s.total) continue;
            if (isExpensive(*s.total)) {
                o.nextExpensiveAt = s.start;
                o.nextExpensivePrice = s.total;
                break;
            }
        }
    }
    return o;
}

namespace {

Decision make(bool on, Reason r, std::string detail, const DecisionInputs& in) {
    return Decision{on, r, std::move(detail), in};
}

Decision decidePriceOnly(const RoomConfig& room, const PlannerInput& in, const PriceTable& prices,
                         const TimeZone& tz, DecisionInputs inputs) {
    const auto& cfg = *room.priceOnly;
    const auto local = tz.toLocal(in.now);
    if (in.away && in.away->activeAt(in.now)) {
        inputs.mode = "away";
        return make(false, Reason::AwayOff, "Away until " + formatLocalHm(tz.toLocal(in.away->until)) + ": off", inputs);
    }
    for (const auto& s : prices.slots) {
        if (s.start == floorToSlot(in.now)) inputs.price = s.total;  // logged even when not used
    }
    for (const auto& w : cfg.avoid) {
        if (w.contains(minuteOfDay(local))) {
            return make(false, Reason::AvoidWindow, "In no-heat window " + formatWindow(w), inputs);
        }
    }
    const auto avoided = [&](Instant t) {
        const int m = minuteOfDay(tz.toLocal(t));
        return std::ranges::any_of(cfg.avoid, [m](const DayWindow& w) { return w.contains(m); });
    };

    const std::string today = formatLocalDate(local);
    const Instant slotNow = floorToSlot(in.now);
    std::optional<double> current;
    std::vector<double> day;
    for (const auto& s : prices.slots) {
        if (!s.total || formatLocalDate(tz.toLocal(s.start)) != today || avoided(s.start)) continue;
        day.push_back(*s.total);
        if (s.start == slotNow) current = s.total;
    }
    inputs.price = current;
    if (!current || day.empty()) {
        return make(true, Reason::NoPricesFallback, "No prices available: heating outside no-heat windows",
                    inputs);
    }
    std::ranges::sort(day);
    const std::size_t k = std::min(day.size(), static_cast<std::size_t>(cfg.hoursPerDay) * 4);
    const double threshold = day[k - 1];
    if (*current <= threshold) {
        return make(true, Reason::CheapHours,
                    std::format("Among today's {} cheapest hours ({:.2f} ≤ {:.2f} {}/kWh)", cfg.hoursPerDay,
                                *current, threshold, prices.currency),
                    inputs);
    }
    return make(false, Reason::NotCheapHours,
                std::format("Not among today's {} cheapest hours ({:.2f} > {:.2f} {}/kWh)", cfg.hoursPerDay,
                            *current, threshold, prices.currency),
                inputs);
}

}  // namespace

Decision decideRoom(const RoomConfig& room, const PlannerInput& in, const PriceTable& prices,
                    const TimeZone& tz) {
    const Strategy& s = room.strategy;
    DecisionInputs inputs;
    inputs.minTemp = room.minTemp;
    inputs.maxTemp = room.maxTemp;
    inputs.temperature = in.temperature;
    if (in.measuredAt) {
        inputs.temperatureAgeMinutes = static_cast<int>(duration_cast<minutes>(in.now - *in.measuredAt).count());
    }

    if (room.priceOnly) return decidePriceOnly(room, in, prices, tz, inputs);

    const PriceOutlook outlook = priceOutlook(prices, in.now, s.preheatWindow);
    inputs.price = outlook.current;
    inputs.medianPrice = outlook.median;
    inputs.priceExpensive = outlook.expensive;
    inputs.priceCheapest = outlook.cheapest;

    const auto period = periodAt(room, minuteOfDay(tz.toLocal(in.now)));
    ProgramMode mode = period ? period->point.mode : ProgramMode::Off;
    double target = period ? period->point.target : room.minTemp;
    inputs.targetLabel = period ? period->label : "no program";
    // Away/holiday replaces the program (off periods too): only keep above the away level.
    const bool away = in.away && in.away->activeAt(in.now);
    if (away) {
        mode = ProgramMode::Keep;
        target = in.away->keepAbove;
        inputs.targetLabel = "away until " + formatLocalDate(tz.toLocal(in.away->until)).substr(5) + " " +
                             formatLocalHm(tz.toLocal(in.away->until));
    }
    if (mode != ProgramMode::Off) inputs.target = target;
    inputs.mode = away ? "away" : std::string(modeName(mode));

    const bool fresh = in.temperature && in.measuredAt && in.now - *in.measuredAt <= s.staleAfter;
    const double t = in.temperature.value_or(0.0);
    const double h = s.hysteresis;
    const std::string currency = prices.currency;

    // 1. Hard ceiling, applies even to manual ON.
    if (fresh && t >= room.maxTemp) {
        return make(false, Reason::AboveMax, std::format("{} is at or above max {}", temp(t), temp(room.maxTemp)),
                    inputs);
    }

    // 2. Manual override.
    if (in.override && in.override->until > in.now) {
        const auto until = formatLocalHm(tz.toLocal(in.override->until));
        if (in.override->on) return make(true, Reason::ManualOn, "Manual ON until " + until, inputs);
        if (fresh && t < room.frostTemp) {
            return make(true, Reason::FrostProtection,
                        std::format("Manual OFF until {}, but {} is below frost protection {}", until, temp(t),
                                    temp(room.frostTemp)),
                        inputs);
        }
        return make(false, Reason::ManualOff, "Manual OFF until " + until, inputs);
    }

    // 3. "Off" in the program (e.g. the 17-21 peak tariff, or the night): off, except to
    //    prevent frost. Comes before limp mode, so missing data never heats here either.
    if (mode == ProgramMode::Off) {
        if (fresh && t < room.frostTemp) {
            return make(true, Reason::FrostProtection,
                        std::format("Off {}, but {} is below frost protection {}", inputs.targetLabel, temp(t),
                                    temp(room.frostTemp)),
                        inputs);
        }
        return make(false, Reason::ScheduledOff, "Off by program " + inputs.targetLabel, inputs);
    }

    // 4. No trustworthy temperature: bounded duty cycle, never continuous heat.
    if (!fresh) {
        const auto cycle = (s.limpOn + s.limpOff).count();
        const auto pos = cycle > 0 ? duration_cast<minutes>(in.now.time_since_epoch()).count() % cycle : 0;
        const bool on = s.limpOn.count() > 0 && pos < s.limpOn.count();
        const std::string why = !in.temperature ? "No temperature reading"
                                                : std::format("Temperature is {} min old", *inputs.temperatureAgeMinutes);
        return make(on, Reason::StaleDataLimp,
                    std::format("{}: limp mode {} min ON / {} min OFF", why, s.limpOn.count(), s.limpOff.count()),
                    inputs);
    }

    // 5. "Keep above": a floor only - no pre-heating or boosting, price does not matter.
    if (mode == ProgramMode::Keep) {
        const std::string where = std::format("{} (keep above {} for {})", temp(t), temp(target), inputs.targetLabel);
        if (t <= target - h || (in.currentlyOn && t < target + h)) {
            return make(true, Reason::KeepAbove, "Below the floor: " + where, inputs);
        }
        return make(false, Reason::KeepSatisfied, "Above the floor: " + where, inputs);
    }

    // 6. Hard minimum in heat periods, whatever the price.
    if (t < room.minTemp) {
        return make(true, Reason::BelowMin, std::format("{} is below minimum {}", temp(t), temp(room.minTemp)),
                    inputs);
    }

    // 7. Expensive now: coast, unless it gets too cold.
    if (outlook.expensive) {
        const double floor = target - s.peakAllowedDrop;
        if (t < floor || (in.currentlyOn && t < floor + h)) {
            return make(true, Reason::BelowComfortDuringPeak,
                        std::format("Expensive now ({:.2f} {}/kWh), but {} is below {} (target {} - {:.1f})",
                                    *outlook.current, currency, temp(t), temp(floor), temp(target), s.peakAllowedDrop),
                        inputs);
        }
        return make(false, Reason::PeakAvoid,
                    std::format("Expensive now ({:.2f} {}/kWh, median {:.2f}): coasting at {}, allowed down to {}",
                                *outlook.current, currency, *outlook.median, temp(t), temp(floor)),
                    inputs);
    }

    // 8. Cheap now, expensive soon: pre-heat.
    if (outlook.nextExpensiveAt && *outlook.current < *outlook.nextExpensivePrice * kPreheatMaxRatio) {
        const double ceiling = std::min(target + s.preheatBoost, room.maxTemp - h);
        if (t < ceiling - h || (in.currentlyOn && t < ceiling)) {
            return make(true, Reason::PreheatBeforePeak,
                        std::format("Pre-heating to {} before expensive period at {} ({:.2f} vs {:.2f} {}/kWh now)",
                                    temp(ceiling), formatLocalHm(tz.toLocal(*outlook.nextExpensiveAt)),
                                    *outlook.nextExpensivePrice, *outlook.current, currency),
                        inputs);
        }
    }

    // 9. Among the cheapest slots: a little extra.
    if (outlook.cheapest) {
        const double ceiling = std::min(target + s.cheapBoost, room.maxTemp - h);
        if (t < ceiling - h || (in.currentlyOn && t < ceiling)) {
            return make(true, Reason::CheapBoost,
                        std::format("Among the cheapest slots ({:.2f} {}/kWh, median {:.2f}): heating to {}",
                                    *outlook.current, currency, *outlook.median, temp(ceiling)),
                        inputs);
        }
    }

    // 10. Plain thermostat with hysteresis.
    const std::string where = std::format("{} (target {} for {})", temp(t), temp(target), inputs.targetLabel);
    if (t <= target - h) return make(true, Reason::BelowTarget, "Below target: " + where, inputs);
    if (t >= target + h) return make(false, Reason::TargetReached, "Target reached: " + where, inputs);
    return in.currentlyOn ? make(true, Reason::HoldOn, "Heating within band: " + where, inputs)
                          : make(false, Reason::HoldOff, "Within band: " + where, inputs);
}

}  // namespace hb
