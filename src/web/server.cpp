#include "web/server.hpp"

#include <httplib.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <regex>

#include "app/config_service.hpp"
#include "app/rooms.hpp"
#include "core/cost.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "notify/notifier.hpp"
#include "plugs/plugs.hpp"
#include "plugs/tasmota.hpp"
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
        if (fs::exists(c / "index.template.html")) return c;
    }
    return {};
}

namespace {

std::string readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The page with every local script/stylesheet URL fingerprinted by its contents
// ("app.js" -> "app.js?v=1a2b3c4d"): a changed file gets a new URL, so no browser can keep
// running an old copy, whatever its cache does.
std::string renderIndex(const fs::path& webRoot) {
    std::string html = readWhole(webRoot / "index.template.html");
    static const std::regex asset(R"re((src|href)="([\w.-]+\.(?:js|css))")re");
    std::string out;
    auto it = html.cbegin();
    for (std::smatch m; std::regex_search(it, html.cend(), m, asset); it = m.suffix().first) {
        out.append(it, m[0].first);
        std::uint32_t h = 2166136261u;  // FNV-1a
        for (unsigned char c : readWhole(webRoot / m[2].str())) {
            h ^= c;
            h *= 16777619u;
        }
        out += std::format(R"({}="{}?v={:08x}")", m[1].str(), m[2].str(), h);
    }
    out.append(it, html.cend());
    return out;
}

}  // namespace

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

    // Energy and cost per room and day, from the decision log, stored prices and each plug's
    // wattage. "saved" compares with paying each day's average price for the same energy.
    s.Get("/api/costs", [this](const httplib::Request& req, httplib::Response& res) {
        const int dayCount = req.has_param("days") ? std::clamp(std::atoi(req.get_param_value("days").c_str()), 1, 90) : 30;
        const json cfg = readConfigFile(ctx_.configFile);
        const TimeZone tz(cfg.value("timezone", std::string("Europe/Copenhagen")));
        const int tick = cfg.contains("control") ? cfg["control"].value("tickSeconds", 300) : 300;
        const Instant now = floor<seconds>(system_clock::now());
        const Instant from = startOfLocalDay(tz, now - hours{24 * (dayCount - 1)});

        History history(ctx_.dataDir / "history.db");
        std::map<Instant, double> prices;
        for (const auto& p : history.prices(from, now + hours{1})) prices[p.slot] = p.total;
        const auto market = dailyAveragePrice(prices, tz);

        std::map<std::string, double> wattsByPlug;
        for (const auto& p : cfg.value("plugs", json::array())) {
            if (p.contains("watts") && p["watts"].is_number()) wattsByPlug[p.value("id", "")] = p["watts"].get<double>();
        }

        json rooms = json::array();
        json totals{{"kWh", 0.0}, {"cost", 0.0}, {"saved", 0.0}};
        for (const auto& r : cfg.value("rooms", json::array())) {
            const std::string id = r.value("id", "");
            double watts = 0;
            json missing = json::array();
            for (const auto& pid : r.value("plugs", json::array())) {
                const std::string plugId = pid.get<std::string>();
                if (auto it = wattsByPlug.find(plugId); it != wattsByPlug.end()) watts += it->second;
                else missing.push_back(plugId);
            }
            json room{{"id", id}, {"name", r.value("name", id)}, {"watts", watts}, {"missingWatts", missing}};
            if (!missing.empty() || watts <= 0) {
                rooms.push_back(room);
                continue;
            }
            std::vector<DecisionPoint> points;
            for (const auto& d : history.decisions(id, from, now + seconds{1}, false)) {
                const bool live = std::ranges::any_of(d.plugs, [](const json& p) { return p.value("mode", "") == "live"; });
                points.push_back({d.at, d.on, live});
            }
            const auto days = dailyCost(points, prices, watts, seconds{3 * tick}, from, now, tz);
            json dj = json::array();
            double kWh = 0, cost = 0, saved = 0, onHours = 0, live = 0;
            for (const auto& d : days) {
                dj.push_back({{"date", d.date}, {"kWh", d.kWh}, {"cost", d.cost}, {"onHours", d.onHours},
                              {"liveKWh", d.liveKWh}, {"kWhWithoutPrice", d.kWhWithoutPrice}});
                kWh += d.kWh;
                cost += d.cost;
                onHours += d.onHours;
                live += d.liveKWh;
                if (auto m = market.find(d.date); m != market.end()) saved += (d.kWh - d.kWhWithoutPrice) * m->second;
            }
            saved -= cost;
            room["days"] = dj;
            room["total"] = {{"kWh", kWh}, {"cost", cost}, {"onHours", onHours}, {"liveKWh", live}, {"saved", saved}};
            totals["kWh"] = totals["kWh"].get<double>() + kWh;
            totals["cost"] = totals["cost"].get<double>() + cost;
            totals["saved"] = totals["saved"].get<double>() + saved;
            rooms.push_back(room);
        }
        json marketJson = json::object();
        for (const auto& [date, avg] : market) marketJson[date] = avg;
        sendJson(res, json{{"currency", cfg.value("currency", std::string("DKK"))},
                           {"from", epoch(from)},
                           {"to", epoch(now)},
                           {"days", dayCount},
                           {"market", marketJson},
                           {"rooms", rooms},
                           {"total", totals}});
    });

    // --- Settings -----------------------------------------------------------------------

    s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        // Rooms are always presented as daily programs (older configs are converted).
        const json cfg = withPrograms(readConfigFile(ctx_.configFile));
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
        json latest;
        try {
            latest = latestFirmware();
        } catch (const std::exception& e) {
            hb::log::debug("Latest firmware lookup failed: {}", e.what());
        }
        std::vector<std::future<json>> futures;
        for (auto& p : plugs) {
            futures.push_back(std::async(std::launch::async, [&p, &latest, this] {
                const auto st = p.sw->status();
                json j{{"id", p.id}, {"reachable", st.reachable}, {"firmware", st.firmware}, {"hardware", st.hardware},
                       {"deviceName", st.deviceName}, {"error", st.error}};
                if (st.on) j["on"] = *st.on;
                if (st.rssi) j["rssi"] = *st.rssi;
                j["failsafe"] = st.reachable ? failsafeJson(p) : json{{"installed", false}, {"problems", json::array()}};
                if (st.reachable && latest.is_object() && !latest.value("version", "").empty()) {
                    j["latest"] = latest;
                    j["updateAvailable"] = compareVersions(st.firmware, latest.value("version", "")) < 0;
                }
                if (const auto rec = loadFirmwareRecord(ctx_.dataDir, p.id)) {
                    j["test"] = {{"version", rec->version}, {"verified", rec->verified}, {"checkedAt", rec->checkedAt},
                                 {"detail", rec->detail},
                                 {"matchesFirmware", st.reachable && compareVersions(st.firmware, rec->version) == 0}};
                }
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

    s.Get("/api/firmware/latest", [this](const httplib::Request&, httplib::Response& res) {
        try {
            sendJson(res, latestFirmware());
        } catch (const std::exception& e) {
            sendError(res, 502, std::string("Could not check for Tasmota releases: ") + e.what());
        }
    });

    s.Get("/api/jobs", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::object();
        std::lock_guard lock(jobsMutex_);
        for (const auto& [id, j] : jobs_) {
            json log = json::array();
            for (const auto& [t, m] : j.log) log.push_back({t, m});
            out[id] = {{"kind", j.kind}, {"state", j.state}, {"started", j.started}, {"finished", j.finished}, {"log", log}};
        }
        sendJson(res, out);
    });

    s.Post(R"(/api/plugs/([\w-]+)/(upgrade|test|restart))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const std::string action = req.matches[2];
        const json cfg = readConfigFile(ctx_.configFile);
        // Each job builds its own client and plug objects: nothing is shared with the control loop.
        auto lan = std::make_shared<HttplibClient>(milliseconds{3000}, milliseconds{5000});
        auto plugs = std::make_shared<std::vector<Plug>>(loadPlugs(cfg, *lan));
        auto it = std::ranges::find_if(*plugs, [&](const Plug& p) { return p.id == id; });
        if (it == plugs->end()) return sendError(res, 404, "unknown plug (save the configuration first)");
        auto* tasmota = dynamic_cast<TasmotaSwitch*>(it->sw.get());
        if (!tasmota) return sendError(res, 400, "only Tasmota plugs support this");

        if (action == "restart") {
            try {
                tasmota->restart();
            } catch (const std::exception& e) {
                return sendError(res, 502, e.what());
            }
            return sendJson(res, json{{"restarting", true}});
        }
        if (it->mode == PlugMode::Live) {
            return sendError(res, 409, "Switch the plug to dry-run and save first: heatbrain never tests or upgrades a plug it is controlling.");
        }
        const auto status = it->sw->status();
        if (!status.reachable) return sendError(res, 502, "plug unreachable: " + status.error);

        const fs::path dataDir = ctx_.dataDir;
        const auto record = [dataDir, id](const std::string& version, bool ok, const std::string& detail) {
            saveFirmwareRecord(dataDir, id, FirmwareRecord{version, ok, formatIsoUtc(floor<seconds>(system_clock::now())), detail});
        };

        bool started = false;
        if (action == "test") {
            started = startJob(id, "test", [this, lan, plugs, tasmota, status, record](const auto& log) {
                log("Fail-safe test on Tasmota " + status.firmware + " (the relay is on for about 30 seconds)");
                const bool ok = tasmota->testPulse(log, [this](milliseconds d) { jobSleep(d); });
                record(status.firmware, ok, ok ? "auto-off and refresh verified" : "fail-safe test failed");
                log(ok ? "Fail-safe works on this firmware" : "Fail-safe test FAILED: this plug will not be switched live");
                return ok;
            });
        } else {
            json latest;
            try {
                latest = latestFirmware();
            } catch (const std::exception& e) {
                return sendError(res, 502, std::string("Could not check for Tasmota releases: ") + e.what());
            }
            const std::string target = latest.value("version", "");
            if (compareVersions(status.firmware, target) >= 0) {
                return sendError(res, 409, "Already on the latest version (" + status.firmware + ")");
            }
            Plug* plug = &*it;
            started = startJob(id, "upgrade", [this, lan, plugs, plug, tasmota, status, target, record, dataDir](const auto& log) {
                log("Upgrading from " + status.firmware + " to " + target + ". Do not unplug the plug.");
                const auto backup = saveBackup(dataDir, *plug, tasmota->backup());
                log("Settings backed up to " + backup.filename().string());
                const std::string url = tasmotaOtaUrl(status.firmware, status.hardware);
                tasmota->startUpgrade(url);
                log("Downloading " + url + " - the plug restarts by itself (twice on 1 MB devices)");

                // Wait for the real target build: 1 MB devices run "minimal" in between.
                std::string seen = status.firmware;
                bool wentOffline = false;
                int stable = 0;
                const auto deadline = std::chrono::steady_clock::now() + minutes{10};
                while (std::chrono::steady_clock::now() < deadline) {
                    jobSleep(seconds{5});
                    const auto st = tasmota->status();
                    if (!st.reachable) {
                        if (!wentOffline) log("Plug is restarting...");
                        wentOffline = true;
                        stable = 0;
                        continue;
                    }
                    if (st.firmware != seen) {
                        log("Plug is back running " + st.firmware);
                        seen = st.firmware;
                    }
                    const bool minimal = st.firmware.find("minimal") != std::string::npos;
                    stable = (!minimal && compareVersions(st.firmware, target) == 0) ? stable + 1 : 0;
                    if (stable >= 2) break;
                }
                if (stable < 2) {
                    log("FAILED: the plug did not come back on " + target + " within 10 minutes (last seen: " + seen + ")");
                    record(seen, false, "upgrade did not complete");
                    return false;
                }
                log("Upgrade complete: now on " + seen);

                const auto fsCheck = tasmota->checkFailsafe();
                if (fsCheck.installed()) {
                    log("Fail-safe settings survived the upgrade");
                } else {
                    for (const auto& p : fsCheck.problems) log("Note: " + p);
                }

                log("Running the fail-safe test (the relay is on for about 30 seconds)");
                const bool ok = tasmota->testPulse(log, [this](milliseconds d) { jobSleep(d); });
                record(seen, ok, ok ? "verified after upgrade" : "fail-safe test failed after upgrade");
                log(ok ? "All good: fail-safe verified on " + seen
                       : "Fail-safe test FAILED on " + seen + ": this plug will not be switched live");
                return ok;
            });
        }
        if (!started) return sendError(res, 409, "Another plug operation is running - wait for it to finish");
        sendJson(res, json{{"started", action}});
    });

    // The page itself is rendered (asset fingerprints); everything else is served as files.
    const auto index = [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(renderIndex(ctx_.webRoot), "text/html; charset=utf-8");
    };
    if (!ctx_.webRoot.empty()) {
        s.Get("/", index);
        s.Get("/index.html", index);
        s.set_mount_point("/", ctx_.webRoot.string());
    }

    // Never let browsers keep UI files: with heuristic caching (and even with no-cache plus
    // 304 revalidation) Chrome kept running an old settings.js against a newer page. The
    // files are small and travel over the LAN only, so always sending them is cheap.
    s.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!req.path.starts_with("/api/")) {
            res.set_header("Cache-Control", "no-store");
            res.headers.erase("ETag");
            res.headers.erase("Last-Modified");
        }
    });

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
    stopping_ = true;
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

