#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "core/time.hpp"

namespace hb {

// One recorded decision: from `at` until the next decision the radiator was on/off.
struct DecisionPoint {
    Instant at;
    bool on = false;
    bool live = false;  // false: dry-run, i.e. what it would have cost
};

struct DayCost {
    std::string date;           // local date "2026-11-21"
    double kWh = 0;
    double cost = 0;            // currency, incl. surcharges (prices are totals)
    double kWhWithoutPrice = 0; // energy in slots with no known price (not in cost)
    double liveKWh = 0;         // part of kWh while the plug was live
    double onHours = 0;
};

// Energy and cost per local day for a radiator of `watts` that followed `decisions`.
// prices: total price per slot start (15 min). Gaps longer than maxGap between decisions
// (controller not running) are not counted. Only [from, to) is considered.
std::vector<DayCost> dailyCost(const std::vector<DecisionPoint>& decisions, const std::map<Instant, double>& prices,
                               double watts, std::chrono::seconds maxGap, Instant from, Instant to,
                               const TimeZone& tz);

// The same from a heating-time counter (a thermostat's lifetime relay seconds), sampled with each
// decision: the seconds added between two samples are spread evenly over that interval's slots.
// Exact energy, not an estimate from the decisions. Intervals longer than maxGap are skipped (the
// controller was not running: the energy is not lost, it just cannot be placed in time).
struct CounterSample {
    Instant at;
    double seconds = 0;  // counter value
    bool live = false;
};
std::vector<DayCost> dailyCostFromCounter(const std::vector<CounterSample>& samples, const std::map<Instant, double>& prices,
                                          double watts, std::chrono::seconds maxGap, Instant from, Instant to,
                                          const TimeZone& tz);

// Plain average of the slot prices of each local day: the "market" price to compare with.
std::map<std::string, double> dailyAveragePrice(const std::map<Instant, double>& prices, const TimeZone& tz);

}  // namespace hb
