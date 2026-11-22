#include "devices/devi.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

extern "C" {
#include "opensdg.h"
}

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

namespace {

std::string toHex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
bool fromHex(const std::string& s, std::uint8_t* out, std::size_t n) {
    if (s.size() != n * 2) return false;
    const auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}
std::string resultText(osdg_result_t r) {
    switch (r) {
        case osdg_no_error: return "ok";
        case osdg_socket_error: return "network error";
        case osdg_connection_failed: return "cannot reach the Danfoss servers";
        case osdg_connection_refused: return "refused";
        case osdg_connection_closed: return "connection closed";
        case osdg_peer_timeout: return "the thermostat did not answer";
        case osdg_server_error: return "Danfoss server error";
        case osdg_wrong_state: return "not connected";
        case osdg_too_many_connections: return "too many connections";
        default: return std::format("OpenSDG error {}", static_cast<int>(r));
    }
}

// OpenSDG callbacks carry the connection; user data points back to our object.
void thermostatState(osdg_connection_t c, enum osdg_connection_state s) {
    if (auto* t = static_cast<DeviThermostat*>(osdg_get_user_data(c))) t->onState(static_cast<int>(s));
}
osdg_result_t thermostatData(osdg_connection_t c, const void* data, unsigned len) {
    if (auto* t = static_cast<DeviThermostat*>(osdg_get_user_data(c))) t->onData(static_cast<const std::uint8_t*>(data), len);
    return osdg_no_error;
}

std::mutex gGridStateMutex;
bool* gGridConnectedFlag = nullptr;
void gridState(osdg_connection_t, enum osdg_connection_state s) {
    std::lock_guard lock(gGridStateMutex);
    if (gGridConnectedFlag && (s == osdg_closed || s == osdg_error)) *gGridConnectedFlag = false;
}

}  // namespace

// ---------------------------------------------------------------- identity

DeviIdentity loadDeviIdentity(const fs::path& dataDir) {
    DeviIdentity id;
    const fs::path file = dataDir / "devi.json";
    if (fs::exists(file)) {
        const json j = json::parse(std::ifstream(file));
        if (!fromHex(j.value("privateKey", ""), id.privateKey.data(), id.privateKey.size())) {
            throw std::runtime_error("devi.json: invalid private key");
        }
        id.houseName = j.value("houseName", "");
        for (const auto& r : j.value("rooms", json::array())) id.rooms.push_back({r.value("name", ""), r.value("peerId", "")});
        return id;
    }
    osdg_create_private_key(id.privateKey.data());
    saveDeviIdentity(dataDir, id);
    return id;
}

void saveDeviIdentity(const fs::path& dataDir, const DeviIdentity& id) {
    json rooms = json::array();
    for (const auto& r : id.rooms) rooms.push_back({{"name", r.name}, {"peerId", r.peerId}});
    fs::create_directories(dataDir);
    const fs::path tmp = dataDir / "devi.json.tmp";
    std::ofstream(tmp) << json{{"privateKey", toHex(id.privateKey.data(), id.privateKey.size())},
                               {"houseName", id.houseName}, {"rooms", rooms}}
                              .dump(2);
    fs::rename(tmp, dataDir / "devi.json");
}

// ---------------------------------------------------------------- grid

DeviGrid& DeviGrid::instance() {
    static DeviGrid g;
    return g;
}

DeviGrid::DeviGrid() {
    if (osdg_init() != osdg_no_error) throw std::runtime_error("OpenSDG could not start");
    osdg_set_log_mask(OSDG_LOG_ERRORS);
}

DeviGrid::~DeviGrid() {
    // Process exit: OpenSDG's thread goes with it. (osdg_shutdown here could block on a
    // connection still being torn down.)
}

bool DeviGrid::connected() const {
    std::lock_guard lock(gGridStateMutex);
    return connected_;
}

void DeviGrid::ensureConnected(const DeviKey& key) {
    std::lock_guard lock(mutex_);
    {
        std::lock_guard s(gGridStateMutex);
        if (connected_) return;
        gGridConnectedFlag = &connected_;
    }
    if (grid_) osdg_connection_destroy(static_cast<osdg_connection_t>(grid_));
    auto* g = osdg_connection_create();
    osdg_set_private_key(g, key.data());
    osdg_set_blocking_mode(g, 1);
    osdg_set_ping_interval(g, 30);  // Danfoss drops idle connections
    osdg_set_state_change_callback(g, gridState);
    const osdg_result_t r = osdg_connect_to_danfoss(g);
    grid_ = g;
    if (r != osdg_no_error) throw std::runtime_error("DEVI: " + resultText(r));
    std::lock_guard s(gGridStateMutex);
    connected_ = true;
    log::info("DEVI: connected to the Danfoss grid");
}

// ---------------------------------------------------------------- thermostat

DeviThermostat::DeviThermostat(std::string peerIdHex, DeviKey privateKey) : peerId_(std::move(peerIdHex)), key_(privateKey) {}

