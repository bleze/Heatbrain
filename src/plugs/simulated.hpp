#pragma once

#include "plugs/switch.hpp"

namespace hb {

// In-memory plug for development and tests: type "simulated" in config.
class SimulatedSwitch final : public Switch {
public:
    explicit SimulatedSwitch(std::string name) : name_(std::move(name)) {}

    PlugStatus status() override {
        if (!reachable_) return {false, std::nullopt, name_, "", "", std::nullopt, "", "simulated: unreachable"};
        return {true, on_, name_, firmware_, "simulated", 100, "", ""};
    }
    bool set(bool on) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        on_ = on;
        return on_;
    }
    bool armOffline(const std::vector<OnSpan>& spans, Instant now, Instant,
                    std::optional<OfflinePeriod> radioOff = {}) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        schedule_ = spans;
        on_ = scheduleOnAt(spans, now);
        radioOff_ = radioOff;
        return radioOff.has_value();
    }
    bool armRadioOff(const OfflinePeriod& radioOff, Instant) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        radioOff_ = radioOff;
        return true;
    }
    void disarmOffline() override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        schedule_.reset();
        radioOff_.reset();
    }
    void keepAlive(Instant, std::optional<Instant> until) override {
        if (!reachable_) throw std::runtime_error("simulated: unreachable");
        ++keepAlives_;
        keptUntil_ = until;
    }
    int keepAlives() const { return keepAlives_; }
    const std::optional<Instant>& keptUntil() const { return keptUntil_; }
    // For tests: Wi-Fi off, and what the device was told to follow.
    void setReachable(bool r) { reachable_ = r; }
    void setFirmware(std::string v) { firmware_ = std::move(v); }  // "" = a status reply without it
    const std::optional<std::vector<OnSpan>>& schedule() const { return schedule_; }
    const std::optional<OfflinePeriod>& radioOff() const { return radioOff_; }
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
    std::string firmware_ = "simulated";
    std::optional<std::vector<OnSpan>> schedule_;
    std::optional<OfflinePeriod> radioOff_;
    int keepAlives_ = 0;
    std::optional<Instant> keptUntil_;
};

}  // namespace hb
