#pragma once

#include <nlohmann/json.hpp>

#include "core/time.hpp"
#include "storage/history.hpp"

namespace hb {

// Energy and estimated cost per room and local day over the last `days` days, from the
// decision log, stored prices and each plug's wattage. "saved" compares with paying each
// day's average price for the same energy. Rooms with plugs lacking a wattage are listed
// with "missingWatts" and no totals.
nlohmann::json costReport(const nlohmann::json& cfg, const History& history, Instant now, int days);

}  // namespace hb
