#include <algorithm>
#include <cstdio>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app/config.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "net/http.hpp"
#include "providers/price_model.hpp"

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

constexpr const char* kUsage = R"(heatbrain - price and temperature aware heating control

Usage:
  heatbrain prices [--slots]     Show today's and tomorrow's electricity price
  heatbrain help

Options:
  --data-dir DIR   Config, cache and state directory
                   (default: $HEATBRAIN_DATA_DIR, else ./data)
  --slots          Show 15-minute slots instead of hourly averages
  --verbose        Debug logging
)";

struct Args {
    std::string command = "help";
    std::optional<fs::path> dataDir;
    bool slots = false;
    bool verbose = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    bool haveCommand = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            a.dataDir = argv[++i];
        } else if (arg == "--slots") {
            a.slots = true;
        } else if (arg == "--verbose" || arg == "-v") {
            a.verbose = true;
        } else if (!arg.starts_with("-") && !haveCommand) {
            a.command = arg;
            haveCommand = true;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return a;
}

fs::path resolveDataDir(const Args& args) {
    if (args.dataDir) return *args.dataDir;
    if (const char* env = std::getenv("HEATBRAIN_DATA_DIR"); env && *env) return env;
#ifdef HEATBRAIN_SOURCE_DIR
    return fs::path(HEATBRAIN_SOURCE_DIR) / "data";  // local dev build
#else
    return "data";
#endif
}

std::vector<fs::path> exampleConfigCandidates() {
    std::vector<fs::path> c;
#ifdef HEATBRAIN_SOURCE_DIR
    c.push_back(fs::path(HEATBRAIN_SOURCE_DIR) / "config" / "heatbrain.example.json");
#endif
    c.push_back(fs::path("config") / "heatbrain.example.json");
    c.push_back("/app/config/heatbrain.example.json");
    return c;
}

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

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        const Args args = parseArgs(argc, argv);
        if (args.verbose) hb::log::threshold() = hb::log::Level::Debug;
        if (args.command == "prices") return runPrices(args);
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
