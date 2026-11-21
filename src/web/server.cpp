#include "web/server.hpp"

#include <httplib.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <future>

#include "app/config_service.hpp"
#include "app/rooms.hpp"
#include "core/log.hpp"
#include "notify/notifier.hpp"
#include "plugs/plugs.hpp"
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

WebServer::WebServer(WebConfig cfg, const StatusBoard& board, WebContext ctx)
    : cfg_(std::move(cfg)), board_(board), ctx_(std::move(ctx)), server_(std::make_unique<httplib::Server>()) {
    routes();
}

WebServer::~WebServer() { stop(); }

void WebServer::setRooms(std::vector<RoomConfig> rooms) {
    std::lock_guard lock(roomsMutex_);
    rooms_ = std::move(rooms);
}

std::vector<RoomConfig> WebServer::rooms() const {
    std::lock_guard lock(roomsMutex_);
    return rooms_;
}

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

json readConfigFile(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    return json::parse(in, nullptr, true, /*ignore_comments=*/true);
}

json parseBody(const httplib::Request& req) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) throw std::invalid_argument("request body is not valid JSON");
    return body;
}

json failsafeJson(Plug& plug) {
    try {
        const auto fs = plug.sw->checkFailsafe();
        return json{{"installed", fs.installed()}, {"problems", fs.problems}};
    } catch (const std::exception& e) {
        return json{{"installed", false}, {"problems", json::array({std::string("could not check: ") + e.what()})}};
    }
}

}  // namespace

