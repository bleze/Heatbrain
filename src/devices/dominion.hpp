#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hb::dominion {

// The application protocol of Danfoss DEVIreg Smart thermostats ("dominion-1.0"), carried over a
// SecureDeviceGrid peer connection. Little-endian throughout.
//
//   incoming:  [class u8][code u16][len u8][payload...]      (several may arrive concatenated)
//   outgoing:  [flag u8][class u8][code u16][len u8][payload] flag = 1 for a read (no payload),
//                                                             0 for a write
// Codes as used by the openHAB DeviSmart binding (Sonic-Amiga) and ha-devi-mqtt.

struct Code {
    std::uint8_t cls;
    std::uint16_t code;
    bool operator==(const Code&) const = default;
};

inline constexpr Code kFloorTemperature{6, 29298};
inline constexpr Code kRoomTemperature{6, 29299};
inline constexpr Code kSetpointComfort{7, 29330};   // "at home"
inline constexpr Code kSetpointEconomy{7, 29331};   // "away" part of the schedule
inline constexpr Code kSetpointManual{7, 29332};
inline constexpr Code kSetpointVacation{7, 29333};
inline constexpr Code kSetpointFrost{7, 29334};
inline constexpr Code kFloorMin{7, 29335};
inline constexpr Code kFloorMinEnabled{7, 29336};
inline constexpr Code kFloorMax{7, 29337};
inline constexpr Code kControlInfo{7, 29328};       // current ControlState (read)
inline constexpr Code kControlMode{7, 29329};       // ControlMode command (write)
inline constexpr Code kAwayIsPlanned{7, 29339};
inline constexpr Code kAwayPeriod{7, 29340};
inline constexpr Code kHeatingOn{5, 29240};         // relay on
inline constexpr Code kAlarms{5, 29241};            // bitfield
inline constexpr Code kWindowOpen{5, 29246};
inline constexpr Code kSoftwareRevision{0, 35};
// Energy bookkeeping in the thermostat (formats not documented yet: read raw, see DeviState::raw).
inline constexpr Code kRelayCount{5, 29232};
inline constexpr Code kRelayOnTime{5, 29233};
inline constexpr Code kSystemRuntime{5, 29234};
inline constexpr Code kWizardInfo{5, 29239};
inline constexpr Code kEnergyTotal{8, 29377};
inline constexpr Code kEnergy30Days{8, 29378};
inline constexpr Code kEnergy7Days{8, 29379};
inline constexpr Code kConnectionCount{3, 30470};

enum class ControlState : std::uint8_t {
    Configuring = 0, Manual = 1, AtHome = 2, Away = 3, Vacation = 4, Fatal = 5, Pause = 6, Off = 7,
    AtHomeOverride = 8,
};
enum class ControlMode : std::uint8_t {
    WeeklyScheduleOn = 0, WeeklyScheduleOff = 1,  // off = manual
    OffStateOn = 2, OffStateOff = 3,
    FrostProtectionOn = 4, FrostProtectionOff = 5,  // "pause"
    TemporaryHomeOn = 6, TemporaryHomeOff = 7,      // "override"
};
std::string controlStateName(ControlState s);

struct Packet {
    Code code;
    std::vector<std::uint8_t> payload;
};

// Bytes to send.
std::vector<std::uint8_t> readRequest(Code c);
std::vector<std::uint8_t> write(Code c, const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> writeDecimal(Code c, double value);
std::vector<std::uint8_t> writeByte(Code c, std::uint8_t value);
std::vector<std::uint8_t> writeBool(Code c, bool value);

// Splits received bytes into packets; an incomplete tail is returned in `rest` (prepend it to
// the next receive).
std::vector<Packet> parse(const std::vector<std::uint8_t>& data, std::vector<std::uint8_t>& rest);

// Temperatures: int16 / 100, 0x8000 = not available.
std::optional<double> decimal(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encodeDecimal(double value);

// The commands that move the thermostat from `from` into manual mode (leaving a special state
// first: in Off, Pause, Override and Vacation it ignores other mode commands). Empty when it is
// already manual, or in a state that must be left alone (Configuring, Fatal, Vacation).
std::vector<std::vector<std::uint8_t>> toManual(ControlState from);

}  // namespace hb::dominion
