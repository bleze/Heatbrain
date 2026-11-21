#include "providers/price_model.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

#include "core/log.hpp"
#include "providers/basic_components.hpp"
#include "providers/energidataservice.hpp"

namespace hb {

const std::map<std::string, PriceComponentFactory>& priceComponentRegistry() {
    static const std::map<std::string, PriceComponentFactory> registry{
        {"fixed", makeFixedComponent},
        {"schedule", makeScheduleComponent},
        {"energidataservice.spot", eds::makeSpotComponent},
        {"energidataservice.tariff", eds::makeDatahubTariffComponent},
    };
    return registry;
}

PriceModel::PriceModel(const nlohmann::json& cfg, ProviderContext& ctx) : currency_(ctx.currency) {
    const auto& registry = priceComponentRegistry();
    for (const auto& c : cfg.at("components")) {
        const std::string type = c.at("type").get<std::string>();
        const auto it = registry.find(type);
        if (it == registry.end()) {
            std::string known;
            for (const auto& [name, _] : registry) known += (known.empty() ? "" : ", ") + name;
            throw std::runtime_error("Unknown price component type '" + type + "'. Known: " + known);
        }
        components_.push_back(it->second(c, ctx));
    }
    if (cfg.contains("surcharges")) {
        for (const auto& s : cfg.at("surcharges")) {
            surcharges_.push_back({s.at("name").get<std::string>(), s.at("percent").get<double>()});
        }
    }
}

PriceTable PriceModel::build(Instant from, Instant to) {
    PriceTable table;
    table.currency = currency_;
    table.surcharges = surcharges_;
    const auto slots = slotsBetween(from, to);

    std::vector<std::vector<std::optional<double>>> columns;
    for (const auto& component : components_) {
        table.componentNames.push_back(component->name());
        try {
            columns.push_back(component->values(slots));
        } catch (const std::exception& e) {
            log::error("{}: {}", component->name(), e.what());
            table.errors.push_back(component->name() + ": " + e.what());
            columns.emplace_back(slots.size());
        }
    }

    table.slots.reserve(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        PriceSlot slot{slots[i], {}, std::nullopt};
        for (const auto& col : columns) slot.parts.push_back(col[i]);
        slot.total = computeTotal(slot.parts, surcharges_);
        table.slots.push_back(std::move(slot));
    }
    return table;
}

}  // namespace hb
