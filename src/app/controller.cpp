#include "app/controller.hpp"

#include <algorithm>
#include <format>

#include "core/log.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

namespace {
constexpr auto kFailsafeRecheck = hours{1};
constexpr auto kHistoryRetention = days{90};
}  // namespace

json toJson(const DecisionInputs& in) {
    json j{{"minTemp", in.minTemp}, {"maxTemp", in.maxTemp}, {"targetLabel", in.targetLabel},
           {"priceExpensive", in.priceExpensive}, {"priceCheapest", in.priceCheapest}};
    if (in.temperature) j["temperature"] = *in.temperature;
    if (in.temperatureAgeMinutes) j["temperatureAgeMinutes"] = *in.temperatureAgeMinutes;
    if (in.target) j["target"] = *in.target;
    if (in.price) j["price"] = *in.price;
    if (in.medianPrice) j["medianPrice"] = *in.medianPrice;
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
    for (const auto& e : fresh.errors) result.errors.push_back("Prices: " + e);
    prices_ = std::move(fresh);
}

void Controller::readSensors(TickResult& result) {
    for (auto& src : sources_) {
        try {
            for (auto& r : src->read()) readings_[src->id() + ":" + r.sensorId] = std::move(r);
        } catch (const std::exception& e) {
            // Keep the previous readings: they age, and the planner stops trusting them.
            result.errors.push_back("Temperature source " + src->id() + ": " + e.what());
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

bool Controller::failsafeVerified(Plug& plug, Instant now) {
    auto it = failsafeChecks_.find(plug.id);
    if (it != failsafeChecks_.end() && it->second.first && now - it->second.second < kFailsafeRecheck) return true;
    try {
        const auto st = plug.sw->checkFailsafe();
        failsafeChecks_[plug.id] = {st.installed(), now};
        if (!st.installed()) {
            log::error("{}: live, but the fail-safe is not installed ({}). Not switching it. "
                       "Run 'heatbrain plug {} provision'.",
                       plug.id, st.problems.front(), plug.id);
        }
        return st.installed();
    } catch (const std::exception&) {
        return false;  // unreachable: reported by the switch attempt path
    }
}

json Controller::applyToPlugs(const RoomConfig& room, bool on, Instant now, TickResult& result) {
    json out = json::array();
    for (const auto& id : room.plugIds) {
        auto it = std::ranges::find_if(plugs_, [&](const Plug& p) { return p.id == id; });
        Plug& plug = *it;
        json pj{{"id", id}, {"mode", plug.mode == PlugMode::Live ? "live" : "dryrun"}};
        if (plug.mode == PlugMode::Live) {
            if (!failsafeVerified(plug, now)) {
                pj["error"] = "fail-safe not verified: not switched";
                result.errors.push_back(std::format("Plug {}: fail-safe not verified, not switched", id));
            } else {
                try {
                    const bool state = plug.sw->set(on);
                    pj["state"] = state;
                    if (state != on) {
                        pj["error"] = "device reports a different state";
                        result.errors.push_back(std::format("Plug {}: commanded {} but device reports {}", id,
                                                            on ? "ON" : "OFF", state ? "ON" : "OFF"));
                    }
                } catch (const std::exception& e) {
                    pj["error"] = e.what();
                    result.errors.push_back(std::format("Plug {} unreachable: {}", id, e.what()));
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
        result.errors.push_back(std::string("Prices: ") + e.what());  // keep the previous table
    }
    readSensors(result);

    const auto overrides = overrides_ ? overrides_->load() : std::map<std::string, ManualOverride>{};

    for (const auto& room : rooms_) {
        const auto prev = lastOn_.find(room.id);
        PlannerInput in;
        in.now = now;
        in.currentlyOn = prev != lastOn_.end() && prev->second;
        const SensorReading* reading = room.sensor.empty() ? nullptr : findSensor(room.sensor);
        if (reading && reading->reachable) {
            in.temperature = reading->temperature;
            in.measuredAt = reading->measuredAt;
        } else if (!room.sensor.empty() && !reading) {
            result.errors.push_back(std::format("Room {}: sensor '{}' not found", room.id, room.sensor));
        }
        if (auto it = overrides.find(room.id); it != overrides.end()) in.override = it->second;

        RoomResult rr;
        rr.room = &room;
        rr.decision = decideRoom(room, in, prices_, tz_);
        rr.changed = prev == lastOn_.end() || prev->second != rr.decision.on;

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
            result.errors.push_back(std::string("History: ") + e.what());
        }
    }
    return result;
}

}  // namespace hb
