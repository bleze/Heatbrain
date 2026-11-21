#include "providers/basic_components.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace hb {

FixedComponent::FixedComponent(std::string name, double amount)
    : PriceComponent(std::move(name)), amount_(amount) {}

std::vector<std::optional<double>> FixedComponent::values(const std::vector<Instant>& slots) {
    return std::vector<std::optional<double>>(slots.size(), amount_);
}

ScheduleComponent::ScheduleComponent(std::string name, Schedule schedule, const TimeZone& tz)
    : PriceComponent(std::move(name)), schedule_(std::move(schedule)), tz_(tz) {}

std::vector<std::optional<double>> ScheduleComponent::values(const std::vector<Instant>& slots) {
    std::vector<std::optional<double>> out;
    out.reserve(slots.size());
    for (Instant s : slots) out.push_back(schedule_.at(tz_.toLocal(s)));
    return out;
}

namespace {

int weekdayIndex(const std::string& s) {
    static constexpr const char* names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    for (int i = 0; i < 7; ++i) {
        if (s == names[i]) return i + 1;
    }
    throw std::runtime_error("schedule: unknown weekday '" + s + "' (use mon..sun)");
}

int clockOrThrow(const std::string& s) {
    auto m = parseClock(s);
    if (!m) throw std::runtime_error("schedule: invalid time '" + s + "' (use HH:MM)");
    return *m;
}

}  // namespace

Schedule parseSchedule(const nlohmann::json& cfg) {
    Schedule schedule;
    for (const auto& sj : cfg.at("seasons")) {
        ScheduleSeason season;
        if (sj.contains("months")) {
            for (int m : sj.at("months")) {
                if (m < 1 || m > 12) throw std::runtime_error("schedule: month out of range");
                season.months[static_cast<std::size_t>(m)] = true;
            }
        } else {
            season.months.fill(true);
        }
        if (sj.contains("weekdays")) {
            for (const auto& w : sj.at("weekdays")) {
                season.weekdays[static_cast<std::size_t>(weekdayIndex(w.get<std::string>()))] = true;
            }
        } else {
            season.weekdays.fill(true);
        }
        for (const auto& bj : sj.at("bands")) {
            ScheduleBand band;
            band.fromMinute = clockOrThrow(bj.at("from").get<std::string>());
            band.toMinute = clockOrThrow(bj.at("to").get<std::string>());
            band.amount = bj.at("amount").get<double>();
            if (band.toMinute <= band.fromMinute) {
                throw std::runtime_error("schedule: band 'to' must be after 'from'");
            }
            season.bands.push_back(band);
        }
        schedule.seasons.push_back(std::move(season));
    }
    return schedule;
}

std::unique_ptr<PriceComponent> makeFixedComponent(const nlohmann::json& cfg, ProviderContext&) {
    return std::make_unique<FixedComponent>(cfg.at("name").get<std::string>(),
                                            cfg.at("amount").get<double>());
}

std::unique_ptr<PriceComponent> makeScheduleComponent(const nlohmann::json& cfg, ProviderContext& ctx) {
    return std::make_unique<ScheduleComponent>(cfg.at("name").get<std::string>(),
                                               parseSchedule(cfg), ctx.tz);
}

}  // namespace hb
