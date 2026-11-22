#include "devices/devi.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/log.hpp"
#include "devices/sdg_incoming.hpp"

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
    if (grid_) {
        // Only reached once the old grid connection reported closed/error (or never connected).
        auto* old = static_cast<osdg_connection_t>(grid_);
        const auto st = osdg_get_connection_state(old);
        if (st == osdg_connected || st == osdg_connecting) osdg_connection_close(old);
        osdg_connection_destroy(old);
        grid_ = nullptr;
    }
    auto* g = osdg_connection_create();
    osdg_set_private_key(g, key.data());
    osdg_set_blocking_mode(g, 1);
    osdg_set_ping_interval(g, 30);  // Danfoss drops idle connections
    osdg_set_state_change_callback(g, gridState);
    const osdg_result_t r = osdg_connect_to_danfoss(g);
    grid_ = g;
    if (r != osdg_no_error) throw std::runtime_error("DEVI: " + resultText(r));
    {
        std::lock_guard s(gGridStateMutex);
        connected_ = true;
    }
    peersSent_ = false;
    sendPairings();
    log::info("DEVI: connected to the Danfoss grid");
}

void DeviGrid::sendPairings() {
    if (!grid_ || peersSent_ || peers_.empty()) return;
    std::vector<osdg_key_t> keys(peers_.size());
    for (std::size_t i = 0; i < peers_.size(); ++i) std::memcpy(keys[i], peers_[i].data(), 32);
    if (osdg_register_pairings(static_cast<osdg_connection_t>(grid_), keys.data(), static_cast<int>(keys.size())) == osdg_no_error) {
        peersSent_ = true;
    }
}

void DeviGrid::registerPeer(const DeviKey& peer) {
    std::lock_guard lock(mutex_);
    if (std::ranges::find(peers_, peer) == peers_.end()) {
        peers_.push_back(peer);
        peersSent_ = false;
    }
    if (connected()) sendPairings();
}

void DeviGrid::reset() {
    std::lock_guard lock(mutex_);
    std::lock_guard s(gGridStateMutex);
    connected_ = false;  // the next ensureConnected() replaces the connection
}

// ---------------------------------------------------------------- thermostat

DeviThermostat::DeviThermostat(std::string peerIdHex, DeviKey privateKey) : peerId_(std::move(peerIdHex)), key_(privateKey) {}

DeviThermostat::~DeviThermostat() {
    std::lock_guard lock(mutex_);
    if (conn_) {
        auto* c = static_cast<osdg_connection_t>(conn_);
        osdg_set_user_data(c, nullptr);
        const auto st = osdg_get_connection_state(c);
        if (st == osdg_connecting) return;  // still pending at the grid: abandoned (see ensureConnected)
        if (st == osdg_connected) {
            osdg_set_blocking_mode(c, 1);  // wait until the main loop has really closed it
            osdg_connection_close(c);
        }
        osdg_connection_destroy(c);
    }
}

