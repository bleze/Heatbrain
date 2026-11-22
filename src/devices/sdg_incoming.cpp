#include "devices/sdg_incoming.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>

#include <sodium.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace hb {

namespace {

constexpr std::uint32_t kPacketMagic = 0xf09f909f;
constexpr std::uint32_t kForwardMagic = 0xF09D8C95;
constexpr const char* kForwardSignature = "Mdg-NaCl/binary";
constexpr std::uint8_t kForwardRequest = 0, kForwardHold = 1, kForwardReply = 2, kForwardError = 3;
constexpr std::size_t kKey = 32, kMac = 16, kShortNonce = 8, kLongNonce = 16, kCookie = 96, kHeloBox = 80;

#ifdef _WIN32
using sock_t = SOCKET;
void closeSocket(std::intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
#else
using sock_t = int;
void closeSocket(std::intptr_t s) { ::close(static_cast<int>(s)); }
#endif

void putU16(std::vector<std::uint8_t>& v, unsigned x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
void putU64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int s = 56; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
std::uint64_t getU64(const std::uint8_t* p) {
    std::uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x = (x << 8) | p[i];
    return x;
}
void putVarint(std::vector<std::uint8_t>& v, std::uint64_t x) {
    do {
        std::uint8_t b = x & 0x7F;
        x >>= 7;
        v.push_back(x ? (b | 0x80) : b);
    } while (x);
}

// 24-byte nonces: a 16-byte prefix and a big-endian counter, or an 8-byte prefix and 16 random bytes.
std::array<std::uint8_t, 24> shortNonce(const char* prefix, std::uint64_t n) {
    std::array<std::uint8_t, 24> out{};
    std::memcpy(out.data(), prefix, 16);
    for (int i = 0; i < 8; ++i) out[16 + i] = static_cast<std::uint8_t>(n >> (56 - 8 * i));
    return out;
}
std::array<std::uint8_t, 24> longNonce(const char* prefix, const std::uint8_t* value) {
    std::array<std::uint8_t, 24> out{};
    std::memcpy(out.data(), prefix, 8);
    std::memcpy(out.data() + 8, value, 16);
    return out;
}

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("incoming tunnel: " + why); }

}  // namespace

SdgIncomingTunnel::SdgIncomingTunnel(const Key& ourPrivateKey, std::chrono::seconds timeout)
    : privateKey_(ourPrivateKey), timeout_(timeout) {
    if (sodium_init() < 0) fail("libsodium did not start");
    crypto_scalarmult_base(publicKey_.data(), privateKey_.data());
}

SdgIncomingTunnel::~SdgIncomingTunnel() {
    if (socket_ != -1) closeSocket(socket_);
    sodium_memzero(shared_.data(), shared_.size());
}

void SdgIncomingTunnel::readExact(std::uint8_t* out, std::size_t n) {
    while (n > 0) {
        const auto got = ::recv(static_cast<sock_t>(socket_), reinterpret_cast<char*>(out), static_cast<int>(n), 0);
        if (got == 0) throw std::out_of_range("closed");
        if (got < 0) fail("no answer from the relay (timeout or network error)");
        out += got;
        n -= static_cast<std::size_t>(got);
    }
}

std::vector<std::uint8_t> SdgIncomingTunnel::readFrame() {
    std::uint8_t len[2];
    readExact(len, 2);
    std::vector<std::uint8_t> frame((len[0] << 8) | len[1]);
    if (!frame.empty()) readExact(frame.data(), frame.size());
    return frame;
}

void SdgIncomingTunnel::writeFrame(const std::vector<std::uint8_t>& frame) {
    const char* p = reinterpret_cast<const char*>(frame.data());
    std::size_t left = frame.size();
    while (left > 0) {
        const auto sent = ::send(static_cast<sock_t>(socket_), p, static_cast<int>(left), 0);
        if (sent <= 0) fail("could not send to the relay");
        p += sent;
        left -= static_cast<std::size_t>(sent);
    }
}

void SdgIncomingTunnel::writePacket(const char command[4], const std::vector<std::uint8_t>& data) {
    if (data.size() > 0xffff - 8) fail("packet too large");
    std::vector<std::uint8_t> f;
    putU16(f, static_cast<unsigned>(8 + data.size()));
    putU32(f, kPacketMagic);
    f.insert(f.end(), command, command + 4);
    f.insert(f.end(), data.begin(), data.end());
    writeFrame(f);
}

std::pair<std::string, std::vector<std::uint8_t>> SdgIncomingTunnel::readPacket() {
    auto f = readFrame();
    if (f.size() < 8) fail("packet too short");
    const std::uint32_t magic = (std::uint32_t{f[0]} << 24) | (f[1] << 16) | (f[2] << 8) | f[3];
    if (magic != kPacketMagic) fail("bad packet magic");
    return {std::string(f.begin() + 4, f.begin() + 8), std::vector<std::uint8_t>(f.begin() + 8, f.end())};
}

void SdgIncomingTunnel::forward(const std::vector<std::uint8_t>& tunnelId) {
    // ForwardRemote protobuf: magic (fixed32), protocol 1.0, the tunnel id and the signature.
    std::vector<std::uint8_t> req;
    req.push_back(0x0D);
    for (int s = 0; s < 32; s += 8) req.push_back(static_cast<std::uint8_t>(kForwardMagic >> s));
    req.push_back(0x10); putVarint(req, 1);
    req.push_back(0x18); putVarint(req, 0);
    req.push_back(0x22); putVarint(req, tunnelId.size()); req.insert(req.end(), tunnelId.begin(), tunnelId.end());
    const std::string sig = kForwardSignature;
    req.push_back(0x2A); putVarint(req, sig.size()); req.insert(req.end(), sig.begin(), sig.end());
    std::vector<std::uint8_t> frame;
    putU16(frame, static_cast<unsigned>(1 + req.size()));
    frame.push_back(kForwardRequest);
    frame.insert(frame.end(), req.begin(), req.end());
    writeFrame(frame);

    while (true) {
        const auto reply = readFrame();
        if (reply.empty()) fail("forwarding packet too short");
        if (reply[0] == kForwardHold) continue;  // the other side has not joined yet
        if (reply[0] == kForwardReply) {
            // ForwardReply { string signature = 1 }
            if (reply.size() < 3 || reply[1] != 0x0A || reply[2] + 3u > reply.size() ||
                std::string(reply.begin() + 3, reply.begin() + 3 + reply[2]) != sig) {
                fail("bad forwarding reply");
            }
            return;
        }
        if (reply[0] == kForwardError) fail("the relay refused the tunnel");
        fail("unexpected forwarding packet");
    }
}

void SdgIncomingTunnel::acceptPeerNonce(std::uint64_t n) {
    if (lastPeerNonce_ && n <= *lastPeerNonce_) fail("replayed or out-of-order packet");
    lastPeerNonce_ = n;
}

std::vector<std::uint8_t> SdgIncomingTunnel::sealWithNonce(const char* prefix, const std::vector<std::uint8_t>& plain) {
    const std::uint64_t n = nonce_++;
    std::vector<std::uint8_t> out;
    putU64(out, n);
    const auto nonce = shortNonce(prefix, n);
    std::vector<std::uint8_t> box(plain.size() + kMac);
    crypto_box_easy_afternm(box.data(), plain.data(), plain.size(), nonce.data(), shared_.data());
    out.insert(out.end(), box.begin(), box.end());
    return out;
}

SdgIncomingTunnel::Key SdgIncomingTunnel::accept(const std::string& host, unsigned port,
                                                 const std::vector<std::uint8_t>& tunnelId) {
    // Connect (with send/receive timeouts, so nothing blocks for long).
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) fail("cannot resolve " + host);
    for (addrinfo* a = res; a; a = a->ai_next) {
        const sock_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
#ifdef _WIN32
        if (s == INVALID_SOCKET) continue;
        const DWORD ms = static_cast<DWORD>(timeout_.count() * 1000);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
        if (s < 0) continue;
        timeval tv{static_cast<decltype(tv.tv_sec)>(timeout_.count()), 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
            socket_ = static_cast<std::intptr_t>(s);
            break;
        }
        closeSocket(static_cast<std::intptr_t>(s));
    }
    freeaddrinfo(res);
    if (socket_ == -1) fail("cannot connect to " + host);

