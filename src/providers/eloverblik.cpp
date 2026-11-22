#include "providers/eloverblik.hpp"

#include <format>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

namespace {

constexpr hours kAccessTokenReuse{20};  // valid 24 h; renew a little early

std::optional<Instant> parseUtc(const std::string& s) {
    static const std::regex iso(R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?)");
    std::smatch m;
    if (!std::regex_search(s, m, iso)) return std::nullopt;
    const auto ymd = year{std::stoi(m[1])} / std::stoi(m[2]) / std::stoi(m[3]);
    if (!ymd.ok()) return std::nullopt;
    return Instant{sys_days{ymd}.time_since_epoch()} + hours{std::stoi(m[4])} + minutes{std::stoi(m[5])} +
           seconds{m[6].matched ? std::stoi(m[6]) : 0};
}

minutes parseResolution(const std::string& r) {
    if (r == "PT15M") return minutes{15};
    if (r == "PT1H") return minutes{60};
    if (r == "P1D") return minutes{1440};
    static const std::regex pt(R"(PT(\d+)M)");
    std::smatch m;
    if (std::regex_match(r, m, pt)) return minutes{std::stoi(m[1])};
    throw std::runtime_error("Eloverblik: unknown resolution " + r);
}

std::string describe(const HttpResponse& r) {
    if (r.status == 401) return "the token was refused - create a new one on eloverblik.dk";
    if (r.status == 429 || r.status == 503) return "Eloverblik is busy right now, heatbrain tries again later";
    return std::format("HTTP {}: {}", r.status, r.body.substr(0, 160));
}

}  // namespace

EloverblikClient::EloverblikClient(HttpClient& http, fs::path tokenFile, std::string refreshToken,
                                   std::function<Instant()> now, std::string baseUrl)
    : http_(http), tokenFile_(std::move(tokenFile)), refreshToken_(std::move(refreshToken)), now_(std::move(now)),
      base_(std::move(baseUrl)) {
    if (refreshToken_.empty()) throw std::runtime_error("Eloverblik: no token configured");
}

std::string EloverblikClient::accessToken(bool renew) {
    if (!renew && fs::exists(tokenFile_)) {
        try {
            const json j = json::parse(std::ifstream(tokenFile_));
            const Instant got{seconds{j.at("obtainedAt").get<long long>()}};
            // Only valid for the refresh token it came from.
            if (j.value("for", "") == refreshToken_.substr(0, 12) && now_() - got < kAccessTokenReuse) {
                return j.at("accessToken").get<std::string>();
            }
        } catch (const std::exception&) {
            // unreadable cache: fetch a new token
        }
    }
    HttpRequest req{"GET", base_ + "api/token", {{"Authorization", "Bearer " + refreshToken_}}, {}, {}};
    const HttpResponse r = http_.send(req);
    if (r.status != 200) throw std::runtime_error("Eloverblik login: " + describe(r));
    const std::string token = json::parse(r.body).at("result").get<std::string>();
    fs::create_directories(tokenFile_.parent_path());
    std::ofstream(tokenFile_) << json{{"accessToken", token},
                                      {"obtainedAt", now_().time_since_epoch().count()},
                                      {"for", refreshToken_.substr(0, 12)}}
                                     .dump();
    return token;
}

HttpResponse EloverblikClient::call(HttpRequest req) {
    req.headers.push_back({"Authorization", "Bearer " + accessToken(false)});
    HttpResponse r = http_.send(req);
    if (r.status == 401) {  // cached access token expired early: renew once
        req.headers.back().second = "Bearer " + accessToken(true);
        r = http_.send(req);
    }
    if (r.status != 200) throw std::runtime_error("Eloverblik: " + describe(r));
    return r;
}

std::vector<MeteringPoint> EloverblikClient::meteringPoints() {
    const json j = json::parse(call({"GET", base_ + "api/meteringpoints/meteringpoints?includeAll=false", {}, {}, {}}).body);
    std::vector<MeteringPoint> out;
    for (const auto& p : j.value("result", json::array())) {
        const auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : ""; };
        std::string address = str("streetName");
        if (!str("buildingNumber").empty()) address += " " + str("buildingNumber");
        if (!str("postcode").empty()) address += ", " + str("postcode") + " " + str("cityName");
        out.push_back({str("meteringPointId"), str("typeOfMP"), address, str("balanceSupplierName")});
    }
    return out;
}

std::vector<MeterReading> EloverblikClient::readings(const std::string& meteringPoint, const std::string& from,
                                                     const std::string& toExclusive) {
    const json body{{"meteringPoints", {{"meteringPoint", json::array({meteringPoint})}}}};
    const HttpRequest req{"POST", base_ + std::format("api/meterdata/gettimeseries/{}/{}/Actual", from, toExclusive),
                          {}, body.dump(), "application/json"};
    return parseEloverblikTimeSeries(json::parse(call(req).body));
}

std::vector<MeterReading> parseEloverblikTimeSeries(const json& reply) {
    std::vector<MeterReading> out;
    for (const auto& r : reply.value("result", json::array())) {
        if (r.contains("success") && r["success"].is_boolean() && !r["success"].get<bool>()) {
            throw std::runtime_error("Eloverblik: " + r.value("errorText", std::string("request failed")));
        }
        if (!r.contains("MyEnergyData_MarketDocument")) continue;
        for (const auto& ts : r["MyEnergyData_MarketDocument"].value("TimeSeries", json::array())) {
            for (const auto& period : ts.value("Period", json::array())) {
                const auto start = parseUtc(period.at("timeInterval").value("start", ""));
                if (!start) continue;
                const minutes res = parseResolution(period.value("resolution", "PT1H"));
                for (const auto& p : period.value("Point", json::array())) {
                    const int pos = std::stoi(p.value("position", "1"));
                    const std::string q = p.value("out_Quantity.quantity", "");
                    if (q.empty()) continue;
                    out.push_back({*start + res * (pos - 1), res, std::stod(q)});
                }
            }
        }
    }
    std::ranges::sort(out, {}, &MeterReading::start);
    return out;
}

}  // namespace hb
