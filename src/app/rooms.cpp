#include "app/rooms.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>

#include "core/schedule.hpp"

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

DayWindow parseWindow(const json& j) {
    const auto from = parseClock(j.at("from").get<std::string>());
    const auto to = parseClock(j.at("to").get<std::string>());
    if (!from || !to) throw std::runtime_error("invalid time window (use HH:MM)");
    if (*from == *to) throw std::runtime_error("time window 'from' and 'to' are equal");
    return DayWindow{*from, *to};
}

namespace {

Strategy parseStrategy(const json& j) {
    Strategy s;
    if (!j.is_object()) return s;
    s.hysteresis = j.value("hysteresis", s.hysteresis);
    s.peakAllowedDrop = j.value("peakAllowedDrop", s.peakAllowedDrop);
    s.preheatBoost = j.value("preheatBoost", s.preheatBoost);
    s.preheatWindow = minutes{j.value("preheatMinutes", s.preheatWindow.count())};
    s.cheapBoost = j.value("cheapBoost", s.cheapBoost);
    s.staleAfter = minutes{j.value("staleMinutes", s.staleAfter.count())};
    s.limpOn = minutes{j.value("limpOnMinutes", s.limpOn.count())};
    s.limpOff = minutes{j.value("limpOffMinutes", s.limpOff.count())};
    if (s.hysteresis < 0 || s.hysteresis > 2) throw std::runtime_error("strategy.hysteresis must be 0..2");
    if (s.staleAfter < minutes{10}) throw std::runtime_error("strategy.staleMinutes must be at least 10");
    return s;
}

RoomConfig parseRoom(const json& rj) {
    RoomConfig r;
    r.id = rj.at("id").get<std::string>();
    r.name = rj.value("name", r.id);
    r.plugIds = rj.at("plugs").get<std::vector<std::string>>();
    if (r.plugIds.empty()) throw std::runtime_error("'plugs' is empty");
    r.sensor = rj.value("sensor", std::string());
    r.minTemp = rj.value("minTemp", r.minTemp);
    r.maxTemp = rj.value("maxTemp", r.maxTemp);
    r.frostTemp = rj.value("frostTemp", r.frostTemp);
    r.strategy = parseStrategy(rj.value("strategy", json::object()));
    for (const auto& w : rj.value("noHeat", json::array())) r.noHeat.push_back(parseWindow(w));

    if (rj.contains("priceOnly")) {
        PriceOnlyConfig p;
        const auto& pj = rj["priceOnly"];
        p.hoursPerDay = pj.value("hoursPerDay", p.hoursPerDay);
        if (p.hoursPerDay < 0 || p.hoursPerDay > 24) throw std::runtime_error("priceOnly.hoursPerDay must be 0..24");
        if (pj.contains("avoid")) {
            for (const auto& w : pj["avoid"]) p.avoid.push_back(parseWindow(w));
        }
        r.priceOnly = p;
        return r;
    }

    if (r.sensor.empty()) throw std::runtime_error("'sensor' is required (or use 'priceOnly')");
    if (r.sensor.find(':') == std::string::npos) {
        throw std::runtime_error("'sensor' must look like \"netatmo:Bedroom\" (source id : sensor id or name)");
    }
    if (r.minTemp >= r.maxTemp) throw std::runtime_error("minTemp must be below maxTemp");
    for (const auto& bj : rj.at("schedule")) {
        TargetBand b;
        b.window = parseWindow(bj);
        b.target = bj.at("target").get<double>();
        b.label = formatWindow(b.window);
        if (b.target < r.minTemp || b.target > r.maxTemp) {
            throw std::runtime_error(std::format("schedule target {} must be between minTemp and maxTemp", b.target));
        }
        r.schedule.push_back(b);
    }
    if (r.schedule.empty()) throw std::runtime_error("'schedule' is empty");
    for (int m = 0; m < 1440; m += 15) {
        if (!targetAt(r, m)) {
            throw std::runtime_error(std::format("schedule does not cover {:02}:{:02}", m / 60, m % 60));
        }
    }
    return r;
}

}  // namespace

std::vector<RoomConfig> parseRooms(const json& cfg, const std::vector<Plug>& plugs) {
    std::vector<RoomConfig> rooms;
    if (!cfg.contains("rooms")) return rooms;
    std::set<std::string> roomIds, usedPlugs;
    for (const auto& rj : cfg.at("rooms")) {
        RoomConfig r;
        const std::string id = rj.value("id", std::string("?"));
        try {
            r = parseRoom(rj);
        } catch (const std::exception& e) {
            throw std::runtime_error("Room " + id + ": " + e.what());
        }
        if (!roomIds.insert(r.id).second) throw std::runtime_error("Duplicate room id: " + r.id);
        for (const auto& p : r.plugIds) {
            if (std::ranges::none_of(plugs, [&](const Plug& pl) { return pl.id == p; })) {
                throw std::runtime_error("Room " + r.id + ": unknown plug '" + p + "'");
            }
            if (!usedPlugs.insert(p).second) {
                throw std::runtime_error("Plug '" + p + "' is used by more than one room");
            }
        }
        rooms.push_back(std::move(r));
    }
    return rooms;
}

// ---------------------------------------------------------------------------------------

std::map<std::string, ManualOverride> Overrides::load() const {
    std::map<std::string, ManualOverride> out;
    if (!fs::exists(file_)) return out;
    try {
        std::ifstream in(file_);
        const json j = json::parse(in);
        for (const auto& [room, o] : j.items()) {
            out[room] = ManualOverride{o.at("on").get<bool>(), Instant{seconds{o.at("until").get<long long>()}}};
        }
    } catch (const std::exception&) {
        // A half-written or hand-edited file must never stop heating control.
    }
    return out;
}

void Overrides::save(const std::map<std::string, ManualOverride>& all) const {
    json j = json::object();
    for (const auto& [room, o] : all) j[room] = {{"on", o.on}, {"until", o.until.time_since_epoch().count()}};
    fs::path tmp = file_;
    tmp += ".tmp";
    std::ofstream(tmp) << j.dump(2);
    fs::rename(tmp, file_);
}

void Overrides::set(const std::string& roomId, ManualOverride o) const {
    auto all = load();
    all[roomId] = o;
    save(all);
}

void Overrides::clear(const std::string& roomId) const {
    auto all = load();
    all.erase(roomId);
    save(all);
}

}  // namespace hb
