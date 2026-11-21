#pragma once

#include <chrono>
#include <string>

namespace hb {

// On-plug safety net. Verified against Tasmota 14.6 on a Sonoff Basic R2:
//  - PulseTime: the relay switches itself OFF `pulse` after the last ON command.
//    Re-sending ON restarts the countdown, so heatbrain must refresh ON plugs well within
//    `pulse`. If heatbrain dies, heaters are OFF within `pulse`.
//  - Watchdog: every heatbrain command re-arms RuleTimer1 (`watchdog`). If it expires,
//    a rule runs limp mode: ON for `limpOn`, OFF for `limpOff`, repeat. Bounded heat for
//    frost protection, never runaway heat. Armed at boot too (after a power cut).
struct FailsafeConfig {
    std::chrono::minutes pulse{20};
    std::chrono::minutes watchdog{30};
    std::chrono::minutes limpOn{10};   // 0 = stay OFF when heatbrain is gone
    std::chrono::minutes limpOff{50};
};

// Throws std::invalid_argument describing the problem.
void validate(const FailsafeConfig& cfg);

// Tasmota PulseTime encoding: 1..111 = tenths of a second, 112..64900 = seconds + 100.
int tasmotaPulseTime(std::chrono::seconds d);

// Rule1 text implementing the watchdog + limp mode.
std::string tasmotaFailsafeRule(const FailsafeConfig& cfg);

}  // namespace hb
