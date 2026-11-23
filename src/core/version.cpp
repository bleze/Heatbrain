#include "core/version.hpp"

#include <cctype>

namespace hb {

std::string buildRevision() {
#ifdef HEATBRAIN_REVISION
    const std::string r = HEATBRAIN_REVISION;
#else
    const std::string r = "dev";
#endif
    return r.size() > 7 && r != "dev" ? r.substr(0, 7) : r;
}

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
    std::string variant = tasmotaVariant(firmware);
    if (hardware.starts_with("ESP32")) {
        // The newer chips need their own image (a Sonoff Basic R4 is an ESP32-C3): never the
        // classic ESP32 one, even if the plug reports the generic name.
        for (const auto& [chip, suffix] : {std::pair{"ESP32-C3", "c3"}, {"ESP32-C6", "c6"}, {"ESP32-S2", "s2"}, {"ESP32-S3", "s3"}}) {
            if (hardware.starts_with(chip) && variant.starts_with("tasmota32") && !variant.starts_with(std::string("tasmota32") + suffix)) {
                variant = std::string("tasmota32") + suffix + variant.substr(9);
            }
        }
        return "http://ota.tasmota.com/tasmota32/release/" + variant + ".bin";
    }
    return "http://ota.tasmota.com/tasmota/release/" + variant + ".bin.gz";
}

}  // namespace hb
