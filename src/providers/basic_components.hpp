#pragma once

#include "core/schedule.hpp"
#include "providers/price_component.hpp"

namespace hb {

// type "fixed": a constant amount per kWh (taxes, supplier markup).
class FixedComponent final : public PriceComponent {
public:
    FixedComponent(std::string name, double amount);
    std::vector<std::optional<double>> values(const std::vector<Instant>& slots) override;

private:
    double amount_;
};

// type "schedule": time-of-day bands with seasons, written directly in config.
class ScheduleComponent final : public PriceComponent {
public:
    ScheduleComponent(std::string name, Schedule schedule, const TimeZone& tz);
    std::vector<std::optional<double>> values(const std::vector<Instant>& slots) override;

private:
    Schedule schedule_;
    const TimeZone& tz_;
};

Schedule parseSchedule(const nlohmann::json& cfg);

std::unique_ptr<PriceComponent> makeFixedComponent(const nlohmann::json& cfg, ProviderContext& ctx);
std::unique_ptr<PriceComponent> makeScheduleComponent(const nlohmann::json& cfg, ProviderContext& ctx);

}  // namespace hb
