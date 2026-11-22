#include "sensors/netatmo.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <thread>
#include <fstream>

#include "core/log.hpp"

namespace hb::netatmo {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

Auth::Auth(HttpClient& http, std::string clientId, std::string clientSecret, std::string configRefreshToken,
           fs::path tokenFile, std::function<Instant()> now, std::string apiBase)
    : http_(http),
      clientId_(std::move(clientId)),
      clientSecret_(std::move(clientSecret)),
      configRefreshToken_(std::move(configRefreshToken)),
      tokenFile_(std::move(tokenFile)),
      now_(std::move(now)),
      apiBase_(std::move(apiBase)) {
    if (clientId_.empty() || clientSecret_.empty()) {
        throw std::runtime_error("Netatmo: clientId and clientSecret are required (see heatbrain.json)");
    }
    tokens_ = load();
}

std::optional<Auth::Tokens> Auth::load() const {
    if (!fs::exists(tokenFile_)) return std::nullopt;
    try {
        std::ifstream in(tokenFile_);
        const json j = json::parse(in);
        return Tokens{j.at("access_token"), j.at("refresh_token"), j.value("bootstrap", ""),
                      Instant{seconds{j.at("expires_at").get<long long>()}}};
    } catch (const std::exception& e) {
        log::warn("Netatmo: ignoring unreadable token file {}: {}", tokenFile_.string(), e.what());
        return std::nullopt;
    }
}

void Auth::save(const Tokens& t) const {
    const json j{{"access_token", t.access},
                 {"refresh_token", t.refresh},
                 {"bootstrap", t.bootstrap},
                 {"expires_at", t.expiresAt.time_since_epoch().count()}};
    fs::create_directories(tokenFile_.parent_path());
    fs::path tmp = tokenFile_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("Netatmo: cannot write " + tmp.string());
    }
    fs::rename(tmp, tokenFile_);
}

void Auth::refresh() {
    // A refresh token newly pasted into the config wins over the stored chain.
    const bool useStored = tokens_ && tokens_->bootstrap == configRefreshToken_ && !tokens_->refresh.empty();
    const std::string refreshToken = useStored ? tokens_->refresh : configRefreshToken_;
    if (refreshToken.empty()) {
        throw AuthError("Netatmo: no refresh token. Generate one at dev.netatmo.com (scope read_station) "
                        "and put it in refreshToken in heatbrain.json");
    }

    HttpRequest req;
    req.method = "POST";
    req.url = apiBase_ + "/oauth2/token";
    req.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
    req.body = formEncode({{"grant_type", "refresh_token"},
                           {"refresh_token", refreshToken},
                           {"client_id", clientId_},
                           {"client_secret", clientSecret_}});
    const HttpResponse res = http_.send(req);

    json j;
    try {
        j = json::parse(res.body);
    } catch (const json::parse_error&) {
        throw HttpError(std::format("Netatmo token endpoint: HTTP {}", res.status));
    }
    if (res.status != 200 || !j.contains("access_token")) {
        const std::string err = j.value("error", std::string("unknown error"));
        if (err == "invalid_grant" || err == "invalid_client") {
            throw AuthError(std::format(
                "Netatmo rejected the {} ({}). Generate a new refresh token at dev.netatmo.com "
                "(scope read_station) and put it in refreshToken in heatbrain.json",
                err == "invalid_client" ? "client id/secret" : "refresh token", err));
        }
        throw HttpError(std::format("Netatmo token endpoint: HTTP {} {}", res.status, err));
    }

    Tokens t;
    t.access = j.at("access_token").get<std::string>();
    t.refresh = j.value("refresh_token", refreshToken);
    t.bootstrap = configRefreshToken_;
    t.expiresAt = now_() + seconds{j.value("expires_in", 10800)};
    save(t);  // persist the rotated refresh token before anything else can fail
    tokens_ = t;
    log::info("Netatmo: access token refreshed (valid until {})", formatIsoUtc(t.expiresAt));
}

std::string Auth::accessToken() {
    const bool configChanged = tokens_ && tokens_->bootstrap != configRefreshToken_;
    if (!tokens_ || tokens_->access.empty() || configChanged || now_() + minutes{5} >= tokens_->expiresAt) {
        refresh();
    }
    return tokens_->access;
}

void Auth::invalidate() {
    if (tokens_) tokens_->access.clear();
}

// ---------------------------------------------------------------------------------------

namespace {

SensorReading readingFrom(const json& m, const std::string& station, const std::string& kind) {
    SensorReading r;
    r.sensorId = m.value("_id", "");
    r.name = m.value("module_name", r.sensorId);
    r.station = station;
    r.kind = kind;
    r.reachable = m.value("reachable", true);
    if (m.contains("battery_percent")) r.batteryPercent = m["battery_percent"].get<int>();
    if (m.contains("dashboard_data") && m["dashboard_data"].is_object()) {
        const auto& d = m["dashboard_data"];
        if (d.contains("Temperature")) r.temperature = d["Temperature"].get<double>();
        if (d.contains("Humidity")) r.humidity = d["Humidity"].get<double>();
        if (d.contains("CO2")) r.co2 = d["CO2"].get<int>();
        if (d.contains("time_utc")) r.measuredAt = Instant{seconds{d["time_utc"].get<long long>()}};
    }
    return r;
}

}  // namespace

std::vector<SensorReading> parseStationsData(const json& j) {
    std::vector<SensorReading> out;
    for (const auto& dev : j.at("body").at("devices")) {
        const std::string station = dev.value("station_name", dev.value("home_name", ""));
        out.push_back(readingFrom(dev, station, "indoor"));  // the base station is indoors
        if (!dev.contains("modules")) continue;
        for (const auto& m : dev["modules"]) {
            const std::string type = m.value("type", "");
            if (type == "NAModule4") out.push_back(readingFrom(m, station, "indoor"));
            if (type == "NAModule1") out.push_back(readingFrom(m, station, "outdoor"));
        }
    }
    return out;
}

Source::Source(std::string id, HttpClient& http, Auth auth, std::string apiBase)
    : id_(std::move(id)), http_(http), auth_(std::move(auth)), apiBase_(std::move(apiBase)) {}

std::vector<SensorReading> Source::read() {
    for (int attempt = 1;; ++attempt) {
        HttpRequest req;
        req.url = apiBase_ + "/api/getstationsdata";
        req.headers = {{"Authorization", "Bearer " + auth_.accessToken()}};
        const HttpResponse res = http_.send(req);
        // Netatmo answers 5xx now and then for a moment: try once more before giving up this round.
        if (res.status >= 500 && attempt == 1) {
            std::this_thread::sleep_for(std::chrono::seconds{5});
            continue;
        }
        json j;
        try {
            j = json::parse(res.body);
        } catch (const json::parse_error&) {
            throw HttpError(std::format("Netatmo getstationsdata: HTTP {}", res.status));
        }
        if (res.status == 200) return parseStationsData(j);

        // {"error":{"code":3,"message":"Access token expired"}}; 2 = invalid token.
        const int code = j.contains("error") && j["error"].is_object() ? j["error"].value("code", 0) : 0;
        if ((code == 2 || code == 3) && attempt == 1) {
            auth_.invalidate();
            continue;
        }
        const std::string msg =
            j.contains("error") && j["error"].is_object() ? j["error"].value("message", "") : res.body.substr(0, 200);
        throw HttpError(std::format("Netatmo getstationsdata: HTTP {} {}", res.status, msg));
    }
}

}  // namespace hb::netatmo
