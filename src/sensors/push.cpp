#include "sensors/push.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

#include "core/log.hpp"

namespace hb {

using namespace std::chrono;

PushRegistry& PushRegistry::instance() {
    static PushRegistry r;
    return r;
}

void PushRegistry::put(const std::string& id, const std::string& name, std::optional<double> temperature,
                       std::optional<double> humidity, std::optional<int> battery, Instant now) {
    std::lock_guard lock(mutex_);
    auto& e = entries_[id];
    if (!name.empty()) e.name = name;
    if (e.name.empty()) e.name = id;
    if (temperature) e.temperature = temperature;
    if (humidity) e.humidity = humidity;
    if (battery) e.battery = battery;
    e.lastReport = now;
    save();
}

void PushRegistry::setFile(const std::filesystem::path& file) {
    std::lock_guard lock(mutex_);
    file_ = file;
    if (!std::filesystem::exists(file_)) return;
    try {
        const auto j = nlohmann::json::parse(std::ifstream(file_));
        for (const auto& [id, v] : j.items()) {
            if (entries_.contains(id)) continue;  // a newer report since start
            Entry e;
            e.name = v.value("name", id);
            if (v.contains("temperature")) e.temperature = v["temperature"].get<double>();
            if (v.contains("humidity")) e.humidity = v["humidity"].get<double>();
            if (v.contains("battery")) e.battery = v["battery"].get<int>();
            e.lastReport = Instant{seconds{v.value("lastReport", 0LL)}};
            entries_[id] = e;
        }
    } catch (const std::exception& ex) {
        log::warn("Ignoring unreadable {}: {}", file_.string(), ex.what());
    }
}

void PushRegistry::save() const {
    if (file_.empty()) return;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [id, e] : entries_) {
        nlohmann::json v{{"name", e.name}, {"lastReport", e.lastReport.time_since_epoch().count()}};
        if (e.temperature) v["temperature"] = *e.temperature;
        if (e.humidity) v["humidity"] = *e.humidity;
        if (e.battery) v["battery"] = *e.battery;
        j[id] = v;
    }
    try {
        auto tmp = file_;
        tmp += ".tmp";
        std::ofstream(tmp, std::ios::trunc) << j.dump(1);
        std::filesystem::rename(tmp, file_);
    } catch (const std::exception& ex) {
        log::warn("Could not save {}: {}", file_.string(), ex.what());
    }
}

std::map<std::string, PushRegistry::Entry> PushRegistry::snapshot() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

void PushRegistry::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

std::vector<SensorReading> pushReadings(const std::map<std::string, PushRegistry::Entry>& entries, Instant now) {
    std::vector<SensorReading> out;
    for (const auto& [id, e] : entries) {
        if (!e.temperature) continue;  // only a humidity report so far
        SensorReading r;
        r.sensorId = id;
        r.name = e.name;
        r.station = "Shelly";
        r.kind = "indoor";
        r.temperature = e.temperature;
        r.humidity = e.humidity;
        r.batteryPercent = e.battery;
        // No report = no change worth reporting: current until kPushQuietOk, then stale (and the
        // planner stops trusting it, as for any sensor that goes quiet).
        const bool quietOk = now - e.lastReport < kPushQuietOk;
        r.measuredAt = quietOk ? now : e.lastReport;
        r.reachable = quietOk;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace hb
