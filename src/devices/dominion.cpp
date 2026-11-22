#include "devices/dominion.hpp"

#include <cmath>

namespace hb::dominion {

std::string controlStateName(ControlState s) {
    switch (s) {
        case ControlState::Configuring: return "configuring";
        case ControlState::Manual: return "manual";
        case ControlState::AtHome: return "schedule (at home)";
        case ControlState::Away: return "schedule (away)";
        case ControlState::Vacation: return "vacation";
        case ControlState::Fatal: return "fault";
        case ControlState::Pause: return "pause (frost protection)";
        case ControlState::Off: return "off";
        case ControlState::AtHomeOverride: return "override";
    }
    return "unknown";
}

namespace {
std::vector<std::uint8_t> header(std::uint8_t flag, Code c, std::uint8_t len) {
    return {flag, c.cls, static_cast<std::uint8_t>(c.code & 0xff), static_cast<std::uint8_t>(c.code >> 8), len};
}
}  // namespace

std::vector<std::uint8_t> readRequest(Code c) { return header(1, c, 0); }

std::vector<std::uint8_t> write(Code c, const std::vector<std::uint8_t>& payload) {
    auto out = header(payload.empty() ? 1 : 0, c, static_cast<std::uint8_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> encodeDecimal(double value) {
    const auto v = static_cast<std::int16_t>(std::lround(value * 100.0));
    return {static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>((v >> 8) & 0xff)};
}

std::vector<std::uint8_t> writeDecimal(Code c, double value) { return write(c, encodeDecimal(value)); }
std::vector<std::uint8_t> writeByte(Code c, std::uint8_t value) { return write(c, {value}); }
std::vector<std::uint8_t> writeBool(Code c, bool value) { return write(c, {static_cast<std::uint8_t>(value ? 1 : 0)}); }

std::optional<double> decimal(const std::vector<std::uint8_t>& p) {
    if (p.size() < 2) return std::nullopt;
    const auto raw = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    if (raw == 0x8000) return std::nullopt;
    return static_cast<std::int16_t>(raw) / 100.0;
}

std::vector<Packet> parse(const std::vector<std::uint8_t>& data, std::vector<std::uint8_t>& rest) {
    std::vector<Packet> out;
    std::size_t i = 0;
    while (data.size() - i >= 4) {
        const std::size_t len = data[i + 3];
        if (data.size() - i < 4 + len) break;
        Packet p;
        p.code = {data[i], static_cast<std::uint16_t>(data[i + 1] | (data[i + 2] << 8))};
        p.payload.assign(data.begin() + static_cast<long>(i + 4), data.begin() + static_cast<long>(i + 4 + len));
        out.push_back(std::move(p));
        i += 4 + len;
    }
    rest.assign(data.begin() + static_cast<long>(i), data.end());
    return out;
}

std::vector<std::vector<std::uint8_t>> toManual(ControlState from) {
    const auto mode = [](ControlMode m) { return writeByte(kControlMode, static_cast<std::uint8_t>(m)); };
    std::vector<std::vector<std::uint8_t>> out;
    switch (from) {
        case ControlState::Manual:
        case ControlState::Configuring:
        case ControlState::Fatal:
        case ControlState::Vacation:  // leaving it needs an away-period message we can't verify
            return out;  // nothing to do / not ours to touch
        case ControlState::Off: out.push_back(mode(ControlMode::OffStateOff)); break;
        case ControlState::Pause: out.push_back(mode(ControlMode::FrostProtectionOff)); break;
        case ControlState::AtHomeOverride: out.push_back(mode(ControlMode::TemporaryHomeOff)); break;
        case ControlState::AtHome:
        case ControlState::Away:
            break;
    }
    out.push_back(mode(ControlMode::WeeklyScheduleOff));  // = manual
    return out;
}

}  // namespace hb::dominion
