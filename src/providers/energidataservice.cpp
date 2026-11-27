#include "providers/energidataservice.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

#include "core/log.hpp"

namespace hb::eds {

using namespace std::chrono;
using nlohmann::json;

namespace {

json parseEnvelope(std::string_view text) {
    json j = json::parse(text);
    // Errors (e.g. rate limiting) come back as {"statusCode": 429, "message": "..."}.
    if (j.contains("statusCode") && !j.contains("records")) {
        throw std::runtime_error("Energi Data Service: " + j.value("message", std::string("error")));
    }
    return j;
}

std::string upper(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

}  // namespace

std::map<Instant, double> parseDayAheadPrices(std::string_view text, std::string_view currency) {
    const json j = parseEnvelope(text);
    const std::string column = "DayAheadPrice" + upper(std::string(currency));
    std::map<Instant, double> out;
    for (const auto& rec : j.at("records")) {
        const auto t = parseIsoUtc(rec.at("TimeUTC").get<std::string>());
        const auto it = rec.find(column);
        if (!t || it == rec.end() || it->is_null()) continue;
        out[*t] = it->get<double>() / 1000.0;  // per MWh -> per kWh
    }
    return out;
}

std::map<std::string, std::vector<TariffRow>> parseDatahubPricelist(std::string_view text) {
    const json j = parseEnvelope(text);
    std::map<std::string, std::vector<TariffRow>> out;
    for (const auto& rec : j.at("records")) {
        TariffRow row;
        const auto from = parseIsoLocal(rec.at("ValidFrom").get<std::string>());
        if (!from) continue;
        row.validFrom = *from;
        if (rec.contains("ValidTo") && rec["ValidTo"].is_string()) {
            row.validTo = parseIsoLocal(rec["ValidTo"].get<std::string>());
        }
        for (std::size_t h = 0; h < 24; ++h) {
            const auto it = rec.find(std::format("Price{}", h + 1));
            if (it != rec.end() && it->is_number()) row.hourly[h] = it->get<double>();
        }
        out[rec.at("ChargeTypeCode").get<std::string>()].push_back(row);
    }
    return out;
}

namespace {
std::string sinceLastYear(LocalTime now) {
    const year_month_day ymd{floor<days>(now)};
    return std::format("{}-01-01", static_cast<int>(ymd.year()) - 1);
}
}  // namespace

std::string companiesUrl(LocalTime now, const std::string& baseUrl) {
    return std::format("{}/dataset/DatahubPricelist?columns=ChargeOwner,GLN_Number&filter={}&start={}&limit=0", baseUrl,
                       urlEncode(json{{"ChargeType", {"D03"}}}.dump()), sinceLastYear(now));
}

std::string chargesUrl(const std::string& gln, LocalTime now, const std::string& baseUrl) {
    std::string columns = "ChargeTypeCode,Note,Description,ValidFrom,ValidTo";
    for (int h = 1; h <= 24; ++h) columns += std::format(",Price{}", h);
    return std::format("{}/dataset/DatahubPricelist?columns={}&filter={}&start={}&limit=0", baseUrl, columns,
                       urlEncode(json{{"GLN_Number", {gln}}, {"ChargeType", {"D03"}}}.dump()), sinceLastYear(now));
}

std::vector<GridCompany> parseGridCompanies(std::string_view text) {
    const json j = parseEnvelope(text);
    std::map<std::string, std::string> byGln;
    for (const auto& rec : j.at("records")) {
        const std::string gln = rec.value("GLN_Number", std::string());
        if (gln.empty()) continue;
        std::string name = rec.contains("ChargeOwner") && rec["ChargeOwner"].is_string() ? rec["ChargeOwner"].get<std::string>() : "";
        if (name.empty() && gln == "5790000432752") name = "Energinet (transmission, system tariff, elafgift)";
        auto& n = byGln[gln];
        if (n.empty()) n = name;
    }
    std::vector<GridCompany> out;
    for (const auto& [gln, name] : byGln) out.push_back({gln, name.empty() ? "GLN " + gln : name});
    // By name; those the list gives no name for last.
    std::ranges::sort(out, [](const GridCompany& a, const GridCompany& b) {
        const bool an = a.name.starts_with("GLN "), bn = b.name.starts_with("GLN ");
        return an != bn ? bn : a.name < b.name;
    });
    return out;
}

std::vector<ChargeInfo> parseCharges(std::string_view text, LocalTime now) {
    const json j = parseEnvelope(text);
    struct Best {
        const json* rec = nullptr;
        LocalTime from{};
        bool current = false;
    };
    std::map<std::string, Best> best;
    for (const auto& rec : j.at("records")) {
        const auto from = parseIsoLocal(rec.value("ValidFrom", std::string()));
        if (!from) continue;
        std::optional<LocalTime> to;
        if (rec.contains("ValidTo") && rec["ValidTo"].is_string()) to = parseIsoLocal(rec["ValidTo"].get<std::string>());
        const bool current = *from <= now && (!to || now < *to);
        auto& b = best[rec.value("ChargeTypeCode", std::string())];
        // The row in effect now wins; else the latest one.
        if (!b.rec || (current && !b.current) || (current == b.current && *from > b.from)) b = {&rec, *from, current};
    }
    std::vector<ChargeInfo> out;
    for (const auto& [code, b] : best) {
        if (code.empty()) continue;
        const json& rec = *b.rec;
        ChargeInfo c;
        c.code = code;
        c.name = rec.contains("Note") && rec["Note"].is_string() ? rec["Note"].get<std::string>() : code;
        c.description = rec.contains("Description") && rec["Description"].is_string() ? rec["Description"].get<std::string>() : "";
        c.validFrom = rec.value("ValidFrom", std::string()).substr(0, 10);
        for (int h = 1; h <= 24; ++h) {
            const auto it = rec.find(std::format("Price{}", h));
            if (it == rec.end() || !it->is_number()) continue;
            const double v = it->get<double>();
            c.low = c.low ? std::min(*c.low, v) : v;
            c.high = c.high ? std::max(*c.high, v) : v;
        }
        out.push_back(std::move(c));
    }
    return out;
}

// ---------------------------------------------------------------------------------------

SpotComponent::SpotComponent(std::string name, std::string area, std::string baseUrl,
                             ProviderContext& ctx)
    : PriceComponent(std::move(name)), area_(std::move(area)), baseUrl_(std::move(baseUrl)), ctx_(ctx) {}

std::vector<std::optional<double>> SpotComponent::values(const std::vector<Instant>& slots) {
    std::vector<std::optional<double>> out(slots.size());
    if (slots.empty()) return out;

    // Query whole local days, padded by one day on each side, so the request URL (and thus
    // the cache entry) is stable over the day regardless of time zone interpretation.
    const auto firstDay = floor<days>(ctx_.tz.toLocal(slots.front()));
    const auto lastDay = floor<days>(ctx_.tz.toLocal(slots.back()));
    const std::string currency = upper(ctx_.currency);
    const std::string url = std::format(
        "{}/dataset/DayAheadPrices?start={}&end={}&filter={}&columns=TimeUTC,DayAheadPrice{}&limit=2000",
        baseUrl_, formatLocalDate(firstDay - days{1}), formatLocalDate(lastDay + days{2}),
        urlEncode(json{{"PriceArea", {area_}}}.dump()), currency);

    auto prices = parseDayAheadPrices(ctx_.http.get(url, hours{6}).body, currency);

    // Tomorrow's prices are published around 13:00 Danish time. If they're missing and
    // should exist by now, refetch - but at most every 15 minutes (rate limit).
    const bool covered = !prices.empty() && prices.rbegin()->first >= slots.back();
    const auto nowLocal = ctx_.tz.toLocal(ctx_.now());
    const bool shouldExist = lastDay <= floor<days>(nowLocal) || hourOfDay(nowLocal) >= 13;
    if (!covered && shouldExist) {
        prices = parseDayAheadPrices(ctx_.http.get(url, minutes{15}).body, currency);
    }

    // Resolution: 15 min today, but hourly in older data. Infer from the data.
    seconds resolution = duration_cast<seconds>(kSlotLength);
    if (prices.size() >= 2) {
        const seconds gap = std::next(prices.begin())->first - prices.begin()->first;
        resolution = std::clamp(gap, resolution, duration_cast<seconds>(hours{1}));
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        auto it = prices.upper_bound(slots[i]);
        if (it == prices.begin()) continue;
        --it;
        if (slots[i] - it->first < resolution) out[i] = it->second;
    }
    return out;
}

DatahubTariffComponent::DatahubTariffComponent(std::string name, std::string gln,
                                               std::vector<std::string> codes, std::string chargeType,
                                               std::string baseUrl, ProviderContext& ctx)
    : PriceComponent(std::move(name)),
      gln_(std::move(gln)),
      codes_(std::move(codes)),
      chargeType_(std::move(chargeType)),
      baseUrl_(std::move(baseUrl)),
      ctx_(ctx) {}

std::vector<std::optional<double>> DatahubTariffComponent::values(const std::vector<Instant>& slots) {
    json filter{{"GLN_Number", {gln_}}, {"ChargeTypeCode", codes_}};
    if (!chargeType_.empty()) filter["ChargeType"] = {chargeType_};
    const std::string url =
        std::format("{}/dataset/DatahubPricelist?filter={}&sort={}&limit={}", baseUrl_,
                    urlEncode(filter.dump()), urlEncode("ValidFrom desc"), 50 * codes_.size());

    // Tariffs are published weeks or months ahead; refreshing twice a day is plenty.
    const auto rowsByCode = parseDatahubPricelist(ctx_.http.get(url, hours{12}).body);
    for (const auto& code : codes_) {
        if (!rowsByCode.contains(code)) {
            log::warn("{}: no tariff rows for GLN {} code {}", name(), gln_, code);
        }
    }

    std::vector<std::optional<double>> out(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const LocalTime local = ctx_.tz.toLocal(slots[i]);
        double sum = 0.0;
        bool complete = true;
        for (const auto& code : codes_) {
            const auto it = rowsByCode.find(code);
            const auto v = it == rowsByCode.end() ? std::nullopt : tariffAt(it->second, local);
            if (!v) {
                complete = false;
                break;
            }
            sum += *v;
        }
        if (complete) out[i] = sum;
    }
    return out;
}

std::unique_ptr<PriceComponent> makeSpotComponent(const json& cfg, ProviderContext& ctx) {
    return std::make_unique<SpotComponent>(cfg.value("name", std::string("Spot")),
                                           cfg.at("area").get<std::string>(),
                                           cfg.value("baseUrl", std::string(kDefaultBaseUrl)), ctx);
}

std::unique_ptr<PriceComponent> makeDatahubTariffComponent(const json& cfg, ProviderContext& ctx) {
    std::vector<std::string> codes;
    const auto& c = cfg.at("codes");
    if (c.is_string()) {
        codes.push_back(c.get<std::string>());
    } else {
        codes = c.get<std::vector<std::string>>();
    }
    if (codes.empty()) throw std::runtime_error("energidataservice.tariff: 'codes' is empty");
    return std::make_unique<DatahubTariffComponent>(
        cfg.at("name").get<std::string>(), cfg.at("gln").get<std::string>(), std::move(codes),
        cfg.value("chargeType", std::string("D03")), cfg.value("baseUrl", std::string(kDefaultBaseUrl)),
        ctx);
}

}  // namespace hb::eds
