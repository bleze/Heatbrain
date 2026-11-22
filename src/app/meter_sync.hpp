#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/time.hpp"
#include "storage/history.hpp"

namespace hb {

// "meter": {"type": "eloverblik", "refreshToken": "...", "meteringPoint": ""}  (point optional)
struct MeterConfig {
    std::string refreshToken;
    std::string meteringPoint;  // empty: the first consumption point on the account
};
std::optional<MeterConfig> parseMeterConfig(const nlohmann::json& cfg);

// Fetches the household's meter readings in the background: at start, every 6 hours, and when
// asked (Settings -> "Fetch now"). Never blocks the control loop. Status for the UI goes to
// <dataDir>/meter-status.json.
class MeterSync {
public:
    MeterSync(MeterConfig cfg, std::filesystem::path dataDir, const TimeZone& tz);
    ~MeterSync();
    MeterSync(const MeterSync&) = delete;
    MeterSync& operator=(const MeterSync&) = delete;

    static void requestNow();  // from the web server

private:
    void run();
    void syncOnce();

    MeterConfig cfg_;
    std::filesystem::path dataDir_;
    const TimeZone& tz_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

nlohmann::json loadMeterStatus(const std::filesystem::path& dataDir);

// Measured household use and heatbrain's radiators (live plugs that were on x their power) in the
// same intervals as the meter readings: [[start, seconds, kWh], ...] each.
nlohmann::json usageReport(const nlohmann::json& cfg, const History& history, Instant from, Instant to);

}  // namespace hb
