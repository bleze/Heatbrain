#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hb {

// "14.6.0(release-tasmota)", "v16.0.0", "16.0.0.1" -> {14, 6, 0} etc. Empty if no digits.
std::vector<int> parseVersion(std::string_view s);

// <0, 0, >0 like strcmp. Missing trailing parts count as 0 (16.0 == 16.0.0).
int compareVersions(std::string_view a, std::string_view b);

// Build variant from a Tasmota version string: "14.6.0(release-tasmota-sensors)" ->
// "tasmota-sensors"; defaults to "tasmota".
std::string tasmotaVariant(std::string_view firmware);

// Official OTA URL for the latest release of the same variant. ESP8266/ESP8285 use the
// compressed image (Tasmota installs the minimal build first on 1 MB devices by itself).
std::string tasmotaOtaUrl(std::string_view firmware, std::string_view hardware);

}  // namespace hb
