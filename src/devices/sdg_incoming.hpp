#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hb {

// The accepting side of a SecureDeviceGrid peer tunnel: used when a peer calls us through the grid
// (OpenSDG only implements the calling side). Needed to receive a shared house from the DEVI iOS
// app, which calls the receiver right after pairing instead of waiting to be called.
//
// After joining the forwarded tunnel at the relay (host:port + tunnel id from the incoming call), it
// answers the CurveCP-style handshake as the server: TELL -> WELC, HELO -> COOK, VOCH -> REDY, then
// exchanges MESG packets. Modelled on the verified Java implementation in soundvibe/ha-danfoss
// (IncomingPeerConnection, PR #45).
class SdgIncomingTunnel {
public:
    using Key = std::array<std::uint8_t, 32>;
    SdgIncomingTunnel(const Key& ourPrivateKey, std::chrono::seconds timeout);
    ~SdgIncomingTunnel();
    SdgIncomingTunnel(const SdgIncomingTunnel&) = delete;
    SdgIncomingTunnel& operator=(const SdgIncomingTunnel&) = delete;

    // Connects, joins the tunnel and completes the handshake. Returns the caller's long-term key
    // (its peer id). Throws std::runtime_error with a reason.
    Key accept(const std::string& host, unsigned port, const std::vector<std::uint8_t>& tunnelId);

    void send(const std::vector<std::uint8_t>& payload);
    // The next data packet; nullopt when the peer closed the connection.
    std::optional<std::vector<std::uint8_t>> receive();

private:
    std::vector<std::uint8_t> readFrame();
    void writeFrame(const std::vector<std::uint8_t>& frame);
    void readExact(std::uint8_t* out, std::size_t n);
    void writePacket(const char command[4], const std::vector<std::uint8_t>& data);
    std::pair<std::string, std::vector<std::uint8_t>> readPacket();
    void forward(const std::vector<std::uint8_t>& tunnelId);
    std::vector<std::uint8_t> sealWithNonce(const char* prefix, const std::vector<std::uint8_t>& plain);
    void acceptPeerNonce(std::uint64_t nonce);

    Key privateKey_{};
    Key publicKey_{};
    std::array<std::uint8_t, 32> shared_{};  // crypto_box_beforenm(client short-term, our short-term)
    std::uint64_t nonce_ = 0;
    std::optional<std::uint64_t> lastPeerNonce_;
    std::chrono::seconds timeout_;
    std::intptr_t socket_ = -1;
};

}  // namespace hb
