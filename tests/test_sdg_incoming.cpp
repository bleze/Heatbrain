// The accepting side of a SecureDeviceGrid tunnel against a simulated relay + caller (what the
// DEVI iOS app does after the relay has joined both ends), modelled on TestMdgClient from
// soundvibe/ha-danfoss PR #45.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sodium.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { ::close(s); }
#endif

#include "devices/sdg_incoming.hpp"

using namespace hb;
using Bytes = std::vector<std::uint8_t>;

namespace {

struct Net {
    Net() {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        REQUIRE(sodium_init() >= 0);
    }
};

void sendAll(sock_t s, const Bytes& b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const auto n = ::send(s, reinterpret_cast<const char*>(b.data() + off), static_cast<int>(b.size() - off), 0);
        if (n <= 0) throw std::runtime_error("send");
        off += static_cast<std::size_t>(n);
    }
}
Bytes recvExact(sock_t s, std::size_t n) {
    Bytes b(n);
    std::size_t off = 0;
    while (off < n) {
        const auto got = ::recv(s, reinterpret_cast<char*>(b.data() + off), static_cast<int>(n - off), 0);
        if (got <= 0) throw std::runtime_error("recv");
        off += static_cast<std::size_t>(got);
    }
    return b;
}
Bytes recvFrame(sock_t s) {
    const auto len = recvExact(s, 2);
    return recvExact(s, static_cast<std::size_t>((len[0] << 8) | len[1]));
}
void cat(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
void cat(Bytes& a, const std::uint8_t* p, std::size_t n) { a.insert(a.end(), p, p + n); }
Bytes be64(std::uint64_t v) {
    Bytes b;
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
    return b;
}
std::array<std::uint8_t, 24> shortNonce(const char* prefix, std::uint64_t v) {
    std::array<std::uint8_t, 24> n{};
    std::memcpy(n.data(), prefix, 16);
    for (int i = 0; i < 8; ++i) n[16 + i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
    return n;
}
std::array<std::uint8_t, 24> longNonce(const char* prefix, const std::uint8_t* v) {
    std::array<std::uint8_t, 24> n{};
    std::memcpy(n.data(), prefix, 8);
    std::memcpy(n.data() + 8, v, 16);
    return n;
}
Bytes packet(const char* cmd, const Bytes& data) {
    Bytes f{static_cast<std::uint8_t>((8 + data.size()) >> 8), static_cast<std::uint8_t>(8 + data.size()), 0xf0, 0x9f, 0x90, 0x9f};
    cat(f, reinterpret_cast<const std::uint8_t*>(cmd), 4);
    cat(f, data);
    return f;
}
Bytes readPacket(sock_t s, const char* expect) {
    const auto f = recvFrame(s);
    if (f.size() < 8 || std::memcmp(f.data() + 4, expect, 4) != 0) throw std::runtime_error(std::string("expected ") + expect);
    return Bytes(f.begin() + 8, f.end());
}

// The relay and the phone in one: answers the forward request, then runs the caller's handshake,
// receives the configuration request and answers with a chunked house configuration.
#define PHONE_CHECK(cond) do { if (!(cond)) throw std::runtime_error("phone check failed: " #cond); } while (0)

struct FakePhone {
    std::array<std::uint8_t, 32> pub{}, priv{};
    Bytes tunnelId{1, 2, 3, 4, 5};
    std::string receivedRequest;
    std::string error;
    bool corruptVouch = false;

    void run(sock_t s, const std::string& house) {
        try {
            // Forwarding: request (type 0) carrying our tunnel id; a HOLD, then the REPLY.
            const auto req = recvFrame(s);
            PHONE_CHECK(!req.empty());
            PHONE_CHECK(req[0] == 0);
            PHONE_CHECK(std::search(req.begin(), req.end(), tunnelId.begin(), tunnelId.end()) != req.end());
            sendAll(s, Bytes{0, 1, 1});
            const std::string sig = "Mdg-NaCl/binary";
            Bytes reply{0, static_cast<std::uint8_t>(3 + sig.size()), 2, 0x0A, static_cast<std::uint8_t>(sig.size())};
            cat(reply, reinterpret_cast<const std::uint8_t*>(sig.data()), sig.size());
            sendAll(s, reply);

            std::uint64_t nonce = 0;
            sendAll(s, packet("TELL", {}));
            const auto serverKey = readPacket(s, "WELC");
            PHONE_CHECK(serverKey.size() == 32);

            std::array<std::uint8_t, 32> stPub{}, stPriv{};
            crypto_box_keypair(stPub.data(), stPriv.data());
            const std::uint64_t heloNonce = nonce++;
            Bytes zeros(64, 0), zbox(80);
            auto n = shortNonce("CurveCP-client-H", heloNonce);
            crypto_box_easy(zbox.data(), zeros.data(), 64, n.data(), serverKey.data(), stPriv.data());
            Bytes helo(stPub.begin(), stPub.end());
            cat(helo, be64(heloNonce));
            cat(helo, zbox);
            sendAll(s, packet("HELO", helo));

            const auto cook = readPacket(s, "COOK");
            Bytes cookPlain(cook.size() - 16 - 16);
            auto cn = longNonce("CurveCPK", cook.data());
            PHONE_CHECK(crypto_box_open_easy(cookPlain.data(), cook.data() + 16, cook.size() - 16, cn.data(), serverKey.data(),
                                         stPriv.data()) == 0);
            std::array<std::uint8_t, 32> shared{};
            crypto_box_beforenm(shared.data(), cookPlain.data(), stPriv.data());

            std::array<std::uint8_t, 16> vn{};
            randombytes_buf(vn.data(), vn.size());
            Bytes vouch(48);
            auto vnn = longNonce("CurveCPV", vn.data());
            crypto_box_easy(vouch.data(), stPub.data(), 32, vnn.data(), serverKey.data(), priv.data());
            if (corruptVouch) vouch[20] ^= 1;
            Bytes inner(pub.begin(), pub.end());
            cat(inner, vn.data(), 16);
            cat(inner, vouch);
            inner.push_back(0);  // the phone sends a trailing byte
            const std::uint64_t vochNonce = nonce++;
            Bytes initiate(inner.size() + 16);
            auto in = shortNonce("CurveCP-client-I", vochNonce);
            crypto_box_easy_afternm(initiate.data(), inner.data(), inner.size(), in.data(), shared.data());
            Bytes voch(cookPlain.begin() + 32, cookPlain.begin() + 128);
            cat(voch, be64(vochNonce));
            cat(voch, initiate);
            sendAll(s, packet("VOCH", voch));

            const auto redy = readPacket(s, "REDY");
            Bytes redyPlain(redy.size() - 8 - 16);
            std::uint64_t rn = 0;
            for (int i = 0; i < 8; ++i) rn = (rn << 8) | redy[i];
            auto rnn = shortNonce("CurveCP-server-R", rn);
            PHONE_CHECK(crypto_box_open_easy_afternm(redyPlain.data(), redy.data() + 8, redy.size() - 8, rnn.data(), shared.data()) == 0);
            PHONE_CHECK(redyPlain == Bytes{0});

            // The configuration request, then the house in two chunks after the size header.
            const auto mesg = readPacket(s, "MESG");
            std::uint64_t mn = 0;
            for (int i = 0; i < 8; ++i) mn = (mn << 8) | mesg[i];
            Bytes plain(mesg.size() - 8 - 16);
            auto mnn = shortNonce("CurveCP-server-M", mn);
            PHONE_CHECK(crypto_box_open_easy_afternm(plain.data(), mesg.data() + 8, mesg.size() - 8, mnn.data(), shared.data()) == 0);
            receivedRequest.assign(plain.begin() + 2, plain.begin() + 2 + ((plain[0] << 8) | plain[1]));

            auto sendMesg = [&](const Bytes& payload) {
                Bytes p{static_cast<std::uint8_t>(payload.size() >> 8), static_cast<std::uint8_t>(payload.size())};
                cat(p, payload);
                const std::uint64_t pn = nonce++;
                Bytes box(p.size() + 16);
                auto pnn = shortNonce("CurveCP-client-M", pn);
                crypto_box_easy_afternm(box.data(), p.data(), p.size(), pnn.data(), shared.data());
                Bytes data = be64(pn);
                cat(data, box);
                sendAll(s, packet("MESG", data));
            };
            const auto len = static_cast<std::uint32_t>(house.size());
            Bytes first{0, 0, 0, 0, static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8),
                        static_cast<std::uint8_t>(len >> 16), static_cast<std::uint8_t>(len >> 24)};
            const auto half = house.size() / 2;
            cat(first, reinterpret_cast<const std::uint8_t*>(house.data()), half);
            sendMesg(first);
            sendMesg(Bytes(house.begin() + static_cast<std::ptrdiff_t>(half), house.end()));
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
};

struct Listener {
    sock_t s;
    unsigned port = 0;
    Listener() {
        s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        REQUIRE(::listen(s, 1) == 0);
        socklen_t l = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
    }
    ~Listener() { closeSock(s); }
};

}  // namespace

TEST_CASE("Incoming SDG tunnel: joins the relay, completes the handshake and receives a chunked house") {
    Net net;
    Listener l;
    FakePhone phone;
    crypto_box_keypair(phone.pub.data(), phone.priv.data());
    SdgIncomingTunnel::Key ours{};
    randombytes_buf(ours.data(), ours.size());

    const std::string house = R"({"houseName":"Pizzeria","rooms":[{"roomName":"Pizzeria","peerId":"ab"}]})";
    std::thread t([&] {
        const sock_t c = ::accept(l.s, nullptr, nullptr);
        phone.run(c, house);
        closeSock(c);
    });

    const std::string request = R"({"phoneName":"heatbrain"})";
    Bytes got;
    SdgIncomingTunnel::Key caller{};
    std::string ourError;
    try {
        SdgIncomingTunnel tunnel(ours, std::chrono::seconds{5});
        caller = tunnel.accept("127.0.0.1", l.port, phone.tunnelId);
        tunnel.send(Bytes(request.begin(), request.end()));
        while (auto p = tunnel.receive()) got.insert(got.end(), p->begin(), p->end());
    } catch (const std::exception& e) {
        ourError = e.what();
    }  // the tunnel closes here, which also ends the phone's side
    t.join();
    CHECK(ourError.empty());
    INFO(ourError);
    CHECK(caller == phone.pub);

    INFO(phone.error);
    CHECK(phone.error.empty());
    CHECK(phone.receivedRequest == request);
    REQUIRE(got.size() == 8 + house.size());
    CHECK(std::string(got.begin() + 8, got.end()) == house);
}

TEST_CASE("Incoming SDG tunnel: a vouch that does not match is refused") {
    Net net;
    Listener l;
    FakePhone phone;
    phone.corruptVouch = true;
    crypto_box_keypair(phone.pub.data(), phone.priv.data());
    SdgIncomingTunnel::Key ours{};
    randombytes_buf(ours.data(), ours.size());
    std::thread t([&] {
        const sock_t c = ::accept(l.s, nullptr, nullptr);
        phone.run(c, "{}");
        closeSock(c);
    });
    bool threw = false;
    try {
        SdgIncomingTunnel tunnel(ours, std::chrono::seconds{2});
        tunnel.accept("127.0.0.1", l.port, phone.tunnelId);
    } catch (const std::exception&) {
        threw = true;
    }
    t.join();
    CHECK(threw);
}