    forward(tunnelId);

    auto [cmd, data] = readPacket();
    if (cmd == "TELL") {
        writePacket("WELC", std::vector<std::uint8_t>(publicKey_.begin(), publicKey_.end()));
        std::tie(cmd, data) = readPacket();
    }

    // HELO: the caller's short-term key, a nonce and a box of zeros for our long-term key.
    if (cmd != "HELO" || data.size() < kKey + kShortNonce + kHeloBox) fail("expected HELO");
    Key clientShort{};
    std::memcpy(clientShort.data(), data.data(), kKey);
    const std::uint64_t heloNonce = getU64(data.data() + kKey);
    {
        std::uint8_t zeros[kHeloBox - kMac];
        const auto n = shortNonce("CurveCP-client-H", heloNonce);
        if (crypto_box_open_easy(zeros, data.data() + kKey + kShortNonce, kHeloBox, n.data(), clientShort.data(),
                                 privateKey_.data()) != 0) {
            fail("HELO does not decrypt (wrong key?)");
        }
    }
    acceptPeerNonce(heloNonce);

    // COOK: our short-term key and a cookie, boxed with our long-term key.
    Key shortPub{}, shortPriv{};
    crypto_box_keypair(shortPub.data(), shortPriv.data());
    std::array<std::uint8_t, kCookie> cookie{};
    randombytes_buf(cookie.data(), cookie.size());
    std::array<std::uint8_t, kLongNonce> cookNonce{};
    randombytes_buf(cookNonce.data(), cookNonce.size());
    {
        std::vector<std::uint8_t> plain(shortPub.begin(), shortPub.end());
        plain.insert(plain.end(), cookie.begin(), cookie.end());
        std::vector<std::uint8_t> box(plain.size() + kMac);
        const auto n = longNonce("CurveCPK", cookNonce.data());
        crypto_box_easy(box.data(), plain.data(), plain.size(), n.data(), clientShort.data(), privateKey_.data());
        std::vector<std::uint8_t> cook(cookNonce.begin(), cookNonce.end());
        cook.insert(cook.end(), box.begin(), box.end());
        writePacket("COOK", cook);
    }

