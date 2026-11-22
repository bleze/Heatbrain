#include "app/cost_report.hpp"

#include <algorithm>
#include <map>

#include "core/cost.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

json costReport(const json& cfg, const History& history, Instant now, int dayCount) {
    const TimeZone tz(cfg.value("timezone", std::string("Europe/Copenhagen")));
    const int tick = cfg.contains("control") ? cfg["control"].value("tickSeconds", 300) : 300;
    const Instant from = startOfLocalDay(tz, now - hours{24 * (dayCount - 1)});

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
        // Left out by choice (Settings -> Rooms): e.g. a radiator whose own thermostat decides, so
        // "plug on x power" says nothing about what it used.
        if (!r.value("countCosts", true)) {
            rooms.push_back({{"id", id}, {"name", r.value("name", id)}, {"excluded", true}});
            continue;
        }
        double watts = 0;
        json missing = json::array();
        const std::string thermostat = r.value("thermostat", "");
        if (!thermostat.empty()) {
            // A thermostat room: its power is set on the thermostat (Settings -> Heating).
            for (const auto& t : cfg.value("thermostats", json::array())) {
                if (t.value("id", "") == thermostat) watts = t.value("watts", 0.0);
            }
            if (watts <= 0) missing.push_back(thermostat);
        }
        for (const auto& pid : r.value("plugs", json::array())) {
            const std::string plugId = pid.get<std::string>();
            if (auto it = wattsByPlug.find(plugId); it != wattsByPlug.end()) watts += it->second;
            else missing.push_back(plugId);
        }
        json room{{"id", id}, {"name", r.value("name", id)}, {"watts", watts}, {"missingWatts", missing}};
        if (!thermostat.empty()) room["thermostat"] = true;
        if (!missing.empty() || watts <= 0) {
            rooms.push_back(room);
            continue;
        }
        std::vector<DecisionPoint> points;
        std::vector<CounterSample> counter;  // thermostat rooms: its real heating time
        for (const auto& d : history.decisions(id, from, now + seconds{1}, false)) {
            const bool live = std::ranges::any_of(d.plugs, [](const json& p) { return p.value("mode", "") == "live"; });
            points.push_back({d.at, d.on, live});
            for (const auto& p : d.plugs) {
                if (p.contains("relayOnSeconds") && p["relayOnSeconds"].is_number()) {
                    counter.push_back({d.at, p["relayOnSeconds"].get<double>(), live});
                }
            }
        }
        const bool measured = counter.size() >= 2;
        // For the label: was heatbrain in control (live) at all in the period, and does the energy come
        // from the thermostat's heating time (from its first reading on)?
        room["live"] = std::ranges::any_of(points, [](const DecisionPoint& p) { return p.live; });
        room["measured"] = !counter.empty();
        json dj = json::array();
        double kWh = 0, cost = 0, saved = 0, onHours = 0, live = 0;
        const auto dayCosts = measured ? dailyCostFromCounter(counter, prices, watts, seconds{3 * tick}, from, now, tz)
                                       : dailyCost(points, prices, watts, seconds{3 * tick}, from, now, tz);
        for (const auto& d : dayCosts) {
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
    return json{{"currency", cfg.value("currency", std::string("DKK"))},
                {"from", from.time_since_epoch().count()},
                {"to", now.time_since_epoch().count()},
                {"days", dayCount},
                {"market", marketJson},
                {"rooms", rooms},
                {"total", totals}};
}

}  // namespace hb
