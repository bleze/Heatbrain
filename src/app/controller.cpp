#include "app/controller.hpp"

#include <fstream>

#include <algorithm>
#include <format>

#include "core/log.hpp"
#include "core/version.hpp"
#include "sensors/netatmo.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

namespace {
constexpr auto kFailsafeRecheck = hours{1};
constexpr auto kHistoryRetention = days{90};
constexpr int kLowBatteryPercent = 20;
constexpr int kTomorrowPricesExpectedHour = 15;  // published ~13:00; alert if still missing at 15

void add(TickResult& r, std::string key, Severity sev, std::string msg, minutes debounce = minutes{0},
         std::optional<minutes> repeat = std::nullopt) {
    r.problems.push_back(Problem{std::move(key), sev, std::move(msg), debounce, repeat});
}
}  // namespace

// Problems that are expected while the Wi-Fi is off (and shortly after, while devices reconnect).
bool expectedWhileOffline(const std::string& key) {
    const auto endsWith = [&](std::string_view suffix) { return key.ends_with(suffix); };
    return (key.starts_with("plug:") && (endsWith(":unreachable") || endsWith(":mismatch") || endsWith(":failsafe"))) ||
           (key.starts_with("sensor:") && endsWith(":offline")) ||
           (key.starts_with("room:") && (endsWith(":limp") || endsWith(":not-warming")));
}
constexpr minutes kOfflineGrace{20};
// Schedules reach this far past the planned end: if the Wi-Fi comes back late, the plugs keep
// following a plan instead of falling into limp mode. Back early: heatbrain takes over at once.
constexpr hours kOfflineBuffer{3};

std::string spansText(const std::vector<OnSpan>& spans, const TimeZone& tz) {
    if (spans.empty()) return "off all period";
    std::string out = "on ";
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (i == 3) { out += std::format(" (+{} more)", spans.size() - 3); break; }
        out += std::format("{}{:%H:%M}-{:%H:%M}", i ? ", " : "", tz.toLocal(spans[i].from), tz.toLocal(spans[i].to));
    }
    return out;
}

// The plan with the Wi-Fi off period replaced by the night schedule.
std::vector<PlanSegment> overlayNight(const std::vector<PlanSegment>& plan, const OfflinePeriod& p,
                                      const std::vector<OnSpan>& spans) {
    std::vector<PlanSegment> out;
    const auto push = [&](Instant from, Instant to, const std::string& kind, std::optional<double> target) {
        if (from >= to) return;
        if (!out.empty() && out.back().kind == kind && out.back().target == target && out.back().to == from) {
            out.back().to = to;
        } else {
            out.push_back({from, to, kind, target});
        }
    };
    for (const auto& seg : plan) {
        for (Instant t = seg.from; t < seg.to; t += kSlotLength) {
            const Instant e = std::min(t + kSlotLength, seg.to);
            if (p.contains(t)) push(t, e, scheduleOnAt(spans, t) ? "night" : "nightoff", std::nullopt);
            else push(t, e, seg.kind, seg.target);
        }
    }
    return out;
}

json toJson(const DecisionInputs& in) {
    json j{{"minTemp", in.minTemp}, {"maxTemp", in.maxTemp}, {"targetLabel", in.targetLabel}, {"mode", in.mode},
           {"priceExpensive", in.priceExpensive}, {"priceCheapest", in.priceCheapest}};
    if (in.temperature) j["temperature"] = *in.temperature;
    if (in.temperatureAgeMinutes) j["temperatureAgeMinutes"] = *in.temperatureAgeMinutes;
    if (in.target) j["target"] = *in.target;
    if (in.price) j["price"] = *in.price;
    if (in.medianPrice) j["medianPrice"] = *in.medianPrice;
    if (in.outdoorTemp) j["outdoorTemp"] = *in.outdoorTemp;
    return j;
}

