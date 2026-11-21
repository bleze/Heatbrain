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
#include "sensors/sensors.hpp"

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
    if (rooms.empty()) throw std::runtime_error("No rooms configured in " + e->cfg.configFile.string());
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
    for (const auto& e : result.errors) std::cout << "WARNING: " << e << "\n";
    return 0;
}

// --- run ---------------------------------------------------------------------------------

namespace {
std::atomic<bool> g_stop{false};
extern "C" void onSignal(int) { g_stop = true; }
}  // namespace

int runDaemon(const Args& args) {
    auto env = loadControlEnv(args);

    // Live plugs must be refreshed well within their fail-safe pulse time.
    for (const auto& p : env->plugs) {
        if (p.mode == hb::PlugMode::Live && env->tick * 2 > duration_cast<seconds>(p.failsafe.pulse)) {
            throw std::runtime_error(std::format("Plug {}: tick ({} s) must be at most half the fail-safe pulse ({} min)",
                                                 p.id, env->tick.count(), p.failsafe.pulse.count()));
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    hb::log::info("heatbrain started: {} room(s), tick every {} s, data in {}", env->controller->rooms().size(),
                  env->tick.count(), env->cfg.dataDir.string());
    for (const auto& p : env->plugs) {
        hb::log::info("  plug {} ({}) {}", p.id, p.host.empty() ? p.type : p.host,
                      p.mode == hb::PlugMode::Live ? "LIVE" : "dryrun");
    }

    std::set<std::string> activeErrors;
    while (!g_stop) {
        const Instant now = nowSeconds();
        hb::TickResult result;
        try {
            result = env->controller->tick(now, /*apply=*/true);
        } catch (const std::exception& e) {
            result.errors.push_back(std::string("Tick failed: ") + e.what());
        }
        for (const auto& r : result.rooms) {
            const std::string line = std::format("{}: {} {} - {}", r.room->id, r.decision.on ? "ON " : "OFF",
                                                 hb::reasonCode(r.decision.reason), r.decision.detail);
            if (r.changed) hb::log::info("{}", line);
            else hb::log::debug("{}", line);
        }
        // Log problems when they appear and when they clear, not every tick.
        const std::set<std::string> errors(result.errors.begin(), result.errors.end());
        for (const auto& e : errors) {
            if (!activeErrors.contains(e)) hb::log::warn("{}", e);
        }
        for (const auto& e : activeErrors) {
            if (!errors.contains(e)) hb::log::info("Resolved: {}", e);
        }
        activeErrors = errors;

        // Sleep until the next tick boundary, waking every second to honour Ctrl+C/SIGTERM.
        const auto tickLen = env->tick;
        const Instant next = Instant{(now.time_since_epoch() / tickLen + 1) * tickLen};
        while (!g_stop && nowSeconds() < next) std::this_thread::sleep_for(seconds{1});
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