void WebServer::jobSleep(std::chrono::milliseconds d) const {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
        if (stopping_) throw std::runtime_error("heatbrain is stopping");
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
}

bool WebServer::startJob(const std::string& plugId, const std::string& kind,
                         std::function<bool(const std::function<void(const std::string&)>&)> fn) {
    const auto nowEpoch = [] { return static_cast<long long>(system_clock::now().time_since_epoch() / seconds{1}); };
    {
        std::lock_guard lock(jobsMutex_);
        // One device operation at a time: upgrades are risky enough one by one.
        for (const auto& [id, j] : jobs_) {
            if (j.state == "running") return false;
        }
        Job job;
        job.plugId = plugId;
        job.kind = kind;
        job.started = nowEpoch();
        jobs_[plugId] = job;
    }
    workers_.emplace_back([this, plugId, kind, fn = std::move(fn), nowEpoch] {
        const auto log = [&](const std::string& msg) {
            hb::log::info("Plug {} {}: {}", plugId, kind, msg);
            std::lock_guard lock(jobsMutex_);
            jobs_[plugId].log.emplace_back(nowEpoch(), msg);
        };
        bool ok = false;
        try {
            ok = fn(log);
        } catch (const std::exception& e) {
            log(std::string("FAILED: ") + e.what());
        }
        std::lock_guard lock(jobsMutex_);
        jobs_[plugId].state = ok ? "done" : "failed";
        jobs_[plugId].finished = nowEpoch();
    });
    return true;
}

json WebServer::latestFirmware() {
    // GitHub allows 60 unauthenticated requests per hour: cache for 12 h.
    HttplibClient internet;
    CachingHttpClient cache(internet, ctx_.dataDir / "cache");
    const json j = json::parse(cache.get("https://api.github.com/repos/arendst/Tasmota/releases/latest", hours{12}).body);
    return json{{"version", j.value("tag_name", "")},
                {"name", j.value("name", "")},
                {"publishedAt", j.value("published_at", "")},
                {"url", j.value("html_url", "")}};
}

}  // namespace hb
