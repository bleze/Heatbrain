#include "core/planner.hpp"

#include <cmath>

#include <algorithm>
#include <map>
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

double outdoorFactor(double target, std::optional<double> outdoor) {
    if (!outdoor) return 1.0;
    return std::clamp((target - *outdoor) / 15.0, 0.5, 1.5);
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
        case Reason::OfflineSchedule: return "OFFLINE_SCHEDULE";
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

    // Choose whole hours (average of their slots), not single 15-minute slots: the relay
    // then switches at most once an hour instead of every quarter.
    const std::string today = formatLocalDate(local);
    const Instant slotNow = floorToSlot(in.now);
    std::optional<double> current;
    std::map<Instant, std::pair<double, int>> hoursOfDay;  // UTC hour start -> (sum, count)
    for (const auto& s : prices.slots) {
        if (s.start == slotNow) current = s.total;
        if (!s.total || formatLocalDate(tz.toLocal(s.start)) != today || avoided(s.start)) continue;
        auto& h = hoursOfDay[floor<hours>(s.start)];
        h.first += *s.total;
        h.second += 1;
    }
    inputs.price = current;
    const auto thisHour = hoursOfDay.find(floor<hours>(in.now));
    if (!current || thisHour == hoursOfDay.end()) {
        return make(true, Reason::NoPricesFallback, "No prices available: heating outside no-heat windows",
                    inputs);
    }
    std::vector<double> averages;
    for (const auto& [h, sc] : hoursOfDay) averages.push_back(sc.first / sc.second);
    std::ranges::sort(averages);
    const std::size_t k = std::min(averages.size(), static_cast<std::size_t>(cfg.hoursPerDay));
    const double hourAvg = thisHour->second.first / thisHour->second.second;
    if (k > 0 && hourAvg <= averages[k - 1]) {
        return make(true, Reason::CheapHours,
                    std::format("Among today's {} cheapest hours ({:.2f} ≤ {:.2f} {}/kWh)", cfg.hoursPerDay, hourAvg,
                                averages[k - 1], prices.currency),
                    inputs);
    }
    return make(false, Reason::NotCheapHours,
                std::format("Not among today's {} cheapest hours ({:.2f} > {:.2f} {}/kWh)", cfg.hoursPerDay, hourAvg,
                            k > 0 ? averages[k - 1] : 0.0, prices.currency),
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
    inputs.outdoorTemp = in.outdoorTemp;
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
    // A booking wins over both: heat to its target, starting `warmup` early.
    const bool booked = in.booking && in.booking->coversAt(in.now);
    if (booked) {
        mode = ProgramMode::Heat;
        target = in.booking->target;
        const auto hm = [&](Instant i) { return formatLocalHm(tz.toLocal(i)); };
        inputs.targetLabel = in.booking->warmingAt(in.now)
                                 ? std::format("warming up for the booking {}-{}", hm(in.booking->from), hm(in.booking->until))
                                 : std::format("booked until {}", hm(in.booking->until));
    }
    if (mode != ProgramMode::Off) inputs.target = target;
    inputs.mode = booked ? "booked" : away ? "away" : std::string(modeName(mode));

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

    // 7. Expensive now: coast, unless it gets too cold (not when booked: someone is there).
    if (outlook.expensive && !booked) {
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
        const double boost = s.preheatBoost * outdoorFactor(target, in.outdoorTemp);
        const double ceiling = std::min(target + boost, room.maxTemp - h);
        if (t < ceiling - h || (in.currentlyOn && t < ceiling)) {
            const std::string weather =
                in.outdoorTemp ? std::format(", +{:.1f}° as it is {} outside", boost, temp(*in.outdoorTemp)) : "";
            return make(true, Reason::PreheatBeforePeak,
                        std::format("Pre-heating to {} before expensive period at {} ({:.2f} vs {:.2f} {}/kWh now{})",
                                    temp(ceiling), formatLocalHm(tz.toLocal(*outlook.nextExpensiveAt)),
                                    *outlook.nextExpensivePrice, *outlook.current, currency, weather),
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

namespace hb {

std::optional<Booking> bookingAt(const std::vector<Booking>& bookings, Instant t) {
    for (const auto& b : bookings) {
        if (b.coversAt(t)) return b;
    }
    return std::nullopt;
}

std::vector<PlanSegment> planAhead(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz, Instant now,
                                   std::chrono::hours horizon, std::optional<AwayMode> away,
                                   std::optional<double> outdoorTemp, const std::vector<Booking>& bookings,
                                   std::optional<double> currentTemp, std::optional<double> coolingRate) {
    std::vector<PlanSegment> out;
    // The room's temperature as it is likely to go: it cools towards outdoors while not heated and is
    // back at the target after a heating period. Only used to tell "keep above" apart from "idle".
    std::optional<double> room_t = currentTemp;
    const double outside = outdoorTemp.value_or(5.0);
    const double cool = std::exp(-coolingRate.value_or(0.05) * 0.25);  // per 15 minutes; cautious default
    for (Instant t = floorToSlot(now); t < now + horizon; t += kSlotLength) {
        // The target in effect at t, so the planner sees a room that is exactly at target.
        PlannerInput in;
        in.now = t;
        in.away = away;
        in.outdoorTemp = outdoorTemp;
        const auto period = periodAt(room, minuteOfDay(tz.toLocal(t)));
        double assumed = period && period->point.mode != ProgramMode::Off ? period->point.target : room.minTemp + 1;
        if (away && away->activeAt(t)) assumed = away->keepAbove + 1;
        in.booking = bookingAt(bookings, t);
        if (in.booking) assumed = in.booking->target;
        in.temperature = std::max(assumed, room.minTemp);
        in.measuredAt = t;
        const Decision d = decideRoom(room, in, prices, tz);

        std::string kind;
        if (d.inputs.mode == "booked" && d.reason != Reason::AboveMax) kind = "booked";
        else switch (d.reason) {
            case Reason::PreheatBeforePeak: kind = "preheat"; break;
            case Reason::PeakAvoid:
            case Reason::BelowComfortDuringPeak: kind = "coast"; break;
            case Reason::CheapBoost: kind = "boost"; break;
            case Reason::KeepAbove:
            case Reason::KeepSatisfied: kind = d.inputs.mode == "away" ? "away" : "keep"; break;
            case Reason::ScheduledOff:
            case Reason::AvoidWindow:
            case Reason::NotCheapHours:
            case Reason::AwayOff: kind = d.inputs.mode == "away" ? "away" : "off"; break;
            case Reason::CheapHours:
            case Reason::NoPricesFallback: kind = "heat"; break;
            default: kind = room.priceOnly ? (d.on ? "heat" : "off") : "heat"; break;
        }
        if (room_t) {
            if (kind == "keep" || kind == "off" || kind == "away") {
                *room_t = outside + (*room_t - outside) * cool;
                const double floor = kind == "off" ? room.frostTemp : d.inputs.target.value_or(room.frostTemp);
                if (kind == "keep" && *room_t > floor + room.strategy.hysteresis) kind = "idle";
                *room_t = std::max(*room_t, floor);  // heating holds it at the floor
            } else if (d.inputs.target && (kind == "heat" || kind == "preheat" || kind == "boost" || kind == "coast")) {
                // A heat period whose target (with any boost) is below where the room still is: no heat
                // either - a room "heated to 5 deg" at 16 deg just cools.
                const double ceiling = *d.inputs.target + (kind == "preheat" ? room.strategy.preheatBoost
                                                           : kind == "boost"  ? room.strategy.cheapBoost
                                                           : kind == "coast"  ? -room.strategy.peakAllowedDrop : 0.0);
                *room_t = outside + (*room_t - outside) * cool;
                if (*room_t > ceiling + room.strategy.hysteresis) kind = "idle";
                else *room_t = std::max(*room_t, ceiling);
            } else if (d.inputs.target) {
                *room_t = std::max(*room_t, *d.inputs.target - (kind == "coast" ? room.strategy.peakAllowedDrop : 0.0));
            }
        }
        const std::optional<double> target = (kind == "off") ? std::nullopt : d.inputs.target;
        if (!out.empty() && out.back().kind == kind && out.back().target == target) {
            out.back().to = t + kSlotLength;
        } else {
            out.push_back({t, t + kSlotLength, kind, target});
        }
    }
    return out;
}

}  // namespace hb
