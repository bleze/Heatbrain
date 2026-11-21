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

    const json program = roomWithProgram(rj).value("program", json::array());
    for (const auto& pj : program) {
        ProgramPoint p;
        const auto at = parseClock(pj.at("at").get<std::string>());
        if (!at || *at >= 1440) throw std::runtime_error("program: invalid time (use HH:MM, 00:00-23:59)");
        p.atMinute = *at;
        const std::string mode = pj.value("mode", std::string("heat"));
        if (mode == "heat") p.mode = ProgramMode::Heat;
        else if (mode == "keep") p.mode = ProgramMode::Keep;
        else if (mode == "off") p.mode = ProgramMode::Off;
        else throw std::runtime_error("program: mode must be heat, keep or off");
        if (p.mode != ProgramMode::Off) {
            p.target = pj.at("target").get<double>();
            const double lo = p.mode == ProgramMode::Heat ? r.minTemp : r.frostTemp;
            if (p.target < lo || p.target > r.maxTemp) {
                throw std::runtime_error(std::format("program: {} target {} must be between {} and {}", mode, p.target,
                                                     lo, r.maxTemp));
            }
        }
        r.program.push_back(p);
    }
    if (r.program.empty()) throw std::runtime_error("the daily program is empty");
    std::ranges::sort(r.program, {}, &ProgramPoint::atMinute);
    for (std::size_t i = 1; i < r.program.size(); ++i) {
        if (r.program[i].atMinute == r.program[i - 1].atMinute) {
            throw std::runtime_error(std::format("program: two switch points at {:02}:{:02}", r.program[i].atMinute / 60,
                                                 r.program[i].atMinute % 60));
        }
    }
    return r;
}

}  // namespace

json roomWithProgram(const json& room) {
    if (room.contains("program") || room.contains("priceOnly") || !room.contains("schedule")) return room;
    // Evaluate the old bands and no-heat windows per 15 minutes, then keep the changes.
    std::vector<DayWindow> off;
    for (const auto& w : room.value("noHeat", json::array())) off.push_back(parseWindow(w));
    std::vector<std::pair<DayWindow, double>> bands;
    for (const auto& b : room.at("schedule")) bands.emplace_back(parseWindow(b), b.at("target").get<double>());
    const auto stateAt = [&](int m) -> std::optional<double> {  // nullopt = off
        for (const auto& w : off) {
            if (w.contains(m)) return std::nullopt;
        }
        for (const auto& [w, target] : bands) {
            if (w.contains(m)) return target;
        }
        return std::nullopt;  // uncovered time used to be invalid; off is the safe reading
    };
    json program = json::array();
    std::optional<std::optional<double>> prev;
    for (int m = 0; m < 1440; m += 15) {
        const auto st = stateAt(m);
        if (prev && *prev == st) continue;
        json p{{"at", std::format("{:02}:{:02}", m / 60, m % 60)}, {"mode", st ? "heat" : "off"}};
        if (st) p["target"] = *st;
        program.push_back(p);
        prev = st;
    }
    // A change exactly at midnight that equals the last state is not a real switch point.
    if (program.size() > 1 && program.front().value("mode", "") == program.back().value("mode", "") &&
        program.front().value("target", 0.0) == program.back().value("target", 0.0)) {
        program.erase(program.begin());
    }
    json out = room;
    out.erase("schedule");
    out.erase("noHeat");
    out["program"] = program;
    return out;
}

json withPrograms(json cfg) {
    if (cfg.contains("rooms")) {
        for (auto& r : cfg["rooms"]) r = roomWithProgram(r);
    }
    return cfg;
}

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

std::optional<AwayMode> AwayStore::load() const {
    if (!fs::exists(file_)) return std::nullopt;
    try {
        std::ifstream in(file_);
        const json j = json::parse(in);
        AwayMode a{j.at("keepAbove").get<double>(), Instant{seconds{j.value("from", 0LL)}},
                   Instant{seconds{j.at("until").get<long long>()}}};
        if (a.until <= floor<seconds>(system_clock::now())) return std::nullopt;
        return a;
    } catch (const std::exception&) {
        return std::nullopt;  // a broken file must never stop heating control
    }
}

void AwayStore::set(const AwayMode& a) const {
    fs::path tmp = file_;
    tmp += ".tmp";
    std::ofstream(tmp) << json{{"keepAbove", a.keepAbove},
                               {"from", a.from.time_since_epoch().count()},
                               {"until", a.until.time_since_epoch().count()}}.dump(2);
    fs::rename(tmp, file_);
}

void AwayStore::clear() const {
    std::error_code ec;
    fs::remove(file_, ec);
}

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
