#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "providers/price_component.hpp"

namespace hb {

// Registry of price component types available in config ("type" field).
// New data sources (ENTSO-E, Tibber, Nord Pool...) register here.
const std::map<std::string, PriceComponentFactory>& priceComponentRegistry();

// The full electricity price: additive components followed by percentage surcharges.
class PriceModel {
public:
    // cfg is the "price" object of the config file.
    PriceModel(const nlohmann::json& cfg, ProviderContext& ctx);

    PriceTable build(Instant from, Instant to);

private:
    std::string currency_;
    std::vector<std::unique_ptr<PriceComponent>> components_;
    std::vector<Surcharge> surcharges_;
};

}  // namespace hb
