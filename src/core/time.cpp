#include "core/time.hpp"

#include <charconv>
#include <format>
#include <stdexcept>

namespace hb {

using namespace std::chrono;

namespace {

bool readInt(std::string_view s, std::size_t pos, std::size_t len, int& out) {
    if (pos + len > s.size()) return false;
    const char* b = s.data() + pos;
    const char* e = b + len;
    auto [p, ec] = std::from_chars(b, e, out);
    return ec == std::errc{} && p == e;
}

}  // namespace

std::optional<LocalTime> parseIsoLocal(std::string_view s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (s.size() < 10 || !readInt(s, 0, 4, y) || s[4] != '-' || !readInt(s, 5, 2, mo) ||
        s[7] != '-' || !readInt(s, 8, 2, d)) {
        return std::nullopt;
    }
    if (s.size() > 10) {
        if (s[10] != 'T' && s[10] != ' ') return std::nullopt;
        if (s.size() < 16 || !readInt(s, 11, 2, h) || s[13] != ':' || !readInt(s, 14, 2, mi)) {
            return std::nullopt;
        }
        if (s.size() > 16 && s[16] == ':' && !readInt(s, 17, 2, sec)) return std::nullopt;
    }
    const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
    if (!ymd.ok() || h > 23 || mi > 59 || sec > 59) return std::nullopt;
    return local_days{ymd} + hours{h} + minutes{mi} + seconds{sec};
}

std::optional<Instant> parseIsoUtc(std::string_view s) {
    auto local = parseIsoLocal(s);
    if (!local) return std::nullopt;
    return Instant{local->time_since_epoch()};
}

std::string formatIsoUtc(Instant t) { return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t); }

std::string formatLocalDate(LocalTime t) {
    return std::format("{:%Y-%m-%d}", floor<days>(t));
}

std::string formatLocalHm(LocalTime t) {
    const int m = minuteOfDay(t);
    return std::format("{:02}:{:02}", m / 60, m % 60);
}

TimeZone::TimeZone(std::string name) : name_(std::move(name)) {
    try {
        tz_ = locate_zone(name_);
    } catch (const std::exception&) {
        throw std::runtime_error("Unknown time zone: " + name_);
    }
}

LocalTime TimeZone::toLocal(Instant t) const { return tz_->to_local(t); }

Instant TimeZone::toInstant(LocalTime t) const { return tz_->to_sys(t, choose::earliest); }

Instant floorToSlot(Instant t) {
    const auto since = t.time_since_epoch();
    return Instant{since - since % duration_cast<seconds>(kSlotLength)};
}

std::vector<Instant> slotsBetween(Instant from, Instant to) {
    std::vector<Instant> out;
    for (Instant t = floorToSlot(from); t < to; t += kSlotLength) out.push_back(t);
    return out;
}

Instant startOfLocalDay(const TimeZone& tz, Instant t) {
    return tz.toInstant(floor<days>(tz.toLocal(t)));
}

int minuteOfDay(LocalTime t) {
    return static_cast<int>(duration_cast<minutes>(t - floor<days>(t)).count());
}

int hourOfDay(LocalTime t) { return minuteOfDay(t) / 60; }

unsigned monthOf(LocalTime t) {
    return static_cast<unsigned>(year_month_day{floor<days>(t)}.month());
}

unsigned isoWeekday(LocalTime t) { return weekday{floor<days>(t)}.iso_encoding(); }

}  // namespace hb
