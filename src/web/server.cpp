#include "web/server.hpp"

#include <httplib.h>

#include <algorithm>
#include <format>

#include "app/rooms.hpp"
#include "core/log.hpp"
#include "storage/history.hpp"

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

WebConfig parseWebConfig(const json& cfg) {
    WebConfig w;
    if (!cfg.contains("web")) return w;
    const auto& j = cfg["web"];
    w.enabled = j.value("enabled", w.enabled);
    w.bind = j.value("bind", w.bind);
    w.port = j.value("port", w.port);
    return w;
}

fs::path locateWebRoot(const std::vector<fs::path>& candidates) {
    for (const auto& c : candidates) {
        if (fs::exists(c / "index.html")) return c;
    }
    return {};
}

WebServer::WebServer(WebConfig cfg, const StatusBoard& board, fs::path dataDir, std::vector<RoomConfig> rooms,
                     fs::path webRoot, std::function<void()> onChange)
    : cfg_(std::move(cfg)),
      board_(board),
      dataDir_(std::move(dataDir)),
      rooms_(std::move(rooms)),
      webRoot_(std::move(webRoot)),
      onChange_(std::move(onChange)),
      server_(std::make_unique<httplib::Server>()) {
    routes();
}

WebServer::~WebServer() { stop(); }

namespace {

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, json{{"error", message}}, status);
}

long long epoch(Instant t) { return t.time_since_epoch().count(); }

}  // namespace

void WebServer::routes() {
    auto& s = *server_;

    s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, board_.get()); });

    s.Get(R"(/api/rooms/([\w-]+)/history)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto room = std::ranges::find_if(rooms_, [&](const RoomConfig& r) { return r.id == id; });
        if (room == rooms_.end()) return sendError(res, 404, "unknown room");
        int hoursBack = 24;
        if (req.has_param("hours")) hoursBack = std::clamp(std::atoi(req.get_param_value("hours").c_str()), 1, 24 * 14);

        try {
            // One SQLite connection per request: connections are not shared across threads.
            History history(dataDir_ / "history.db");
            const Instant to = floor<seconds>(system_clock::now()) + seconds{1};
            const Instant from = to - hours{hoursBack};

            json temps = json::array();
            for (const auto& t : history.temperatures(id, from, to)) temps.push_back({epoch(t.at), t.temperature});

            json decisions = json::array();
            json events = json::array();
            for (const auto& d : history.decisions(id, from, to, false)) {
                decisions.push_back({epoch(d.at), d.on ? 1 : 0, d.reason,
                                     d.inputs.contains("target") ? d.inputs["target"] : json(nullptr)});
                if (d.changed) {
                    events.push_back({{"ts", epoch(d.at)}, {"on", d.on}, {"reason", d.reason}, {"detail", d.detail},
                                      {"inputs", d.inputs}, {"plugs", d.plugs}});
                }
            }
            json prices = json::array();
            for (const auto& p : history.prices(from, to + hours{36})) prices.push_back({epoch(p.slot), p.total});

            sendJson(res, json{{"room",
                                {{"id", room->id},
                                 {"name", room->name},
                                 {"minTemp", room->minTemp},
                                 {"maxTemp", room->maxTemp},
                                 {"priceOnly", room->priceOnly.has_value()}}},
                               {"from", epoch(from)},
                               {"to", epoch(to)},
                               {"temperatures", temps},
                               {"decisions", decisions},
                               {"events", events},
                               {"prices", prices}});
        } catch (const std::exception& e) {
            sendError(res, 500, e.what());
        }
    });

    s.Post(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        if (std::ranges::none_of(rooms_, [&](const RoomConfig& r) { return r.id == id; })) {
            return sendError(res, 404, "unknown room");
        }
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("on") || !body.contains("minutes")) {
            return sendError(res, 400, R"(expected {"on": true|false, "minutes": N})");
        }
        const int minutesFor = body.value("minutes", 0);
        if (minutesFor < 1 || minutesFor > 7 * 24 * 60) return sendError(res, 400, "minutes must be 1..10080");
        const Instant until = floor<seconds>(system_clock::now()) + minutes{minutesFor};
        Overrides(dataDir_ / "overrides.json").set(id, ManualOverride{body.value("on", false), until});
        log::info("Web: manual {} for {} until {}", body.value("on", false) ? "ON" : "OFF", id, formatIsoUtc(until));
        if (onChange_) onChange_();
        sendJson(res, json{{"room", id}, {"on", body.value("on", false)}, {"until", epoch(until)}});
    });

    s.Delete(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        Overrides(dataDir_ / "overrides.json").clear(id);
        log::info("Web: override cleared for {}", id);
        if (onChange_) onChange_();
        sendJson(res, json{{"room", id}, {"cleared", true}});
    });

    if (!webRoot_.empty()) s.set_mount_point("/", webRoot_.string());

    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            sendError(res, 500, e.what());
        }
    });
}

void WebServer::start() {
    if (!server_->bind_to_port(cfg_.bind, cfg_.port)) {
        throw std::runtime_error(std::format("Web UI: cannot listen on {}:{} (port in use?)", cfg_.bind, cfg_.port));
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    log::info("Web UI on http://{}:{}/{}", cfg_.bind == "0.0.0.0" ? "localhost" : cfg_.bind, cfg_.port,
              webRoot_.empty() ? " (API only: web files not found)" : "");
}

void WebServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

}  // namespace hb
