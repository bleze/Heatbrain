#include "core/tariff.hpp"

namespace hb {

std::optional<double> tariffAt(const std::vector<TariffRow>& rows, LocalTime t) {
    const TariffRow* best = nullptr;
    for (const auto& row : rows) {
        if (t < row.validFrom) continue;
        if (row.validTo && t >= *row.validTo) continue;
        if (!best || row.validFrom > best->validFrom) best = &row;
    }
    if (!best) return std::nullopt;
    const auto& price = best->hourly[static_cast<std::size_t>(hourOfDay(t))];
    return price ? price : best->hourly[0];
}

}  // namespace hb
