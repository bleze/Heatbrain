#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/room.hpp"
#include "core/time.hpp"

namespace httplib {
class Server;
}

namespace hb {

// Latest controller state, written by the control loop and read by web requests.
class StatusBoard {
public:
    void set(nlohmann::json status) {
        std::lock_guard lock(mutex_);
        status_ = std::move(status);
    }
    nlohmann::json get() const {
        std::lock_guard lock(mutex_);
        return status_;
    }

private:
    mutable std::mutex mutex_;
    nlohmann::json status_ = nlohmann::json::object();
};

struct WebConfig {
    bool enabled = true;
    std::string bind = "0.0.0.0";
    int port = 8080;
};

WebConfig parseWebConfig(const nlohmann::json& cfg);

struct WebContext {
    std::filesystem::path dataDir;
    std::filesystem::path configFile;
    std::filesystem::path webRoot;           // empty: API only
    std::function<void()> onOverride;        // an override was set/cleared: re-plan now
    std::function<void()> onConfigSaved;     // config replaced: reload the controller
};

// JSON API + static frontend, served from its own thread.
//
// Dashboard:
//   GET    /api/status                      current decisions, plugs, problems, prices, sensors
//   GET    /api/rooms/<id>/history?hours=N  temperatures, decisions, events, prices
//   POST   /api/rooms/<id>/override         {"on": true, "minutes": 60}
//   DELETE /api/rooms/<id>/override
// Settings:
//   GET    /api/config                      config with secrets masked
//   POST   /api/config/validate             {"errors": [...]}
//   PUT    /api/config                      validate, back up, save, reload
//   GET    /api/plugs                       live status + fail-safe check of configured plugs
//   POST   /api/plugs/<id>/provision        {"force": false}
//   POST   /api/plugs/<id>/restore          refused while the plug is live
//   POST   /api/discover                    {"subnet": "192.168.1.0/24"}
//   POST   /api/notify/test
//   GET    /api/firmware/latest             newest Tasmota release (cached 12 h)
//   POST   /api/plugs/<id>/upgrade          background job: OTA upgrade + fail-safe test
//   POST   /api/plugs/<id>/test             background job: fail-safe function test
//   POST   /api/plugs/<id>/restart
//   GET    /api/jobs                        progress of running/finished jobs per plug

// A long-running device operation with a progress log, polled by the UI.
struct Job {
    std::string plugId;
    std::string kind;              // "upgrade" | "test"
    std::string state = "running"; // running | done | failed
    std::vector<std::pair<long long, std::string>> log;  // (unix time, message)
    long long started = 0;
    long long finished = 0;
};

class WebServer {
public:
    WebServer(WebConfig cfg, const StatusBoard& board, WebContext ctx);
    ~WebServer();

    void start();  // throws if the port cannot be bound
    void stop();
    void setRooms(std::vector<RoomConfig> rooms);

private:
    void routes();
    std::vector<RoomConfig> rooms() const;
    nlohmann::json latestFirmware();
    // Starts fn on a worker thread as the job for plugId; false if any job is running.
    bool startJob(const std::string& plugId, const std::string& kind,
                  std::function<bool(const std::function<void(const std::string&)>& log)> fn);
    void jobSleep(std::chrono::milliseconds d) const;  // throws when the server stops

    WebConfig cfg_;
    const StatusBoard& board_;
    WebContext ctx_;
    mutable std::mutex roomsMutex_;
    std::vector<RoomConfig> rooms_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;

    mutable std::mutex jobsMutex_;
    std::map<std::string, Job> jobs_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
};

// Finds the frontend files: <source>/web in dev builds, /app/web in Docker, ./web otherwise.
std::filesystem::path locateWebRoot(const std::vector<std::filesystem::path>& candidates);

}  // namespace hb
