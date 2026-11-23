#include "sensors/velux.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

#include "core/log.hpp"

namespace hb::velux {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace {

// The VELUX ACTIVE app's own client (as used by the community integrations).
constexpr const char* kClientId = "5931426da127d981e76bdd3f";
constexpr const char* kClientSecret = "6ae2d89d15e767ae5c56b456b452d319";
constexpr const char* kAppVersion = "791304000";
// Room names change rarely: homesdata once every 6 hours, homestatus every read.
constexpr auto kHomesDataMaxAge = hours{6};

std::optional<Tokens> loadTokens(const fs::path& file) {
    if (!fs::exists(file)) return std::nullopt;
    try {
        std::ifstream in(file);
        const json j = json::parse(in);
        return Tokens{j.at("access_token"), j.at("refresh_token"), Instant{seconds{j.at("expires_at").get<long long>()}}};
    } catch (const std::exception& e) {
        log::warn("VELUX: ignoring unreadable token file {}: {}", file.string(), e.what());
        return std::nullopt;
    }
}

void saveTokens(const fs::path& file, const Tokens& t) {
    const json j{{"access_token", t.access}, {"refresh_token", t.refresh}, {"expires_at", t.expiresAt.time_since_epoch().count()}};
    fs::create_directories(file.parent_path());
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("VELUX: cannot write " + tmp.string());
    }
    fs::rename(tmp, file);
}

// POST to the token endpoint; the answer's tokens, or why not.
Tokens tokenRequest(HttpClient& http, const std::string& apiBase, std::vector<std::pair<std::string, std::string>> fields,
                    Instant now, const std::string& previousRefresh) {
    fields.insert(fields.end(), {{"client_id", kClientId}, {"client_secret", kClientSecret}, {"app_version", kAppVersion}});
    HttpRequest req;
    req.method = "POST";
    req.url = apiBase + "/oauth2/token";
    req.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
    req.body = formEncode(fields);
    const HttpResponse res = http.send(req);
    json j;
    try {
        j = json::parse(res.body);
    } catch (const json::parse_error&) {
        throw HttpError(std::format("VELUX login: HTTP {}", res.status));
    }
    if (res.status != 200 || !j.contains("access_token")) {
        const std::string err = j.value("error", std::string("unknown error"));
        if (res.status == 400 || res.status == 401 || res.status == 403 || err == "invalid_grant" || err == "invalid_client") {
            throw SourceAuthError(std::format("VELUX ACTIVE rejected the login ({}). Connect again in Settings -> Sensors.", err));
        }
        throw HttpError(std::format("VELUX login: HTTP {} {}", res.status, err));
    }
    Tokens t;
    t.access = j.at("access_token").get<std::string>();
    t.refresh = j.value("refresh_token", previousRefresh);
    t.expiresAt = now + seconds{j.value("expires_in", j.value("expire_in", 10800))};
    return t;
}

std::optional<double> number(const json& o, const char* key) {
    if (o.contains(key) && o[key].is_number()) return o[key].get<double>();
    return std::nullopt;
}

}  // namespace

Tokens login(HttpClient& http, const std::string& email, const std::string& password, const fs::path& tokenFile,
             Instant now, const std::string& apiBase) {
    if (email.empty() || password.empty()) throw std::invalid_argument("Enter the e-mail and password of your VELUX ACTIVE account");
    const Tokens t = tokenRequest(http, apiBase,
                                  {{"grant_type", "password"}, {"username", email}, {"password", password},
                                   {"user_prefix", "velux"}, {"scope", "velux_scopes"}},
                                  now, "");
    saveTokens(tokenFile, t);
    log::info("VELUX: logged in (tokens saved, password not kept)");
    return t;
}

Auth::Auth(HttpClient& http, fs::path tokenFile, std::function<Instant()> now, std::string apiBase)
    : http_(http), tokenFile_(std::move(tokenFile)), now_(std::move(now)), apiBase_(std::move(apiBase)) {
    tokens_ = loadTokens(tokenFile_);
}

void Auth::refresh() {
    if (!tokens_) tokens_ = loadTokens(tokenFile_);  // logged in since heatbrain started
    if (!tokens_ || tokens_->refresh.empty()) {
        throw SourceAuthError("VELUX ACTIVE is not connected: log in under Settings -> Sensors.");
    }
    const Tokens t = tokenRequest(http_, apiBase_, {{"grant_type", "refresh_token"}, {"refresh_token", tokens_->refresh}},
                                  now_(), tokens_->refresh);
    saveTokens(tokenFile_, t);  // the rotated refresh token first
    tokens_ = t;
    log::info("VELUX: access token refreshed (valid until {})", formatIsoUtc(t.expiresAt));
}

std::string Auth::accessToken() {
    // A new login from the settings page replaces the file: pick it up.
    if (const auto onDisk = loadTokens(tokenFile_); onDisk && (!tokens_ || onDisk->refresh != tokens_->refresh)) tokens_ = onDisk;
    if (!tokens_ || tokens_->access.empty() || now_() + minutes{5} >= tokens_->expiresAt) refresh();
    return tokens_->access;
}

void Auth::invalidate() {
    if (tokens_) tokens_->access.clear();
}

