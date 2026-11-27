#include "app/cost_report.hpp"

#include <algorithm>
#include <map>

#include "core/cost.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

namespace {

// A room's heating power: its thermostat's, or the sum of its plugs' ("missing": those without one).
struct RoomPower {
    double watts = 0;
    json missing = json::array();
    bool thermostat = false;
};

RoomPower roomPower(const json& cfg, const json& r) {
    RoomPower p;
    const std::string thermostat = r.value("thermostat", "");
    if (!thermostat.empty()) {
        // A thermostat room: its power is set on the thermostat (Settings -> Heating).
        p.thermostat = true;
        for (const auto& t : cfg.value("thermostats", json::array())) {
            if (t.value("id", "") == thermostat) p.watts = t.value("watts", 0.0);
        }
        if (p.watts <= 0) p.missing.push_back(thermostat);
    }
    for (const auto& pid : r.value("plugs", json::array())) {
        const std::string plugId = pid.get<std::string>();
        bool found = false;
        for (const auto& pl : cfg.value("plugs", json::array())) {
            if (pl.value("id", "") == plugId && pl.contains("watts") && pl["watts"].is_number()) {
                p.watts += pl["watts"].get<double>();
                found = true;
            }
        }
        if (!found) p.missing.push_back(plugId);
    }
    return p;
}

bool priceOnly(const json& r) { return r.contains("priceOnly") && !r["priceOnly"].is_null(); }

// One room's heating per local day over [from, to): energy and cost from its decisions (or from its
// thermostat's heating-time counter, when it reports one), and how long heatbrain ran for it each day.
struct RoomRun {
    std::vector<DayCost> days;
    std::map<std::string, double> coveredHours;
    bool live = false;      // a live plug at some point in [from, to)
    bool measured = false;  // a heating-time counter
};

RoomRun runRoom(const History& history, const std::string& id, double watts, const std::map<Instant, double>& prices,
                seconds maxGap, Instant from, Instant to, const TimeZone& tz) {
    RoomRun run;
    std::vector<DecisionPoint> points;
    std::vector<CounterSample> counter;  // thermostat rooms: its real heating time
    for (const auto& d : history.decisions(id, from - maxGap, to + maxGap, false)) {
        const bool live = std::ranges::any_of(d.plugs, [](const json& p) { return p.value("mode", "") == "live"; });
        points.push_back({d.at, d.on, live});
        for (const auto& p : d.plugs) {
            if (p.contains("relayOnSeconds") && p["relayOnSeconds"].is_number()) {
                counter.push_back({d.at, p["relayOnSeconds"].get<double>(), live});
            }
        }
    }
    run.live = std::ranges::any_of(points, [&](const DecisionPoint& p) { return p.live && p.at >= from && p.at < to; });
    run.measured = !counter.empty();
    run.days = counter.size() >= 2 ? dailyCostFromCounter(counter, prices, watts, maxGap, from, to, tz)
                                   : dailyCost(points, prices, watts, maxGap, from, to, tz);

    // Time covered by decisions, per local day (the day's bounds cached: thousands of short intervals).
    Instant dayStart{}, dayEnd{};
    std::string date;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Instant a = points[i].at;
        Instant t = std::max(a, from);
        const Instant end = std::min({i + 1 < points.size() ? points[i + 1].at : to, a + maxGap, to});
        while (t < end) {
            if (t < dayStart || t >= dayEnd) {
                dayStart = startOfLocalDay(tz, t);
                dayEnd = startOfLocalDay(tz, dayStart + hours{30});
                date = formatLocalDate(tz.toLocal(dayStart));
            }
            const Instant segEnd = std::min(end, dayEnd);
            run.coveredHours[date] += duration<double, std::ratio<3600>>(segEnd - t).count();
            t = segEnd;
        }
    }
    return run;
}

}  // namespace

