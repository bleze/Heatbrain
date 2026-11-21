#pragma once

#include "plugs/switch.hpp"

namespace hb {

// In-memory plug for development and tests: type "simulated" in config.
class SimulatedSwitch final : public Switch {
public:
    explicit SimulatedSwitch(std::string name) : name_(std::move(name)) {}

    PlugStatus status() override {
        return {true, on_, name_, "simulated", "simulated", 100, "", ""};
    }
    bool set(bool on) override {
        on_ = on;
        return on_;
    }
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
};

}  // namespace hb
