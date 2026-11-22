#include "core/alerts.hpp"

#include <set>

namespace hb {

std::string_view severityName(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Critical: return "critical";
    }
    return "?";
}

bool AlertManager::quiet(Instant now) const {
    return policy_.quietHours && policy_.quietHours->contains(minuteOfDay(tz_.toLocal(now)));
}

std::vector<Alert> AlertManager::update(Instant now, const std::vector<Problem>& active, const Deliver& deliver) {
    std::vector<Alert> sent;
    std::set<std::string> seen;

    for (const auto& p : active) {
        if (!seen.insert(p.key).second) continue;  // duplicates within a tick
        auto [it, inserted] = states_.try_emplace(p.key, State{p, now, std::nullopt});
        State& st = it->second;
        st.problem = p;  // keep the latest message
        if (quiet(now) && p.severity != Severity::Critical) continue;

        std::optional<Alert::Kind> kind;
        if (!st.notifiedAt) {
            if (now - st.firstSeen >= p.debounce) kind = Alert::Kind::Raised;
        } else if (now - *st.notifiedAt >= p.repeat.value_or(policy_.repeat)) {
            kind = Alert::Kind::Reminder;
        }
        if (!kind) continue;
        const Alert a{*kind, p.key, p.severity, p.message, st.firstSeen};
        if (deliver(a)) {
            st.notifiedAt = now;
            sent.push_back(a);
        }
    }

    // Problems that went away: tell the user only if they were told about the problem.
    for (auto it = states_.begin(); it != states_.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second.notifiedAt) {
            const Alert a{Alert::Kind::Resolved, it->first, it->second.problem.severity, it->second.problem.message,
                          it->second.firstSeen};
            if (deliver(a)) sent.push_back(a);  // a lost "resolved" is not retried
        }
        it = states_.erase(it);
    }
    return sent;
}

std::vector<Problem> AlertManager::activeProblems(Instant now) const {
    std::vector<Problem> out;
    for (const auto& [key, st] : states_) {
        if (now - st.firstSeen >= st.problem.debounce) out.push_back(st.problem);
    }
    return out;
}

}  // namespace hb
