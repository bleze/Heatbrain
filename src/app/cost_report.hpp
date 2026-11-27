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

// Hours each room's radiator was on, per local day, over the last `days` days (0: everything
// stored), with the outdoor sensor's daily temperatures to compare. Finished days are summed up once
// and kept in the history (room_days, never pruned); today is worked out on each call. Each day also
// says how long heatbrain ran for the room, so a day without data is not read as a day without heat.
nlohmann::json heatingDaysReport(const nlohmann::json& cfg, History& history, Instant now, int days,
                                 const std::string& outdoorKey, const std::string& outdoorName);

}  // namespace hb
