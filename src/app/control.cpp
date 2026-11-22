// Commands built on the controller: plan, run, history, override.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <format>
#include <iostream>
#include <memory>
#include <set>
#include <thread>

#include <fstream>
#include "core/version.hpp"
#include "app/cli.hpp"
#include "app/config.hpp"
#include "app/controller.hpp"
#include "app/cost_report.hpp"
#include "app/digest.hpp"
#include "app/meter_sync.hpp"
#include "app/power_mix_sync.hpp"
#include "app/rooms.hpp"
#include "core/log.hpp"
#include "notify/notifier.hpp"
#include "sensors/sensors.hpp"
#include "web/server.hpp"

namespace hbcli {

using namespace std::chrono;
namespace fs = std::filesystem;
using hb::Instant;

namespace {

Instant nowSeconds() { return floor<seconds>(system_clock::now()); }

// Everything the controller needs, owned in one place (members are declared in
// dependency order: later members hold references to earlier ones).
struct ControlEnv {
    hb::AppConfig cfg;
    std::unique_ptr<hb::TimeZone> tz;
    hb::HttplibClient internet;
    hb::HttplibClient lan{hb::kPlugConnectTimeout, hb::kPlugReadTimeout};
    std::unique_ptr<hb::CachingHttpClient> cache;
    std::unique_ptr<hb::ProviderContext> ctx;
    std::unique_ptr<hb::PriceModel> prices;
    std::vector<std::unique_ptr<hb::TemperatureSource>> sources;
    std::vector<hb::Plug> plugs;
    std::unique_ptr<hb::Overrides> overrides;
    std::unique_ptr<hb::AwayStore> away;
    std::unique_ptr<hb::BookingStore> bookings;
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
    e->away = std::make_unique<hb::AwayStore>(e->cfg.dataDir / "away.json");
    e->bookings = std::make_unique<hb::BookingStore>(e->cfg.dataDir / "bookings.json");
    e->history = std::make_unique<hb::History>(e->cfg.dataDir / "history.db");
    if (e->cfg.raw.contains("control")) e->tick = seconds{e->cfg.raw["control"].value("tickSeconds", 300)};
    if (e->tick < seconds{60}) throw std::runtime_error("control.tickSeconds must be at least 60");
    e->controller = std::make_unique<hb::Controller>(*e->tz, *e->prices, e->sources, e->plugs, std::move(rooms),
                                                     e->overrides.get(), e->history.get());
    e->controller->setDataDir(e->cfg.dataDir);
    e->controller->setAwayStore(e->away.get());
    e->controller->setBookingStore(e->bookings.get());
    e->controller->setOutdoorSensor(e->cfg.raw.value("outdoorSensor", std::string()));
    {
        // Open-window detection, on unless switched off: "openWindow": {"enabled", "drop", "pauseMinutes"}.
        hb::OpenWindowConfig ow;
        if (const auto& j = e->cfg.raw.value("openWindow", nlohmann::json::object()); j.is_object()) {
            ow.enabled = j.value("enabled", true);
            ow.drop = std::clamp(j.value("drop", 1.0), 0.3, 5.0);
            ow.pause = minutes{std::clamp(j.value("pauseMinutes", 30), 5, 240)};
        }
        e->controller->setOpenWindow(ow);
        // Weather forecast, on unless switched off: "forecast": {"enabled", "latitude", "longitude"}; without a
        // position, where the Netatmo station is.
        const auto& f = e->cfg.raw.value("forecast", nlohmann::json::object());
        if (!f.is_object() || f.value("enabled", true)) {
            std::optional<std::pair<double, double>> at;
            if (f.is_object() && f.contains("latitude") && f.contains("longitude") && f["latitude"].is_number() &&
                f["longitude"].is_number()) {
                at = {{f["latitude"].get<double>(), f["longitude"].get<double>()}};
            }
            e->controller->setForecast(e->cache.get(), at);
        }
    }
    // Thermostats (DEVIreg Smart): their own sensors are a temperature source too.
    hb::DeviRegistry::instance().setDataDir(e->cfg.dataDir);
    if (auto thermostats = hb::parseThermostats(e->cfg.raw); !thermostats.empty()) {
        e->sources.push_back(std::make_unique<hb::DeviSource>(thermostats));
        e->controller->setThermostats(std::move(thermostats));
    }
    // Load the night schedule two control steps before the Wi-Fi goes off (at least 10 minutes).
    e->controller->setOfflineWindow(hb::parseWifiOff(e->cfg.raw),
                                    std::max(minutes{10}, duration_cast<minutes>(e->tick * 2)));
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
        json plan = json::array();
        for (const auto& s : r.plan) {
            json seg{{"from", s.from.time_since_epoch().count()}, {"to", s.to.time_since_epoch().count()}, {"kind", s.kind}};
            if (s.target) seg["target"] = *s.target;
            plan.push_back(seg);
        }
        room["plan"] = plan;
        json booked = json::array();
        for (const auto& b : r.bookings) {
            booked.push_back({{"id", b.id}, {"target", b.booking.target},
                              {"from", b.booking.from.time_since_epoch().count()},
                              {"until", b.booking.until.time_since_epoch().count()},
                              {"warmupFrom", (b.booking.from - b.booking.warmup).time_since_epoch().count()},
                              {"warmupMinutes", b.booking.warmup.count()},
                              {"basis", b.basis}, {"reachable", b.reachable},
                              {"active", b.booking.coversAt(now)}});
        }
        room["bookings"] = booked;
        if (r.thermal) room["thermal"] = {{"gain", r.thermal->gain}, {"loss", r.thermal->loss}};
        room["maxTemp"] = r.room->maxTemp;
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
    json away = nullptr;
    if (const auto a = env.away->load()) {
        away = {{"keepAbove", a->keepAbove}, {"from", a->from.time_since_epoch().count()},
                {"until", a->until.time_since_epoch().count()}, {"active", a->activeAt(now)}};
    }
    json wifiOff = nullptr;
    if (const auto o = env.controller->offlineInfo(now)) {
        wifiOff = {{"start", o->period.start.time_since_epoch().count()},
                   {"end", o->period.end.time_since_epoch().count()},
                   {"active", o->active},
                   {"late", o->late},
                   {"scheduleUntil", o->scheduleUntil ? json(o->scheduleUntil->time_since_epoch().count()) : json(nullptr)},
                   {"armedPlugs", o->armedPlugs},
                   {"errors", o->errors}};
    }
    json outdoor = nullptr;
    if (const auto& o = env.controller->outdoor()) outdoor = {{"key", o->key}, {"name", o->name}, {"temperature", o->temperature}};
    // The weather forecast, for Settings and the dashboard: lowest and highest in the next 24 h.
    json forecast = nullptr;
    if (const auto& fi = env.controller->forecast(); fi.location) {
        forecast = {{"latitude", fi.location->first}, {"longitude", fi.location->second}, {"from", fi.locationFrom}};
        if (!fi.error.empty()) forecast["error"] = fi.error;
        if (fi.forecast) {
            std::optional<std::pair<Instant, double>> lo, hi;
            for (Instant t = ceil<hours>(now); t < now + hours{24}; t += hours{1}) {
                const auto v = fi.forecast->at(t);
                if (!v) continue;
                if (!lo || *v < lo->second) lo = {{t, *v}};
                if (!hi || *v > hi->second) hi = {{t, *v}};
            }
            if (lo) forecast["min"] = {{"at", lo->first.time_since_epoch().count()}, {"temperature", lo->second}};
            if (hi) forecast["max"] = {{"at", hi->first.time_since_epoch().count()}, {"temperature", hi->second}};
        }
    }
    return json{{"now", now.time_since_epoch().count()},
                {"away", away},
                {"wifiOff", wifiOff},
                {"outdoor", outdoor},
                {"forecast", forecast},
                {"tickSeconds", env.tick.count()},
                {"version", hb::buildRevision()},
                {"timezone", env.cfg.timezone},
                {"currency", env.cfg.currency},
                {"configError", nullptr},
                {"rooms", rooms},
                {"problems", problems},
                {"prices", prices},
                {"sensors", sensors}};
}

// Short words for the start message (the web UI has its own, longer labels).
std::string shortReason(hb::Reason r) {
    using hb::Reason;
    switch (r) {
        case Reason::BelowTarget: case Reason::HoldOn: return "heating";
        case Reason::TargetReached: case Reason::HoldOff: return "off, target reached";
        case Reason::PreheatBeforePeak: return "pre-heating before the peak";
        case Reason::PeakAvoid: return "off, expensive now";
        case Reason::BelowComfortDuringPeak: return "heating, too cold to wait";
        case Reason::CheapBoost: return "heating, cheap hour";
        case Reason::CheapHours: return "on, cheapest hours";
        case Reason::NotCheapHours: return "off, waiting for cheap hours";
        case Reason::ScheduledOff: case Reason::AvoidWindow: return "off by program";
        case Reason::KeepAbove: return "heating to stay above the floor";
        case Reason::KeepSatisfied: return "off, above the floor";
        case Reason::StaleDataLimp: return "no temperature yet (limp mode)";
        case Reason::ManualOn: return "on (manual)";
        case Reason::ManualOff: return "off (manual)";
        case Reason::AboveMax: return "off, above maximum";
        case Reason::BelowMin: return "heating, below minimum";
        case Reason::FrostProtection: return "heating, frost protection";
        case Reason::AwayOff: return "away";
        case Reason::OfflineSchedule: return "following the night schedule";
        case Reason::NoPricesFallback: return "on, no prices (fallback)";
    }
    return std::string(hb::reasonCode(r));
}

// Title + text for the start notification: updated / restarted / started, then what each room is
// doing, the plugs, tonight's Wi-Fi off period and any problems. Remembers the version it announced.
std::pair<std::string, std::string> startMessage(const ControlEnv& env, const hb::TickResult& result, Instant now) {
    const std::string rev = hb::buildRevision();
    const fs::path versionFile = env.cfg.dataDir / "version.json";
    std::string previous;
    try {
        if (fs::exists(versionFile)) previous = nlohmann::json::parse(std::ifstream(versionFile)).value("revision", "");
    } catch (const std::exception&) {
    }
    std::ofstream(versionFile) << nlohmann::json{{"revision", rev}}.dump();

    std::string title, text;
    if (previous.empty()) title = "heatbrain started";
    else if (previous != rev) title = "heatbrain updated";
    else title = "heatbrain restarted";
    // The version only matters when it changed.
    if (previous != rev && !previous.empty()) text += std::format("New version {} (was {})\n", rev, previous);

    for (const auto& r : result.rooms) {
        const auto& in = r.decision.inputs;
        std::string line = r.room->name;
        if (in.temperature) line += std::format(" {:.1f}°", *in.temperature);
        std::string what = shortReason(r.decision.reason);
        if (what == "heating" && in.target) what = std::format("heating to {:.0f}°", *in.target);
        line += " · " + what;
        text += line + "\n";
    }

    int live = 0, dry = 0;
    for (const auto& p : env.plugs) (p.mode == hb::PlugMode::Live ? live : dry)++;
    std::string tail = live && !dry ? std::format("{} plug{} live", live, live == 1 ? "" : "s")
                       : !live      ? std::format("{} plug{} in dry-run", dry, dry == 1 ? "" : "s")
                                    : std::format("{} live, {} in dry-run", live, dry);
    if (const auto o = env.controller->offlineInfo(now)) {
        const auto start = env.tz->toLocal(o->period.start), end = env.tz->toLocal(o->period.end);
        tail += o->active ? std::format(" · Wi-Fi off until {:%H:%M}", end)
                          : std::format(" · Wi-Fi off {:%H:%M}-{:%H:%M}", start, end);
    }
    const auto problems = result.problems.size();
    tail += problems == 0 ? " · no problems" : std::format(" · {} problem{} (see the dashboard)", problems, problems == 1 ? "" : "s");
    text += tail;
    return {title, text};
}

// The control loop for one configuration. Returns when stopping or when the config changed.
void controlLoop(ControlEnv& env, hb::StatusBoard& board, std::unique_ptr<hb::AlertManager>& alertsPtr, bool announce) {
    hb::log::info("Control: {} room(s), {} plug(s), tick every {} s", env.controller->rooms().size(), env.plugs.size(),
                  env.tick.count());
    for (const auto& p : env.plugs) {
        hb::log::info("  plug {} ({}) {}", p.id, p.host.empty() ? p.type : p.host,
                      p.mode == hb::PlugMode::Live ? "LIVE" : "dryrun");
    }

    auto notifications = hb::loadNotifications(env.cfg.raw, env.internet);
    // Kept across reloads, so saving settings does not re-send alerts already delivered.
    if (!alertsPtr) alertsPtr = std::make_unique<hb::AlertManager>(notifications.policy, *env.tz);
    else alertsPtr->reconfigure(notifications.policy, *env.tz);
    hb::AlertManager& alerts = *alertsPtr;
    hb::log::info("  notifications: {} channel(s)", notifications.channels.size());
    // The start message goes out after the first control step, so it can tell what is happening.
    bool announcePending = announce && notifications.notifyOnStart;
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

    const fs::path digestFile = env.cfg.dataDir / "digest.json";
    hb::DigestState digest = hb::loadDigestState(digestFile);
    const auto digestOpts = hb::parseDigestOptions(env.cfg.raw);

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
        if (announcePending) {
            announcePending = false;
            const auto [title, text] = startMessage(env, result, now);
            hb::log::info("{}: {}", title, text);
            for (auto& ch : notifications.channels) {
                try {
                    ch->sendText(title, text);
                } catch (const std::exception& e) {
                    hb::log::warn("{}: {}", ch->name(), e.what());
                }
            }
        }
        board.set(buildStatus(env, result, alerts.activeProblems(now), now));

        // Weekly summary (Monday morning), monthly report (the 1st) and tomorrow's-price heads-up.
        try {
            const auto recent = env.history->prices(now - hours{24 * 7}, hb::startOfLocalDay(*env.tz, now));
            const auto messages = hb::dueDigests(
                now, *env.tz, digest, digestOpts,
                [&](Instant end, int days) { return hb::costReport(env.cfg.raw, *env.history, end, days); }, env.controller->prices(),
                recent);
            if (!messages.empty()) hb::saveDigestState(digestFile, digest);
            for (const auto& m : messages) {
                hb::log::info("{}: {}", m.title, m.text);
                for (auto& ch : notifications.channels) {
                    try {
                        ch->sendText(m.title, m.text);
                    } catch (const std::exception& e) {
                        hb::log::warn("{}: {}", ch->name(), e.what());
                    }
                }
            }
        } catch (const std::exception& e) {
            hb::log::warn("Digest: {}", e.what());
        }

        // Once a day: is there a Tasmota release newer than what the plugs run?
        if (hb::firmwareCheckDue(now, *env.tz, digest, digestOpts)) {
            digest.lastFirmwareCheck = hb::formatLocalDate(env.tz->toLocal(now));
            try {
                // Same cached request as the Plugs page (GitHub allows 60 per hour without a token).
                const auto rel = nlohmann::json::parse(
                    env.cache->get("https://api.github.com/repos/arendst/Tasmota/releases/latest", hours{12}).body);
                std::vector<hb::PlugFirmware> fw;
                for (auto& p : env.plugs) {
                    if (p.type != "tasmota") continue;
                    std::string version;
                    try {
                        version = p.sw->status().firmware;
                    } catch (const std::exception&) {
                    }
                    fw.push_back({p.name.empty() ? p.id : p.name, version});
                }
                const std::string tag = rel.value("tag_name", "");
                if (const auto m = hb::firmwareNotice(tag, fw, digest.lastFirmware)) {
                    digest.lastFirmware = tag;
                    hb::log::info("{}: {}", m->title, m->text);
                    for (auto& ch : notifications.channels) {
                        try {
                            ch->sendText(m->title, m->text);
                        } catch (const std::exception& e) {
                            hb::log::warn("{}: {}", ch->name(), e.what());
                        }
                    }
                }
            } catch (const std::exception& e) {
                hb::log::warn("Tasmota release check: {}", e.what());
            }
            hb::saveDigestState(digestFile, digest);
        }

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
    std::unique_ptr<hb::AlertManager> alerts;
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
        // Meter readings in the background while this configuration runs.
        std::unique_ptr<hb::MeterSync> meter;
        try {
            if (const auto mc = hb::parseMeterConfig(env->cfg.raw)) {
                meter = std::make_unique<hb::MeterSync>(*mc, env->cfg.dataDir, *env->tz);
            }
        } catch (const std::exception& e) {
            hb::log::warn("Meter: {}", e.what());
        }
        // Where the area's electricity comes from (wind, solar, ...), for the room charts.
        std::unique_ptr<hb::PowerMixSync> mix;
        if (const std::string area = hb::spotArea(env->cfg.raw); area == "DK1" || area == "DK2") {
            mix = std::make_unique<hb::PowerMixSync>(area, env->cfg.dataDir);
        }
        controlLoop(*env, board, alerts, first);
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
