#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"
#include "devices/dominion.hpp"

namespace hb {

// Danfoss DEVIreg Smart thermostats, through Danfoss's SecureDeviceGrid servers (OpenSDG).
// There is no local protocol: even at home the connection goes via the grid.
//
// heatbrain's identity on the grid is a 32-byte private key (its public key is its "peer id").
// It is created once and kept in <dataDir>/devi.json together with the paired thermostats;
// losing it means pairing again from the DEVI app.

using DeviKey = std::array<std::uint8_t, 32>;

struct DeviIdentity {
    DeviKey privateKey{};
    std::string houseName;
    struct Room {
        std::string name;
        std::string peerId;  // thermostat's public key, hex
    };
    std::vector<Room> rooms;
};
// Loads or creates (and saves) the identity.
DeviIdentity loadDeviIdentity(const std::filesystem::path& dataDir);
void saveDeviIdentity(const std::filesystem::path& dataDir, const DeviIdentity& id);

// The last known state of one thermostat.
struct DeviState {
    bool connected = false;
    std::string error;
    std::optional<double> floorTemp, roomTemp;
    std::optional<double> manualSetpoint, comfortSetpoint, frostSetpoint;
    std::optional<dominion::ControlState> controlState;
    std::optional<bool> heating, windowOpen;
    std::uint8_t alarms = 0;
    std::optional<Instant> updated;  // last packet received
};

// The process-wide grid connection (OpenSDG has one worker thread for everything).
class DeviGrid {
public:
    static DeviGrid& instance();
    // Connects (blocking, with OpenSDG's own timeouts) if not connected. Throws with a reason.
    void ensureConnected(const DeviKey& privateKey);
    bool connected() const;
    void* handle() const { return grid_; }  // osdg_connection_t
    // Tells the grid which peers we are paired with (the official app does this after connecting);
    // sent again whenever the list grows or the grid reconnects.
    void registerPeer(const DeviKey& peer);
    // Drops the grid connection, so the next use connects afresh (after pairing).
    void reset();

private:
    DeviGrid();
    ~DeviGrid();
    void sendPairings();  // with mutex_ held
    mutable std::mutex mutex_;
    void* grid_ = nullptr;
    bool connected_ = false;
    std::vector<DeviKey> peers_;
    bool peersSent_ = false;
};

// One thermostat. Thread-safe: OpenSDG delivers data on its own thread.
class DeviThermostat {
public:
    DeviThermostat(std::string peerIdHex, DeviKey privateKey);
    ~DeviThermostat();
    DeviThermostat(const DeviThermostat&) = delete;
    DeviThermostat& operator=(const DeviThermostat&) = delete;

    // Connects if needed, then asks for all values (answers arrive asynchronously).
    void refresh();
    // Puts the thermostat in manual mode (out of its own schedule) and sets the manual setpoint.
    // Does nothing in states heatbrain must not touch (configuring, fault, vacation).
    void setManualSetpoint(double celsius);
    DeviState state() const;
    const std::string& peerId() const { return peerId_; }

    // Called from OpenSDG's thread.
    void onData(const std::uint8_t* data, unsigned length);
    void onState(int osdgState);

private:
    std::optional<Instant> askedAt_;  // the previous refresh
    // Starts connecting in the background if needed; true once connected. A connection that has
    // not come up in 30 s is given up and tried again (OpenSDG itself would wait forever).
    bool ensureConnected();
    void send(const std::vector<std::uint8_t>& bytes);
    std::optional<Instant> connectingSince_;

    std::string peerId_;
    DeviKey key_;
    mutable std::mutex mutex_;
    void* conn_ = nullptr;
    DeviState state_;
    std::vector<std::uint8_t> partial_;
};

// Pairing with a one-time code from the DEVI app (Settings -> Share house). Adds the house's
// thermostats to the identity (and saves it). Throws with a readable reason.
DeviIdentity deviPair(const std::filesystem::path& dataDir, const std::string& otp, const std::string& phoneName);

}  // namespace hb
