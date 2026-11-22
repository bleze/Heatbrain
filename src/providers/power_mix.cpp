#include "providers/power_mix.hpp"

#include <algorithm>
#include <map>

#include "net/http.hpp"

namespace hb {

using json = nlohmann::json;
using namespace std::chrono;

namespace {

constexpr const char* kBase = "https://api.energidataservice.dk/dataset/";

std::string filter(const std::string& area) { return urlEncode(R"({"PriceArea":[")" + area + R"("]})"); }

// "2026-11-22T20:05:00" (UTC) -> the hour it belongs to.
std::optional<Instant> hourOf(const json& rec, const char* key) {
    if (!rec.contains(key) || !rec[key].is_string()) return std::nullopt;
    const std::string s = rec[key].get<std::string>();
    if (s.size() < 13) return std::nullopt;
    try {
        const int y = std::stoi(s.substr(0, 4)), mo = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2)),
                  h = std::stoi(s.substr(11, 2));
        return Instant{sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h}};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

double num(const json& rec, const char* key) {
    return rec.contains(key) && rec[key].is_number() ? rec[key].get<double>() : 0.0;
}

}  // namespace

std::string prodexUrl(const std::string& area) {
    return std::string(kBase) + "ElectricityProdex5MinRealtime?start=now-P8D&limit=0&filter=" + filter(area) +
           "&columns=Minutes5UTC,ProductionLt100MW,ProductionGe100MW,OffshoreWindPower,OnshoreWindPower,SolarPower,"
           "ExchangeGreatBelt,ExchangeGermany,ExchangeNetherlands,ExchangeGreatBritain,ExchangeNorway,ExchangeSweden,"
           "BornholmSE4";
}

std::string co2Url(const std::string& area) {
    return std::string(kBase) + "CO2Emis?start=now-P8D&limit=0&filter=" + filter(area) +
           "&columns=Minutes5UTC,CO2Emission";
}

std::vector<MixHour> parsePowerMix(const json& prodex, const json& co2) {
    struct Sum {
        MixHour h;
        int n = 0;
        double co2 = 0;
        int nCo2 = 0;
    };
    std::map<Instant, Sum> by;
    for (const auto& r : prodex.value("records", json::array())) {
        const auto at = hourOf(r, "Minutes5UTC");
        if (!at) continue;
        auto& s = by[*at];
        s.h.hour = *at;
        s.h.offshoreWind += num(r, "OffshoreWindPower");
        s.h.onshoreWind += num(r, "OnshoreWindPower");
        s.h.solar += num(r, "SolarPower");
        s.h.plants += num(r, "ProductionGe100MW") + num(r, "ProductionLt100MW");
        // Positive exchange = flowing into the area; only the net import counts as a source.
        double exchange = 0;
        for (const char* k : {"ExchangeGreatBelt", "ExchangeGermany", "ExchangeNetherlands", "ExchangeGreatBritain",
                              "ExchangeNorway", "ExchangeSweden", "BornholmSE4"}) {
            exchange += num(r, k);
        }
        s.h.imports += std::max(0.0, exchange);
        ++s.n;
    }
    for (const auto& r : co2.value("records", json::array())) {
        const auto at = hourOf(r, "Minutes5UTC");
        if (!at || !r.contains("CO2Emission") || !r["CO2Emission"].is_number()) continue;
        auto& s = by[*at];
        s.h.hour = *at;
        s.co2 += r["CO2Emission"].get<double>();
        ++s.nCo2;
    }
    std::vector<MixHour> out;
    for (auto& [at, s] : by) {
        if (s.n == 0) continue;  // CO2 without production data: not shown
        const double n = s.n;
        MixHour h = s.h;
        h.offshoreWind /= n;
        h.onshoreWind /= n;
        h.solar /= n;
        h.plants /= n;
        h.imports /= n;
        if (s.nCo2) h.co2 = s.co2 / s.nCo2;
        out.push_back(h);
    }
    return out;
}

}  // namespace hb