    // VOCH: the cookie, then the caller's long-term key vouching for its short-term key.
    std::tie(cmd, data) = readPacket();
    constexpr std::size_t kVouch = kKey + kLongNonce + kMac + kKey;  // inside the outer box
    if (cmd != "VOCH" || data.size() < kCookie + kShortNonce + kMac + kVouch) fail("expected VOCH");
    if (sodium_memcmp(cookie.data(), data.data(), kCookie) != 0) fail("VOCH carries a wrong cookie");
    if (crypto_box_beforenm(shared_.data(), clientShort.data(), shortPriv.data()) != 0) fail("key agreement failed");
    sodium_memzero(shortPriv.data(), shortPriv.size());
    const std::uint64_t vochNonce = getU64(data.data() + kCookie);
    const std::uint8_t* outer = data.data() + kCookie + kShortNonce;
    const std::size_t outerLen = data.size() - kCookie - kShortNonce;
    std::vector<std::uint8_t> vouch(outerLen - kMac);
    {
        const auto n = shortNonce("CurveCP-client-I", vochNonce);
        if (crypto_box_open_easy_afternm(vouch.data(), outer, outerLen, n.data(), shared_.data()) != 0) {
            fail("VOCH does not decrypt");
        }
    }
    acceptPeerNonce(vochNonce);
    Key clientKey{};
    std::memcpy(clientKey.data(), vouch.data(), kKey);
    {
        Key vouched{};
        const auto n = longNonce("CurveCPV", vouch.data() + kKey);
        if (crypto_box_open_easy(vouched.data(), vouch.data() + kKey + kLongNonce, kMac + kKey, n.data(), clientKey.data(),
                                 privateKey_.data()) != 0 ||
            sodium_memcmp(vouched.data(), clientShort.data(), kKey) != 0) {
            fail("VOCH does not vouch for the HELO key");
        }
    }

    // REDY with a single zero byte, like the grid server's (the iOS app drops an empty one).
    writePacket("REDY", sealWithNonce("CurveCP-server-R", {0}));
    return clientKey;
}

void SdgIncomingTunnel::send(const std::vector<std::uint8_t>& payload) {
    if (payload.size() > 0xffff - 8 - kShortNonce - kMac - 2) fail("message too large");
    std::vector<std::uint8_t> plain;
    putU16(plain, static_cast<unsigned>(payload.size()));
    plain.insert(plain.end(), payload.begin(), payload.end());
    writePacket("MESG", sealWithNonce("CurveCP-server-M", plain));
}

std::optional<std::vector<std::uint8_t>> SdgIncomingTunnel::receive() {
    std::pair<std::string, std::vector<std::uint8_t>> p;
    try {
        p = readPacket();
    } catch (const std::out_of_range&) {
        return std::nullopt;  // closed
    }
    const auto& [cmd, data] = p;
    if (cmd != "MESG" || data.size() < kShortNonce + kMac + 2) fail("expected MESG");
    const std::uint64_t n = getU64(data.data());
    std::vector<std::uint8_t> plain(data.size() - kShortNonce - kMac);
    const auto nonce = shortNonce("CurveCP-client-M", n);
    if (crypto_box_open_easy_afternm(plain.data(), data.data() + kShortNonce, data.size() - kShortNonce, nonce.data(),
                                     shared_.data()) != 0) {
        fail("MESG does not decrypt");
    }
    acceptPeerNonce(n);
    const std::size_t len = (plain[0] << 8) | plain[1];
    if (len > plain.size() - 2) fail("bad MESG length");
    return std::vector<std::uint8_t>(plain.begin() + 2, plain.begin() + 2 + static_cast<std::ptrdiff_t>(len));
}

}  // namespace hb
