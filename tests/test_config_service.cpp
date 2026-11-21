#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>

#include "app/config_service.hpp"
#include "test_support.hpp"

using namespace hb;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

json sampleConfig() {
    return json::parse(R"({
      "timezone": "Europe/Copenhagen",
      "currency": "DKK",
      "price": { "components": [ { "type": "fixed", "name": "Tax", "amount": 0.1 } ] },
      "plugs": [ { "id": "bedroom", "type": "simulated", "mode": "dryrun", "password": "plugpw" } ],
      "rooms": [ { "id": "bedroom", "plugs": ["bedroom"], "sensor": "netatmo:Bedroom",
                   "schedule": [ { "from": "00:00", "to": "24:00", "target": 19 } ] } ],
      "temperatureSources": [ { "type": "netatmo", "clientId": "cid", "clientSecret": "s3cret", "refreshToken": "rt" } ],
      "notifications": { "channels": [ { "type": "discord", "webhookUrl": "https://discord.com/api/webhooks/1/x" } ] }
    })");
}

}  // namespace

TEST_CASE("Secrets never reach the browser") {
    const json masked = maskSecrets(sampleConfig());
    const std::string text = masked.dump();
    CHECK(text.find("s3cret") == std::string::npos);
    CHECK(text.find("plugpw") == std::string::npos);
    CHECK(text.find("webhooks") == std::string::npos);
    CHECK(masked["temperatureSources"][0]["clientId"] == "cid");  // not a secret
    CHECK(masked["temperatureSources"][0]["clientSecret"] == kSecretMask);
}

TEST_CASE("Masked secrets sent back keep their stored values; edited ones change") {
    const json stored = sampleConfig();
    json incoming = maskSecrets(stored);
    incoming["notifications"]["channels"][0]["webhookUrl"] = "https://discord.com/api/webhooks/2/new";
    const json merged = unmaskSecrets(incoming, stored);
    CHECK(merged["temperatureSources"][0]["clientSecret"] == "s3cret");
    CHECK(merged["temperatureSources"][0]["refreshToken"] == "rt");
    CHECK(merged["plugs"][0]["password"] == "plugpw");
    CHECK(merged["notifications"]["channels"][0]["webhookUrl"] == "https://discord.com/api/webhooks/2/new");
}

TEST_CASE("A new item with a masked secret never stores the mask") {
    json incoming = sampleConfig();
    incoming["plugs"].push_back({{"id", "new"}, {"type", "simulated"}, {"password", kSecretMask}});
    const json merged = unmaskSecrets(incoming, json::object());
    CHECK(merged["plugs"][1]["password"] == "");
}

TEST_CASE("Validation reports every broken part, without network access") {
    test::TempDir dir;
    CHECK(validateConfig(sampleConfig(), dir.path()).empty());

    json bad = sampleConfig();
    bad["timezone"] = "Mars/Olympus";
    bad["rooms"][0]["plugs"] = json::array({"nope"});
    bad["notifications"]["channels"][0]["webhookUrl"] = "http://insecure";
    const auto errors = validateConfig(bad, dir.path());
    REQUIRE(errors.size() == 3);
    CHECK_THAT(errors[0], ContainsSubstring("Time zone"));
    CHECK_THAT(errors[1], ContainsSubstring("unknown plug"));
    CHECK_THAT(errors[2], ContainsSubstring("https"));
}

TEST_CASE("Live plugs need a tick well inside their fail-safe pulse") {
    test::TempDir dir;
    json cfg = sampleConfig();
    cfg["plugs"][0]["mode"] = "live";
    cfg["control"] = {{"tickSeconds", 900}};  // 15 min vs 20 min pulse
    CHECK_THAT(validateConfig(cfg, dir.path()).at(0), ContainsSubstring("half"));
}

TEST_CASE("An empty config (fresh install) is valid") {
    test::TempDir dir;
    const json empty = json::parse(R"({"price":{"components":[]}})");
    CHECK(validateConfig(empty, dir.path()).empty());
}

TEST_CASE("Saving keeps a backup of the previous version") {
    test::TempDir dir;
    const auto file = dir.path() / "heatbrain.json";
    saveConfig(file, json{{"v", 1}});
    saveConfig(file, json{{"v", 2}});
    std::ifstream in(file);
    CHECK(json::parse(in)["v"] == 2);
    int backups = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir.path() / "config-backups")) {
        (void)e;
        ++backups;
    }
    CHECK(backups >= 1);
}
