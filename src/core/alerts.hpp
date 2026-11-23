#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/room.hpp"
#include "core/time.hpp"

namespace hb {

enum class Severity { Info, Warning, Critical };
std::string_view severityName(Severity s);

// Something wrong right now. Reported every tick while it lasts; the key identifies it
// across ticks ("plug:bedroom:unreachable").
struct Problem {
    std::string key;
    Severity severity = Severity::Warning;
    std::string message;
    std::chrono::minutes debounce{0};       // must persist this long before alerting
    std::optional<std::chrono::minutes> repeat;  // reminder interval (default: policy)
};

struct Alert {
    enum class Kind { Raised, Reminder, Resolved };
    Kind kind = Kind::Raised;
    std::string key;
    Severity severity = Severity::Warning;
    std::string message;
    Instant since;  // when the problem was first seen
};

struct AlertPolicy {
    std::chrono::minutes repeat{6 * 60};
    std::optional<DayWindow> quietHours;  // non-critical alerts wait until it ends
};

// Turns per-tick problem lists into a sparse stream of alerts.
class AlertManager {
public:
    using Deliver = std::function<bool(const Alert&)>;  // false = not delivered, retry later

    AlertManager(AlertPolicy policy, TimeZone tz) : policy_(std::move(policy)), tz_(std::move(tz)) {}

    // After a configuration reload: new policy/time zone, same memory of what was sent.
    void reconfigure(AlertPolicy policy, TimeZone tz) {
        policy_ = std::move(policy);
        tz_ = std::move(tz);
    }

    // Returns the alerts that were delivered.
    std::vector<Alert> update(Instant now, const std::vector<Problem>& active, const Deliver& deliver);

    // For the web UI banner: like notifications, a problem shows once it has lasted its debounce,
    // so a single failed request (Netatmo answering 503 for a moment) does not flash up.
    std::vector<Problem> activeProblems(Instant now) const;

    // Inside the quiet hours (non-critical alerts wait)?
    bool quietAt(Instant now) const { return quiet(now); }

private:
    struct State {
        Problem problem;
        Instant firstSeen;
        std::optional<Instant> notifiedAt;
    };
    bool quiet(Instant now) const;

    AlertPolicy policy_;
    TimeZone tz_;
    std::map<std::string, State> states_;
};

}  // namespace hb
