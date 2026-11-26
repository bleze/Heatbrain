#include "app/controller.hpp"

#include "plugs/tasmota.hpp"

#include "providers/forecast.hpp"

#include <fstream>

#include <algorithm>
#include <cmath>
#include <format>

#include "core/log.hpp"
#include "core/overshoot.hpp"
#include "core/version.hpp"
#include "sensors/netatmo.hpp"

namespace hb {

using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono;

namespace {
constexpr auto kFailsafeRecheck = hours{1};
// A room's new sensor: hours not learned from while it settles where it now is.
constexpr auto kNewSensorSettle = hours{3};
// A quiet sensor: its last reading is extrapolated with the learned rates for this long, then limp mode.
constexpr auto kEstimateFor = hours{3};
constexpr auto kHistoryRetention = days{3 * 366};  // ~100 MB a year with four rooms: years fit easily
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
           (key.starts_with("thermostat:") && endsWith(":unreachable")) ||
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
    const auto push = [&](Instant from, Instant to, const std::string& kind, const PlanSegment& seg) {
        if (from >= to) return;
        PlanSegment piece = seg;
        piece.from = from;
        piece.to = to;
        piece.kind = kind;
        if (!out.empty() && continuesSegment(out.back(), piece)) out.back().to = to;
        else out.push_back(std::move(piece));
    };
    for (const auto& seg : plan) {
        for (Instant t = seg.from; t < seg.to; t += kSlotLength) {
            const Instant e = std::min(t + kSlotLength, seg.to);
            // The target stays, so the chart's target line (a warm-up curve too) runs on through the night.
            if (p.contains(t)) push(t, e, scheduleOnAt(spans, t) ? "night" : "nightoff", seg);
            else push(t, e, seg.kind, seg);
        }
    }
    return out;
}

json toJson(const DecisionInputs& in) {
    json j{{"minTemp", in.minTemp}, {"maxTemp", in.maxTemp}, {"targetLabel", in.targetLabel}, {"mode", in.mode},
           {"priceExpensive", in.priceExpensive}, {"priceCheapest", in.priceCheapest}};
    if (!in.sensor.empty()) j["sensor"] = in.sensor;
    if (in.temperature) j["temperature"] = *in.temperature;
    if (in.temperatureAgeMinutes) j["temperatureAgeMinutes"] = *in.temperatureAgeMinutes;
    if (in.target) j["target"] = *in.target;
    if (in.price) j["price"] = *in.price;
    if (in.medianPrice) j["medianPrice"] = *in.medianPrice;
    if (in.outdoorTemp) j["outdoorTemp"] = *in.outdoorTemp;
    if (in.ceiling) j["ceiling"] = *in.ceiling;
    if (in.warmupTo) j["warmupTo"] = *in.warmupTo;
    if (in.coastAbove) j["coastAbove"] = *in.coastAbove;
    if (in.estimate) j["estimate"] = *in.estimate;
    if (in.overshoot) j["overshoot"] = *in.overshoot;
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
    // Continue the band (and the minimum on-time) where the previous run left off.
    if (history_) {
        for (const auto& r : rooms_) {
            if (auto last = history_->lastDecision(r.id)) {
                lastOn_[r.id] = last->on;
                if (last->on) onSince_[r.id] = last->at;
            }
        }
    }
}

void Controller::refreshPrices(Instant now, TickResult& result) {
    // From the start of today to the end of tomorrow. Today's past hours matter: a room without a
    // sensor runs "the N cheapest hours of the day", and picking them only among the hours still
    // left would put almost every remaining hour on by the morning. Providers cache, so this
    // rarely touches the network.
    PriceTable fresh = priceModel_.build(startOfLocalDay(tz_, now), startOfLocalDay(tz_, now + hours{50}));
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

void Controller::backfillPrices(Instant now) {
    if (!firstTick_) firstTick_ = now;
    // Not in the burst right after a start (prices, power mix): Energi Data Service allows only a few
    // requests a minute, and a rate-limited answer would hold up this control step.
    if (!history_ || now - *firstTick_ < minutes{10} || (backfillTried_ && now - *backfillTried_ < hours{24})) return;
    backfillTried_ = now;
    const Instant from = startOfLocalDay(tz_, now - days{8});
    const Instant to = floor<hours>(now);
    const auto have = history_->prices(from, to);
    const auto expected = static_cast<std::size_t>(duration_cast<minutes>(to - from) / kSlotLength);
    if (have.size() >= expected * 98 / 100) return;  // (nearly) complete
    try {
        const PriceTable past = priceModel_.build(from, to);
        history_->recordPrices(past);
        log::info("Prices: filled in {} past slots from {:%d %b}", past.slots.size(), tz_.toLocal(from));
    } catch (const std::exception& e) {
        log::warn("Prices: could not fill in past prices: {}", e.what());
    }
}

void Controller::checkBatteries(Instant now, const std::optional<AwayMode>& away, TickResult& result) {
    const fs::path file = dataDir_.empty() ? fs::path() : dataDir_ / "battery.json";
    if (!batteryLoaded_ && !file.empty()) {
        batteryLoaded_ = true;
        try {
            if (std::ifstream in(file); in) {
                const json saved = json::parse(in);  // kept alive: items() only refers to it
                for (const auto& [key, arr] : saved.items()) {
                    for (const auto& p : arr) battery_[key].push_back({Instant{seconds{p.at(0).get<long long>()}}, p.at(1).get<int>()});
                }
            }
        } catch (const std::exception& e) {
            log::warn("Battery history: {}", e.what());
        }
    }
    const Instant today = startOfLocalDay(tz_, now);
    bool changed = false;
    for (const auto& [key, r] : readings_) {
        if (!r.batteryPercent) continue;
        auto& log = battery_[key];
        // A new battery: the level jumps up - start the history afresh.
        if (!log.empty() && *r.batteryPercent >= log.back().percent + 15) log.clear();
        if (log.empty() || log.back().day != today) {
            log.push_back({today, *r.batteryPercent});
            changed = true;
        } else if (log.back().percent != *r.batteryPercent) {
            log.back().percent = std::min(log.back().percent, *r.batteryPercent);  // the day's lowest
            changed = true;
        }
        std::erase_if(log, [&](const BatterySample& s) { return today - s.day > days{3 * 366}; });
        if (const auto w = batteryWarning(r.name, *r.batteryPercent, log, now, away, tz_)) {
            const bool urgent = *r.batteryPercent <= kBatteryEmptyPercent || w->message.find("away") != std::string::npos;
            add(result, "sensor:" + key + ":battery", urgent ? Severity::Warning : Severity::Info, w->message, minutes{0},
                hours{24});
        }
    }
    if (changed && !file.empty()) {
        try {
            json j = json::object();
            for (const auto& [key, log] : battery_) {
                json arr = json::array();
                for (const auto& s : log) arr.push_back({s.day.time_since_epoch().count(), s.percent});
                j[key] = arr;
            }
            const fs::path tmp = fs::path(file) += ".tmp";
            std::ofstream(tmp) << j.dump();
            fs::rename(tmp, file);
        } catch (const std::exception& e) {
            log::warn("Battery history: {}", e.what());
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
                // An offline module reports no temperature at all. Keep the last good one
                // with its measurement time: the planner's age check (stale after 30 min)
                // then rides out a short restart instead of switching to limp mode at once.
                if (!r.temperature) {
                    if (auto it = readings_.find(key); it != readings_.end() && it->second.temperature) {
                        r.temperature = it->second.temperature;
                        r.humidity = it->second.humidity;
                        r.co2 = it->second.co2;
                        r.measuredAt = it->second.measuredAt;
                    }
                }
                readings_[key] = std::move(r);
            }
        } catch (const SourceAuthError& e) {
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
                } else if (status.firmware.empty()) {
                    // The plug did not answer the status request: unknown, not "changed".
                    problem = "its firmware version could not be read" +
                              (status.error.empty() ? std::string() : " (" + status.error + ")");
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
                // Rechecked every step: a plug that misses one request is not worth a critical alert,
                // only one whose fail-safe stays unverified (it is not switched meanwhile).
                add(result, "plug:" + id + ":failsafe", Severity::Critical,
                    std::format("Plug '{}' is live, but {}, so heatbrain will not switch it. Fix it under "
                                "Settings → Heating.",
                                id, problem),
                    minutes{15});
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

// A thermostat room: work out the setpoint and, for a live thermostat, make sure it is in manual
// mode with that setpoint (written only when it differs - the thermostat keeps it by itself).
json Controller::applyToThermostat(const RoomConfig& room, const Decision& d, TickResult& result) {
    json out = json::array();
    const auto t = std::ranges::find_if(thermostats_, [&](const ThermostatConfig& c) { return c.id == room.thermostat; });
    if (t == thermostats_.end()) return out;
    const double sp = thermostatSetpoint(room, d);
    json tj{{"id", t->id}, {"thermostat", true}, {"mode", t->live ? "live" : "dryrun"}, {"setpoint", sp}};
    try {
        auto th = DeviRegistry::instance().get(t->peerId);
        const DeviState s = th->state();  // refreshed by the DEVI temperature source this step
        if (s.heating) tj["state"] = *s.heating;
        if (s.relayOnSeconds) tj["relayOnSeconds"] = *s.relayOnSeconds;  // for exact energy (cost report)
        if (s.manualSetpoint) tj["deviSetpoint"] = *s.manualSetpoint;
        if (s.controlState) tj["deviMode"] = dominion::controlStateName(*s.controlState);
        if (s.floorTemp) tj["floor"] = *s.floorTemp;
        if (s.windowOpen && *s.windowOpen) tj["windowOpen"] = true;
        if (t->live) {
            const bool manual = s.controlState && *s.controlState == dominion::ControlState::Manual;
            if (!manual || !s.manualSetpoint || std::abs(*s.manualSetpoint - sp) > 0.05) {
                th->setManualSetpoint(sp);
                tj["written"] = true;
            }
        }
    } catch (const std::exception& e) {
        tj["error"] = e.what();
        add(result, "thermostat:" + t->id + ":unreachable", Severity::Warning,
            std::format("Thermostat '{}': {}", t->name, e.what()), minutes{15});
    }
    out.push_back(std::move(tj));
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
    checkBatteries(now, away, result);

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
    refreshForecast(now, outdoor, result);
    const auto forecast = forecastInfo_.forecast;

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
        if (in.currentlyOn) {
            if (auto since = onSince_.find(room.id); since != onSince_.end()) in.onSince = since->second;
        }
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
        in.forecast = forecast;
        // Open window: the temperature fell fast. Pause heating for a while (manual ON still wins).
        // Only below the room's max: above it heatbrain never heats, so a pause would change nothing
        // (and a sensor brought in from somewhere warmer falls fast without any window open).
        if (!room.priceOnly && reading && reading->temperature && reading->measuredAt &&
            *reading->temperature < room.maxTemp) {
            auto& detector = windows_[room.id];
            if (apply) {
                if (const auto ev = detector.update(*reading->measuredAt, *reading->temperature, openWindow_)) {
                    log::info("{}: {:.1f}° fall in {} min, window open? Heating paused until {:%H:%M}", room.id,
                              ev->drop, ev->over.count(), tz_.toLocal(ev->until));
                }
            }
            in.windowOpen = detector.pausedAt(now);
            if (in.windowOpen) {
                add(result, "room:" + room.id + ":window", Severity::Info,
                    std::format("{}: the temperature fell {:.1f}° in {} min. Window open? Heating is paused until {:%H:%M}.",
                                room.name, in.windowOpen->drop, in.windowOpen->over.count(),
                                tz_.toLocal(in.windowOpen->until)));
            }
        }
        if (!room.priceOnly) {
            const ThermalModel m = thermalModel(room, now);
            if (m.usable()) in.thermal = m;
            if (m.coolingStretches >= 3 && m.loss > 0) in.coolingRate = m.loss;  // as the plan's expected coast
            // The sensor is quiet (the reading is older than trusted, but recent): estimate the temperature
            // from the last reading - cooling towards outdoors, warming while heatbrain heated since -
            // rather than fall back to limp mode at once. Needs the learned rates and an outdoor temperature.
            const auto age = in.measuredAt ? now - *in.measuredAt : seconds{0};
            std::optional<double> outside = outdoor;
            if (!outside && forecast) outside = forecast->at(now);
            if (in.thermal && in.temperature && in.measuredAt && age > room.strategy.staleAfter && age <= kEstimateFor &&
                outside && history_) {
                try {
                    std::vector<std::pair<Instant, Instant>> heating;
                    const auto ds = history_->decisions(room.id, *in.measuredAt - hours{1}, now, false);
                    for (std::size_t i = 0; i < ds.size(); ++i) {
                        const Instant end = i + 1 < ds.size() ? ds[i + 1].at : now;
                        if (ds[i].on && end > *in.measuredAt) heating.emplace_back(std::max(ds[i].at, *in.measuredAt), end);
                    }
                    in.estimate = estimateTemperature(*in.thermal, *in.temperature, *in.measuredAt, now, heating, *outside);
                } catch (const std::exception& e) {
                    log::warn("{}: no estimate for the quiet sensor ({})", room.id, e.what());
                }
            }
        }
        // No usable temperature at all (no estimate either): limp mode heats as much as the room
        // usually does in such a period, learned from the last week.
        if (!room.priceOnly && !in.estimate &&
            !(in.temperature && in.measuredAt && now - *in.measuredAt <= room.strategy.staleAfter)) {
            if (const auto duty = learnedDuty(room.id, now); duty.learned) in.duty = duty;
        }
        // How far the room keeps warming after the heat goes off: heating stops that much earlier.
        // Not for a thermostat (it regulates itself) or a price-only room (no temperature).
        if (!room.priceOnly && room.thermostat.empty()) in.overshoot = learnedOvershoot(room, now);
        // Bookings: warm up as long as the room needs - learned from its history, from the current
        // and the outdoor temperature - or the room's warm-up setting until that is known.
        std::vector<BookingPlan> bookingPlans;
        std::vector<Booking> bookings;
        std::optional<ThermalModel> learned;
        if (bookings_ && !room.priceOnly) {
            const ThermalModel model = thermalModel(room, now);
            if (model.usable()) learned = model;
            const bool freshTemp = in.temperature && in.measuredAt && now - *in.measuredAt <= room.strategy.staleAfter;
            for (const auto& s : bookings_->load()) {
                if (s.room != room.id) continue;
                BookingPlan bp{s.id, s.booking, "set", true, s.warmingSince};
                bp.booking.warmup = room.warmup;
                if (learned && freshTemp) {
                    // The outdoor temperature while warming up: the forecast for the hours before the booking.
                    const Instant guessStart = s.booking.from - room.warmup;
                    std::optional<double> outside =
                        forecast ? forecast->meanOver(std::max(now, guessStart), std::max(now, s.booking.from)) : std::nullopt;
                    if (!outside) outside = outdoor;
                    const auto est = estimateWarmup(*learned, *in.temperature, s.booking.target, outside.value_or(5.0));
                    bp.booking.warmup = est.duration;
                    bp.basis = "learned";
                    bp.reachable = est.reachable;
                }
                if (s.warmingSince) {
                    bp.booking.warmup = std::max(bp.booking.warmup, duration_cast<minutes>(s.booking.from - *s.warmingSince));
                }
                if (apply && !s.warmingSince && bp.booking.coversAt(now)) {
                    bookings_->markWarming(s.id, now);
                    bp.warmingSince = now;
                }
                bookings.push_back(bp.booking);
                bookingPlans.push_back(std::move(bp));
            }
        }
        in.booking = bookingAt(bookings, now);

        RoomResult rr;
        rr.room = &room;
        rr.bookings = std::move(bookingPlans);
        rr.thermal = learned;
        rr.overshoot = in.overshoot;
        if (!room.priceOnly) {
            auto& t = trend_[room.id];
            if (std::get<0>(t) == Instant{} || now - std::get<0>(t) >= hours{24}) {
                t = {now, fitBetween(room, now - days{7}, now), fitBetween(room, now - days{35}, now - days{7})};
            }
            rr.recentThermal = std::get<1>(t);
            rr.earlierThermal = std::get<2>(t);
        }
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
        {
            // From the room's current temperature, cooling at its learned rate (once it has seen enough).
            std::optional<double> current, cooling;
            if (in.temperature && in.measuredAt && now - *in.measuredAt <= room.strategy.staleAfter) current = in.temperature;
            cooling = in.coolingRate;
            rr.plan = planAhead(room, prices_, tz_, now, hours{24}, away, outdoor, bookings, current, cooling, in.thermal,
                                forecast);
        }
        if (offline && offline->start < now + hours{24} && room.thermostat.empty()) {  // thermostats keep their setpoint
            // Show the night as it will run: the loaded schedule, or what would be loaded now.
            const auto spans = following ? armed->second.spans
                                         : offlineSchedule(room, prices_, tz_, std::max(now, offline->start),
                                                           offline->end, learnedDuty(room.id, now), away,
                                                           offlineStart(room, now), in.thermal);
            rr.plan = overlayNight(rr.plan, *offline, spans);
        }

        if (rr.decision.reason == Reason::StaleDataLimp) {
            add(result, "room:" + room.id + ":limp", Severity::Warning,
                std::format("Room '{}' is in limp mode: {}", room.name, rr.decision.detail), minutes{15});
        }
        if (rr.decision.reason == Reason::EstimateHeating || rr.decision.reason == Reason::EstimateOff) {
            add(result, "room:" + room.id + ":estimate", Severity::Warning,
                std::format("Room '{}': its sensor is quiet, heating on an estimate. {}", room.name, rr.decision.detail),
                minutes{30});
        }
        if (rr.decision.on && rr.decision.inputs.temperature && *rr.decision.inputs.temperature < room.minTemp) {
            add(result, "room:" + room.id + ":not-warming", Severity::Warning,
                std::format("Room '{}' is {:.1f}°C, below its minimum {:.1f}°C, despite heating for an hour. "
                            "Window open, or radiator switched off?",
                            room.name, *rr.decision.inputs.temperature, room.minTemp),
                minutes{60});
        }

        if (apply) {
            rr.plugs = room.thermostat.empty() ? applyToPlugs(room, rr.decision.on, now, result)
                                               : applyToThermostat(room, rr.decision, result);
            if (rr.decision.on && !(lastOn_.contains(room.id) && lastOn_[room.id])) onSince_[room.id] = now;
            if (!rr.decision.on) onSince_.erase(room.id);
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
                history_->recordTemperature(
                    {*r.measuredAt, key, roomId, *r.temperature, r.humidity, startOfLocalDay(tz_, *r.measuredAt),
                     r.co2 ? std::optional<double>(*r.co2) : std::nullopt});
            }
            history_->recordPrices(prices_);
            backfillPrices(now);
            if (!sensorDaysFilled_) {
                sensorDaysFilled_ = true;
                history_->fillSensorDays([this](Instant t) { return startOfLocalDay(tz_, t); });
            }
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

// How fast the room warms and cools, from three weeks of its own decisions (temperature, outdoor
// temperature, heating or not). Refitted once an hour.
ThermalModel Controller::thermalModel(const RoomConfig& room, Instant now) {
    if (auto it = thermal_.find(room.id); it != thermal_.end() && now - it->second.first < hours{1}) {
        return it->second.second;
    }
    const ThermalModel m = fitBetween(room, now - days{21}, now);
    thermal_[room.id] = {now, m};
    return m;
}

std::optional<double> Controller::learnedOvershoot(const RoomConfig& room, Instant now) {
    if (auto it = overshoot_.find(room.id); it != overshoot_.end() && now - it->second.first < hours{1}) {
        return it->second.second;
    }
    std::optional<double> out;
    if (history_) {
        try {
            // The last two weeks with the room's current sensor: the switches heatbrain made on a real
            // temperature, and the readings around them.
            std::vector<HeatSwitch> switches;
            for (const auto& d : history_->decisions(room.id, now - days{14}, now, true)) {
                if (d.inputs.is_object() && d.inputs.contains("sensor") && d.inputs["sensor"] != room.sensor) {
                    switches.clear();  // another sensor: what it saw says nothing about this one
                    continue;
                }
                const bool usable = d.reason != "STALE_DATA_LIMP" && d.reason != "ESTIMATE_HEATING" && d.reason != "ESTIMATE_OFF" &&
                                    d.reason != "OFFLINE_SCHEDULE" && d.reason != "WINDOW_OPEN" && d.reason != "MANUAL_ON" &&
                                    d.reason != "MANUAL_OFF";
                switches.push_back({d.at, d.on, usable});
            }
            std::vector<std::pair<Instant, double>> temps;
            for (const auto& t : history_->temperatures(room.id, now - days{14}, now)) temps.emplace_back(t.at, t.temperature);
            std::sort(temps.begin(), temps.end());
            out = learnOvershoot(switches, temps);
        } catch (const std::exception& e) {
            log::warn("{}: learning the overshoot failed: {}", room.id, e.what());
        }
    }
    overshoot_[room.id] = {now, out};
    return out;
}

ThermalModel Controller::fitBetween(const RoomConfig& room, Instant from, Instant to) {
    ThermalModel m;
    if (history_) {
        try {
            std::vector<ThermalSample> samples;
            std::string sensor;            // the sensor of the decisions so far (older ones do not say)
            std::optional<Instant> settle;  // a new sensor: its first hours are skipped
            for (const auto& d : history_->decisions(room.id, from, to, false)) {
                // Another sensor: what the old one measured says nothing about the new one's place, and
                // the new one first has to settle (brought in from somewhere warmer or colder).
                if (d.inputs.is_object() && d.inputs.contains("sensor")) {
                    const std::string key = d.inputs["sensor"].get<std::string>();
                    if (!sensor.empty() && key != sensor) {
                        samples.clear();
                        settle = d.at + kNewSensorSettle;
                    }
                    sensor = key;
                }
                if (settle && d.at < *settle) continue;
                // Only real states: not limp mode or night schedules (the plug did its own thing).
                // An open window is not how the room normally cools.
                if (d.reason == "STALE_DATA_LIMP" || d.reason == "ESTIMATE_HEATING" || d.reason == "ESTIMATE_OFF" ||
                    d.reason == "OFFLINE_SCHEDULE" || d.reason == "WINDOW_OPEN" || !d.inputs.is_object()) {
                    continue;
                }
                if (!d.inputs.contains("temperature") || d.inputs.value("temperatureAgeMinutes", 99) > 30) continue;
                // Above the room's max: not where heatbrain keeps it - a sensor just brought in from
                // somewhere warmer, or the sun - and how it cools from there is not how it holds its range.
                if (d.inputs.contains("maxTemp") && d.inputs["temperature"].get<double>() > d.inputs["maxTemp"].get<double>()) continue;
                ThermalSample s{d.at, d.inputs["temperature"].get<double>(), std::nullopt, d.on};
                if (d.inputs.contains("outdoorTemp")) s.outdoor = d.inputs["outdoorTemp"].get<double>();
                samples.push_back(s);
            }
            m = fitThermalModel(samples);
        } catch (const std::exception& e) {
            log::warn("Learning how fast {} warms up: {}", room.id, e.what());
        }
    }
    return m;
}

// The weather forecast: refetched every half hour (the HTTP cache keeps it an hour), for the set
// location or the Netatmo station's. Kept for 12 hours when fetching fails.
void Controller::refreshForecast(Instant now, std::optional<double> outdoor, TickResult& result) {
    forecastInfo_.location = forecastLocation_;
    forecastInfo_.locationFrom = forecastLocation_ ? "settings" : "";
    if (!forecastLocation_) {
        for (const auto& [key, r] : readings_) {
            if (r.location && (!forecastInfo_.location || r.kind == "outdoor")) {
                forecastInfo_.location = r.location;
                forecastInfo_.locationFrom = "netatmo";
            }
        }
    }
    if (!forecastHttp_ || !forecastInfo_.location) {
        forecastInfo_.forecast.reset();
        return;
    }
    const bool due = (!forecastFetched_ || now - *forecastFetched_ >= minutes{30}) &&
                     (!forecastFailedAt_ || now - *forecastFailedAt_ >= minutes{15});
    if (due) {
        try {
            rawForecast_ = fetchForecast(*forecastHttp_, forecastInfo_.location->first, forecastInfo_.location->second);
            forecastFetched_ = now;
            forecastFailedAt_.reset();
            forecastInfo_.error.clear();
        } catch (const std::exception& e) {
            forecastFailedAt_ = now;
            forecastInfo_.error = e.what();
            log::warn("Weather forecast: {}", e.what());
        }
    }
    if (forecastFetched_ && now - *forecastFetched_ > hours{12}) rawForecast_ = {};
    if (rawForecast_.empty()) {
        forecastInfo_.forecast.reset();
        if (!forecastInfo_.error.empty()) {
            add(result, "forecast:error", Severity::Info,
                "No weather forecast (" + forecastInfo_.error + "): pre-heating uses the current outdoor temperature",
                minutes{180});
        }
        return;
    }
    forecastInfo_.forecast = std::make_shared<const OutdoorForecast>(
        outdoor ? rawForecast_.correctedTo(*outdoor, now) : rawForecast_);
}

OfflineDuty Controller::learnedDuty(const std::string& roomId, Instant now) const {
    OfflineDuty duty;  // fallback until there is enough history: 15 min per hour
    if (!history_) return duty;
    int on[2] = {0, 0}, total[2] = {0, 0};
    std::map<std::pair<int, int>, std::pair<int, int>> perTarget;  // key -> (on, total)
    try {
        for (const auto& d : history_->decisions(roomId, now - days{7}, now, false)) {
            // Only decisions made on a real temperature by the thermostat logic.
            if (d.reason == "STALE_DATA_LIMP" || d.reason == "ESTIMATE_HEATING" || d.reason == "ESTIMATE_OFF" ||
                d.reason == "MANUAL_ON" || d.reason == "MANUAL_OFF" || d.reason == "OFFLINE_SCHEDULE" || d.reason == "ABOVE_MAX") {
                continue;
            }
            // Warming up: full heat by design, not the room's usual share.
            if (d.inputs.is_object() && d.inputs.contains("warmupTo")) continue;
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
    duty.learned = total[0] >= kEnough || total[1] >= kEnough;
    constexpr int kEnoughForTarget = 12;  // an hour of decisions with that target
    for (const auto& [k, c] : perTarget) {
        if (c.second >= kEnoughForTarget) duty.byTarget[k] = std::min(0.9, static_cast<double>(c.first) / c.second);
    }
    return duty;
}

std::optional<OfflineStart> Controller::offlineStart(const RoomConfig& room, Instant now) {
    if (room.priceOnly || room.sensor.empty()) return std::nullopt;
    const SensorReading* r = findSensor(room.sensor);
    if (!r || !r->temperature || !r->measuredAt || now - *r->measuredAt > room.strategy.staleAfter) return std::nullopt;
    OfflineStart s;
    s.at = *r->measuredAt;
    s.temperature = *r->temperature;
    if (const auto m = thermalModel(room, now); m.coolingStretches >= 3 && m.loss > 0) s.coolingRate = m.loss;
    if (outdoorUsed_) s.outdoor = outdoorUsed_->temperature;
    s.forecast = forecastInfo_.forecast;
    return s;
}

void Controller::armOffline(const OfflinePeriod& period, Instant now, std::optional<AwayMode> away) {
    // The plugs' own Wi-Fi off too (unless turned off in settings): no reconnect attempts all night.
    const auto radio = offlineWindow_ && offlineWindow_->plugRadio ? plugRadioOffWindow(period) : std::nullopt;
    const auto radioNote = [&](const std::string& id, bool off) {
        if (!radio) return;
        if (off) {
            radioOffPlugs_.insert(id);
            log::info("Plug {}: its own Wi-Fi goes off {:%H:%M}-{:%H:%M}", id, tz_.toLocal(radio->start),
                      tz_.toLocal(radio->end));
        } else {
            log::info("Plug {}: its own Wi-Fi stays on tonight (switching it off needs Tasmota {} or newer)", id,
                      kTasmotaWifiOffSince);
        }
    };
    radioOffPlugs_.clear();
    for (const auto& room : rooms_) {
        const auto duty = learnedDuty(room.id, now);
        const Instant horizon = period.end + kOfflineBuffer;
        std::optional<ThermalModel> thermal;
        if (!room.priceOnly) {
            if (const ThermalModel m = thermalModel(room, now); m.usable()) thermal = m;
        }
        const auto spans = offlineSchedule(room, prices_, tz_, now, horizon, duty, away, offlineStart(room, now), thermal);
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
                    // Dry-run switches nothing, but its Wi-Fi can rest all the same.
                    if (radio) {
                        try {
                            const bool off = it->sw->armRadioOff(*radio, now);
                            if (off) radioOnlyPlugs_[id] = period.end + kOfflineBuffer;
                            radioNote(id, off);
                        } catch (const std::exception& e) {
                            log::warn("Plug {} (dry-run): its Wi-Fi stays on tonight: {}", id, e.what());
                        }
                    }
                }
                continue;
            }
            if (!failsafeProblem(*it, now).empty()) continue;               // not switched by heatbrain anyway
            try {
                const bool off = it->sw->armOffline(spans, now, period.end + kOfflineBuffer, radio);
                armedPlugs_[id] = period.end + kOfflineBuffer;
                log::info("Plug {}: night schedule loaded", id);
                radioNote(id, off);
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
    const auto handBack = [&](std::map<std::string, Instant>& armed, const char* what) {
        for (auto it = armed.begin(); it != armed.end();) {
            if (now < it->second && !networkBack && !periodic) { ++it; continue; }
            auto plug = std::ranges::find_if(plugs_, [&](const Plug& p) { return p.id == it->first; });
            if (plug == plugs_.end()) { it = armed.erase(it); changed = true; continue; }
            try {
                plug->sw->disarmOffline();
                log::info("Plug {}: back on the network, {}", it->first, what);
                failsafeChecks_.erase(it->first);
                radioOffPlugs_.erase(it->first);
                it = armed.erase(it);
                changed = true;
            } catch (const std::exception&) {
                ++it;  // still offline: try again next tick (alerts resume after the grace time)
            }
        }
    };
    handBack(armedPlugs_, "normal fail-safe restored");
    handBack(radioOnlyPlugs_, "its Wi-Fi off/on for the night removed");
    std::erase_if(offlineArmErrors_, [&](const auto& kv) { return now >= kv.second.second + kOfflineGrace; });
    if (armedPeriod_ && armedRooms_.empty() && armedPlugs_.empty() && radioOnlyPlugs_.empty()) {
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
    info.radioOff.assign(radioOffPlugs_.begin(), radioOffPlugs_.end());
    for (const auto& [id, err] : offlineArmErrors_) info.errors.push_back(id + ": " + err.first);
    return info;
}

void Controller::loadOfflineState() {
    armedRooms_.clear();
    armedPlugs_.clear();
    radioOnlyPlugs_.clear();
    radioOffPlugs_.clear();
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
        const json radioOnly = j.value("radioOnly", json::object());
        for (const auto& [id, until] : radioOnly.items()) radioOnlyPlugs_[id] = at(until);
        for (const auto& id : j.value("radioOff", json::array())) radioOffPlugs_.insert(id.get<std::string>());
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
    json radioOnly = json::object();
    for (const auto& [id, until] : radioOnlyPlugs_) radioOnly[id] = epoch(until);
    json out{{"rooms", rooms}, {"plugs", plugs}, {"radioOnly", radioOnly}, {"radioOff", radioOffPlugs_}};
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
            if (r.contains("co2")) s.co2 = r["co2"].get<int>();
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
        if (r.co2) j["co2"] = *r.co2;
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
