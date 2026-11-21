#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/time.hpp"

namespace hb {

// One published tariff period, e.g. a row of the Danish Datahub price list.
// Validity is expressed in local wall-clock time; hourly[h] applies to local hour h.
// Daily-resolution tariffs only set hourly[0], which then applies to all hours.
struct TariffRow {
    LocalTime validFrom;
    std::optional<LocalTime> validTo;  // exclusive; nullopt = open-ended
    std::array<std::optional<double>, 24> hourly{};
};

// Tariff in effect at local time t, or nullopt if no row covers t.
// When rows overlap, the one with the latest validFrom wins.
std::optional<double> tariffAt(const std::vector<TariffRow>& rows, LocalTime t);

}  // namespace hb