DeviThermostat::~DeviThermostat() {
    std::lock_guard lock(mutex_);
    if (conn_) {
        auto* c = static_cast<osdg_connection_t>(conn_);
        osdg_set_user_data(c, nullptr);
        osdg_connection_close(c);
        osdg_connection_destroy(c);
    }
}

void DeviThermostat::onState(int s) {
    std::lock_guard lock(mutex_);
    state_.connected = s == osdg_connected;
    if (s == osdg_closed || s == osdg_error) state_.error = s == osdg_error ? "connection lost" : "closed";
}

void DeviThermostat::onData(const std::uint8_t* data, unsigned length) {
    std::lock_guard lock(mutex_);
    std::vector<std::uint8_t> bytes = partial_;
    bytes.insert(bytes.end(), data, data + length);
    using namespace dominion;
    for (const auto& p : parse(bytes, partial_)) {
        const auto b = [&] { return !p.payload.empty() && p.payload[0] != 0; };
        if (p.code == kFloorTemperature) state_.floorTemp = decimal(p.payload);
        else if (p.code == kRoomTemperature) state_.roomTemp = decimal(p.payload);
        else if (p.code == kSetpointManual) state_.manualSetpoint = decimal(p.payload);
        else if (p.code == kSetpointComfort) state_.comfortSetpoint = decimal(p.payload);
        else if (p.code == kSetpointFrost) state_.frostSetpoint = decimal(p.payload);
        else if (p.code == kControlInfo && !p.payload.empty()) state_.controlState = static_cast<ControlState>(p.payload[0]);
        else if (p.code == kHeatingOn) state_.heating = b();
        else if (p.code == kWindowOpen) state_.windowOpen = b();
        else if (p.code == kAlarms && !p.payload.empty()) state_.alarms = p.payload[0];
    }
    state_.updated = floor<seconds>(system_clock::now());
    state_.error.clear();
}

void DeviThermostat::ensureConnected() {
    DeviGrid::instance().ensureConnected(key_);
    std::lock_guard lock(mutex_);
    if (conn_ && state_.connected) return;
    if (conn_) {
        auto* old = static_cast<osdg_connection_t>(conn_);
        osdg_set_user_data(old, nullptr);
        osdg_connection_destroy(old);
        conn_ = nullptr;
    }
    std::uint8_t peer[32];
    if (!fromHex(peerId_, peer, 32)) throw std::runtime_error("DEVI: invalid thermostat id");
    auto* c = osdg_connection_create();
    osdg_set_user_data(c, this);
    osdg_set_blocking_mode(c, 1);
    osdg_set_state_change_callback(c, thermostatState);
    osdg_set_receive_data_callback(c, thermostatData);
    const osdg_result_t r =
        osdg_connect_to_remote(static_cast<osdg_connection_t>(DeviGrid::instance().handle()), c, peer, "dominion-1.0");
    conn_ = c;
    if (r != osdg_no_error) {
        state_.connected = false;
        state_.error = resultText(r);
        throw std::runtime_error("DEVI thermostat: " + resultText(r));
    }
    state_.connected = true;
    partial_.clear();
}

void DeviThermostat::send(const std::vector<std::uint8_t>& bytes) {
    osdg_connection_t c;
    {
        std::lock_guard lock(mutex_);
        c = static_cast<osdg_connection_t>(conn_);
    }
    if (!c) throw std::runtime_error("DEVI thermostat: not connected");
    const osdg_result_t r = osdg_send_data(c, bytes.data(), static_cast<int>(bytes.size()));
    if (r != osdg_no_error) {
        std::lock_guard lock(mutex_);
        state_.connected = false;
        throw std::runtime_error("DEVI thermostat: " + resultText(r));
    }
}

void DeviThermostat::refresh() {
    ensureConnected();
    using namespace dominion;
    std::vector<std::uint8_t> all;
    for (const Code& c : {kFloorTemperature, kRoomTemperature, kSetpointManual, kSetpointComfort, kSetpointFrost,
                          kControlInfo, kHeatingOn, kWindowOpen, kAlarms}) {
        const auto r = readRequest(c);
        all.insert(all.end(), r.begin(), r.end());
    }
    send(all);  // several requests in one send, as the binding does
}

void DeviThermostat::setManualSetpoint(double celsius) {
    ensureConnected();
    using namespace dominion;
    std::optional<ControlState> cs;
    {
        std::lock_guard lock(mutex_);
        cs = state_.controlState;
    }
    if (!cs) throw std::runtime_error("DEVI thermostat: state not known yet");
    if (*cs == ControlState::Configuring || *cs == ControlState::Fatal || *cs == ControlState::Vacation) {
        throw std::runtime_error("DEVI thermostat is in " + controlStateName(*cs) + " - left alone");
    }
    std::vector<std::uint8_t> all;
    for (const auto& m : toManual(*cs)) all.insert(all.end(), m.begin(), m.end());
    const auto w = writeDecimal(kSetpointManual, celsius);
    all.insert(all.end(), w.begin(), w.end());
    const auto again = readRequest(kControlInfo);  // learn the new state
    all.insert(all.end(), again.begin(), again.end());
    send(all);
}