Controller::Controller(const TimeZone& tz, PriceModel& prices, std::vector<std::unique_ptr<TemperatureSource>>& sources,
                       std::vector<Plug>& plugs, std::vector<RoomConfig> rooms, const Overrides* overrides,
                       History* history)
    : tz_(tz),
      priceModel_(prices),
      sources_(sources),
      plugs_(plugs),
      rooms_(std::move(rooms)),
      overrides_(overrides),
      history_(history) {
    // Continue hysteresis where the previous run left off.
    if (history_) {
        for (const auto& r : rooms_) {
            if (auto last = history_->lastDecision(r.id)) lastOn_[r.id] = last->on;
        }
    }
}

void Controller::refreshPrices(Instant now, TickResult& result) {
    // From the start of the current hour to the end of tomorrow. Providers cache, so this
    // rarely touches the network.
    PriceTable fresh = priceModel_.build(floor<hours>(now), startOfLocalDay(tz_, now + hours{50}));
    for (const auto& e : fresh.errors) {
        add(result, "prices:error", Severity::Warning, "Price data problem: " + e, minutes{30});
    }
    prices_ = std::move(fresh);

    const auto local = tz_.toLocal(now);
    if (hourOfDay(local) >= kTomorrowPricesExpectedHour) {
        const Instant tomorrowNoon = tz_.toInstant(floor<days>(local) + days{1} + hours{12});
        const bool known = std::ranges::any_of(prices_.slots, [&](const PriceSlot& s) {
            return s.start == tomorrowNoon && s.total.has_value();
        });
        if (!known) {
            add(result, "prices:tomorrow", Severity::Warning,
                "Tomorrow's electricity prices are still missing. Planning falls back to the current day's data.");
        }
    }
}

void Controller::readSensors(TickResult& result) {
    sourcesAnswered_.clear();
    for (auto& src : sources_) {
        try {
            auto fresh = src->read();
            sourcesAnswered_.insert(src->id());
            for (auto& r : fresh) {
                const std::string key = src->id() + ":" + r.sensorId;
                if (!r.reachable) {
                    add(result, "sensor:" + key + ":offline", Severity::Warning,
                        std::format("Sensor '{}' is offline (no contact with the station)", r.name), minutes{30});
                }
                if (r.batteryPercent && *r.batteryPercent <= kLowBatteryPercent) {
                    add(result, "sensor:" + key + ":battery", Severity::Info,
                        std::format("Sensor '{}' battery is at {}% - replace soon", r.name, *r.batteryPercent),
                        minutes{0}, hours{24});
                }
                // An offline module reports no temperature at all. Keep the last good one
                // with its measurement time: the planner's age check (stale after 30 min)
                // then rides out a short restart instead of switching to limp mode at once.
                if (!r.temperature) {
                    if (auto it = readings_.find(key); it != readings_.end() && it->second.temperature) {
                        r.temperature = it->second.temperature;
                        r.humidity = it->second.humidity;
                        r.measuredAt = it->second.measuredAt;
                    }
                }
                readings_[key] = std::move(r);
            }
        } catch (const netatmo::AuthError& e) {
            add(result, "source:" + src->id() + ":auth", Severity::Critical, e.what());
        } catch (const std::exception& e) {
            // Keep the previous readings: they age, and the planner stops trusting them.
            add(result, "source:" + src->id() + ":error", Severity::Warning,
                std::format("Temperature source '{}' failed: {}", src->id(), e.what()), minutes{15});
        }
    }
}

const SensorReading* Controller::findSensor(const std::string& ref) const {
    const auto colon = ref.find(':');
    const std::string source = ref.substr(0, colon);
    const std::string key = ref.substr(colon + 1);
    if (auto it = readings_.find(ref); it != readings_.end()) return &it->second;
    for (const auto& [k, r] : readings_) {
        if (k.starts_with(source + ":") && r.name == key) return &r;
    }
    return nullptr;
}

