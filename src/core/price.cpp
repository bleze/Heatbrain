#include "core/price.hpp"

namespace hb {

std::optional<double> computeTotal(const std::vector<std::optional<double>>& parts,
                                   const std::vector<Surcharge>& surcharges) {
    double sum = 0.0;
    for (const auto& p : parts) {
        if (!p) return std::nullopt;
        sum += *p;
    }
    for (const auto& s : surcharges) sum *= 1.0 + s.percent / 100.0;
    return sum;
}

}  // namespace hb
