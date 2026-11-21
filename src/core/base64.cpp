#include "core/base64.hpp"

#include <array>
#include <stdexcept>

namespace hb {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<int, 256> makeReverse() {
    std::array<int, 256> r{};
    for (auto& v : r) v = -1;
    for (int i = 0; i < 64; ++i) r[static_cast<unsigned char>(kAlphabet[i])] = i;
    return r;
}
constexpr auto kReverse = makeReverse();
}  // namespace

std::string base64Encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (const size_t rest = bytes.size() - i; rest > 0) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (rest == 2) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(std::string_view text) {
    std::string out;
    out.reserve(text.size() / 4 * 3);
    unsigned acc = 0;
    int bits = 0;
    bool padding = false;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') {
            padding = true;
            continue;
        }
        const int v = kReverse[static_cast<unsigned char>(c)];
        if (v < 0 || padding) throw std::invalid_argument("invalid base64 data");
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

}  // namespace hb