json costReport(const json& cfg, const History& history, Instant now, int dayCount) {
    const TimeZone tz(cfg.value("timezone", std::string("Europe/Copenhagen")));
    const int tick = cfg.contains("control") ? cfg["control"].value("tickSeconds", 300) : 300;
    const Instant from = startOfLocalDay(tz, now - hours{24 * (dayCount - 1)});

    std::map<Instant, double> prices;
    for (const auto& p : history.prices(from, now + hours{1})) prices[p.slot] = p.total;
    const auto market = dailyAveragePrice(prices, tz);

    json rooms = json::array();
    json totals{{"kWh", 0.0}, {"cost", 0.0}, {"saved", 0.0}};
    for (const auto& r : cfg.value("rooms", json::array())) {
        const std::string id = r.value("id", "");
        // A room without a sensor (price-only) is left out: its radiator's own thermostat decides
        // when it really heats, so "plug on x power" says little about what it used.
        if (priceOnly(r)) {
            rooms.push_back({{"id", id}, {"name", r.value("name", id)}, {"excluded", true}});
            continue;
        }
        const RoomPower power = roomPower(cfg, r);
        const double watts = power.watts;
        json room{{"id", id}, {"name", r.value("name", id)}, {"watts", watts}, {"missingWatts", power.missing}};
        if (power.thermostat) room["thermostat"] = true;
        if (!power.missing.empty() || watts <= 0) {
            rooms.push_back(room);
            continue;
        }
        const RoomRun run = runRoom(history, id, watts, prices, seconds{3 * tick}, from, now, tz);
        // For the label: was heatbrain in control (live) at all in the period, and does the energy come
        // from the thermostat's heating time (from its first reading on)?
        room["live"] = run.live;
        room["measured"] = run.measured;
        json dj = json::array();
        double kWh = 0, cost = 0, saved = 0, onHours = 0, live = 0;
        for (const auto& d : run.days) {
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

json heatingDaysReport(const json& cfg, History& history, Instant now, int dayCount, const std::string& outdoorKey,
                       const std::string& outdoorName) {
    const TimeZone tz(cfg.value("timezone", std::string("Europe/Copenhagen")));
    const int tick = cfg.contains("control") ? cfg["control"].value("tickSeconds", 300) : 300;
    const seconds maxGap{3 * tick};
    const auto dateOf = [&](Instant t) { return formatLocalDate(tz.toLocal(t)); };
    const auto nextDay = [&](Instant dayStart) { return startOfLocalDay(tz, dayStart + hours{30}); };
    const Instant todayStart = startOfLocalDay(tz, now);
    const std::string today = dateOf(todayStart);
    // A day is summed up for good once all its decisions are in (the next one may close its last interval).
    const Instant settled = now - maxGap;

    // The first day shown: dayCount days back, or (0) the oldest day any room has.
    const Instant rangeStart = dayCount > 0 ? startOfLocalDay(tz, now - hours{24 * (dayCount - 1)}) : Instant{};
    std::string fromDate = dayCount > 0 ? dateOf(rangeStart) : today;
    if (dayCount <= 0) {
        for (const auto& r : cfg.value("rooms", json::array())) {
            if (priceOnly(r)) continue;
            const std::string id = r.value("id", "");
            if (const auto f = history.firstRoomDay(id); f && *f < fromDate) fromDate = *f;
            if (const auto f = history.firstDecision(id); f && dateOf(*f) < fromDate) fromDate = dateOf(*f);
        }
    }

    json rooms = json::array(), excluded = json::array();
    for (const auto& r : cfg.value("rooms", json::array())) {
        const std::string id = r.value("id", ""), name = r.value("name", id);
        // A room without a sensor: its radiator's own thermostat decides when it really heats (see costReport).
        if (priceOnly(r)) {
            excluded.push_back(name);
            continue;
        }
        const RoomPower power = roomPower(cfg, r);
        std::map<std::string, RoomDay> days;
        for (auto& d : history.roomDays(id, fromDate, today)) days[d.date] = d;

        bool measured = false;
        const auto toDay = [](const RoomRun& run, const std::string& date) {
            RoomDay d{date};
            for (const auto& c : run.days) {
                if (c.date == date) {
                    d.onHours = c.onHours;
                    d.kWh = c.kWh;
                }
            }
            if (auto it = run.coveredHours.find(date); it != run.coveredHours.end()) d.coveredHours = it->second;
            return d;
        };
        if (const auto first = history.firstDecision(id)) {
            // Days not summed up yet, from the room's first decision (or the range's start) on, in runs
            // of up to a month: years of decisions are read once, not on every call.
            Instant d = std::max(startOfLocalDay(tz, *first), rangeStart);
            std::vector<Instant> pending;
            const auto flush = [&] {
                if (pending.empty()) return;
                const RoomRun rr = runRoom(history, id, power.watts, {}, maxGap, pending.front(), nextDay(pending.back()), tz);
                measured = measured || rr.measured;
                std::vector<RoomDay> fresh;
                for (const Instant day : pending) fresh.push_back(days[dateOf(day)] = toDay(rr, dateOf(day)));
                history.saveRoomDays(id, fresh);
                pending.clear();
            };
            for (; d < todayStart && nextDay(d) <= settled; d = nextDay(d)) {
                if (days.contains(dateOf(d))) {
                    flush();
                    continue;
                }
                pending.push_back(d);
                if (pending.size() >= 31) flush();
            }
            flush();
            // The rest (today, and yesterday just after midnight): worked out now, not kept.
            if (d < now) {
                const RoomRun rr = runRoom(history, id, power.watts, {}, maxGap, d, now, tz);
                measured = measured || rr.measured;
                for (; d < now; d = nextDay(d)) days[dateOf(d)] = toDay(rr, dateOf(d));
            }
        }
        json dj = json::array();
        for (const auto& [date, d] : days) dj.push_back({date, d.onHours, d.kWh, d.coveredHours});
        rooms.push_back({{"id", id}, {"name", name}, {"watts", power.watts}, {"thermostat", power.thermostat},
                         {"measured", measured}, {"days", dj}});
    }

    json outdoor = nullptr;
    if (!outdoorKey.empty()) {
        json od = json::array();
        for (const auto& d : history.sensorDays(outdoorKey, now - std::chrono::days{4000}, now + std::chrono::days{1})) {
            const std::string date = dateOf(d.day + hours{12});  // the stored day start, as a local date
            if (date >= fromDate && d.readings > 0) od.push_back({date, d.avgTemp, d.minTemp, d.maxTemp});
        }
        outdoor = {{"key", outdoorKey}, {"name", outdoorName}, {"days", od}};
    }
    return json{{"from", fromDate}, {"today", today}, {"days", dayCount}, {"rooms", rooms},
                {"excluded", excluded}, {"outdoor", outdoor}};
}

}  // namespace hb
