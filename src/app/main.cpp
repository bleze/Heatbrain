#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app/cli.hpp"
#include "app/config.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "net/http.hpp"
#include "plugs/plugs.hpp"
#include "providers/price_model.hpp"
#include "sensors/sensors.hpp"

namespace fs = std::filesystem;
using namespace std::chrono;

using namespace hbcli;

namespace {

constexpr const char* kUsage = R"(heatbrain - price and temperature aware heating control

Usage:
  heatbrain prices [--slots]          Today's and tomorrow's electricity price
  heatbrain discover <subnet>         Find Tasmota plugs, e.g. 192.168.1.0/24
  heatbrain plugs                     Status of configured plugs and their fail-safe
  heatbrain plug <id> on|off          Switch a plug (ON lasts at most the fail-safe
                                      pulse time unless heatbrain keeps refreshing it)
  heatbrain plug <id> provision       Back up plug settings, install the fail-safe and
                                      disable the plug's own timers  [--force]
  heatbrain plug <id> restore         Put back the settings from the latest backup
  heatbrain temps                     Current readings of all temperature sensors
  heatbrain plan                      What every room would do right now, and why
  heatbrain run                       Control loop: decide, switch live plugs, record
  heatbrain history <room>            Switch events with reasons  [--hours N] [--all]
  heatbrain override                  List manual overrides
  heatbrain override <room> on|off <minutes>
  heatbrain override <room> clear
  heatbrain notify-test               Send a test message to the notification channels
  heatbrain help

Options:
  --data-dir DIR   Config, cache and state directory
                   (default: $HEATBRAIN_DATA_DIR, else ./data)
  --slots          Show 15-minute slots instead of hourly averages
  --force          Allow provision to replace existing plug rules
  --hours N        history: how far back (default 24)
  --all            history: every decision, not only switch events
  --verbose        Debug logging
)";

// One printed row: either a single slot or an hourly average of complete slots.
struct Row {
    hb::Instant start;
    std::vector<std::optional<double>> parts;
    std::optional<double> total;
};

