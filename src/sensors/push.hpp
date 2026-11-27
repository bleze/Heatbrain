#pragma once

// Sensors that report by themselves instead of being asked: battery devices that sleep between
// readings, like the Shelly H&T Gen3. The device calls heatbrain's /api/push/shelly with its readings
// (a "webhook" / Action set up on the device); they are kept here and read as the source "shelly".
//
// Such a sensor only reports when a value changes (by its threshold, e.g. 0.5 °C), so no news means
// "still about the same": a reading is treated as current for kPushQuietOk after it came, then as stale.
// A steady room can be quiet for many hours (a shed at 6.4 °C all day), and while the Wi-Fi is off at
// night nothing can come through at all - so the limit is long; a device that has really gone (battery)
// shows up as low battery first.

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sensors/temperature_source.hpp"

namespace hb {

inline constexpr std::chrono::hours kPushQuietOk{12};

class PushRegistry {
public:
    static PushRegistry& instance();
    // Merges what one report carried (a temperature and a humidity report may come separately).
    void put(const std::string& id, const std::string& name, std::optional<double> temperature,
             std::optional<double> humidity, std::optional<int> battery, Instant now);
    struct Entry {
        std::string name;
        std::optional<double> temperature, humidity;
        std::optional<int> battery;
        Instant lastReport{};
    };
    std::map<std::string, Entry> snapshot() const;
    void clear();  // tests
    // Kept in this file across restarts (a sensor may not report again for hours).
    void setFile(const std::filesystem::path& file);

private:
    void save() const;  // with mutex_ held
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::filesystem::path file_;
};

// The pushed readings as a temperature source (id "shelly").
std::vector<SensorReading> pushReadings(const std::map<std::string, PushRegistry::Entry>& entries, Instant now);

class PushSource final : public TemperatureSource {
public:
    PushSource(std::string id, std::function<Instant()> now) : id_(std::move(id)), now_(std::move(now)) {}
    const std::string& id() const override { return id_; }
    std::vector<SensorReading> read() override { return pushReadings(PushRegistry::instance().snapshot(), now_()); }

private:
    std::string id_;
    std::function<Instant()> now_;
};

}  // namespace hb