std::string Controller::failsafeProblem(Plug& plug, Instant now) {
    auto it = failsafeChecks_.find(plug.id);
    if (it != failsafeChecks_.end() && it->second.first.empty() && now - it->second.second < kFailsafeRecheck) return {};
    std::string problem;
    try {
        const auto st = plug.sw->checkFailsafe();
        if (!st.installed()) problem = "its fail-safe is not installed (" + st.problems.front() + ")";
        // A recorded fail-safe test must match the firmware that is running now.
        if (problem.empty() && !dataDir_.empty()) {
            if (const auto rec = loadFirmwareRecord(dataDir_, plug.id)) {
                const auto status = plug.sw->status();
                if (!rec->verified) {
                    problem = "its last fail-safe test failed";
                } else if (compareVersions(status.firmware, rec->version) != 0) {
                    problem = "its firmware changed to " + status.firmware + " and the fail-safe test has not been run on it";
                }
            }
        }
    } catch (const std::exception& e) {
        problem = std::string("its fail-safe could not be checked: ") + e.what();
    }
    failsafeChecks_[plug.id] = {problem, now};
    return problem;
}

json Controller::applyToPlugs(const RoomConfig& room, bool on, Instant now, TickResult& result) {
    json out = json::array();
    for (const auto& id : room.plugIds) {
        auto it = std::ranges::find_if(plugs_, [&](const Plug& p) { return p.id == id; });
        Plug& plug = *it;
        json pj{{"id", id}, {"mode", plug.mode == PlugMode::Live ? "live" : "dryrun"}};
        if (auto armed = armedPlugs_.find(id); armed != armedPlugs_.end()) {
            pj["schedule"] = true;  // switching by itself; not reachable, and not to be touched
            pj["state"] = on;
        } else if (plug.mode != PlugMode::Live && dryRunHasFailsafe(plug, now)) {
            // Dry-run never switches, but a fail-safe on the plug would otherwise run limp mode
            // around the clock: keep its watchdog fed (quietly - dry-run problems are not alerts).
            try {
                const auto inOffline = armedPeriod_ && now < armedPeriod_->end + kOfflineBuffer;
                plug.sw->keepAlive(now, inOffline ? std::optional<Instant>(armedPeriod_->end + kOfflineBuffer)
                                                  : std::nullopt);
                pj["watchdog"] = true;
            } catch (const std::exception&) {
                dryRunFailsafe_.erase(id);  // check again once it answers
            }
        } else if (plug.mode == PlugMode::Live) {
            if (const std::string problem = failsafeProblem(plug, now); !problem.empty()) {
                pj["error"] = "fail-safe not verified: not switched";
                add(result, "plug:" + id + ":failsafe", Severity::Critical,
                    std::format("Plug '{}' is live, but {}, so heatbrain will not switch it. Fix it under "
                                "Settings → Plugs.",
                                id, problem));
            } else {
                try {
                    const bool state = plug.sw->set(on);
                    pj["state"] = state;
                    if (state != on) {
                        pj["error"] = "device reports a different state";
                        add(result, "plug:" + id + ":mismatch", Severity::Warning,
                            std::format("Plug '{}' was switched {} but reports {}", id, on ? "ON" : "OFF",
                                        state ? "ON" : "OFF"),
                            minutes{10});
                    }
                } catch (const std::exception& e) {
                    pj["error"] = e.what();
                    add(result, "plug:" + id + ":unreachable", Severity::Warning,
                        std::format("Plug '{}' ({}) is unreachable: {}", id, plug.host, e.what()), minutes{10});
                    failsafeChecks_.erase(id);  // re-verify once it is back
                }
            }
        }
        out.push_back(std::move(pj));
    }
    return out;
}

