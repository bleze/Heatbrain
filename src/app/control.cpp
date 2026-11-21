// Commands built on the controller: plan, run, history, override.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <format>
#include <iostream>
#include <memory>
#include <set>
#include <thread>

#include "app/cli.hpp"
#include "app/config.hpp"
#include "app/controller.hpp"
#include "app/rooms.hpp"
#include "core/log.hpp"
#include "notify/notifier.hpp"
#include "sensors/sensors.hpp"
#include "web/server.hpp"

namespace hbcli {

using namespace std::chrono;
using hb::Instant;

namespace {

Instant nowSeconds() { return floor<seconds>(system_clock::now()); }

// Everything the controller needs, owned in one place (members are declared in
// dependency order: later members hold references to earlier ones).
struct ControlEnv {
    hb::AppConfig cfg;
    std::unique_ptr<hb::TimeZone> tz;
    hb::HttplibClient internet;
    hb::HttplibClient lan{milliseconds{3000}, milliseconds{5000}};
    std::unique_ptr<hb::CachingHttpClient> cache;
    std::unique_ptr<hb::ProviderContext> ctx;
    std::unique_ptr<hb::PriceModel> prices;
    std::vector<std::unique_ptr<hb::TemperatureSource>> sources;
    std::vector<hb::Plug> plugs;
    std::unique_ptr<hb::Overrides> overrides;
    std::unique_ptr<hb::History> history;
    std::unique_ptr<hb::Controller> controller;
    seconds tick{300};
};

std::unique_ptr<ControlEnv> loadControlEnv(const Args& args) {
    auto e = std::make_unique<ControlEnv>();
    e->cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    e->tz = std::make_unique<hb::TimeZone>(e->cfg.timezone);
    e->cache = std::make_unique<hb::CachingHttpClient>(e->internet, e->cfg.dataDir / "cache");
    e->ctx = std::make_unique<hb::ProviderContext>(hb::ProviderContext{*e->cache, *e->tz, e->cfg.currency, nowSeconds});
    e->prices = std::make_unique<hb::PriceModel>(e->cfg.raw.at("price"), *e->ctx);
    e->sources = hb::loadTemperatureSources(e->cfg.raw, e->internet, e->cfg.dataDir, nowSeconds);
    e->plugs = hb::loadPlugs(e->cfg.raw, e->lan);
    auto rooms = hb::parseRooms(e->cfg.raw, e->plugs);
    e->overrides = std::make_unique<hb::Overrides>(e->cfg.dataDir / "overrides.json");
    e->history = std::make_unique<hb::History>(e->cfg.dataDir / "history.db");
    if (e->cfg.raw.contains("control")) e->tick = seconds{e->cfg.raw["control"].value("tickSeconds", 300)};
    if (e->tick < seconds{60}) throw std::runtime_error("control.tickSeconds must be at least 60");
    e->controller = std::make_unique<hb::Controller>(*e->tz, *e->prices, e->sources, e->plugs, std::move(rooms),
                                                     e->overrides.get(), e->history.get());
    return e;
}

std::string fmtTemp(const nlohmann::json& j, const char* key) {
    return j.contains(key) ? std::format("{:.1f}°C", j[key].get<double>()) : "-";
}

void printRoomResult(const hb::RoomResult& r, const hb::TimeZone&, const std::string& currency) {
    const auto& d = r.decision;
    const auto& in = d.inputs;
    std::cout << std::format("{} ({})  ->  {:<3}  {}\n", r.room->id, r.room->name, d.on ? "ON" : "OFF",
                             hb::reasonCode(d.reason));
    std::cout << "    " << d.detail << "\n";
    std::string facts;
    if (in.temperature) {
        facts += std::format("{:.1f}°C", *in.temperature);
        if (in.temperatureAgeMinutes) facts += std::format(" ({} min old)", *in.temperatureAgeMinutes);
    } else if (!r.room->priceOnly) {
        facts += "no temperature";
    }
    if (in.target) facts += std::format(", target {:.1f}°C ({})", *in.target, in.targetLabel);
    if (in.price) {
        facts += std::format("{}price {:.2f} {}/kWh", facts.empty() ? "" : ", ", *in.price, currency);
        if (in.medianPrice) facts += std::format(" (24h median {:.2f})", *in.medianPrice);
    }
    if (!facts.empty()) std::cout << "    " << facts << "\n";
}

}  // namespace

int runPlan(const Args& args) {
    auto env = loadControlEnv(args);
    const auto result = env->controller->tick(nowSeconds(), /*apply=*/false);
    std::cout << std::format("What heatbrain would do now ({}):\n\n", hb::formatLocalHm(env->tz->toLocal(nowSeconds())));
    for (const auto& r : result.rooms) {
        printRoomResult(r, *env->tz, env->cfg.currency);
        std::string plugs;
        for (const auto& id : r.room->plugIds) {
            const auto& p = *std::ranges::find_if(env->plugs, [&](const hb::Plug& pl) { return pl.id == id; });
            plugs += std::format("{}{} [{}]", plugs.empty() ? "" : ", ", id,
                                 p.mode == hb::PlugMode::Live ? "live" : "dryrun");
        }
        std::cout << "    plugs: " << plugs << "\n\n";
    }
    for (const auto& p : result.problems) {
        std::cout << std::format("{}: {}  [{}]\n", p.severity == hb::Severity::Critical ? "CRITICAL" : "WARNING",
                                 p.message, p.key);
    }
    return 0;
}

int runNotifyTest(const Args& args) {
    const auto cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    hb::HttplibClient http;
    auto nc = hb::loadNotifications(cfg.raw, http);
    if (nc.channels.empty()) {
        std::cout << "No notification channels configured (notifications.channels in "
                  << cfg.configFile.string() << ")\n";
        return 1;
    }
    int failures = 0;
    for (auto& ch : nc.channels) {
        try {
            ch->sendText("heatbrain test", "If you can read this, notifications work. 🔥");
            std::cout << ch->name() << ": sent\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cout << ch->name() << ": FAILED - " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}

// --- run ---------------------------------------------------------------------------------

namespace {
std::atomic<bool> g_stop{false};
std::atomic<bool> g_wake{false};    // set by the web UI after an override: tick now
std::atomic<bool> g_reload{false};  // set by the web UI after saving settings
extern "C" void onSignal(int) { g_stop = true; }

std::vector<fs::path> webRootCandidates() {
    std::vector<fs::path> c;
#ifdef HEATBRAIN_SOURCE_DIR
    c.push_back(fs::path(HEATBRAIN_SOURCE_DIR) / "web");
#endif
    c.push_back("/app/web");
    c.push_back("web");
    return c;
}

// Snapshot for the web UI, rebuilt after every tick.
nlohmann::json buildStatus(const ControlEnv& env, const hb::TickResult& result,
                           const std::vector<hb::Problem>& active, Instant now) {
    using nlohmann::json;
    const auto overrides = env.overrides->load();
    json rooms = json::array();
    for (const auto& r : result.rooms) {
        json room{{"id", r.room->id},
                  {"name", r.room->name},
                  {"on", r.decision.on},
                  {"reason", hb::reasonCode(r.decision.reason)},
                  {"detail", r.decision.detail},
                  {"inputs", hb::toJson(r.decision.inputs)},
                  {"plugs", r.plugs},
                  {"priceOnly", r.room->priceOnly.has_value()},
                  {"override", nullptr}};
        if (auto it = overrides.find(r.room->id); it != overrides.end() && it->second.until > now) {
            room["override"] = {{"on", it->second.on}, {"until", it->second.until.time_since_epoch().count()}};
        }
        rooms.push_back(std::move(room));
    }
    json problems = json::array();
    for (const auto& p : active) {
        problems.push_back({{"key", p.key}, {"severity", hb::severityName(p.severity)}, {"message", p.message}});
    }
    json prices = json::array();
    for (const auto& s : env.controller->prices().slots) {
        if (s.total) prices.push_back({s.start.time_since_epoch().count(), *s.total});
    }
    json sensors = json::array();
    for (const auto& [key, r] : env.controller->readings()) {
        json j{{"key", key}, {"name", r.name}, {"kind", r.kind}, {"station", r.station}, {"reachable", r.reachable}};
        if (r.temperature) j["temperature"] = *r.temperature;
        if (r.humidity) j["humidity"] = *r.humidity;
        if (r.batteryPercent) j["battery"] = *r.batteryPercent;
        if (r.measuredAt) j["ageMinutes"] = duration_cast<minutes>(now - *r.measuredAt).count();
        sensors.push_back(std::move(j));
    }
    return json{{"now", now.time_since_epoch().count()},
                {"tickSeconds", env.tick.count()},
                {"timezone", env.cfg.timezone},
                {"currency", env.cfg.currency},
                {"configError", nullptr},
                {"rooms", rooms},
                {"problems", problems},
                {"prices", prices},
                {"sensors", sensors}};
}

// The control loop for one configuration. Returns when stopping or when the config changed.
void controlLoop(ControlEnv& env, hb::StatusBoard& board, bool announce) {
    hb::log::info("Control: {} room(s), {} plug(s), tick every {} s", env.controller->rooms().size(), env.plugs.size(),
                  env.tick.count());
    for (const auto& p : env.plugs) {
        hb::log::info("  plug {} ({}) {}", p.id, p.host.empty() ? p.type : p.host,
                      p.mode == hb::PlugMode::Live ? "LIVE" : "dryrun");
    }

    auto notifications = hb::loadNotifications(env.cfg.raw, env.internet);
    hb::AlertManager alerts(notifications.policy, *env.tz);
    hb::log::info("  notifications: {} channel(s)", notifications.channels.size());
    if (announce && notifications.notifyOnStart) {
        for (auto& ch : notifications.channels) {
            try {
                ch->sendText("heatbrain started", std::format("{} room(s), tick every {} s",
                                                              env.controller->rooms().size(), env.tick.count()));
            } catch (const std::exception& e) {
                hb::log::warn("{}: {}", ch->name(), e.what());
            }
        }
    }
    const auto deliver = [&](const hb::Alert& a) {
        const char* kind = a.kind == hb::Alert::Kind::Resolved   ? "RESOLVED"
                           : a.kind == hb::Alert::Kind::Reminder ? "REMINDER"
                                                                 : "ALERT";
        hb::log::warn("{} [{}] {}", kind, a.key, a.message);
        if (notifications.channels.empty()) return true;  // logging is the delivery
        bool any = false;
        for (auto& ch : notifications.channels) {
            try {
                ch->send(a);
                any = true;
            } catch (const std::exception& e) {
                hb::log::warn("{}: {}", ch->name(), e.what());
            }
        }
        return any;
    };

    std::set<std::string> seenKeys;
    while (!g_stop && !g_reload) {
        const Instant now = nowSeconds();
        hb::TickResult result;
        try {
            result = env.controller->tick(now, /*apply=*/true);
        } catch (const std::exception& e) {
            result.problems.push_back({"controller:tick", hb::Severity::Critical,
                                       std::string("Control step failed: ") + e.what(), minutes{0}, std::nullopt});
        }
        for (const auto& r : result.rooms) {
            const std::string line = std::format("{}: {} {} - {}", r.room->id, r.decision.on ? "ON " : "OFF",
                                                 hb::reasonCode(r.decision.reason), r.decision.detail);
            if (r.changed) hb::log::info("{}", line);
            else hb::log::debug("{}", line);
        }
        // Problems are logged as soon as they appear; alerts follow the debounce policy.
        std::set<std::string> keys;
        for (const auto& p : result.problems) {
            keys.insert(p.key);
            if (!seenKeys.contains(p.key)) hb::log::info("Problem seen [{}] {}", p.key, p.message);
        }
        seenKeys = keys;
        alerts.update(now, result.problems, deliver);
        board.set(buildStatus(env, result, alerts.activeProblems(), now));

        // Sleep until the next tick boundary; wake early for Ctrl+C/SIGTERM, overrides, reloads.
        const auto tickLen = env.tick;
        const Instant next = Instant{(now.time_since_epoch() / tickLen + 1) * tickLen};
        while (!g_stop && !g_wake && !g_reload && nowSeconds() < next) std::this_thread::sleep_for(milliseconds{250});
        g_wake = false;
    }
}

}  // namespace

int runDaemon(const Args& args) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // The web UI runs for the whole process, even when the configuration is incomplete or
    // invalid: that is where it gets fixed.
    const auto initial = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    hb::log::info("heatbrain started, data in {}", initial.dataDir.string());
    hb::StatusBoard board;
    const auto webCfg = hb::parseWebConfig(initial.raw);
    hb::WebServer web(webCfg, board,
                      hb::WebContext{initial.dataDir, initial.configFile, hb::locateWebRoot(webRootCandidates()),
                                     [] { g_wake = true; }, [] { g_reload = true; }});
    if (webCfg.enabled) web.start();

    bool first = true;
    while (!g_stop) {
        g_reload = false;
        std::unique_ptr<ControlEnv> env;
        try {
            env = loadControlEnv(args);
            for (const auto& p : env->plugs) {
                if (p.mode == hb::PlugMode::Live && env->tick * 2 > duration_cast<seconds>(p.failsafe.pulse)) {
                    throw std::runtime_error(std::format(
                        "plug {}: tick ({} s) must be at most half its fail-safe pulse ({} min)", p.id,
                        env->tick.count(), p.failsafe.pulse.count()));
                }
            }
        } catch (const std::exception& e) {
            hb::log::error("Configuration problem: {}. Fix it in the web UI (Settings).", e.what());
            web.setRooms({});
            board.set(nlohmann::json{{"now", nowSeconds().time_since_epoch().count()},
                                     {"tickSeconds", 300},
                                     {"timezone", initial.timezone},
                                     {"currency", initial.currency},
                                     {"configError", e.what()},
                                     {"rooms", nlohmann::json::array()},
                                     {"problems", nlohmann::json::array()},
                                     {"prices", nlohmann::json::array()},
                                     {"sensors", nlohmann::json::array()}});
            while (!g_stop && !g_reload) std::this_thread::sleep_for(milliseconds{250});
            continue;
        }
        web.setRooms(env->controller->rooms());
        controlLoop(*env, board, first);
        first = false;
        if (g_reload) hb::log::info("Configuration changed - reloading");
    }
    hb::log::info("heatbrain stopping");
    return 0;
}

// --- history ---------------------------------------------------------------------------

int runHistory(const Args& args) {
    if (args.positional.size() != 1) throw std::runtime_error("Usage: heatbrain history <room> [--hours N] [--all]");
    const auto cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    const hb::TimeZone tz(cfg.timezone);
    hb::History history(cfg.dataDir / "history.db");
    const std::string& room = args.positional[0];
    const Instant to = nowSeconds();
    const Instant from = to - hours{args.hours};

    const auto decisions = history.decisions(room, from, to + seconds{1}, !args.all);
    std::cout << std::format("{} - {} in the last {} h:\n\n", room, args.all ? "all decisions" : "switch events",
                             args.hours);
    std::cout << std::format("  {:<11} {:<3} {:<26} {:>7} {:>7} {:>6}  {}\n", "When", "", "Reason", "Temp", "Target",
                             "Price", "Detail");
    for (const auto& d : decisions) {
        const auto local = tz.toLocal(d.at);
        const std::string when = std::format("{} {}", hb::formatLocalDate(local).substr(5), hb::formatLocalHm(local));
        const std::string price = d.inputs.contains("price") ? std::format("{:.2f}", d.inputs["price"].get<double>()) : "-";
        std::cout << std::format("  {:<11} {:<3} {:<26} {:>7} {:>7} {:>6}  {}\n", when, d.on ? "ON" : "OFF", d.reason,
                                 fmtTemp(d.inputs, "temperature"), fmtTemp(d.inputs, "target"), price, d.detail);
        for (const auto& p : d.plugs) {
            if (p.contains("error")) std::cout << "              ! plug " << p.value("id", "?") << ": " << p["error"].get<std::string>() << "\n";
        }
    }
    if (decisions.empty()) std::cout << "  (nothing recorded - is 'heatbrain run' running?)\n";

    const auto temps = history.temperatures(room, from, to + seconds{1});
    if (!temps.empty()) {
        const auto [mn, mx] = std::ranges::minmax(temps, {}, &hb::TemperatureRecord::temperature);
        std::cout << std::format("\n  Temperature: {} readings, min {:.1f}°C, max {:.1f}°C, latest {:.1f}°C\n",
                                 temps.size(), mn.temperature, mx.temperature, temps.back().temperature);
    }
    return 0;
}

// --- override --------------------------------------------------------------------------

int runOverride(const Args& args) {
    const auto cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    const hb::TimeZone tz(cfg.timezone);
    const hb::Overrides overrides(cfg.dataDir / "overrides.json");

    if (args.positional.empty()) {
        const auto all = overrides.load();
        if (all.empty()) std::cout << "No manual overrides.\n";
        for (const auto& [room, o] : all) {
            const bool active = o.until > nowSeconds();
            std::cout << std::format("  {:<12} {} until {}{}\n", room, o.on ? "ON " : "OFF",
                                     hb::formatLocalHm(tz.toLocal(o.until)), active ? "" : " (expired)");
        }
        return 0;
    }
    const std::string& room = args.positional[0];
    const std::string action = args.positional.size() > 1 ? args.positional[1] : "";
    if (action == "clear") {
        overrides.clear(room);
        std::cout << room << ": override cleared\n";
        return 0;
    }
    if ((action != "on" && action != "off") || args.positional.size() != 3) {
        throw std::runtime_error("Usage: heatbrain override [<room> on|off <minutes> | <room> clear]");
    }
    const int mins = std::stoi(args.positional[2]);
    if (mins < 1 || mins > 7 * 24 * 60) throw std::runtime_error("minutes must be 1..10080");
    const Instant until = nowSeconds() + minutes{mins};
    overrides.set(room, hb::ManualOverride{action == "on", until});
    std::cout << std::format("{}: manual {} until {} (applied on the next tick of 'heatbrain run')\n", room,
                             action == "on" ? "ON" : "OFF", hb::formatLocalHm(tz.toLocal(until)));
    return 0;
}

}  // namespace hbcli
