#include "core/schedule.hpp"

#include <charconv>

namespace hb {

std::optional<double> Schedule::at(LocalTime t) const {
    const unsigned month = monthOf(t);
    const unsigned wd = isoWeekday(t);
    const int minute = minuteOfDay(t);
    for (const auto& season : seasons) {
        if (!season.months[month] || !season.weekdays[wd]) continue;
        for (const auto& band : season.bands) {
            if (minute >= band.fromMinute && minute < band.toMinute) return band.amount;
        }
    }
    return std::nullopt;
}

std::optional<int> parseClock(std::string_view s) {
    if (s.size() != 5 || s[2] != ':') return std::nullopt;
    int h = 0, m = 0;
    auto r1 = std::from_chars(s.data(), s.data() + 2, h);
    auto r2 = std::from_chars(s.data() + 3, s.data() + 5, m);
    if (r1.ec != std::errc{} || r2.ec != std::errc{} || r1.ptr != s.data() + 2 ||
        r2.ptr != s.data() + 5) {
        return std::nullopt;
    }
    if (m > 59 || h > 24 || (h == 24 && m != 0)) return std::nullopt;
    return h * 60 + m;
}

}  // namespace hb