std::vector<Row> toRows(const hb::PriceTable& table, bool slots) {
    std::vector<Row> rows;
    if (slots) {
        for (const auto& s : table.slots) rows.push_back({s.start, s.parts, s.total});
        return rows;
    }
    // Group by UTC hour: unambiguous across DST changes (Danish offsets are whole hours).
    std::map<hb::Instant, std::vector<const hb::PriceSlot*>> byHour;
    for (const auto& s : table.slots) byHour[floor<hours>(s.start)].push_back(&s);
    for (const auto& [hour, group] : byHour) {
        Row row{hour, std::vector<std::optional<double>>(table.componentNames.size()), std::nullopt};
        const bool complete = std::ranges::all_of(group, [](auto* s) { return s->total.has_value(); });
        if (complete) {
            double total = 0;
            for (std::size_t c = 0; c < row.parts.size(); ++c) {
                double sum = 0;
                for (auto* s : group) sum += *s->parts[c];
                row.parts[c] = sum / static_cast<double>(group.size());
            }
            for (auto* s : group) total += *s->total;
            row.total = total / static_cast<double>(group.size());
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void printPrices(const hb::PriceTable& table, const hb::TimeZone& tz, bool slots) {
    const auto rows = toRows(table, slots);

    std::vector<std::size_t> widths;
    for (const auto& name : table.componentNames) widths.push_back(std::max<std::size_t>(name.size(), 7));

    std::map<std::string, std::vector<const Row*>> byDay;
    for (const auto& r : rows) byDay[hb::formatLocalDate(tz.toLocal(r.start))].push_back(&r);

    std::string surcharges;
    for (const auto& s : table.surcharges) surcharges += std::format(" + {} {}%", s.name, s.percent);
    std::cout << std::format("Prices in {}/kWh. Total = sum of components{}.\n", table.currency, surcharges);
    std::cout << "  ▼ = cheapest quarter of the day, ▲ = most expensive quarter\n";

    for (const auto& [day, dayRows] : byDay) {
        // Rank complete rows of the day to mark cheap/expensive periods.
        std::vector<double> totals;
        for (auto* r : dayRows) {
            if (r->total) totals.push_back(*r->total);
        }
        std::ranges::sort(totals);
        const std::size_t quarter = totals.size() / 4;
        const double cheap = quarter ? totals[quarter - 1] : -1e9;
        const double dear = quarter ? totals[totals.size() - quarter] : 1e9;

        std::cout << std::format("\n{}\n  {:<6}", day, "Time");
        for (std::size_t c = 0; c < widths.size(); ++c) {
            std::cout << std::format(" {:>{}}", table.componentNames[c], widths[c]);
        }
        std::cout << std::format(" {:>8}\n", "Total");

        if (totals.empty()) {
            std::cout << "  (prices not published yet)\n";
            continue;
        }
        for (auto* r : dayRows) {
            if (!r->total) continue;
            std::cout << std::format("  {:<6}", hb::formatLocalHm(tz.toLocal(r->start)));
            for (std::size_t c = 0; c < widths.size(); ++c) {
                std::cout << std::format(" {:>{}.3f}", *r->parts[c], widths[c]);
            }
            const char* mark = *r->total <= cheap ? " ▼" : (*r->total >= dear ? " ▲" : "");
            std::cout << std::format(" {:>8.3f}{}\n", *r->total, mark);
        }
    }
    for (const auto& e : table.errors) std::cout << "\nWARNING: " << e << "\n";
}

int runPrices(const Args& args) {
    const auto cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    const hb::TimeZone tz(cfg.timezone);

    hb::HttplibClient http;
    hb::CachingHttpClient cache(http, cfg.dataDir / "cache");
    hb::ProviderContext ctx{cache, tz, cfg.currency, [] { return floor<seconds>(system_clock::now()); }};
    hb::PriceModel model(cfg.raw.at("price"), ctx);

    const auto now = floor<seconds>(system_clock::now());
    const auto from = hb::startOfLocalDay(tz, now);
    const auto to = hb::startOfLocalDay(tz, from + hours{50});  // end of tomorrow, DST-safe
    printPrices(model.build(from, to), tz, args.slots);
    return 0;
}

// --- Plugs ------------------------------------------------------------------------------

std::string onOff(std::optional<bool> on) { return on ? (*on ? "ON" : "OFF") : "?"; }

int runDiscover(const Args& args) {
    if (args.positional.size() != 1) throw std::runtime_error("Usage: heatbrain discover 192.168.1.0/24");
    hb::HttplibClient http(milliseconds{1500}, milliseconds{2000});
    std::cout << "Scanning " << args.positional[0] << " ...\n";
    const auto found = hb::discoverTasmota(args.positional[0], http);
    for (const auto& d : found) {
        std::cout << std::format("  {:<15} {:<24} Tasmota {:<26} {:<10} {}\n", d.host, d.deviceName,
                                 d.firmware, d.hardware, onOff(d.on));
    }
    std::cout << std::format("{} Tasmota device(s) found.\n", found.size());
    if (!found.empty()) {
        std::cout << "Add them to \"plugs\" in heatbrain.json, e.g.\n"
                  << std::format(R"(  {{ "id": "bedroom", "name": "{}", "type": "tasmota", "host": "{}", "mode": "dryrun" }})",
                                 found[0].deviceName, found[0].host)
                  << "\n";
    }
    return 0;
}

void printFailsafe(hb::Plug& plug) {
    try {
        const auto fs = plug.sw->checkFailsafe();
        if (fs.installed()) {
            std::cout << std::format("    fail-safe: installed (auto-off {} min, limp {}/{} min after {} min silence)\n",
                                     plug.failsafe.pulse.count(), plug.failsafe.limpOn.count(),
                                     plug.failsafe.limpOff.count(), plug.failsafe.watchdog.count());
        } else {
            std::cout << "    fail-safe: NOT installed\n";
            for (const auto& p : fs.problems) std::cout << "      - " << p << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "    fail-safe: could not check (" << e.what() << ")\n";
    }
}

void printPlug(hb::Plug& plug) {
    const auto st = plug.sw->status();
    const char* mode = plug.mode == hb::PlugMode::Live ? "live" : "dryrun";
    if (!st.reachable) {
        std::cout << std::format("  {:<10} {:<20} {:<15} {:<6} UNREACHABLE: {}\n", plug.id, plug.name,
                                 plug.host, mode, st.error);
        return;
    }
    std::cout << std::format("  {:<10} {:<20} {:<15} {:<6} {:<3}  {} {}  wifi {}%  up {}\n", plug.id,
                             plug.name, plug.host, mode, onOff(st.on), st.firmware, st.hardware,
                             st.rssi.value_or(0), st.uptime);
    printFailsafe(plug);
}

struct PlugEnv {
    hb::AppConfig cfg;
    hb::HttplibClient http{milliseconds{3000}, milliseconds{5000}};
    std::vector<hb::Plug> plugs;
};

std::unique_ptr<PlugEnv> loadPlugEnv(const Args& args) {
    auto env = std::make_unique<PlugEnv>();
    env->cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    env->plugs = hb::loadPlugs(env->cfg.raw, env->http);
    return env;
}

int runPlugs(const Args& args) {
    auto env = loadPlugEnv(args);
    if (env->plugs.empty()) {
        std::cout << "No plugs configured. Run 'heatbrain discover <subnet>' and add them to "
                  << env->cfg.configFile.string() << "\n";
        return 0;
    }
    for (auto& plug : env->plugs) printPlug(plug);
    return 0;
}

int runPlug(const Args& args) {
    if (args.positional.size() != 2) {
        throw std::runtime_error("Usage: heatbrain plug <id> on|off|status|provision|restore");
    }
    auto env = loadPlugEnv(args);
    const auto& id = args.positional[0];
    const auto& action = args.positional[1];
    auto it = std::ranges::find_if(env->plugs, [&](const hb::Plug& p) { return p.id == id; });
    if (it == env->plugs.end()) throw std::runtime_error("No plug with id '" + id + "' in config");
    hb::Plug& plug = *it;

    if (action == "status") {
        printPlug(plug);
    } else if (action == "on" || action == "off") {
        const bool on = action == "on";
        if (on && !args.force && !plug.sw->checkFailsafe().installed()) {
            throw std::runtime_error("Fail-safe not installed on " + id +
                                     ": it would stay ON indefinitely. Run 'heatbrain plug " + id +
                                     " provision' first, or use --force.");
        }
        const bool state = plug.sw->set(on);
        std::cout << std::format("{}: {}", id, state ? "ON" : "OFF");
        if (state) std::cout << std::format(" (switches itself off in {} min unless refreshed)", plug.failsafe.pulse.count());
        std::cout << "\n";
    } else if (action == "provision") {
        const auto backup = plug.sw->backup();
        const auto file = hb::saveBackup(env->cfg.dataDir, plug, backup);
        std::cout << "Backed up current settings to " << file.string() << "\n";
        plug.sw->provision(args.force);
        printFailsafe(plug);
        std::cout << std::format(
            "\nNote: from now on heatbrain must keep running. Without it, the plug enters limp mode\n"
            "after {} min ({} min ON / {} min OFF). Undo with 'heatbrain plug {} restore'.\n",
            plug.failsafe.watchdog.count(), plug.failsafe.limpOn.count(), plug.failsafe.limpOff.count(), id);
    } else if (action == "restore") {
        const auto file = hb::latestBackup(env->cfg.dataDir, plug);
        std::ifstream in(file);
        plug.sw->restore(nlohmann::json::parse(in));
        std::cout << "Restored settings from " << file.string() << "\n";
        printPlug(plug);
    } else {
        throw std::runtime_error("Unknown plug action '" + action + "'");
    }
    return 0;
}

// --- Temperatures -----------------------------------------------------------------------

template <class T>
std::string fmtOpt(const std::optional<T>& v, const char* fmt) {
    return v ? std::vformat(fmt, std::make_format_args(*v)) : "-";
}

int runTemps(const Args& args) {
    const auto cfg = hb::loadConfig(resolveDataDir(args), exampleConfigCandidates());
    hb::HttplibClient http;
    const auto now = [] { return floor<seconds>(system_clock::now()); };
    auto sources = hb::loadTemperatureSources(cfg.raw, http, cfg.dataDir, now);
    if (sources.empty()) {
        std::cout << "No temperatureSources configured in " << cfg.configFile.string() << "\n";
        return 0;
    }
    int failures = 0;
    for (auto& src : sources) {
        std::cout << "Source '" << src->id() << "'\n";
        try {
            const auto readings = src->read();
            std::size_t wName = 4, wStation = 7;
            for (const auto& r : readings) {
                wName = std::max(wName, r.name.size());
                wStation = std::max(wStation, r.station.size());
            }
            const auto row = [&](const auto& id, const auto& name, const auto& kind, const auto& temp,
                                 const auto& hum, const auto& co2, const auto& age, const auto& batt,
                                 const auto& station) {
                std::cout << std::format("  {:<18} {:<{}} {:<7} {:>6} {:>5} {:>6} {:>8} {:>7}  {}\n", id, name,
                                         wName, kind, temp, hum, co2, age, batt, station);
            };
            row("Sensor id", "Name", "Kind", "Temp", "Hum%", "CO2", "Age", "Battery", "Station");
            for (const auto& r : readings) {
                std::string age = "-";
                if (r.measuredAt) age = std::format("{} min", duration_cast<minutes>(now() - *r.measuredAt).count());
                if (!r.reachable) age = "OFFLINE";
                row(r.sensorId, r.name, r.kind, fmtOpt(r.temperature, "{:.1f}"), fmtOpt(r.humidity, "{:.0f}"),
                    fmtOpt(r.co2, "{}"), age, fmtOpt(r.batteryPercent, "{}%"), r.station);
            }
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "  ERROR: " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        const Args args = parseArgs(argc, argv);
        if (args.verbose) hb::log::threshold() = hb::log::Level::Debug;
        if (args.command == "prices") return runPrices(args);
        if (args.command == "discover") return runDiscover(args);
        if (args.command == "plugs") return runPlugs(args);
        if (args.command == "plug") return runPlug(args);
        if (args.command == "temps") return runTemps(args);
        if (args.command == "plan") return runPlan(args);
        if (args.command == "run") return runDaemon(args);
        if (args.command == "history") return runHistory(args);
        if (args.command == "override") return runOverride(args);
        if (args.command == "notify-test") return runNotifyTest(args);
        if (args.command == "help") {
            std::cout << kUsage;
            return 0;
        }
        std::cerr << "Unknown command: " << args.command << "\n\n" << kUsage;
        return 2;
    } catch (const std::exception& e) {
        hb::log::error("{}", e.what());
        return 1;
    }
}
