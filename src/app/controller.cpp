#include "app/controller.hpp"

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
    for (auto& src : sources_) {
        try {
            for (auto& r : src->read()) {
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
        if (plug.mode == PlugMode::Live) {
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
        if (o && o->temperature && o->measuredAt && now - *o->measuredAt < hours{1}) outdoor = o->temperature;
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
        } else if (!room.sensor.empty() && !reading && !sources_.empty()) {
            add(result, "room:" + room.id + ":sensor", Severity::Warning,
                std::format("Room '{}': sensor '{}' not found", room.name, room.sensor));
        }
        if (auto it = overrides.find(room.id); it != overrides.end()) in.override = it->second;
        in.away = away;
        in.outdoorTemp = outdoor;

        RoomResult rr;
        rr.room = &room;
        rr.decision = decideRoom(room, in, prices_, tz_);
        rr.changed = prev == lastOn_.end() || prev->second != rr.decision.on;

        if (rr.decision.reason == Reason::StaleDataLimp) {
            add(result, "room:" + room.id + ":limp", Severity::Warning,
                std::format("Room '{}' is in limp mode: {}", room.name, rr.decision.detail));
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

}  // namespace hb