TickResult Controller::tick(Instant now, bool apply) {
    TickResult result;
    try {
        refreshPrices(now, result);
    } catch (const std::exception& e) {
        add(result, "prices:error", Severity::Warning, std::string("Price data problem: ") + e.what(), minutes{30});
    }
    readSensors(result);

    const auto overrides = overrides_ ? overrides_->load() : std::map<std::string, ManualOverride>{};
    const auto away = away_ ? away_->load() : std::nullopt;

    // Outdoor temperature: the configured sensor, else the first outdoor one; only if recent.
    std::optional<double> outdoor;
    {
        const SensorReading* o = outdoorSensor_.empty() ? nullptr : findSensor(outdoorSensor_);
        if (!o) {
            for (const auto& [k, r] : readings_) {
                if (r.kind == "outdoor") { o = &r; break; }
            }
        }
        std::string okey;
        for (const auto& [k, r] : readings_) {
            if (&r == o) okey = k;
        }
        if (o && o->temperature && o->measuredAt && now - *o->measuredAt < hours{1}) outdoor = o->temperature;
        outdoorUsed_.reset();
        if (outdoor) outdoorUsed_ = OutdoorUsed{okey, o->name, *outdoor};
    }

    // Wi-Fi off period: hand back after it, load the night schedule just before it.
    std::optional<OfflinePeriod> offline;
    if (offlineWindow_) offline = currentOrNextOfflinePeriod(*offlineWindow_, tz_, now);
    // Fresh readings after the period started: the network is back (Netatmo uses the same Wi-Fi).
    const bool networkBack = armedPeriod_ && now >= armedPeriod_->start && networkBackSince(armedPeriod_->start);
    if (apply) disarmOffline(now, networkBack);
    if (apply && offline && now >= offline->start - offlineLead_ && now < offline->end) {
        // Once per period: after an early takeover the schedules are not loaded again.
        const bool loaded = handledPeriodEnd_ == offline->end;
        if (!loaded) armOffline(*offline, now, away);
    }
    if (armedPeriod_ && !armedPlugs_.empty() && now >= armedPeriod_->end + kOfflineGrace) {
        std::string ids;
        for (const auto& [id, until] : armedPlugs_) ids += (ids.empty() ? "" : ", ") + id;
        add(result, "wifi:late", Severity::Info,
            std::format("The Wi-Fi seems to be off longer than planned (the period ended {:%H:%M}). {} keep "
                        "following their night schedule until {:%H:%M}.",
                        tz_.toLocal(armedPeriod_->end), ids, tz_.toLocal(armedPlugs_.begin()->second)));
    }
    for (const auto& [plugId, err] : offlineArmErrors_) {
        const std::string& why = err.first;
        add(result, "plug:" + plugId + ":night-schedule", Severity::Warning,
            std::format("Plug '{}': the schedule for the Wi-Fi off period could not be loaded ({}). It runs limp "
                        "mode (10 min on, 50 off) while the Wi-Fi is off.",
                        plugId, why));
    }

    for (const auto& room : rooms_) {
        const auto prev = lastOn_.find(room.id);
        PlannerInput in;
        in.now = now;
        in.currentlyOn = prev != lastOn_.end() && prev->second;
        const SensorReading* reading = room.sensor.empty() ? nullptr : findSensor(room.sensor);
        if (reading) {
            in.temperature = reading->temperature;
            in.measuredAt = reading->measuredAt;
        } else if (!room.sensor.empty() && !reading && sourcesAnswered_.contains(room.sensor.substr(0, room.sensor.find(':')))) {
            add(result, "room:" + room.id + ":sensor", Severity::Warning,
                std::format("Room '{}': sensor '{}' not found", room.name, room.sensor));
        }
        if (auto it = overrides.find(room.id); it != overrides.end()) in.override = it->second;
        in.away = away;
        in.outdoorTemp = outdoor;

        RoomResult rr;
        rr.room = &room;
        rr.decision = decideRoom(room, in, prices_, tz_);
        const auto armed = armedRooms_.find(room.id);
        const bool following = armed != armedRooms_.end() && now < armed->second.until && !networkBack;
        if (following) {
            rr.decision.on = scheduleOnAt(armed->second.spans, now);
            rr.decision.reason = Reason::OfflineSchedule;
            const auto& a = armed->second;
            rr.decision.detail =
                now < a.start
                    ? std::format("Wi-Fi goes off at {:%H:%M}: the schedule until {:%H:%M} is loaded onto the plug ({})",
                                  tz_.toLocal(a.start), tz_.toLocal(a.until), spansText(a.spans, tz_))
                    : std::format("Wi-Fi off until {:%H:%M}: the plug follows the schedule loaded at {:%H:%M} ({})",
                                  tz_.toLocal(a.until), tz_.toLocal(a.loadedAt), spansText(a.spans, tz_));
        }
        rr.changed = prev == lastOn_.end() || prev->second != rr.decision.on;
        rr.plan = planAhead(room, prices_, tz_, now, hours{24}, away, outdoor);
        if (offline && offline->start < now + hours{24}) {
            // Show the night as it will run: the loaded schedule, or what would be loaded now.
            const auto spans = following ? armed->second.spans
                                         : offlineSchedule(room, prices_, tz_, std::max(now, offline->start),
                                                           offline->end, learnedDuty(room.id, now), away);
            rr.plan = overlayNight(rr.plan, *offline, spans);
        }

        if (rr.decision.reason == Reason::StaleDataLimp) {
            add(result, "room:" + room.id + ":limp", Severity::Warning,
                std::format("Room '{}' is in limp mode: {}", room.name, rr.decision.detail), minutes{15});
        }
        if (rr.decision.on && rr.decision.inputs.temperature && *rr.decision.inputs.temperature < room.minTemp) {
            add(result, "room:" + room.id + ":not-warming", Severity::Warning,
                std::format("Room '{}' is {:.1f}°C, below its minimum {:.1f}°C, despite heating for an hour. "
                            "Window open, or radiator switched off?",
                            room.name, *rr.decision.inputs.temperature, room.minTemp),
                minutes{60});
        }

        if (apply) {
            rr.plugs = applyToPlugs(room, rr.decision.on, now, result);
            lastOn_[room.id] = rr.decision.on;
            if (history_) {
                history_->recordDecision({now, room.id, rr.decision.on, rr.changed,
                                          std::string(reasonCode(rr.decision.reason)), rr.decision.detail,
                                          toJson(rr.decision.inputs), rr.plugs});
            }
        }
        result.rooms.push_back(std::move(rr));
    }

    const bool inWindow = offline && now >= offline->start - offlineLead_ && now < offline->end + kOfflineGrace;
    const bool onSchedule = armedPeriod_ && !networkBack && !armedPlugs_.empty();  // late Wi-Fi: one note is enough
    if (inWindow || onSchedule) {
        std::erase_if(result.problems, [&](const Problem& p) {
            // Before the period only plugs following a schedule are exempt; inside it, everything expected.
            return expectedWhileOffline(p.key) && (onSchedule || now >= offline->start || p.key.starts_with("plug:"));
        });
    }

    if (apply) saveReadings();

    if (apply && history_) {
        try {
            for (const auto& [key, r] : readings_) {
                if (!r.temperature || !r.measuredAt) continue;
                std::string roomId;
                for (const auto& room : rooms_) {
                    if (const auto* mapped = room.sensor.empty() ? nullptr : findSensor(room.sensor); mapped == &r) {
                        roomId = room.id;
                    }
                }
                history_->recordTemperature({*r.measuredAt, key, roomId, *r.temperature, r.humidity});
            }
            history_->recordPrices(prices_);
            if (!lastPrune_ || now - *lastPrune_ > hours{24}) {
                history_->prune(now - kHistoryRetention);
                lastPrune_ = now;
            }
        } catch (const std::exception& e) {
            add(result, "history:error", Severity::Warning, std::string("Could not write history: ") + e.what());
        }
    }
    return result;
}

