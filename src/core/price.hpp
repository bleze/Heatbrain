#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"

namespace hb {

// A percentage applied on top of the running total, e.g. VAT. Applied in order.
struct Surcharge {
    std::string name;
    double percent = 0.0;
};

struct PriceSlot {
    Instant start;
    std::vector<std::optional<double>> parts;  // per component, currency/kWh, before surcharges
    std::optional<double> total;               // nullopt if any part is unknown
};

// Price of electricity per slot, broken down by component.
struct PriceTable {
    std::string currency;
    std::vector<std::string> componentNames;
    std::vector<Surcharge> surcharges;
    std::vector<PriceSlot> slots;
    std::vector<std::string> errors;  // provider problems, for display
};

std::optional<double> computeTotal(const std::vector<std::optional<double>>& parts,
                                   const std::vector<Surcharge>& surcharges);

}  // namespace hb
