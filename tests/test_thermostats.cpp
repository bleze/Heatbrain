#include <catch2/catch_test_macros.hpp>

#include "app/thermostats.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;

namespace {

RoomConfig pizzeria() {
    RoomConfig r;
    r.id = "pizzeria";
    r.thermostat = "pizzeria";
    r.frostTemp = 5.0;
    r.minTemp = 15.0;
    r.maxTemp = 24.0;
    return r;
}

Decision decision(bool on, Reason reason, const std::string& mode, double target) {
    Decision d;
    d.on = on;
    d.reason = reason;
    d.inputs.mode = mode;
    d.inputs.target = target;
    return d;
}

DecisionRecord record(Instant at, bool on, std::optional<double> counter, std::optional<bool> state = std::nullopt) {
    json p{{"id", "pizzeria"}, {"thermostat", true}};
    if (counter) p["relayOnSeconds"] = *counter;
    if (state) p["state"] = *state;
    return DecisionRecord{at, "pizzeria", on, false, "X", "", json::object(), json::array({p})};
}

}  // namespace

TEST_CASE("Thermostat setpoint: without the thermostat's own reading, as before") {
    const auto room = pizzeria();
    CHECK(thermostatSetpoint(room, decision(true, Reason::KeepAbove, "keep", 5.0)) == 5.0);
    CHECK(thermostatSetpoint(room, decision(false, Reason::KeepSatisfied, "keep", 5.0)) == 5.0);
}

TEST_CASE("Thermostat setpoint: steered so the relay follows heatbrain's decision") {
    const auto room = pizzeria();
    // Keep above 5: the room sensor says 4.9, the thermostat reads 6.2 - a setpoint of 5 would not heat.
    CHECK(thermostatSetpoint(room, decision(true, Reason::KeepAbove, "keep", 5.0), {6.2, 5.0}) == 7.5);
    // Already steered (7.5 is still above 6.4 + margin): kept, not written again.
    CHECK(thermostatSetpoint(room, decision(true, Reason::KeepAbove, "keep", 5.0), {6.4, 7.5}) == 7.5);
    // Warmer: steered up again.
    CHECK(thermostatSetpoint(room, decision(true, Reason::KeepAbove, "keep", 5.0), {7.2, 7.5}) == 8.5);
    // Off: the target is well below the thermostat's reading - the target, as before.
    CHECK(thermostatSetpoint(room, decision(false, Reason::KeepSatisfied, "keep", 5.0), {7.2, 8.5}) == 5.0);
    // Off in a heat period, but the thermostat reads colder than the room sensor: below its reading.
    CHECK(thermostatSetpoint(room, decision(false, Reason::HoldOff, "heat", 20.0), {19.8, 20.0}) == 18.5);
    // ...never below the minimum (its guard when heatbrain is gone), nor frost protection off a heat period.
    CHECK(thermostatSetpoint(room, decision(false, Reason::HoldOff, "heat", 16.0), {15.6, 16.0}) == 15.0);
    CHECK(thermostatSetpoint(room, decision(false, Reason::KeepSatisfied, "keep", 6.0), {5.7, 6.0}) == 5.0);
    // On: the target is enough when the thermostat reads well below it.
    CHECK(thermostatSetpoint(room, decision(true, Reason::BelowTarget, "heat", 20.0), {18.0, 15.0}) == 20.0);
    // ...and never above the room's maximum.
    CHECK(thermostatSetpoint(room, decision(true, Reason::BelowTarget, "heat", 20.0), {23.8, 20.0}) == 24.0);
}

TEST_CASE("Relay time: decisions say what the thermostat's relay did") {
    const Instant t0 = sys_days{2026y / 11 / 26} + hours{10};
    std::vector<DecisionRecord> ds{
        record(t0, false, 1000),                  // heatbrain: off, but the relay ran 4 of 5 minutes
        record(t0 + minutes{5}, true, 1240),      // heatbrain: on, the relay ran 0
        record(t0 + minutes{10}, true, 1240),     // ran 60 s: less than half
        record(t0 + minutes{15}, false, 1300),    // last: its relay state at the time
    };
    ds[3].plugs[0]["state"] = true;
    useRelayTime(ds);
    CHECK(ds[0].on);
    CHECK_FALSE(ds[1].on);
    CHECK(ds[1].changed);
    CHECK_FALSE(ds[2].on);
    CHECK_FALSE(ds[2].changed);
    CHECK(ds[3].on);
    CHECK(ds[3].changed);
}

TEST_CASE("Relay time: gaps, counter resets and plug rooms") {
    const Instant t0 = sys_days{2026y / 11 / 26} + hours{10};
    std::vector<DecisionRecord> ds{
        record(t0, true, 5000, false),              // next decision 2 h later: its relay state
        record(t0 + hours{2}, true, 100, true),     // counter reset: its relay state
        record(t0 + hours{2} + minutes{5}, false, 400),
    };
    useRelayTime(ds);
    CHECK_FALSE(ds[0].on);
    CHECK(ds[1].on);
    CHECK_FALSE(ds[2].on);  // no counter after it, no state: as decided

    std::vector<DecisionRecord> plug{DecisionRecord{t0, "shed", true, true, "X", "", json::object(),
                                                    json::array({json{{"id", "shed"}, {"state", false}}})}};
    useRelayTime(plug);
    CHECK(plug[0].on);
}
