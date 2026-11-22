#include <catch2/catch_test_macros.hpp>

#include "devices/dominion.hpp"

using namespace hb::dominion;
using Bytes = std::vector<std::uint8_t>;

TEST_CASE("Dominion: read requests and writes are framed like the DEVI app does") {
    // floor temperature: class 6, code 29298 = 0x7272
    CHECK(readRequest(kFloorTemperature) == Bytes{1, 6, 0x72, 0x72, 0});
    // manual setpoint 22.5 deg -> 2250 = 0x08CA, little-endian
    CHECK(writeDecimal(kSetpointManual, 22.5) == Bytes{0, 7, 0x94, 0x72, 2, 0xCA, 0x08});
    CHECK(writeByte(kControlMode, 1) == Bytes{0, 7, 0x91, 0x72, 1, 1});
}

TEST_CASE("Dominion: concatenated packets are split, a partial one waits for more") {
    Bytes rest;
    // floor 21.37 (2137 = 0x0859), room n/a (0x8000), and half of a third packet
    const Bytes data{6, 0x72, 0x72, 2, 0x59, 0x08, 6, 0x73, 0x72, 2, 0x00, 0x80, 5, 0x38};
    auto packets = parse(data, rest);
    REQUIRE(packets.size() == 2);
    CHECK(packets[0].code == kFloorTemperature);
    CHECK(*decimal(packets[0].payload) == 21.37);
    CHECK(packets[1].code == kRoomTemperature);
    CHECK_FALSE(decimal(packets[1].payload));
    CHECK(rest == Bytes{5, 0x38});
    // The rest of the third packet arrives: heating on
    Bytes more = rest;
    more.insert(more.end(), {0x72, 1, 1});
    packets = parse(more, rest);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0].code == kHeatingOn);
    CHECK(packets[0].payload == Bytes{1});
    CHECK(rest.empty());
}

TEST_CASE("Dominion: negative temperatures and rounding") {
    CHECK(*decimal(encodeDecimal(-3.25)) == -3.25);
    CHECK(*decimal(encodeDecimal(5.004)) == 5.0);
}

TEST_CASE("Dominion: getting into manual mode leaves special states first") {
    CHECK(toManual(ControlState::Manual).empty());
    CHECK(toManual(ControlState::Fatal).empty());
    CHECK(toManual(ControlState::AtHome) == std::vector<Bytes>{writeByte(kControlMode, 1)});
    const auto fromOff = toManual(ControlState::Off);
    REQUIRE(fromOff.size() == 2);
    CHECK(fromOff[0] == writeByte(kControlMode, static_cast<std::uint8_t>(ControlMode::OffStateOff)));
    CHECK(toManual(ControlState::Vacation).empty());  // left alone
}
