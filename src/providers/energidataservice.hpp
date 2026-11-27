#pragma once

// Energi Data Service (Energinet, Denmark): https://www.energidataservice.dk
// Free, no authentication, but strictly rate limited -> always go through the cache.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/tariff.hpp"
#include "providers/price_component.hpp"

namespace hb::eds {

inline constexpr const char* kDefaultBaseUrl = "https://api.energidataservice.dk";

// Parses a DayAheadPrices response. Returns currency/kWh keyed by slot start (UTC).
// currency: "DKK" or "EUR".
std::map<Instant, double> parseDayAheadPrices(std::string_view json, std::string_view currency);

// Parses a DatahubPricelist response into tariff rows grouped by ChargeTypeCode.
std::map<std::string, std::vector<TariffRow>> parseDatahubPricelist(std::string_view json);

// For choosing a grid tariff in Settings: the companies with energy tariffs (D03) in the price list,
// and one company's charge codes with what each costs now.
struct GridCompany {
    std::string gln;
    std::string name;
};
struct ChargeInfo {
    std::string code;
    std::string name;         // "Nettarif C"
    std::string description;  // "Tarif, hvor aftagepunktet typisk er i 0,4 kV-nettet"
    std::string validFrom;    // of the row in effect at `now` (else the latest)
    std::optional<double> low, high;  // its lowest and highest hourly price, per kWh excl. VAT
};
std::vector<GridCompany> parseGridCompanies(std::string_view json);
std::vector<ChargeInfo> parseCharges(std::string_view json, LocalTime now);
// The request URLs (stable for a year, so the cache works): price list rows since the start of last year.
std::string companiesUrl(LocalTime now, const std::string& baseUrl = kDefaultBaseUrl);
std::string chargesUrl(const std::string& gln, LocalTime now, const std::string& baseUrl = kDefaultBaseUrl);

// type "energidataservice.spot": day-ahead spot price for a bidding zone (DK1/DK2).
class SpotComponent final : public PriceComponent {
public:
    SpotComponent(std::string name, std::string area, std::string baseUrl, ProviderContext& ctx);
    std::vector<std::optional<double>> values(const std::vector<Instant>& slots) override;

private:
    std::string area_;
    std::string baseUrl_;
    ProviderContext& ctx_;
};

// type "energidataservice.tariff": grid tariffs from the Datahub price list, identified by
// the grid company's GLN number and one or more charge type codes (summed).
class DatahubTariffComponent final : public PriceComponent {
public:
    DatahubTariffComponent(std::string name, std::string gln, std::vector<std::string> codes,
                           std::string chargeType, std::string baseUrl, ProviderContext& ctx);
    std::vector<std::optional<double>> values(const std::vector<Instant>& slots) override;

private:
    std::string gln_;
    std::vector<std::string> codes_;
    std::string chargeType_;
    std::string baseUrl_;
    ProviderContext& ctx_;
};

std::unique_ptr<PriceComponent> makeSpotComponent(const nlohmann::json& cfg, ProviderContext& ctx);
std::unique_ptr<PriceComponent> makeDatahubTariffComponent(const nlohmann::json& cfg,
                                                           ProviderContext& ctx);

}  // namespace hb::eds
