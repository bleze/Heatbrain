#include <catch2/catch_test_macros.hpp>

#include "core/version.hpp"

using namespace hb;

TEST_CASE("Version parsing and comparison") {
    CHECK(parseVersion("14.6.0(release-tasmota)") == std::vector<int>{14, 6, 0});
    CHECK(parseVersion("v16.0.0") == std::vector<int>{16, 0, 0});
    CHECK(parseVersion("16.0.0.1(tasmota)") == std::vector<int>{16, 0, 0, 1});
    CHECK(parseVersion("unknown").empty());

    CHECK(compareVersions("14.6.0(release-tasmota)", "v16.0.0") < 0);
    CHECK(compareVersions("v16.0.0", "16.0") == 0);
    CHECK(compareVersions("16.0.0.1", "16.0.0") > 0);
    CHECK(compareVersions("14.10.0", "14.9.9") > 0);  // numeric, not lexical
}

TEST_CASE("Tasmota variant and OTA URL") {
    CHECK(tasmotaVariant("14.6.0(release-tasmota)") == "tasmota");
    CHECK(tasmotaVariant("14.6.0(release-tasmota-sensors)") == "tasmota-sensors");
    CHECK(tasmotaVariant("14.6.0") == "tasmota");

    CHECK(tasmotaOtaUrl("14.6.0(release-tasmota)", "ESP8285N08") ==
          "http://ota.tasmota.com/tasmota/release/tasmota.bin.gz");
    CHECK(tasmotaOtaUrl("15.1.0(release-tasmota32)", "ESP32-C3") ==
          "http://ota.tasmota.com/tasmota32/release/tasmota32.bin");
}