void DeviThermostat::onState(int s) {
    std::lock_guard lock(mutex_);
    state_.connected = s == osdg_connected;
    if (s == osdg_connected) {
        connectingSince_.reset();
        state_.error.clear();
        partial_.clear();
        log::info("DEVI: connected to thermostat {}...", peerId_.substr(0, 8));
    }
    if (s == osdg_closed || s == osdg_error) {
        connectingSince_.reset();
        state_.error = s == osdg_error ? "connection lost" : "closed";
    }
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

bool DeviThermostat::ensureConnected() {
    std::uint8_t peer[32];
    if (!fromHex(peerId_, peer, 32)) throw std::runtime_error("DEVI: invalid thermostat id");
    DeviKey peerKey{};
    std::memcpy(peerKey.data(), peer, 32);
    DeviGrid::instance().ensureConnected(key_);
    DeviGrid::instance().registerPeer(peerKey);

    std::lock_guard lock(mutex_);
    if (conn_ && state_.connected) return true;
    const auto now = floor<seconds>(system_clock::now());
    if (conn_ && connectingSince_ && now - *connectingSince_ < seconds{30}) return false;  // still connecting
    if (conn_) {
        auto* old = static_cast<osdg_connection_t>(conn_);
        osdg_set_user_data(old, nullptr);  // its callbacks no longer reach us
        const auto st = osdg_get_connection_state(old);
        if (st == osdg_connecting) {
            // Never answered: it may still sit in the grid's list of pending calls, so destroying it
            // could crash later. Abandon it (a few hundred bytes).
            log::warn("DEVI: thermostat {}... did not answer within 30 s - trying again", peerId_.substr(0, 8));
        } else {
            if (st == osdg_connected) {
                osdg_set_blocking_mode(old, 1);
                osdg_connection_close(old);
            }
            osdg_connection_destroy(old);  // closed or failed: OpenSDG is done with it
        }
        conn_ = nullptr;
    }
    auto* c = osdg_connection_create();
    osdg_set_user_data(c, this);
    osdg_set_blocking_mode(c, 0);  // answers come through the callbacks; we keep our own timeout
    osdg_set_state_change_callback(c, thermostatState);
    osdg_set_receive_data_callback(c, thermostatData);
    const osdg_result_t r =
        osdg_connect_to_remote(static_cast<osdg_connection_t>(DeviGrid::instance().handle()), c, peer, "dominion-1.0");
    conn_ = c;
    state_.connected = false;
    if (r != osdg_no_error) {
        connectingSince_.reset();
        state_.error = resultText(r);
        throw std::runtime_error("DEVI thermostat: " + resultText(r));
    }
    connectingSince_ = now;
    state_.error = "connecting...";
    return false;
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
    {
        // The previous request got no answer at all: the connection died silently (the Wi-Fi was
        // off, a router dropped it). Reconnect instead of asking into the void.
        std::lock_guard lock(mutex_);
        const auto now = floor<seconds>(system_clock::now());
        const bool unanswered = askedAt_ && (!state_.updated || *state_.updated < *askedAt_) && now - *askedAt_ >= seconds{30};
        if (conn_ && state_.connected && unanswered) {
            state_.connected = false;
            state_.error = "no answer - reconnecting";
        }
        askedAt_ = now;
    }
    if (!ensureConnected()) return;  // connecting in the background; asked on the next round
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
    if (!ensureConnected()) throw std::runtime_error("DEVI thermostat: still connecting");
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
void feedConfig(ConfigReceiver* r, const std::uint8_t* p, unsigned len) {
    std::lock_guard lock(r->m);
    if (r->data.empty() && !r->total && len >= 8 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) {
        // chunked: int32 0, then the total size (little-endian), then data
        r->total = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<std::uint32_t>(p[7]) << 24);
        r->data.insert(r->data.end(), p + 8, p + len);
    } else {
        r->data.insert(r->data.end(), p, p + len);
    }
    if (!r->total || r->data.size() >= *r->total) r->done = true;
    r->cv.notify_all();
}
osdg_result_t configData(osdg_connection_t c, const void* d, unsigned len) {
    auto* r = static_cast<ConfigReceiver*>(osdg_get_user_data(c));
    if (r) feedConfig(r, static_cast<const std::uint8_t*>(d), len);
    return osdg_no_error;
}

// The iOS app calls us right after pairing (the Android app waits to be called). While a pairing
// runs, the first configuration call is accepted and parked here for the pairing thread.
struct IncomingCall {
    std::string peerHex, host;
    unsigned port = 0;
    std::vector<std::uint8_t> tunnelId;
};
std::mutex gCallMutex;
std::condition_variable gCallCv;
bool gAcceptCalls = false;
std::optional<IncomingCall> gCall;

int onIncomingCall(osdg_connection_t, const char* peerIdHex, const char* protocol, const char* host, unsigned port,
                   const unsigned char* tunnelId, unsigned tunnelIdSize) {
    std::lock_guard lock(gCallMutex);
    const bool take = gAcceptCalls && !gCall && protocol && std::string(protocol) == "dominion-configuration-1.0";
    log::info("DEVI: incoming call ({}) - {}", protocol ? protocol : "?", take ? "accepted" : "refused");
    if (!take) return 0;
    gCall = IncomingCall{peerIdHex ? peerIdHex : "", host ? host : "", port,
                         std::vector<std::uint8_t>(tunnelId, tunnelId + tunnelIdSize)};
    gCallCv.notify_all();
    return 1;
}

// Accepting calls only for the duration of one pairing.
struct AcceptCallsWhilePairing {
    AcceptCallsWhilePairing() {
        std::lock_guard lock(gCallMutex);
        gAcceptCalls = true;
        gCall.reset();
        osdg_set_incoming_call_callback(onIncomingCall);
    }
    ~AcceptCallsWhilePairing() {
        osdg_set_incoming_call_callback(nullptr);
        std::lock_guard lock(gCallMutex);
        gAcceptCalls = false;
        gCall.reset();
    }
};

bool sameHex(std::string a, std::string b) {
    for (auto& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (auto& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return a == b;
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

DeviIdentity finishPairing(const fs::path& dataDir, DeviIdentity id, const std::vector<std::uint8_t>& data);

DeviIdentity deviPair(const fs::path& dataDir, const std::string& otpIn, const std::string& phoneName) {
    std::string otp;
    for (char ch : otpIn) if (ch >= '0' && ch <= '9') otp += ch;
    if (otp.size() < SDG_MIN_OTP_LENGTH) throw std::invalid_argument("The code from the DEVI app is too short.");
    DeviIdentity id = loadDeviIdentity(dataDir);
    DeviGrid::instance().ensureConnected(id.privateKey);
    auto* grid = static_cast<osdg_connection_t>(DeviGrid::instance().handle());

    // 1. Pair with the phone that shared the house. OpenSDG registers the phone with the grid while
    //    pairing, so the grid passes on the call the iOS app makes right afterwards.
    AcceptCallsWhilePairing acceptCalls;
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

    const unsigned char* me = osdg_get_my_peer_id(grid);
    const std::string request =
        json{{"phoneName", phoneName}, {"phonePublicKey", toHex(me, 32)}, {"chunkedMessage", true}}.dump();
    const std::string phoneHex = toHex(phone, 32);

    // 2a. iOS: the phone calls us (within milliseconds of the pairing result).
    std::optional<IncomingCall> call;
    {
        std::unique_lock lock(gCallMutex);
        gCallCv.wait_for(lock, seconds{3}, [] { return gCall.has_value(); });
        call = gCall;
    }
    if (call && sameHex(call->peerHex, phoneHex)) {
        log::info("DEVI: the phone called back - receiving the house over its call");
        ConfigReceiver rx;
        SdgIncomingTunnel tunnel(id.privateKey, seconds{15});
        const auto caller = tunnel.accept(call->host, call->port, call->tunnelId);
        if (!sameHex(toHex(caller.data(), 32), phoneHex)) {
            throw std::runtime_error("The call came from another device than the paired phone.");
        }
        tunnel.send(std::vector<std::uint8_t>(request.begin(), request.end()));
        const auto deadline = steady_clock::now() + seconds{30};
        while (!rx.done && steady_clock::now() < deadline) {
            const auto packet = tunnel.receive();
            if (!packet) break;
            feedConfig(&rx, packet->data(), static_cast<unsigned>(packet->size()));
        }
        if (!rx.done) throw std::runtime_error("The phone did not send the whole house configuration.");
        return finishPairing(dataDir, std::move(id), rx.data);
    }
    if (call) log::warn("DEVI: an unexpected device called during pairing - ignored");

    // 2b. Android: we call the phone and ask for the house configuration.
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
    osdg_send_data(cfg, request.data(), static_cast<int>(request.size()));
    {
        std::unique_lock lock(rx.m);
        rx.cv.wait_for(lock, seconds{30}, [&] { return rx.done || rx.failed; });
    }
    osdg_set_user_data(cfg, nullptr);
    osdg_connection_close(cfg);
    osdg_connection_destroy(cfg);
    if (!rx.done) throw std::runtime_error("The phone did not send the house configuration in time.");
    return finishPairing(dataDir, std::move(id), rx.data);
}

DeviIdentity finishPairing(const fs::path& dataDir, DeviIdentity id, const std::vector<std::uint8_t>& data) {
    const json house = json::parse(std::string(data.begin(), data.end()), nullptr, false);
    if (house.is_discarded()) throw std::runtime_error("The phone sent an unreadable house configuration.");
    id.houseName = house.value("houseName", id.houseName);
    for (const auto& room : house.value("rooms", json::array())) {
        const std::string peer = room.is_object() ? room.value("peerId", "") : "";
        if (peer.size() != 64) {
            log::warn("DEVI: skipped a room whose peerId is {} characters long (expected 64)", peer.size());
            continue;
        }
        bool known = false;
        for (auto& existing : id.rooms) {
            if (existing.peerId == peer) { existing.name = room.value("roomName", existing.name); known = true; }
        }
        if (!known) id.rooms.push_back({room.value("roomName", "DEVI"), peer});
    }
    if (id.rooms.empty()) {
        // Paired, but nothing usable came: say what did come (field names only, no device ids).
        std::string shape;
        for (const auto& [k, v] : house.items()) {
            shape += (shape.empty() ? "" : ", ") + k;
            if (v.is_array()) {
                shape += std::format("[{}]", v.size());
                if (!v.empty() && v[0].is_object()) {
                    std::string inner;
                    for (const auto& [ik, iv] : v[0].items()) inner += (inner.empty() ? "" : " ") + ik;
                    shape += "{" + inner + "}";
                }
            }
        }
        log::warn("DEVI: the phone sent a house configuration without thermostats ({} bytes): {}", data.size(), shape);
        throw std::runtime_error("Paired, but the phone sent no thermostats (it sent: " + shape +
                                 "). Check that the thermostat is in the house you shared.");
    }
    saveDeviIdentity(dataDir, id);
    log::info("DEVI: paired with house '{}', {} thermostat(s)", id.houseName, id.rooms.size());
    // During pairing the grid was told about the phone only; start afresh so the thermostats get
    // registered (see DeviThermostat::ensureConnected).
    DeviGrid::instance().reset();
    return id;
}

}  // namespace hb