void WebServer::routes() {
    auto& s = *server_;

    // --- Dashboard ----------------------------------------------------------------------

    s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, board_.get()); });

    s.Get(R"(/api/rooms/([\w-]+)/history)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto all = rooms();
        const auto room = std::ranges::find_if(all, [&](const RoomConfig& r) { return r.id == id; });
        if (room == all.end()) return sendError(res, 404, "unknown room");
        int hoursBack = 24;
        if (req.has_param("hours")) hoursBack = std::clamp(std::atoi(req.get_param_value("hours").c_str()), 1, 24 * 14);

        // One SQLite connection per request: connections are not shared across threads.
        History history(ctx_.dataDir / "history.db");
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
    });

    s.Post(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto all = rooms();
        if (std::ranges::none_of(all, [&](const RoomConfig& r) { return r.id == id; })) {
            return sendError(res, 404, "unknown room");
        }
        const json body = parseBody(req);
        if (!body.is_object() || !body.contains("on") || !body.contains("minutes")) {
            return sendError(res, 400, R"(expected {"on": true|false, "minutes": N})");
        }
        const int minutesFor = body.value("minutes", 0);
        if (minutesFor < 1 || minutesFor > 7 * 24 * 60) return sendError(res, 400, "minutes must be 1..10080");
        const Instant until = floor<seconds>(system_clock::now()) + minutes{minutesFor};
        Overrides(ctx_.dataDir / "overrides.json").set(id, ManualOverride{body.value("on", false), until});
        log::info("Web: manual {} for {} until {}", body.value("on", false) ? "ON" : "OFF", id, formatIsoUtc(until));
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"room", id}, {"on", body.value("on", false)}, {"until", epoch(until)}});
    });

    s.Delete(R"(/api/rooms/([\w-]+)/override)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        Overrides(ctx_.dataDir / "overrides.json").clear(id);
        log::info("Web: override cleared for {}", id);
        if (ctx_.onOverride) ctx_.onOverride();
        sendJson(res, json{{"room", id}, {"cleared", true}});
    });

    // --- Settings -----------------------------------------------------------------------

    s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        const json cfg = readConfigFile(ctx_.configFile);
        sendJson(res, json{{"config", maskSecrets(cfg)}, {"errors", validateConfig(cfg, ctx_.dataDir)}});
    });

    s.Post("/api/config/validate", [this](const httplib::Request& req, httplib::Response& res) {
        const json incoming = unmaskSecrets(parseBody(req), readConfigFile(ctx_.configFile));
        sendJson(res, json{{"errors", validateConfig(incoming, ctx_.dataDir)}});
    });

    s.Put("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        const json incoming = unmaskSecrets(parseBody(req), readConfigFile(ctx_.configFile));
        const auto errors = validateConfig(incoming, ctx_.dataDir);
        if (!errors.empty()) return sendJson(res, json{{"errors", errors}}, 400);
        saveConfig(ctx_.configFile, incoming);
        log::info("Web: configuration saved - reloading");
        if (ctx_.onConfigSaved) ctx_.onConfigSaved();
        sendJson(res, json{{"saved", true}});
    });

    s.Get("/api/plugs", [this](const httplib::Request&, httplib::Response& res) {
        // Own plug objects from the saved config: never shares state with the control loop.
        const json cfg = readConfigFile(ctx_.configFile);
        HttplibClient lan(milliseconds{2500}, milliseconds{4000});
        auto plugs = loadPlugs(cfg, lan);
        std::vector<std::future<json>> futures;
        for (auto& p : plugs) {
            futures.push_back(std::async(std::launch::async, [&p] {
                const auto st = p.sw->status();
                json j{{"id", p.id}, {"reachable", st.reachable}, {"firmware", st.firmware}, {"hardware", st.hardware},
                       {"deviceName", st.deviceName}, {"error", st.error}};
                if (st.on) j["on"] = *st.on;
                if (st.rssi) j["rssi"] = *st.rssi;
                j["failsafe"] = st.reachable ? failsafeJson(p) : json{{"installed", false}, {"problems", json::array()}};
                return j;
            }));
        }
        json out = json::array();
        for (auto& f : futures) out.push_back(f.get());
        sendJson(res, out);
    });

    s.Post(R"(/api/plugs/([\w-]+)/(provision|restore))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const std::string action = req.matches[2];
        const json cfg = readConfigFile(ctx_.configFile);
        HttplibClient lan(milliseconds{3000}, milliseconds{5000});
        auto plugs = loadPlugs(cfg, lan);
        auto it = std::ranges::find_if(plugs, [&](const Plug& p) { return p.id == id; });
        if (it == plugs.end()) return sendError(res, 404, "unknown plug (save the configuration first)");
        Plug& plug = *it;
        try {
            if (action == "provision") {
                const json body = req.body.empty() ? json::object() : parseBody(req);
                const auto backupFile = saveBackup(ctx_.dataDir, plug, plug.sw->backup());
                plug.sw->provision(body.value("force", false));
                log::info("Web: provisioned plug {} (backup {})", id, backupFile.string());
                sendJson(res, json{{"failsafe", failsafeJson(plug)}, {"backup", backupFile.filename().string()}});
            } else {
                if (plug.mode == PlugMode::Live) {
                    return sendError(res, 409, "Set the plug to dry-run and save before restoring: a live plug "
                                               "without its fail-safe must not be switched.");
                }
                const auto file = latestBackup(ctx_.dataDir, plug);
                std::ifstream in(file);
                plug.sw->restore(json::parse(in));
                log::info("Web: restored plug {} from {}", id, file.string());
                sendJson(res, json{{"restored", file.filename().string()}, {"failsafe", failsafeJson(plug)}});
            }
        } catch (const std::exception& e) {
            sendError(res, 502, e.what());
        }
    });

    s.Post("/api/discover", [](const httplib::Request& req, httplib::Response& res) {
        const json body = parseBody(req);
        HttplibClient probe(milliseconds{1500}, milliseconds{2000});
        json out = json::array();
        for (const auto& d : discoverTasmota(body.value("subnet", std::string()), probe)) {
            json j{{"host", d.host}, {"deviceName", d.deviceName}, {"firmware", d.firmware}, {"hardware", d.hardware}};
            if (d.on) j["on"] = *d.on;
            out.push_back(j);
        }
        sendJson(res, out);
    });

    s.Post("/api/notify/test", [this](const httplib::Request&, httplib::Response& res) {
        HttplibClient http;
        auto nc = loadNotifications(readConfigFile(ctx_.configFile), http);
        if (nc.channels.empty()) return sendError(res, 400, "No notification channel configured (save first)");
        json results = json::array();
        for (auto& ch : nc.channels) {
            try {
                ch->sendText("heatbrain test", "If you can read this, notifications work. 🔥");
                results.push_back({{"channel", ch->name()}, {"ok", true}});
            } catch (const std::exception& e) {
                results.push_back({{"channel", ch->name()}, {"ok", false}, {"error", e.what()}});
            }
        }
        sendJson(res, results);
    });

    if (!ctx_.webRoot.empty()) s.set_mount_point("/", ctx_.webRoot.string());

    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::invalid_argument& e) {
            sendError(res, 400, e.what());
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
              ctx_.webRoot.empty() ? " (API only: web files not found)" : "");
}

void WebServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

}  // namespace hb
