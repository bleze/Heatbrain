#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <fstream>

#include "app/transfer.hpp"
#include "core/base64.hpp"
#include "storage/history.hpp"
#include "test_support.hpp"

using namespace hb;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

json config(const std::string& tz = "Europe/Copenhagen") {
    return json{{"timezone", tz},
                {"price", {{"components", json::array({{{"type", "fixed"}, {"name", "Tax"}, {"amount", 0.1}}})}}},
                {"plugs", json::array({{{"id", "bedroom"}, {"type", "simulated"}, {"mode", "dryrun"}, {"password", "pw"}}})}};
}

void writeJson(const fs::path& file, const json& j) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << j.dump();
}

json readJson(const fs::path& file) { return json::parse(std::ifstream(file)); }

}  // namespace

TEST_CASE("Base64 round trip, including binary and padding") {
    for (const std::string s : {std::string(""), std::string("f"), std::string("fo"), std::string("foo"),
                                std::string("foob"), std::string("\0\xff\x10 binary", 10)}) {
        CHECK(base64Decode(base64Encode(s)) == s);
    }
    CHECK(base64Encode("foobar") == "Zm9vYmFy");
    CHECK(base64Encode("fo") == "Zm8=");
    CHECK_THROWS(base64Decode("Zm9v!"));
}

TEST_CASE("Only heatbrain's own state files travel") {
    CHECK(isTransferablePath("netatmo-tokens.json"));
    CHECK(isTransferablePath("away.json"));
    CHECK(isTransferablePath("plugs/bedroom/firmware.json"));
    CHECK(isTransferablePath("plugs/bedroom/backup-20261121T203903Z.json"));
    CHECK_FALSE(isTransferablePath("heatbrain.json"));  // travels as "config"
    CHECK_FALSE(isTransferablePath("../etc/passwd"));
    CHECK_FALSE(isTransferablePath("plugs/../../x/firmware.json"));
    CHECK_FALSE(isTransferablePath("cache/abc.cache"));
    CHECK_FALSE(isTransferablePath("/netatmo-tokens.json"));
}

TEST_CASE("Export then import moves config, state files and history") {
    using namespace std::chrono;
    test::TempDir from, to;
    writeJson(from.path() / "heatbrain.json", config("Europe/Copenhagen"));
    writeJson(from.path() / "netatmo-tokens.json", {{"refreshToken", "rt-1"}});
    writeJson(from.path() / "plugs/bedroom/firmware.json", {{"version", "16.0.0"}, {"verified", true}});
    writeJson(from.path() / "cache/x.json", {{"not", "exported"}});
    {
        History h(from.path() / "history.db");
        h.recordTemperature({Instant{seconds{1795300000}}, "netatmo:a", "bedroom", 19.5, std::nullopt});
    }

    const json bundle = buildExport(from.path(), from.path() / "heatbrain.json", /*withHistory=*/true);
    CHECK(bundle["format"] == "heatbrain-export");
    CHECK(bundle["config"]["plugs"][0]["password"] == "pw");  // secrets included, it's a move
    CHECK(bundle["files"].contains("netatmo-tokens.json"));
    CHECK(bundle["files"].contains("plugs/bedroom/firmware.json"));
    CHECK_FALSE(bundle["files"].contains("cache/x.json"));
    REQUIRE(bundle.contains("history"));

    // The target has its own config and history, which are replaced.
    writeJson(to.path() / "heatbrain.json", config("Europe/Berlin"));
    {
        History h(to.path() / "history.db");
        h.recordTemperature({Instant{seconds{1795200000}}, "netatmo:old", "old", 10.0, std::nullopt});
    }
    const auto r = applyImport(bundle, to.path(), to.path() / "heatbrain.json");
    CHECK(r.files == 2);
    CHECK(r.history);
    CHECK(readJson(to.path() / "heatbrain.json")["timezone"] == "Europe/Copenhagen");
    CHECK(readJson(to.path() / "netatmo-tokens.json")["refreshToken"] == "rt-1");
    CHECK(fs::exists(to.path() / "config-backups"));  // the replaced config is kept
    History h(to.path() / "history.db");
    const auto temps = h.temperatures("bedroom", Instant{}, Instant{seconds{1795400000}});
    REQUIRE(temps.size() == 1);
    CHECK(temps[0].temperature == 19.5);
    CHECK(h.temperatures("old", Instant{}, Instant{seconds{1795400000}}).empty());
}

TEST_CASE("A bad import changes nothing") {
    test::TempDir dir;
    writeJson(dir.path() / "heatbrain.json", config("Europe/Berlin"));
    const auto unchanged = [&] { return readJson(dir.path() / "heatbrain.json")["timezone"] == "Europe/Berlin"; };

    CHECK_THROWS_WITH(applyImport(json{{"hello", 1}}, dir.path(), dir.path() / "heatbrain.json"),
                      ContainsSubstring("not a heatbrain export"));
    json evil{{"format", "heatbrain-export"}, {"version", 1}, {"config", config()},
              {"files", {{"../../evil.json", json::object()}}}};
    CHECK_THROWS_WITH(applyImport(evil, dir.path(), dir.path() / "heatbrain.json"), ContainsSubstring("Unexpected file"));
    json invalid{{"format", "heatbrain-export"}, {"version", 1}, {"config", {{"timezone", "Mars/Olympus"}}}};
    CHECK_THROWS_WITH(applyImport(invalid, dir.path(), dir.path() / "heatbrain.json"), ContainsSubstring("not valid"));
    json damaged{{"format", "heatbrain-export"}, {"version", 1}, {"config", config()}, {"history", "bm90IGEgZGI="}};
    CHECK_THROWS_WITH(applyImport(damaged, dir.path(), dir.path() / "heatbrain.json"), ContainsSubstring("damaged"));
    CHECK(unchanged());
    CHECK_FALSE(fs::exists(dir.path() / "evil.json"));
}
