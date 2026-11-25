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

std::optional<ProgramStep> nextStepUp(const RoomConfig& room, const TimeZone& tz, Instant now, minutes within) {
    const auto& p = room.program;
    if (p.size() < 2) return std::nullopt;
    const LocalTime local = tz.toLocal(now);
    const int m = minuteOfDay(local);
    const ProgramPoint* next = nullptr;
    int ahead = 0;
    for (const auto& pt : p) {
        int d = ((pt.atMinute - m) % 1440 + 1440) % 1440;
        if (d == 0) d = 1440;  // starts now: under way, the next one comes later
        if (!next || d < ahead) { next = &pt; ahead = d; }
    }
    const auto current = periodAt(room, m);
    if (!current || current->point.mode == ProgramMode::Off || next->mode == ProgramMode::Off) return std::nullopt;
    if (next->target < current->point.target + 0.5) return std::nullopt;
    const Instant at = tz.toInstant(floor<minutes>(local) + minutes{ahead});
    if (at - now > within) return std::nullopt;
    return ProgramStep{at, current->point.target, next->target, next->mode};
}

std::optional<CoastStep> nextCoast(const RoomConfig& room, const TimeZone& tz, Instant now, std::optional<double> learnedLoss,
                                   const OutdoorForecast* forecast, std::optional<double> outdoor, minutes within) {
    constexpr double kCautiousLoss = 0.05;  // per hour, as the plan assumes until the room is learned
    const auto& p = room.program;
    if (p.size() < 2) return std::nullopt;
    const LocalTime local = tz.toLocal(now);
    const int mod = minuteOfDay(local);
    std::size_t next = 0;
    int ahead = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        int d = ((p[i].atMinute - mod) % 1440 + 1440) % 1440;
        if (d == 0) d = 1440;
        if (i == 0 || d < ahead) { next = i; ahead = d; }
    }
    const auto current = periodAt(room, mod);
    if (!current || current->point.mode == ProgramMode::Off) return std::nullopt;
    const ProgramPoint& off = p[next];
    if (off.mode != ProgramMode::Off || !off.coastAbove) return std::nullopt;
    int length = ((p[(next + 1) % p.size()].atMinute - off.atMinute) % 1440 + 1440) % 1440;
    if (length == 0) length = 1440;
    CoastStep c;
    c.at = tz.toInstant(floor<minutes>(local) + minutes{ahead});
    if (c.at - now > within) return std::nullopt;
    c.until = c.at + minutes{length};
    c.floor = *off.coastAbove;
    c.outdoor = (forecast ? forecast->meanOver(c.at, c.until) : std::nullopt).value_or(outdoor.value_or(5.0));
    const double loss = learnedLoss && *learnedLoss > 0 ? *learnedLoss : kCautiousLoss;
    // Newton cooling backwards from the floor at the end: T(h) = out + (T0 - out) e^(-loss h).
    const double hours = length / 60.0;
    c.level = c.floor <= c.outdoor ? c.floor : c.outdoor + (c.floor - c.outdoor) * std::exp(std::min(loss * hours, 50.0));
    c.level = std::min(c.level, room.maxTemp - room.strategy.hysteresis);
    c.atEnd = c.level <= c.outdoor ? c.level : c.outdoor + (c.level - c.outdoor) * std::exp(-std::min(loss * hours, 50.0));
    return c;
}

bool continuesSegment(const PlanSegment& a, const PlanSegment& b) {
    return a.to == b.from && a.kind == b.kind && a.target == b.target && a.warmup == b.warmup && a.booked == b.booked &&
           a.coastAbove == b.coastAbove && !a.coastTo && !b.coastFrom;
}

namespace {

// Smooth max of a and b: equal to the larger one more than w apart, a parabola in between.
double smoothMax(double a, double b, double w) {
    const double d = a - b;
    if (d >= w) return a;
    if (d <= -w) return b;
    return b + (d + w) * (d + w) / (4 * w);
}
double smoothMin(double a, double b, double w) { return -smoothMax(-a, -b, w); }

}  // namespace

