#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace hb {

// The power mix of the price area (wind, solar, power plants, imports, CO2), fetched in the
// background every 15 minutes for the room charts. Never blocks the control loop; the result goes
// to <dataDir>/power-mix.json for the web server.
class PowerMixSync {
public:
    PowerMixSync(std::string area, std::filesystem::path dataDir);
    ~PowerMixSync();
    PowerMixSync(const PowerMixSync&) = delete;
    PowerMixSync& operator=(const PowerMixSync&) = delete;

private:
    void run();
    void syncOnce();

    std::string area_;
    std::filesystem::path dataDir_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// {"area": "DK2", "updated": <epoch>, "hours": [[epoch, offshore, onshore, solar, plants, imports, co2|null], ...]}
nlohmann::json loadPowerMix(const std::filesystem::path& dataDir);

// The price area of the configured spot price ("DK1"/"DK2"), empty if none.
std::string spotArea(const nlohmann::json& cfg);

}  // namespace hb
