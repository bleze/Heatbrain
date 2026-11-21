#include "core/cost.hpp"

#include <algorithm>

namespace hb {

using namespace std::chrono;

std::vector<DayCost> dailyCost(const std::vector<DecisionPoint>& decisions, const std::map<Instant, double>& prices,
                               double watts, seconds maxGap, Instant from, Instant to, const TimeZone& tz) {
    std::map<std::string, DayCost> days;
    const double kW = watts / 1000.0;

    for (std::size_t i = 0; i < decisions.size(); ++i) {
        const auto& d = decisions[i];
        if (!d.on) continue;
        Instant end = i + 1 < decisions.size() ? decisions[i + 1].at : to;
        end = std::min({end, d.at + maxGap, to});
        Instant t = std::max(d.at, from);

        // Walk the interval slot by slot: each 15-minute slot has its own price.
        while (t < end) {
            const Instant slot = floorToSlot(t);
            const Instant segEnd = std::min(end, slot + kSlotLength);
            const double hoursOn = duration<double, std::ratio<3600>>(segEnd - t).count();
            const double kWh = kW * hoursOn;

            DayCost& day = days[formatLocalDate(tz.toLocal(slot))];
            day.kWh += kWh;
            day.onHours += hoursOn;
            if (d.live) day.liveKWh += kWh;
            if (auto p = prices.find(slot); p != prices.end()) day.cost += kWh * p->second;
            else day.kWhWithoutPrice += kWh;
            t = segEnd;
        }
    }

    std::vector<DayCost> out;
    for (auto& [date, day] : days) {
        day.date = date;
        out.push_back(day);
    }
    return out;  // std::map keeps dates in order
}

std::map<std::string, double> dailyAveragePrice(const std::map<Instant, double>& prices, const TimeZone& tz) {
    std::map<std::string, std::pair<double, int>> acc;
    for (const auto& [slot, price] : prices) {
        auto& a = acc[formatLocalDate(tz.toLocal(slot))];
        a.first += price;
        a.second += 1;
    }
    std::map<std::string, double> out;
    for (const auto& [date, a] : acc) out[date] = a.first / a.second;
    return out;
}

}  // namespace hb