double warmupCurve(const RoomConfig& room, const ProgramStep& step, Instant t, const std::optional<ThermalModel>& m,
                   double outdoor, std::optional<minutes> ramp) {
    constexpr double kEarlyHours = 0.5;  // at the target this long before the step
    constexpr double kGainMargin = 0.8;  // heat as if a little weaker than learned
    const double lo = step.from, hi = step.target;
    // Hours of heating still to come before the room must be at `hi` (negative: past that point).
    const double tau = duration<double, std::ratio<3600>>(step.at - t).count() - kEarlyHours;
    std::optional<double> need;
    if (m && m->usable()) {
        const double gain = m->gain * kGainMargin;
        if (m->loss <= 1e-6) {
            need = hi - gain * tau;
        } else if (const double eq = outdoor + gain / m->loss; eq > hi + 0.3) {
            // Full heat follows T(h) = eq - (eq - T0) e^(-loss h); backwards from `hi`:
            need = eq - (eq - hi) * std::exp(std::min(m->loss * tau, 50.0));
        }
    }
    if (!need) {  // not learned, or out of reach by the model: a straight ramp over the warm-up setting
        const double len = std::max(0.25, duration<double, std::ratio<3600>>(ramp.value_or(room.warmup)).count() - kEarlyHours);
        need = hi - (hi - lo) * tau / len;
    }
    if (hi <= lo + 0.05) return std::max(lo, hi);
    const double w = std::min(0.4, (hi - lo) / 4);
    return smoothMin(smoothMax(*need, lo, w), hi, w);
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
        case Reason::WindowOpen: return "WINDOW_OPEN";
        case Reason::WarmUp: return "WARM_UP";
        case Reason::WarmUpOnTrack: return "WARM_UP_ON_TRACK";
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
        if (o.nextExpensiveAt) {
            // The stretch lasts until the first slot that is not expensive (or prices run out).
            Instant until = *o.nextExpensiveAt + kSlotLength;
            for (const auto& s : prices.slots) {
                if (s.start < until || s.start >= slotNow + hours{24}) continue;
                if (s.start != until || !s.total || !isExpensive(*s.total)) break;
                until += kSlotLength;
            }
            o.nextExpensiveUntil = until;
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
    inputs.sensor = room.sensor;
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
    // A booking wins over both: heat to its target, starting `warmup` early - along the warm-up
    // curve from what the room would have had otherwise, regardless of price.
    bool warming = false;
    bool warmingAnyPrice = false;  // towards a keep-above period or a booking: regardless of price
    Instant warmupAt{};            // when the warmer period or the booking begins
    const bool booked = in.booking && in.booking->coversAt(in.now);
    if (booked) {
        const double base = mode == ProgramMode::Off ? room.minTemp : target;
        mode = ProgramMode::Heat;
        target = in.booking->target;
        const auto hm = [&](Instant i) { return formatLocalHm(tz.toLocal(i)); };
        inputs.targetLabel = in.booking->warmingAt(in.now)
                                 ? std::format("warming up for the booking {}-{}", hm(in.booking->from), hm(in.booking->until))
                                 : std::format("booked until {}", hm(in.booking->until));
        if (in.booking->warmingAt(in.now) && base < target - 0.05) {
            const double outside = (in.forecast ? in.forecast->meanOver(in.now, in.booking->from) : std::nullopt)
                                       .value_or(in.outdoorTemp.value_or(5.0));
            const ProgramStep step{in.booking->from, base, in.booking->target, ProgramMode::Keep};
            warming = true;
            warmingAnyPrice = true;
            warmupAt = in.booking->from;
            target = std::max(base, warmupCurve(room, step, in.now, in.thermal, outside, in.booking->warmup));
            inputs.warmupTo = in.booking->target;
        }
    }
    // A warmer period ahead: the target follows the warm-up curve towards it, so the room is warm
    // when the period begins (not while booked or away: those replace the program).
    // An off period ahead that must not fall below a floor: heat is stored before it, along the same
    // curve, up to the level the room needs to coast through (regardless of price: the floor was asked for).
    std::string shortNote;  // the floor will not hold (max caps the stored heat)
    const auto coast = !booked && !away && mode != ProgramMode::Off
                           ? nextCoast(room, tz, in.now,
                                       in.coolingRate ? in.coolingRate
                                                      : in.thermal ? std::optional<double>(in.thermal->loss) : std::nullopt,
                                       in.forecast.get(), in.outdoorTemp)
                           : std::nullopt;
    if (coast && coast->level > target + 0.05) {
        const double outside = (in.forecast ? in.forecast->meanOver(in.now, coast->at) : std::nullopt)
                                   .value_or(in.outdoorTemp.value_or(5.0));
        const ProgramStep step{coast->at, target, coast->level, ProgramMode::Keep};
        const double curve = warmupCurve(room, step, in.now, in.thermal, outside,
                                         room.warmup > minutes{0} ? room.warmup : s.preheatWindow);
        if (curve > target + 0.05) {
            warming = true;
            warmingAnyPrice = true;
            warmupAt = coast->at;
            mode = ProgramMode::Heat;
            target = curve;
            inputs.warmupTo = coast->level;
            inputs.coastAbove = coast->floor;
            inputs.storing = true;
            // Max caps the stored heat: the period stays off, but say that the floor will not hold.
            if (coast->fallsShort()) {
                shortNote = std::format(". Max {} only lasts until about {} at {}: below {}", temp(room.maxTemp),
                                        temp(coast->atEnd), formatLocalHm(tz.toLocal(coast->until)), temp(coast->floor));
            }
            inputs.targetLabel = std::format(
                "storing heat for the off period {} (stay above {})",
                formatWindow(DayWindow{minuteOfDay(tz.toLocal(coast->at)), minuteOfDay(tz.toLocal(coast->until))}),
                temp(coast->floor));
        }
    }
    if (!coast && !booked && !away && mode != ProgramMode::Off && room.warmup > minutes{0}) {  // 0: no early start
        if (const auto step = nextStepUp(room, tz, in.now)) {
            const double outside = (in.forecast ? in.forecast->meanOver(in.now, step->at) : std::nullopt)
                                       .value_or(in.outdoorTemp.value_or(5.0));
            const double curve = warmupCurve(room, *step, in.now, in.thermal, outside);
            if (curve > target + 0.05) {
                warming = true;
                warmingAnyPrice = step->mode == ProgramMode::Keep;
                warmupAt = step->at;
                mode = ProgramMode::Heat;
                target = curve;
                inputs.warmupTo = step->target;
                inputs.targetLabel = std::format("warming up for {} at {}", temp(step->target),
                                                 formatLocalHm(tz.toLocal(step->at)));
            }
        }
    }
    if (mode != ProgramMode::Off) inputs.target = target;
    else if (!away && period && period->point.coastAbove) inputs.coastAbove = period->point.coastAbove;
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

    // 2b. Window open (the temperature fell fast): pause, except to prevent frost.
    if (in.windowOpen && in.windowOpen->until > in.now) {
        const std::string why = std::format("{} fell {:.1f}° in {} min: window open? Heating paused until {}",
                                            fresh ? temp(t) : std::string("The temperature"), in.windowOpen->drop,
                                            in.windowOpen->over.count(), formatLocalHm(tz.toLocal(in.windowOpen->until)));
        if (fresh && t < room.frostTemp) {
            return make(true, Reason::FrostProtection, why + std::format(", but below frost protection {}", temp(room.frostTemp)),
                        inputs);
        }
        return make(false, Reason::WindowOpen, why, inputs);
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
        if (inputs.coastAbove) {
            return make(false, Reason::ScheduledOff,
                        std::format("Off by program {}: coasting on stored heat, to stay above {}", inputs.targetLabel,
                                    temp(*inputs.coastAbove)),
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
    if (outlook.expensive && !booked && !warmingAnyPrice) {
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
    if (!warmingAnyPrice && outlook.nextExpensiveAt && *outlook.current < *outlook.nextExpensivePrice * kPreheatMaxRatio) {
        const Instant peak = *outlook.nextExpensiveAt;
        const Instant peakEnd = outlook.nextExpensiveUntil.value_or(peak + hours{1});
        // The outdoor temperature during the expensive stretch: forecast, else the current reading.
        std::optional<double> outside = in.forecast ? in.forecast->meanOver(peak, peakEnd) : std::nullopt;
        const bool forecast = outside.has_value();
        if (!outside) outside = in.outdoorTemp;
        double boost = s.preheatBoost * outdoorFactor(target, outside);
        std::string why = outside ? std::format(", +{:.1f}° as it will be {} outside", boost, temp(*outside)) : "";
        bool due = true;
        if (in.thermal && in.thermal->usable()) {
            // Learned: just enough to coast through the stretch, at most twice the boost setting, and
            // started only as early as heating that far takes (stored heat leaks away).
            const double needed = neededPreheat(*in.thermal, target, s.peakAllowedDrop, outside.value_or(5.0), peak, peakEnd);
            boost = std::min(needed, 2 * s.preheatBoost);
            const auto lead = estimateWarmup(*in.thermal, t, std::min(target + boost, room.maxTemp - h),
                                             in.outdoorTemp.value_or(outside.value_or(5.0)));
            due = boost > 0.05 && peak - in.now <= lead.duration;
            why = std::format(", +{:.1f}° to last until {}{}", boost, formatLocalHm(tz.toLocal(peakEnd)),
                              outside ? std::format(" at {} outside{}", temp(*outside), forecast ? " (forecast)" : "") : "");
        }
        const double ceiling = std::min(target + boost, room.maxTemp - h);
        if (due && (t < ceiling - h || (in.currentlyOn && t < ceiling))) {
            inputs.ceiling = ceiling;
            return make(true, Reason::PreheatBeforePeak,
                        std::format("Pre-heating to {} before expensive period at {} ({:.2f} vs {:.2f} {}/kWh now{})",
                                    temp(ceiling), formatLocalHm(tz.toLocal(peak)), *outlook.nextExpensivePrice,
                                    *outlook.current, currency, why),
                        inputs);
        }
    }

    // 9. Among the cheapest slots: a little extra.
    if (outlook.cheapest && !warmingAnyPrice) {
        const double ceiling = std::min(target + s.cheapBoost, room.maxTemp - h);
        if (t < ceiling - h || (in.currentlyOn && t < ceiling)) {
            inputs.ceiling = ceiling;
            return make(true, Reason::CheapBoost,
                        std::format("Among the cheapest slots ({:.2f} {}/kWh, median {:.2f}): heating to {}",
                                    *outlook.current, currency, *outlook.median, temp(ceiling)),
                        inputs);
        }
    }

    // 9b. Warming up for the next period or a booking: the curve instead of the plain thermostat.
    //     Towards a heat period, expensive slots still coast and pre-heating still stores heat before
    //     them (both against the curve), so a warm-up does not run through the peak; towards a
    //     keep-above period or a booking price does not matter. Heats below the curve, stops a little above it.
    if (warming) {
        const std::string what = booked ? "the booking" : inputs.storing ? "the off period" : "the next period";
        const std::string where = std::format("{} (curve {} now, {} at {})", temp(t), temp(target), temp(*inputs.warmupTo),
                                              formatLocalHm(tz.toLocal(warmupAt)));
        if (t < target || (in.currentlyOn && t < target + h)) {
            if (inputs.storing) {
                return make(true, Reason::WarmUp,
                            std::format("Storing heat for the off period (to stay above {}): {}{}", temp(*inputs.coastAbove),
                                        where, shortNote),
                            inputs);
            }
            return make(true, Reason::WarmUp, "Warming up for " + what + ": " + where, inputs);
        }
        return make(false, Reason::WarmUpOnTrack, "Warm enough for now, on track for " + what + ": " + where + shortNote,
                    inputs);
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

double neededPreheat(const ThermalModel& m, double target, double allowedDrop, double outdoor, Instant from,
                     Instant until) {
    const double hours = duration<double, std::ratio<3600>>(until - from).count();
    const double floor = target - allowedDrop;
    if (floor <= outdoor || m.loss <= 0) return 0.0;
    // Newton cooling: T(h) = out + (T0 - out) e^(-loss h) must still be >= floor at the end.
    const double start = outdoor + (floor - outdoor) * std::exp(m.loss * hours);
    return std::max(0.0, start - target);
}

std::optional<Booking> bookingAt(const std::vector<Booking>& bookings, Instant t) {
    for (const auto& b : bookings) {
        if (b.coversAt(t)) return b;
    }
    return std::nullopt;
}

std::vector<PlanSegment> planAhead(const RoomConfig& room, const PriceTable& prices, const TimeZone& tz, Instant now,
                                   std::chrono::hours horizon, std::optional<AwayMode> away,
                                   std::optional<double> outdoorTemp, const std::vector<Booking>& bookings,
                                   std::optional<double> currentTemp, std::optional<double> coolingRate,
                                   std::optional<ThermalModel> thermal, std::shared_ptr<const OutdoorForecast> forecast) {
    std::vector<PlanSegment> out;
    // The room's temperature as it is likely to go: it cools towards outdoors while not heated and is
    // back at the target after a heating period. Only used to tell "keep above" apart from "idle".
    std::optional<double> room_t = currentTemp;
    const double cool = std::exp(-coolingRate.value_or(0.05) * 0.25);  // per 15 minutes; cautious default
    for (Instant t = floorToSlot(now); t < now + horizon; t += kSlotLength) {
        // The target in effect at t, so the planner sees a room that is exactly at target.
        PlannerInput in;
        in.now = t;
        in.away = away;
        in.outdoorTemp = outdoorTemp;
        in.thermal = thermal;
        in.coolingRate = coolingRate;
        in.forecast = forecast;
        const double outside = (forecast ? forecast->at(t) : std::nullopt).value_or(outdoorTemp.value_or(5.0));
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
            case Reason::WarmUp:
            case Reason::WarmUpOnTrack: kind = d.inputs.storing ? "preheat" : "warmup"; break;
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
        std::optional<double> coastFrom, coastTo;
        if (room_t) {
            if (kind == "keep" || kind == "off" || kind == "away") {
                if (kind == "off") coastFrom = *room_t;
                *room_t = outside + (*room_t - outside) * cool;
                const double floor = kind == "off" ? room.frostTemp : d.inputs.target.value_or(room.frostTemp);
                if (kind == "keep" && *room_t > floor + room.strategy.hysteresis) kind = "idle";
                *room_t = std::max(*room_t, floor);  // heating holds it at the floor
                if (kind == "off") coastTo = *room_t;
            } else if (d.inputs.target &&
                       (kind == "heat" || kind == "preheat" || kind == "boost" || kind == "coast" || kind == "warmup")) {
                // A heat period whose target (with any boost) is below where the room still is: no heat
                // either - a room "heated to 5 deg" at 16 deg just cools.
                const double ceiling = d.inputs.ceiling.value_or(
                    *d.inputs.target + (kind == "coast" ? -room.strategy.peakAllowedDrop : 0.0));
                *room_t = outside + (*room_t - outside) * cool;
                if (*room_t > ceiling + room.strategy.hysteresis) kind = "idle";
                else *room_t = std::max(*room_t, ceiling);
            } else if (d.inputs.target) {
                *room_t = std::max(*room_t, *d.inputs.target - (kind == "coast" ? room.strategy.peakAllowedDrop : 0.0));
            }
        }
        const std::optional<double> target = (kind == "off") ? std::nullopt : d.inputs.target;
        const bool warmup = d.inputs.warmupTo.has_value();
        const bool booked = d.inputs.mode == "booked";
        PlanSegment seg{t, t + kSlotLength, kind, target, warmup, booked, coastFrom, coastTo,
                        kind == "off" ? d.inputs.coastAbove : std::nullopt};
        if (!out.empty() && continuesSegment(out.back(), seg)) {
            out.back().to = seg.to;
        } else {
            out.push_back(std::move(seg));
        }
    }
    return out;
}

}  // namespace hb
