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

std::optional<WifiOffPlan> parseWifiOff(const json& cfg) {
    if (!cfg.contains("wifiOff") || cfg["wifiOff"].is_null()) return std::nullopt;
    const auto window = [](const json& j, const char* what) {
        const DayWindow w = parseWindow(j);
        const int length = ((w.toMinute - w.fromMinute) % 1440 + 1440) % 1440;
        if (length < 30 || length > 16 * 60) {
            throw std::runtime_error(std::string(what) + " must be 30 minutes to 16 hours");
        }
        return w;
    };
    WifiOffPlan plan{window(cfg["wifiOff"], "Wi-Fi off period"), std::nullopt};
    if (cfg["wifiOff"].contains("weekend") && !cfg["wifiOff"]["weekend"].is_null()) {
        plan.weekend = window(cfg["wifiOff"]["weekend"], "Weekend Wi-Fi off period");
    }
    plan.plugRadio = cfg["wifiOff"].value("plugWifi", true);
    return plan;
}

namespace {

Strategy parseStrategy(const json& j) {
    Strategy s;
    if (!j.is_object()) return s;
    s.peakAllowedDrop = j.value("peakAllowedDrop", s.peakAllowedDrop);
    s.preheatBoost = j.value("preheatBoost", s.preheatBoost);
    s.preheatWindow = minutes{j.value("preheatMinutes", s.preheatWindow.count())};
    s.cheapBoost = j.value("cheapBoost", s.cheapBoost);
    s.staleAfter = minutes{j.value("staleMinutes", s.staleAfter.count())};
    s.limpOn = minutes{j.value("limpOnMinutes", s.limpOn.count())};
    s.limpOff = minutes{j.value("limpOffMinutes", s.limpOff.count())};
    s.limpConfigured = j.contains("limpOnMinutes") || j.contains("limpOffMinutes");
    if (s.staleAfter < minutes{10}) throw std::runtime_error("strategy.staleMinutes must be at least 10");
    return s;
}

RoomConfig parseRoom(const json& rj) {
    RoomConfig r;
    r.id = rj.at("id").get<std::string>();
    r.name = rj.value("name", r.id);
    r.thermostat = rj.value("thermostat", std::string());
    if (r.thermostat.empty()) {
        r.plugIds = rj.at("plugs").get<std::vector<std::string>>();
        if (r.plugIds.empty()) throw std::runtime_error("'plugs' is empty");
    } else if (rj.contains("priceOnly")) {
        throw std::runtime_error("a thermostat room needs a temperature, not 'priceOnly'");
    }
    r.sensor = rj.value("sensor", std::string());
    r.minTemp = rj.value("minTemp", r.minTemp);
    r.maxTemp = rj.value("maxTemp", r.maxTemp);
    r.frostTemp = rj.value("frostTemp", r.frostTemp);
    r.warmup = minutes{rj.value("warmupMinutes", 180)};
    if (r.warmup < minutes{0} || r.warmup > minutes{12 * 60}) throw std::runtime_error("warmupMinutes must be 0..720");
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
    if (r.minTemp >= r.maxTemp) {
        // Equal would switch the plug on and off at every crossing, and leave no room for a target.
        throw std::runtime_error(std::format(
            "'Never below' ({:g}°) must be lower than 'Never above' ({:g}°): heating needs room between them, "
            "e.g. never below {:g}°, never above {:g}°", r.minTemp, r.maxTemp, r.minTemp, r.minTemp + 3));
    }

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
        } else if (pj.contains("coastAbove") && !pj["coastAbove"].is_null()) {
            const double c = pj["coastAbove"].get<double>();
            if (c < r.frostTemp || c > r.maxTemp) {
                throw std::runtime_error(std::format("program: off 'stay above' {} must be between {} and {}", c,
                                                     r.frostTemp, r.maxTemp));
            }
            p.coastAbove = c;
        } else {
            p.warmAtEnd = pj.value("warmAtEnd", false);
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
        if (!r.thermostat.empty()) {
            bool known = false;
            for (const auto& t : cfg.value("thermostats", json::array())) known |= t.value("id", "") == r.thermostat;
            if (!known) throw std::runtime_error("Room " + r.id + ": unknown thermostat '" + r.thermostat + "'");
        }
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

std::vector<StoredBooking> BookingStore::load() const {
    std::vector<StoredBooking> out;
    if (!fs::exists(file_)) return out;
    try {
        const json j = json::parse(std::ifstream(file_));
        const auto now = floor<seconds>(system_clock::now());
        for (const auto& b : j) {
            StoredBooking s{b.at("id").get<std::string>(), b.at("room").get<std::string>(),
                            Booking{b.at("target").get<double>(), Instant{seconds{b.at("from").get<long long>()}},
                                    Instant{seconds{b.at("until").get<long long>()}}}};
            if (b.contains("warmingSince")) s.warmingSince = Instant{seconds{b["warmingSince"].get<long long>()}};
            if (s.booking.until > now) out.push_back(std::move(s));
        }
    } catch (const std::exception&) {
        // a broken file must never stop heating control
    }
    std::ranges::sort(out, {}, [](const StoredBooking& s) { return s.booking.from; });
    return out;
}

std::vector<Booking> BookingStore::forRoom(const RoomConfig& room) const {
    std::vector<Booking> out;
    for (auto s : load()) {
        if (s.room != room.id) continue;
        s.booking.warmup = room.warmup;
        out.push_back(s.booking);
    }
    return out;
}

void BookingStore::save(const std::vector<StoredBooking>& all) const {
    json j = json::array();
    for (const auto& s : all) {
        j.push_back({{"id", s.id}, {"room", s.room}, {"target", s.booking.target},
                     {"from", s.booking.from.time_since_epoch().count()},
                     {"until", s.booking.until.time_since_epoch().count()}});
        if (s.warmingSince) j.back()["warmingSince"] = s.warmingSince->time_since_epoch().count();
    }
    fs::path tmp = file_;
    tmp += ".tmp";
    std::ofstream(tmp) << j.dump(2);
    fs::rename(tmp, file_);
}

std::string BookingStore::add(const std::string& roomId, const Booking& b) const {
    auto all = load();
    for (const auto& s : all) {
        if (s.room == roomId && b.from < s.booking.until && s.booking.from < b.until) {
            throw std::invalid_argument("It overlaps another booking of this room");
        }
    }
    const std::string id = std::format("b{}", b.from.time_since_epoch().count());
    all.push_back({id, roomId, b});
    save(all);
    return id;
}

void BookingStore::markWarming(const std::string& id, Instant since) const {
    auto all = load();
    for (auto& s : all) {
        if (s.id == id && !s.warmingSince) s.warmingSince = since;
    }
    save(all);
}

bool BookingStore::update(const std::string& id, const std::string& roomId, const Booking& b) const {
    auto all = load();
    const auto it = std::ranges::find_if(all, [&](const StoredBooking& s) { return s.id == id; });
    if (it == all.end()) return false;
    for (const auto& s : all) {
        if (s.id != id && s.room == roomId && b.from < s.booking.until && s.booking.from < b.until) {
            throw std::invalid_argument("It overlaps another booking of this room");
        }
    }
    const bool sameStart = it->room == roomId && it->booking.from == b.from;
    it->room = roomId;
    it->booking = b;
    if (!sameStart) it->warmingSince.reset();  // a moved booking works out its warm-up afresh
    save(all);
    return true;
}

bool BookingStore::remove(const std::string& id) const {
    auto all = load();
    const auto n = std::erase_if(all, [&](const StoredBooking& s) { return s.id == id; });
    save(all);
    return n > 0;
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
