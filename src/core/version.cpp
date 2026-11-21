#include "core/version.hpp"

#include <cctype>

namespace hb {

std::vector<int> parseVersion(std::string_view s) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    while (i < s.size()) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) break;
        int v = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
        out.push_back(v);
        if (i < s.size() && s[i] == '.') ++i;
        else break;
    }
    return out;
}

int compareVersions(std::string_view a, std::string_view b) {
    const auto va = parseVersion(a), vb = parseVersion(b);
    for (std::size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
        const int x = i < va.size() ? va[i] : 0, y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string tasmotaVariant(std::string_view firmware) {
    const auto open = firmware.find('(');
    const auto close = firmware.find(')', open);
    if (open == std::string_view::npos || close == std::string_view::npos) return "tasmota";
    std::string_view inner = firmware.substr(open + 1, close - open - 1);
    if (inner.starts_with("release-")) inner.remove_prefix(8);
    return inner.empty() ? "tasmota" : std::string(inner);
}

std::string tasmotaOtaUrl(std::string_view firmware, std::string_view hardware) {
    const std::string variant = tasmotaVariant(firmware);
    if (hardware.starts_with("ESP32")) return "http://ota.tasmota.com/tasmota32/release/" + variant + ".bin";
    return "http://ota.tasmota.com/tasmota/release/" + variant + ".bin.gz";
}

}  // namespace hb