std::vector<SensorReading> parseHome(const json& homesData, const json& homeStatus, const std::string& homeId, Instant now) {
    // Names and which room each module is in, from homesdata.
    std::string homeName;
    std::map<std::string, std::string> roomNames, moduleRoom;
    for (const auto& h : homesData.at("body").value("homes", json::array())) {
        if (h.value("id", "") != homeId) continue;
        homeName = h.value("name", "");
        for (const auto& r : h.value("rooms", json::array())) roomNames[r.value("id", "")] = r.value("name", "");
        for (const auto& m : h.value("modules", json::array())) {
            if (m.contains("room_id") && m["room_id"].is_string()) moduleRoom[m.value("id", "")] = m["room_id"].get<std::string>();
        }
    }
    const json& home = homeStatus.at("body").at("home");
    // The room's sensor modules (wall switch NXD, climate sensor NXS): last contact, reachable, battery.
    struct RoomModules {
        std::optional<Instant> lastSeen;
        bool anyReachable = false, any = false;
        std::optional<int> battery;
    };
    std::map<std::string, RoomModules> mods;
    bool gatewayReachable = true;
    for (const auto& m : home.value("modules", json::array())) {
        const std::string type = m.value("type", "");
        if (type == "NXG" && m.contains("reachable") && m["reachable"].is_boolean()) gatewayReachable = m["reachable"].get<bool>();
        if (type != "NXD" && type != "NXS") continue;
        const auto it = moduleRoom.find(m.value("id", ""));
        const std::string room = it != moduleRoom.end() ? it->second : m.value("room_id", "");
        if (room.empty()) continue;
        auto& rm = mods[room];
        rm.any = true;
        if (m.value("reachable", true)) rm.anyReachable = true;
        if (const auto ls = number(m, "last_seen")) {
            const Instant t{seconds{static_cast<long long>(*ls)}};
            if (!rm.lastSeen || t > *rm.lastSeen) rm.lastSeen = t;
        }
        if (const auto b = number(m, "battery_percent")) rm.battery = rm.battery ? std::min(*rm.battery, static_cast<int>(*b)) : static_cast<int>(*b);
    }
    std::vector<SensorReading> out;
    for (const auto& r : home.value("rooms", json::array())) {
        const auto tenths = number(r, "temperature");
        if (!tenths) continue;  // a room with only windows: no climate readings
        SensorReading s;
        s.sensorId = r.value("id", "");
        const auto name = roomNames.find(s.sensorId);
        s.name = name != roomNames.end() && !name->second.empty() ? name->second : s.sensorId;
        s.station = homeName.empty() ? "VELUX" : homeName;
        s.kind = "indoor";
        s.temperature = *tenths / 10.0;  // tenths of a degree
        if (const auto h = number(r, "humidity")) s.humidity = *h;
        if (const auto c = number(r, "co2")) s.co2 = static_cast<int>(*c);
        const auto m = mods.find(s.sensorId);
        s.measuredAt = m != mods.end() && m->second.lastSeen ? *m->second.lastSeen : now;
        s.reachable = gatewayReachable && (m == mods.end() || !m->second.any || m->second.anyReachable);
        if (m != mods.end()) s.batteryPercent = m->second.battery;
        out.push_back(std::move(s));
    }
    return out;
}

Source::Source(std::string id, HttpClient& http, Auth auth, std::function<Instant()> now, std::string apiBase)
    : id_(std::move(id)), http_(http), auth_(std::move(auth)), now_(std::move(now)), apiBase_(std::move(apiBase)) {}

json Source::post(const std::string& what, HttpRequest req, bool retried) {
    const HttpResponse res = http_.send(req);
    json j;
    try {
        j = json::parse(res.body);
    } catch (const json::parse_error&) {
        throw HttpError(std::format("VELUX {}: HTTP {}", what, res.status));
    }
    if (res.status == 200 && j.contains("body")) return j;
    // An expired or revoked access token: refresh once and try again.
    const int code = j.contains("error") && j["error"].is_object() ? j["error"].value("code", 0) : 0;
    if (!retried && (res.status == 401 || res.status == 403 || code == 2 || code == 3)) {
        auth_.invalidate();
        const std::string token = auth_.accessToken();
        for (auto& [k, v] : req.headers) if (k == "Authorization") v = "Bearer " + token;
        if (req.body.starts_with("access_token=")) req.body = formEncode({{"access_token", token}});
        return post(what, req, true);
    }
    const std::string msg = j.contains("error") && j["error"].is_object() ? j["error"].value("message", "") : res.body.substr(0, 200);
    throw HttpError(std::format("VELUX {}: HTTP {} {}", what, res.status, msg));
}

std::vector<SensorReading> Source::read() {
    const Instant now = now_();
    if (!homesData_ || now - homesData_->first >= kHomesDataMaxAge) {
        HttpRequest req;
        req.method = "POST";
        req.url = apiBase_ + "/api/homesdata";
        req.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
        req.body = formEncode({{"access_token", auth_.accessToken()}});
        homesData_ = {now, post("homesdata", req).dump()};
    }
    const json homes = json::parse(homesData_->second);
    std::vector<SensorReading> out;
    for (const auto& h : homes.at("body").value("homes", json::array())) {
        const std::string homeId = h.value("id", "");
        if (homeId.empty()) continue;
        HttpRequest req;
        req.method = "POST";
        req.url = apiBase_ + "/syncapi/v1/homestatus";
        req.contentType = "application/json";
        req.headers = {{"Authorization", "Bearer " + auth_.accessToken()}};
        req.body = json{{"home_id", homeId}}.dump();
        const auto readings = parseHome(homes, post("homestatus", req), homeId, now);
        out.insert(out.end(), readings.begin(), readings.end());
    }
    return out;
}

}  // namespace hb::velux
