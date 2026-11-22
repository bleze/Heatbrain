#pragma once

#include "plugs/switch.hpp"

namespace hb {

// In-memory plug for development and tests: type "simulated" in config.
class SimulatedSwitch final : public Switch {
public:
    explicit SimulatedSwitch(std::string name) : name_(std::move(name)) {}

    PlugStatus status() override {
        if (!reachable_) return {false, std::nullopt, name_, "", "", std::nullopt, "", "simulated: unreachable"};
        return {true, on_, name_, "simulated", "simulated", 100, "", ""};
    }
    bool set(bool on) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        on_ = on;
        return on_;
    }
    void armOffline(const std::vector<OnSpan>& spans, Instant now, Instant) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        schedule_ = spans;
        on_ = scheduleOnAt(spans, now);
    }
    void disarmOffline() override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        schedule_.reset();
    }
    // For tests: Wi-Fi off, and what the device was told to follow.
    void setReachable(bool r) { reachable_ = r; }
    const std::optional<std::vector<OnSpan>>& schedule() const { return schedule_; }
    FailsafeStatus checkFailsafe() override {
        FailsafeStatus s;
        if (!provisioned_) s.problems.push_back("Fail-safe not installed (simulated)");
        return s;
    }
    nlohmann::json backup() override { return {{"simulated", true}, {"provisioned", provisioned_}}; }
    void provision(bool) override { provisioned_ = true; }
    void restore(const nlohmann::json& b) override { provisioned_ = b.value("provisioned", false); }

private:
    std::string name_;
    bool on_ = false;
    bool provisioned_ = false;
    bool reachable_ = true;
    std::optional<std::vector<OnSpan>> schedule_;
};

}  // namespace hb