void Controller::setOfflineWindow(std::optional<WifiOffPlan> plan, minutes lead) {
    offlineWindow_ = plan;
    offlineLead_ = lead;
    loadOfflineState();
}

OfflineDuty Controller::learnedDuty(const std::string& roomId, Instant now) const {
    OfflineDuty duty;  // fallback until there is enough history: 15 min per hour
    if (!history_) return duty;
    int on[2] = {0, 0}, total[2] = {0, 0};
    std::map<std::pair<int, int>, std::pair<int, int>> perTarget;  // key -> (on, total)
    try {
        for (const auto& d : history_->decisions(roomId, now - days{7}, now, false)) {
            // Only decisions made on a real temperature by the thermostat logic.
            if (d.reason == "STALE_DATA_LIMP" || d.reason == "MANUAL_ON" || d.reason == "MANUAL_OFF" ||
                d.reason == "OFFLINE_SCHEDULE" || d.reason == "ABOVE_MAX") {
                continue;
            }
            const std::string mode = d.inputs.is_object() ? d.inputs.value("mode", "") : "";
            const int g = mode == "heat" ? 0 : (mode == "keep" || mode == "away") ? 1 : -1;
            if (g < 0) continue;
            ++total[g];
            if (d.on) ++on[g];
            if (d.inputs.contains("target") && d.inputs["target"].is_number()) {
                auto& c = perTarget[OfflineDuty::key(g == 1, d.inputs["target"].get<double>())];
                ++c.second;
                if (d.on) ++c.first;
            }
        }
    } catch (const std::exception& e) {
        log::warn("Learning the duty cycle of {}: {}", roomId, e.what());
    }
    constexpr int kEnough = 36;  // 3 hours of 5-minute decisions
    if (total[0] >= kEnough) duty.heat = std::min(0.9, static_cast<double>(on[0]) / total[0]);
    if (total[1] >= kEnough) duty.keep = std::min(0.9, static_cast<double>(on[1]) / total[1]);
    constexpr int kEnoughForTarget = 12;  // an hour of decisions with that target
    for (const auto& [k, c] : perTarget) {
        if (c.second >= kEnoughForTarget) duty.byTarget[k] = std::min(0.9, static_cast<double>(c.first) / c.second);
    }
    return duty;
}

