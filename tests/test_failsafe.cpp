#include <catch2/catch_test_macros.hpp>

#include "core/failsafe.hpp"

using namespace hb;
using namespace std::chrono;

TEST_CASE("Tasmota PulseTime encoding") {
    CHECK(tasmotaPulseTime(0s) == 0);
    CHECK(tasmotaPulseTime(1s) == 10);       // tenths of a second up to 11.1 s
    CHECK(tasmotaPulseTime(11s) == 110);
    CHECK(tasmotaPulseTime(30s) == 130);     // verified on the bedroom plug
    CHECK(tasmotaPulseTime(20min) == 1300);
    CHECK(tasmotaPulseTime(hours{24}) == 64900);  // clamped to the device maximum
}

TEST_CASE("Failsafe rule text") {
    FailsafeConfig cfg;  // defaults: 30 min watchdog, 10 on / 50 off
    CHECK(tasmotaFailsafeRule(cfg) ==
          "ON System#Boot DO RuleTimer1 1800 ENDON "
          "ON Rules#Timer=1 DO Backlog Power1 On; RuleTimer2 600 ENDON "
          "ON Rules#Timer=2 DO Backlog Power1 Off; RuleTimer1 3000 ENDON");

    cfg.limpOn = 0min;
    CHECK(tasmotaFailsafeRule(cfg) ==
          "ON System#Boot DO RuleTimer1 1800 ENDON ON Rules#Timer=1 DO Power1 Off ENDON");
}

TEST_CASE("Failsafe validation") {
    CHECK_NOTHROW(validate(FailsafeConfig{}));
    CHECK_THROWS(validate(FailsafeConfig{1min, 30min, 0min, 50min}));    // pulse too short
    CHECK_THROWS(validate(FailsafeConfig{20min, 2min, 10min, 50min}));   // watchdog too short
    CHECK_THROWS(validate(FailsafeConfig{20min, 30min, 25min, 50min}));  // limp ON longer than pulse
    CHECK_THROWS(validate(FailsafeConfig{20min, 30min, 10min, 1min}));   // limp OFF too short
    CHECK_NOTHROW(validate(FailsafeConfig{20min, 30min, 0min, 0min}));   // no limp heating
}
