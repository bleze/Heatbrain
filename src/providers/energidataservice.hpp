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
