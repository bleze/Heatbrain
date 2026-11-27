#include "providers/price_model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>

#include "core/log.hpp"
#include "providers/basic_components.hpp"
#include "providers/energidataservice.hpp"

namespace hb {

namespace {

// A part whose amount changes on given dates ("changes": [{"from": "2028-01-01", "amount": 0.727}]):
// each version is the part's own config with the change's fields on top, in effect from local midnight
// of its date until the next one. Before the first change, the part as written.
class DatedComponent final : public PriceComponent {
public:
    DatedComponent(std::string name, std::vector<std::pair<LocalTime, std::unique_ptr<PriceComponent>>> versions,
                   const TimeZone& tz)
        : PriceComponent(std::move(name)), versions_(std::move(versions)), tz_(tz) {}

    std::vector<std::optional<double>> values(const std::vector<Instant>& slots) override {
        std::vector<std::optional<double>> out(slots.size());
        for (std::size_t v = 0; v < versions_.size(); ++v) {
            std::vector<Instant> mine;
            std::vector<std::size_t> where;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                const LocalTime local = tz_.toLocal(slots[i]);
                if (local >= versions_[v].first && (v + 1 == versions_.size() || local < versions_[v + 1].first)) {
                    mine.push_back(slots[i]);
                    where.push_back(i);
                }
            }
            if (mine.empty()) continue;
            const auto vals = versions_[v].second->values(mine);
            for (std::size_t k = 0; k < where.size(); ++k) out[where[k]] = vals[k];
        }
        return out;
    }

private:
    std::vector<std::pair<LocalTime, std::unique_ptr<PriceComponent>>> versions_;
    const TimeZone& tz_;
};

std::unique_ptr<PriceComponent> makeDated(const nlohmann::json& c, const PriceComponentFactory& factory,
                                          ProviderContext& ctx) {
    nlohmann::json base = c;
    base.erase("changes");
    std::vector<std::pair<LocalTime, std::unique_ptr<PriceComponent>>> versions;
    versions.emplace_back(LocalTime::min(), factory(base, ctx));
    std::vector<std::pair<LocalTime, nlohmann::json>> changes;
    for (const auto& ch : c.at("changes")) {
        const std::string date = ch.at("from").get<std::string>();
        const auto from = parseIsoLocal(date + "T00:00:00");
        if (!from || date.size() != 10) {
            throw std::runtime_error(std::format("{}: change date '{}' must look like 2028-01-01", base.value("name", ""), date));
        }
        changes.emplace_back(*from, ch);
    }
    std::ranges::sort(changes, {}, &std::pair<LocalTime, nlohmann::json>::first);
    nlohmann::json current = base;
    for (const auto& [from, ch] : changes) {
        if (versions.back().first == from) {
            throw std::runtime_error(std::format("{}: two changes on {}", base.value("name", ""), formatLocalDate(from)));
        }
        for (const auto& [key, value] : ch.items()) {
            if (key != "from") current[key] = value;
        }
        versions.emplace_back(from, factory(current, ctx));
    }
    return std::make_unique<DatedComponent>(base.value("name", ""), std::move(versions), ctx.tz);
}

}  // namespace

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
        const bool dated = c.contains("changes") && c["changes"].is_array() && !c["changes"].empty();
        components_.push_back(dated ? makeDated(c, it->second, ctx) : it->second(c, ctx));
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
