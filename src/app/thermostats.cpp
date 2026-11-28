#include "app/thermostats.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <set>
#include <thread>

#include "core/log.hpp"

namespace hb {

using json = nlohmann::json;
using namespace std::chrono;

std::vector<ThermostatConfig> parseThermostats(const json& cfg) {
    std::vector<ThermostatConfig> out;
    if (!cfg.contains("thermostats")) return out;
    std::set<std::string> ids;
    for (const auto& tj : cfg.at("thermostats")) {
        ThermostatConfig t;
        t.id = tj.at("id").get<std::string>();
        if (t.id.empty() || !ids.insert(t.id).second) throw std::runtime_error("thermostat id missing or duplicated: '" + t.id + "'");
        t.type = tj.value("type", "devi");
        if (t.type != "devi") throw std::runtime_error("thermostat " + t.id + ": unknown type '" + t.type + "' (known: devi)");
        t.name = tj.value("name", t.id);
        t.peerId = tj.value("peerId", "");
        if (t.peerId.size() != 64) throw std::runtime_error("thermostat " + t.id + ": peerId must be 64 hex characters (pair it first)");
        const std::string mode = tj.value("mode", "dryrun");
        if (mode != "live" && mode != "dryrun") throw std::runtime_error("thermostat " + t.id + ": mode must be 'live' or 'dryrun'");
        t.live = mode == "live";
        t.watts = tj.value("watts", 0.0);
        out.push_back(std::move(t));
    }
    return out;
}

// ---------------------------------------------------------------- registry

DeviRegistry& DeviRegistry::instance() {
    static DeviRegistry r;
    return r;
}

void DeviRegistry::setDataDir(const std::filesystem::path& dir) {
    std::lock_guard lock(mutex_);
    if (dataDir_ == dir) return;
    dataDir_ = dir;
    identity_.reset();
    thermostats_.clear();
}

std::optional<DeviIdentity> DeviRegistry::identity() {
    std::lock_guard lock(mutex_);
    if (dataDir_.empty() || !std::filesystem::exists(dataDir_ / "devi.json")) return std::nullopt;
    if (!identity_) identity_ = loadDeviIdentity(dataDir_);
    return identity_;
}

void DeviRegistry::reload() {
    std::lock_guard lock(mutex_);
    identity_.reset();  // keep the connections: the private key does not change on pairing
}

std::shared_ptr<DeviThermostat> DeviRegistry::get(const std::string& peerId) {
    std::lock_guard lock(mutex_);
    if (auto it = thermostats_.find(peerId); it != thermostats_.end()) return it->second;
    if (dataDir_.empty()) throw std::runtime_error("DEVI: no data folder");
    if (!identity_) identity_ = loadDeviIdentity(dataDir_);
    auto t = std::make_shared<DeviThermostat>(peerId, identity_->privateKey);
    thermostats_[peerId] = t;
    return t;
}

// ---------------------------------------------------------------- source

DeviSource::DeviSource(std::vector<ThermostatConfig> thermostats) : thermostats_(std::move(thermostats)) {}

std::vector<SensorReading> DeviSource::read() {
    // Ask every thermostat (answers arrive on OpenSDG's thread), then give them a moment.
    std::vector<std::pair<const ThermostatConfig*, std::shared_ptr<DeviThermostat>>> asked;
    const auto started = floor<seconds>(system_clock::now());
    for (const auto& t : thermostats_) {
        try {
            auto th = DeviRegistry::instance().get(t.peerId);
            th->refresh();
            asked.push_back({&t, th});
        } catch (const std::exception& e) {
            log::debug("DEVI {}: {}", t.id, e.what());
            asked.push_back({&t, nullptr});
        }
    }
    for (int i = 0; i < 30; ++i) {  // up to 3 s
        const bool all = std::ranges::all_of(asked, [&](const auto& a) {
            if (!a.second) return true;
            const auto s = a.second->state();
            return s.updated && *s.updated >= started;
        });
        if (all) break;
        std::this_thread::sleep_for(milliseconds{100});
    }
    std::vector<SensorReading> out;
    for (const auto& [cfg, th] : asked) {
        const DeviState s = th ? th->state() : DeviState{};
        const bool fresh = s.updated && *s.updated >= started - minutes{10};
        SensorReading wall;
        wall.sensorId = cfg->id;
        wall.name = cfg->name;
        wall.station = "DEVI";
        wall.kind = "indoor";
        wall.reachable = th && s.connected && fresh;
        if (wall.reachable) { wall.temperature = s.roomTemp; wall.measuredAt = s.updated; }
        out.push_back(wall);
        SensorReading floorProbe = wall;
        floorProbe.sensorId = cfg->id + "-floor";
        floorProbe.name = cfg->name + " (floor)";
        floorProbe.temperature = wall.reachable ? s.floorTemp : std::nullopt;
        out.push_back(floorProbe);
    }
    return out;
}

// ---------------------------------------------------------------- setpoint

namespace {
double ceilHalf(double v) { return std::ceil(v * 2.0 - 1e-9) / 2.0; }
double floorHalf(double v) { return std::floor(v * 2.0 + 1e-9) / 2.0; }
}  // namespace

double thermostatSetpoint(const RoomConfig& room, const Decision& d, const DeviReading& devi) {
    const auto& in = d.inputs;
    const double frost = room.frostTemp;
    double sp;
    switch (d.reason) {
        case Reason::ManualOn: sp = room.maxTemp; break;
        case Reason::ManualOff:
        case Reason::ScheduledOff:
        case Reason::WindowOpen:
        case Reason::AboveMax:
            sp = frost;
            break;
        case Reason::KeepAbove:
        case Reason::KeepSatisfied:
        case Reason::AwayOff:
            sp = in.target.value_or(frost);
            break;
        case Reason::PeakAvoid: sp = in.target.value_or(frost) - room.strategy.peakAllowedDrop; break;
        case Reason::PreheatBeforePeak:
            sp = in.ceiling.value_or(in.target.value_or(frost) + room.strategy.preheatBoost);
            break;
        case Reason::CheapBoost: sp = in.ceiling.value_or(in.target.value_or(frost) + room.strategy.cheapBoost); break;
        case Reason::WarmUp:
        case Reason::WarmUpOnTrack:
            // Warming up: the coming period's target at once - a thermostat regulates towards its
            // setpoint itself, so stepping it up along the curve would only make it lag behind.
            sp = in.warmupTo.value_or(in.target.value_or(frost));
            break;
        default:
            // heat periods (thermostat logic, below minimum, limp, ...): the target; the
            // thermostat does the regulating itself.
            sp = in.mode == "off" ? frost : in.target.value_or(frost);
            break;
    }
    if (in.mode == "heat") sp = std::max(sp, room.minTemp);
    const double base = std::clamp(std::round(sp * 2.0) / 2.0, frost, room.maxTemp);  // half degrees, like the DEVI app
    if (!devi.temperature) return base;

    // The thermostat heats by its own sensor, which can read a degree off the room's: a setpoint at
    // the target may have it heat while heatbrain decided off (or not heat while it decided on).
    const double t = *devi.temperature;
    const double lowest = std::min(in.mode == "heat" ? std::max(frost, room.minTemp) : frost, room.maxTemp);
    const auto follows = [&](double s) { return d.on ? s >= t + kSteerMargin : s <= t - kSteerMargin; };
    if (follows(base)) return base;
    if (devi.setpoint && follows(*devi.setpoint) && *devi.setpoint >= lowest && *devi.setpoint <= room.maxTemp) {
        return *devi.setpoint;  // already steered: not written again every step
    }
    return std::clamp(d.on ? ceilHalf(t + 2 * kSteerMargin) : floorHalf(t - 2 * kSteerMargin), lowest, room.maxTemp);
}

// ---------------------------------------------------------------- relay time

const json* thermostatEntry(const DecisionRecord& d) {
    if (!d.plugs.is_array()) return nullptr;
    for (const auto& p : d.plugs) {
        if (p.is_object() && p.value("thermostat", false)) return &p;
    }
    return nullptr;
}

void useRelayTime(std::vector<DecisionRecord>& ds, seconds maxGap) {
    const auto counter = [](const json* p) -> std::optional<double> {
        if (!p || !p->contains("relayOnSeconds") || !(*p)["relayOnSeconds"].is_number()) return std::nullopt;
        return (*p)["relayOnSeconds"].get<double>();
    };
    for (std::size_t i = 0; i < ds.size(); ++i) {
        DecisionRecord& d = ds[i];
        if (const json* p = thermostatEntry(d)) {
            std::optional<bool> relay;
            if (i + 1 < ds.size() && ds[i + 1].at > d.at && ds[i + 1].at - d.at <= maxGap) {
                const auto a = counter(p), b = counter(thermostatEntry(ds[i + 1]));
                const double span = duration<double>(ds[i + 1].at - d.at).count();
                // Added less than nothing (reset) or more than the interval (catching up): not this interval's.
                if (a && b && *b >= *a && *b - *a <= span + 60) relay = *b - *a >= span / 2;
            }
            if (!relay && p->contains("state") && (*p)["state"].is_boolean()) relay = (*p)["state"].get<bool>();
            if (relay) d.on = *relay;
        }
        if (i > 0) d.changed = d.on != ds[i - 1].on;
    }
}

}  // namespace hb
