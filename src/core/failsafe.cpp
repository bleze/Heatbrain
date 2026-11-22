#include "core/failsafe.hpp"

#include <regex>

#include <format>
#include <stdexcept>

namespace hb {

using namespace std::chrono;

void validate(const FailsafeConfig& cfg) {
    if (cfg.pulse < minutes{2} || cfg.pulse > hours{18}) {
        throw std::invalid_argument("failsafe.pulseMinutes must be between 2 and 1080");
    }
    if (cfg.watchdog < minutes{5} || cfg.watchdog > hours{24}) {
        throw std::invalid_argument("failsafe.watchdogMinutes must be between 5 and 1440");
    }
    if (cfg.limpOn < minutes{0} || cfg.limpOn > cfg.pulse) {
        throw std::invalid_argument(
            "failsafe.limpOnMinutes must be between 0 and pulseMinutes (PulseTime would cut it short)");
    }
    if (cfg.limpOn > minutes{0} && cfg.limpOff < minutes{5}) {
        throw std::invalid_argument("failsafe.limpOffMinutes must be at least 5");
    }
}

int tasmotaPulseTime(seconds d) {
    if (d <= seconds{0}) return 0;
    if (d <= seconds{11}) return static_cast<int>(d.count() * 10);
    const auto v = d.count() + 100;
    return static_cast<int>(v > 64900 ? 64900 : v);
}

bool isHeatbrainFailsafeRule(const std::string& rules) {
    static const std::regex ours(
        R"(^ON System#Boot DO RuleTimer1 \d+ ENDON ON Rules#Timer=1 DO (Power1 Off|Backlog Power1 On; RuleTimer2 \d+) ENDON)"
        R"(( ON Rules#Timer=2 DO Backlog Power1 Off; RuleTimer1 \d+ ENDON)?$)");
    return std::regex_match(rules, ours);
}

std::string tasmotaFailsafeRule(const FailsafeConfig& cfg) {
    const auto s = [](minutes m) { return duration_cast<seconds>(m).count(); };
    if (cfg.limpOn == minutes{0}) {
        return std::format(
            "ON System#Boot DO RuleTimer1 {0} ENDON "
            "ON Rules#Timer=1 DO Power1 Off ENDON",
            s(cfg.watchdog));
    }
    return std::format(
        "ON System#Boot DO RuleTimer1 {0} ENDON "
        "ON Rules#Timer=1 DO Backlog Power1 On; RuleTimer2 {1} ENDON "
        "ON Rules#Timer=2 DO Backlog Power1 Off; RuleTimer1 {2} ENDON",
        s(cfg.watchdog), s(cfg.limpOn), s(cfg.limpOff));
}

}  // namespace hb