void Controller::armOffline(const OfflinePeriod& period, Instant now, std::optional<AwayMode> away) {
    for (const auto& room : rooms_) {
        const auto duty = learnedDuty(room.id, now);
        const Instant horizon = period.end + kOfflineBuffer;
        const auto spans = offlineSchedule(room, prices_, tz_, now, horizon, duty, away);
        armedRooms_[room.id] = {period.start, horizon, spans, now};
        log::info("Wi-Fi off period {:%H:%M}-{:%H:%M}: {} {} (duty heat {:.0f}%, keep {:.0f}%)",
                  tz_.toLocal(period.start), tz_.toLocal(period.end), room.id, spansText(spans, tz_),
                  duty.heat * 100, duty.keep * 100);
        for (const auto& id : room.plugIds) {
            auto it = std::ranges::find_if(plugs_, [&](const Plug& p) { return p.id == id; });
            if (it == plugs_.end()) continue;
            if (it->mode != PlugMode::Live) {
                // Dry-run: no schedule, but a fail-safe must not run limp mode all night.
                if (dryRunHasFailsafe(*it, now)) {
                    try {
                        it->sw->keepAlive(now, period.end + kOfflineBuffer);
                    } catch (const std::exception& e) {
                        log::warn("Plug {} (dry-run): watchdog not extended for the night: {}", id, e.what());
                    }
                }
                continue;
            }
            if (!failsafeProblem(*it, now).empty()) continue;               // not switched by heatbrain anyway
            try {
                it->sw->armOffline(spans, now, period.end + kOfflineBuffer);
                armedPlugs_[id] = period.end + kOfflineBuffer;
                log::info("Plug {}: night schedule loaded", id);
            } catch (const std::exception& e) {
                offlineArmErrors_[id] = {e.what(), period.end};
                log::warn("Plug {}: night schedule not loaded: {}", id, e.what());
            }
        }
    }
    armedPeriod_ = period;
    handledPeriodEnd_ = period.end;
    saveOfflineState();
}

bool Controller::networkBackSince(Instant t) const {
    return std::ranges::any_of(readings_, [&](const auto& kv) {
        return kv.second.measuredAt && *kv.second.measuredAt > t + minutes{2};
    });
}