DeviState DeviThermostat::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

// ---------------------------------------------------------------- pairing

namespace {
struct ConfigReceiver {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint8_t> data;
    std::optional<std::uint32_t> total;  // chunked: total size announced in the first packet
    bool done = false;
    bool failed = false;
};
osdg_result_t configData(osdg_connection_t c, const void* d, unsigned len) {
    auto* r = static_cast<ConfigReceiver*>(osdg_get_user_data(c));
    if (!r) return osdg_no_error;
    std::lock_guard lock(r->m);
    const auto* p = static_cast<const std::uint8_t*>(d);
    if (r->data.empty() && !r->total && len >= 8 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) {
        // chunked: int32 0, then the total size (little-endian), then data
        r->total = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<std::uint32_t>(p[7]) << 24);
        r->data.insert(r->data.end(), p + 8, p + len);
    } else {
        r->data.insert(r->data.end(), p, p + len);
    }
    if (!r->total || r->data.size() >= *r->total) r->done = true;
    r->cv.notify_all();
    return osdg_no_error;
}
void configState(osdg_connection_t c, enum osdg_connection_state s) {
    auto* r = static_cast<ConfigReceiver*>(osdg_get_user_data(c));
    if (!r) return;
    if (s == osdg_closed || s == osdg_error) {
        std::lock_guard lock(r->m);
        r->failed = !r->done;
        r->cv.notify_all();
    }
}
}  // namespace

DeviIdentity deviPair(const fs::path& dataDir, const std::string& otpIn, const std::string& phoneName) {
    std::string otp;
    for (char ch : otpIn) if (ch >= '0' && ch <= '9') otp += ch;
    if (otp.size() < SDG_MIN_OTP_LENGTH) throw std::invalid_argument("The code from the DEVI app is too short.");
    DeviIdentity id = loadDeviIdentity(dataDir);
    DeviGrid::instance().ensureConnected(id.privateKey);
    auto* grid = static_cast<osdg_connection_t>(DeviGrid::instance().handle());

    // 1. Pair with the phone that shared the house.
    auto* pairing = osdg_connection_create();
    osdg_set_blocking_mode(pairing, 1);
    osdg_result_t r = osdg_pair_remote(grid, pairing, otp.c_str());
    if (r != osdg_no_error || osdg_get_connection_state(pairing) != osdg_pairing_complete) {
        osdg_connection_destroy(pairing);
        throw std::runtime_error("Pairing failed (" + resultText(r) + "). Create a new code in the DEVI app and try again.");
    }
    std::uint8_t phone[32];
    std::memcpy(phone, osdg_get_peer_id(pairing), 32);
    osdg_connection_close(pairing);
    osdg_connection_destroy(pairing);

    // 2. Ask the phone for the house configuration.
    ConfigReceiver rx;
    auto* cfg = osdg_connection_create();
    osdg_set_user_data(cfg, &rx);
    osdg_set_blocking_mode(cfg, 1);
    osdg_set_receive_data_callback(cfg, configData);
    osdg_set_state_change_callback(cfg, configState);
    r = osdg_connect_to_remote(grid, cfg, phone, "dominion-configuration-1.0");
    if (r != osdg_no_error) {
        osdg_set_user_data(cfg, nullptr);
        osdg_connection_destroy(cfg);
        throw std::runtime_error("Paired, but the phone did not send the house (" + resultText(r) +
                                 "). Keep the DEVI app open and try again; the Android app is known to work better.");
    }
    const unsigned char* me = osdg_get_my_peer_id(grid);
    const std::string request =
        json{{"phoneName", phoneName}, {"phonePublicKey", toHex(me, 32)}, {"chunkedMessage", true}}.dump();
    osdg_send_data(cfg, request.data(), static_cast<int>(request.size()));
    {
        std::unique_lock lock(rx.m);
        rx.cv.wait_for(lock, seconds{30}, [&] { return rx.done || rx.failed; });
    }
    osdg_set_user_data(cfg, nullptr);
    osdg_connection_close(cfg);
    osdg_connection_destroy(cfg);
    if (!rx.done) throw std::runtime_error("The phone did not send the house configuration in time.");

    const json house = json::parse(std::string(rx.data.begin(), rx.data.end()), nullptr, false);
    if (house.is_discarded()) throw std::runtime_error("The phone sent an unreadable house configuration.");
    id.houseName = house.value("houseName", id.houseName);
    for (const auto& room : house.value("rooms", json::array())) {
        const std::string peer = room.value("peerId", "");
        if (peer.size() != 64) continue;
        bool known = false;
        for (auto& existing : id.rooms) {
            if (existing.peerId == peer) { existing.name = room.value("roomName", existing.name); known = true; }
        }
        if (!known) id.rooms.push_back({room.value("roomName", "DEVI"), peer});
    }
    saveDeviIdentity(dataDir, id);
    log::info("DEVI: paired with house '{}', {} thermostat(s)", id.houseName, id.rooms.size());
    return id;
}

}  // namespace hb
