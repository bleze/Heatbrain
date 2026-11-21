#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/price.hpp"
#include "core/time.hpp"
#include "net/http.hpp"

namespace hb {

// Shared services handed to providers when they are created from config.
struct ProviderContext {
    CachingHttpClient& http;
    const TimeZone& tz;
    std::string currency;
    std::function<Instant()> now;
};

// One additive part of the electricity price (spot, grid tariff, tax, markup...).
// Implementations are selected by the "type" field in config; see price_model.cpp.
class PriceComponent {
public:
    explicit PriceComponent(std::string name) : name_(std::move(name)) {}
    virtual ~PriceComponent() = default;

    const std::string& name() const { return name_; }

    // Amount in currency/kWh (before surcharges) for each slot start; nullopt where unknown.
    // May throw; the model reports the error and treats all slots as unknown.
    virtual std::vector<std::optional<double>> values(const std::vector<Instant>& slots) = 0;

private:
    std::string name_;
};

using PriceComponentFactory =
    std::function<std::unique_ptr<PriceComponent>(const nlohmann::json& cfg, ProviderContext& ctx)>;

}  // namespace hb