void Controller::disarmOffline(Instant now, bool networkBack) {
    bool changed = false;
    for (auto it = armedRooms_.begin(); it != armedRooms_.end();) {
        if (now >= it->second.until || networkBack) { it = armedRooms_.erase(it); changed = true; }
        else ++it;
    }
    // Reaching an offline plug costs seconds of timeouts, so only try when there is reason to:
    // the network is back, the schedule ran out, or every 15 minutes once the planned end passed.
    const bool pastEnd = armedPeriod_ && now >= armedPeriod_->end;
    const bool periodic = pastEnd && (!lastDisarmTry_ || now - *lastDisarmTry_ >= minutes{15});
    if (periodic) lastDisarmTry_ = now;
    for (auto it = armedPlugs_.begin(); it != armedPlugs_.end();) {
        if (now < it->second && !networkBack && !periodic) { ++it; continue; }
        auto plug = std::ranges::find_if(plugs_, [&](const Plug& p) { return p.id == it->first; });
        if (plug == plugs_.end()) { it = armedPlugs_.erase(it); changed = true; continue; }
        try {
            plug->sw->disarmOffline();
            log::info("Plug {}: back on the network, normal fail-safe restored", it->first);
            failsafeChecks_.erase(it->first);
            it = armedPlugs_.erase(it);
            changed = true;
        } catch (const std::exception&) {
            ++it;  // still offline: try again next tick (alerts resume after the grace time)
        }
    }
    std::erase_if(offlineArmErrors_, [&](const auto& kv) { return now >= kv.second.second + kOfflineGrace; });
    if (armedPeriod_ && armedRooms_.empty() && armedPlugs_.empty()) {
        armedPeriod_.reset();
        lastDisarmTry_.reset();
        changed = true;
    }
    if (changed) saveOfflineState();
}

std::optional<Controller::OfflineInfo> Controller::offlineInfo(Instant now) const {
    if (!offlineWindow_) return std::nullopt;
    OfflineInfo info;
    info.period = armedPeriod_ ? *armedPeriod_ : currentOrNextOfflinePeriod(*offlineWindow_, tz_, now);
    info.late = armedPeriod_ && now >= armedPeriod_->end && !armedPlugs_.empty();
    info.active = info.period.contains(now) || info.late;
    if (!armedPlugs_.empty()) info.scheduleUntil = armedPlugs_.begin()->second;
    for (const auto& [id, until] : armedPlugs_) info.armedPlugs.push_back(id);
    for (const auto& [id, err] : offlineArmErrors_) info.errors.push_back(id + ": " + err.first);
    return info;
}

void Controller::loadOfflineState() {
    armedRooms_.clear();
    armedPlugs_.clear();
    armedPeriod_.reset();
    handledPeriodEnd_.reset();
    if (dataDir_.empty()) return;
    const auto file = dataDir_ / "offline.json";
    if (!std::filesystem::exists(file)) return;
    try {
        const json j = json::parse(std::ifstream(file));
        const auto at = [](long long s) { return Instant{seconds{s}}; };
        // Named copies: iterating .items() of a temporary would read freed memory.
        const json rooms = j.value("rooms", json::object());
        const json plugs = j.value("plugs", json::object());
        for (const auto& [id, r] : rooms.items()) {
            const long long loadedAt = r.value("loadedAt", r.at("until").get<long long>());
            ArmedRoom a{at(r.value("start", loadedAt)), at(r.at("until")), {}, at(loadedAt)};
            for (const auto& s : r.at("spans")) a.spans.push_back({at(s.at(0)), at(s.at(1))});
            armedRooms_[id] = std::move(a);
        }
        for (const auto& [id, until] : plugs.items()) armedPlugs_[id] = at(until);
        if (j.contains("handledEnd")) handledPeriodEnd_ = at(j["handledEnd"]);
        if (j.contains("period")) armedPeriod_ = OfflinePeriod{at(j["period"].at(0)), at(j["period"].at(1))};
        else if (!armedRooms_.empty()) {
            const auto& a = armedRooms_.begin()->second;
            armedPeriod_ = OfflinePeriod{a.start, a.until};
        }
        if (!handledPeriodEnd_ && armedPeriod_) handledPeriodEnd_ = armedPeriod_->end;
    } catch (const std::exception& e) {
        log::warn("Ignoring unreadable {}: {}", file.string(), e.what());
    }
}

