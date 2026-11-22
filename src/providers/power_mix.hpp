#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/time.hpp"

namespace hb {

// Where the electricity in the price area came from, per hour (average MW), from Energinet's
// open data (Energi Data Service): wind, solar, the power plants (central and local, fuels not
// split in real time) and net imports; plus the CO2 intensity of the electricity used.
struct MixHour {
    Instant hour;
    double offshoreWind = 0, onshoreWind = 0, solar = 0, plants = 0, imports = 0;  // MW
    std::optional<double> co2;  // g/kWh
};

std::string prodexUrl(const std::string& area);  // ElectricityProdex5MinRealtime, the last 8 days
std::string co2Url(const std::string& area);     // CO2Emis, the last 8 days
// From the two answers (5-minute records), averaged per hour, sorted.
std::vector<MixHour> parsePowerMix(const nlohmann::json& prodex, const nlohmann::json& co2);

}  // namespace hb
