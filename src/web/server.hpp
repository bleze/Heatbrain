#pragma once

#include <filesystem>
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

// JSON API + static frontend, served from its own thread.
//   GET    /api/status                      current decisions, plugs, problems, prices
//   GET    /api/rooms/<id>/history?hours=N  temperatures, decisions, events, prices
//   POST   /api/rooms/<id>/override         {"on": true, "minutes": 60}
//   DELETE /api/rooms/<id>/override
class WebServer {
public:
    // onChange is called after an override was set or cleared (wakes the control loop).
    WebServer(WebConfig cfg, const StatusBoard& board, std::filesystem::path dataDir, std::vector<RoomConfig> rooms,
              std::filesystem::path webRoot, std::function<void()> onChange = {});
    ~WebServer();

    void start();  // throws if the port cannot be bound
    void stop();

private:
    void routes();

    WebConfig cfg_;
    const StatusBoard& board_;
    std::filesystem::path dataDir_;
    std::vector<RoomConfig> rooms_;
    std::filesystem::path webRoot_;
    std::function<void()> onChange_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};

// Finds the frontend files: <source>/web in dev builds, /app/web in Docker, ./web otherwise.
std::filesystem::path locateWebRoot(const std::vector<std::filesystem::path>& candidates);

}  // namespace hb