void Controller::saveOfflineState() const {
    if (dataDir_.empty()) return;
    const auto epoch = [](Instant t) { return t.time_since_epoch().count(); };
    json rooms = json::object(), plugs = json::object();
    for (const auto& [id, a] : armedRooms_) {
        json spans = json::array();
        for (const auto& s : a.spans) spans.push_back({epoch(s.from), epoch(s.to)});
        rooms[id] = {{"start", epoch(a.start)}, {"until", epoch(a.until)}, {"loadedAt", epoch(a.loadedAt)}, {"spans", spans}};
    }
    for (const auto& [id, until] : armedPlugs_) plugs[id] = epoch(until);
    json out{{"rooms", rooms}, {"plugs", plugs}};
    if (armedPeriod_) out["period"] = {epoch(armedPeriod_->start), epoch(armedPeriod_->end)};
    if (handledPeriodEnd_) out["handledEnd"] = epoch(*handledPeriodEnd_);
    std::ofstream(dataDir_ / "offline.json") << out.dump(2);
}

bool Controller::dryRunHasFailsafe(Plug& plug, Instant now) {
    // Checked every 30 minutes: a dry-run plug without a fail-safe is never touched.
    auto it = dryRunFailsafe_.find(plug.id);
    if (it != dryRunFailsafe_.end() && now - it->second.second < minutes{30}) return it->second.first;
    bool installed = false;
    try {
        installed = plug.sw->checkFailsafe().installed();
    } catch (const std::exception&) {
        installed = false;
    }
    dryRunFailsafe_[plug.id] = {installed, now};
    return installed;
}

void Controller::loadReadings() {
    if (dataDir_.empty() || !std::filesystem::exists(dataDir_ / "readings.json")) return;
    try {
        const json j = json::parse(std::ifstream(dataDir_ / "readings.json"));
        const json items = j.value("readings", json::object());
        for (const auto& [key, r] : items.items()) {
            SensorReading s;
            s.sensorId = r.value("sensorId", "");
            s.name = r.value("name", "");
            s.station = r.value("station", "");
            s.kind = r.value("kind", "");
            if (r.contains("temperature")) s.temperature = r["temperature"].get<double>();
            if (r.contains("humidity")) s.humidity = r["humidity"].get<double>();
            if (r.contains("measuredAt")) s.measuredAt = Instant{seconds{r["measuredAt"].get<long long>()}};
            s.reachable = r.value("reachable", true);
            if (r.contains("battery")) s.batteryPercent = r["battery"].get<int>();
            readings_[key] = std::move(s);
        }
    } catch (const std::exception& e) {
        log::warn("Ignoring unreadable readings.json: {}", e.what());
    }
}

void Controller::saveReadings() const {
    if (dataDir_.empty()) return;
    json items = json::object();
    for (const auto& [key, r] : readings_) {
        json j{{"sensorId", r.sensorId}, {"name", r.name}, {"station", r.station}, {"kind", r.kind}, {"reachable", r.reachable}};
        if (r.temperature) j["temperature"] = *r.temperature;
        if (r.humidity) j["humidity"] = *r.humidity;
        if (r.measuredAt) j["measuredAt"] = r.measuredAt->time_since_epoch().count();
        if (r.batteryPercent) j["battery"] = *r.batteryPercent;
        items[key] = std::move(j);
    }
    const auto tmp = dataDir_ / "readings.json.tmp";
    std::ofstream(tmp) << json{{"readings", items}}.dump();
    std::error_code ec;
    std::filesystem::rename(tmp, dataDir_ / "readings.json", ec);
}

}  // namespace hb
