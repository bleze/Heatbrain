#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "sensors/shelly.hpp"
#include "test_support.hpp"

using namespace hb;
using json = nlohmann::json;

namespace {
const char* kHtG3Info =
    R"({"name":null,"id":"shellyhtg3-84fce63ad204","mac":"84FCE63AD204","slot":0,"model":"S3SN-0U12A","gen":3,)"
    R"("fw_id":"20241011-114455/1.4.5-gbf870ca","ver":"1.4.5","app":"HTG3","auth_en":false,"auth_domain":null})";
const char* kHtG3Status =
    R"({"temperature:0":{"id":0,"tC":7.4,"tF":45.3},"humidity:0":{"id":0,"rh":81.2},)"
    R"("devicepower:0":{"id":0,"battery":{"V":5.9,"percent":96},"external":{"present":false}}})";
}  // namespace

TEST_CASE("Shelly: /shelly tells a Gen2+ device, a Gen1 device or something else") {
    auto d = parseShellyInfo("192.168.1.42", kHtG3Info);
    REQUIRE(d);
    CHECK(d->gen == 3);
    CHECK(d->id == "shellyhtg3-84fce63ad204");
    CHECK(d->model == "S3SN-0U12A");
    CHECK(d->app == "HTG3");
    CHECK(d->name.empty());
    CHECK_FALSE(d->auth);

    auto g1 = parseShellyInfo("192.168.1.43", R"({"type":"SHHT-1","mac":"AABBCC","auth":true,"fw":"20230913"})");
    REQUIRE(g1);
    CHECK(g1->gen == 1);
    CHECK(g1->auth);

    CHECK_FALSE(parseShellyInfo("192.168.1.44", "<html>router</html>"));
    CHECK_FALSE(parseShellyInfo("192.168.1.44", R"({"StatusFWR":{}})"));

    applyShellyStatus(*d, kHtG3Status);
    CHECK(d->sensor);
    CHECK(*d->temperature == 7.4);
    CHECK(*d->humidity == 81.2);
    CHECK(*d->battery == 96);
}

TEST_CASE("Shelly setup: replaces heatbrain's earlier webhooks and adds one for temperature and one for humidity") {
    test::FakeHttp http;
    std::vector<json> calls;
    http.requestHandler = [&](const HttpRequest& r) -> HttpResponse {
        if (r.url == "http://192.168.1.42/shelly") return {200, kHtG3Info};
        const json call = json::parse(r.body);
        calls.push_back(call);
        const std::string m = call["method"];
        if (m == "Shelly.GetStatus") return {200, json{{"id", 1}, {"result", json::parse(kHtG3Status)}}.dump()};
        if (m == "Webhook.List") {
            return {200, R"({"id":1,"result":{"hooks":[
                {"id":3,"cid":0,"event":"temperature.change","urls":["http://old:8090/api/push/shelly?id=shed&temp=${ev.tC}"]},
                {"id":4,"cid":0,"event":"temperature.change","urls":["http://example.org/other"]}],"rev":5}})"};
        }
        return {200, R"({"id":1,"result":{"rev":6}})"};
    };
    const auto r = setupShelly(http, "192.168.1.42", "http://192.168.1.10:8090/", "Shed");
    CHECK(r.removed == 1);
    CHECK(r.device.id == "shed");  // as heatbrain set it up before: the same sensor
    CHECK(r.device.name == "Shed");
    CHECK(*r.device.temperature == 7.4);

    std::vector<json> created;
    for (const auto& c : calls) {
        if (c["method"] == "Webhook.Delete") CHECK(c["params"]["id"] == 3);  // not the other one
        if (c["method"] == "Webhook.Create") created.push_back(c["params"]);
    }
    REQUIRE(created.size() == 2);
    CHECK(created[0]["event"] == "temperature.change");
    CHECK(created[0]["urls"][0] ==
          "http://192.168.1.10:8090/api/push/shelly?id=shed&name=Shed&temp=${ev.tC}");
    CHECK(created[1]["event"] == "humidity.change");
    CHECK(created[1]["urls"][0] ==
          "http://192.168.1.10:8090/api/push/shelly?id=shed&name=Shed&hum=${ev.rh}");
}

TEST_CASE("Shelly setup: a sleeping, password protected or sensorless Shelly gives a readable reason") {
    test::FakeHttp http;
    http.handler = [](const std::string&) -> HttpResponse { throw HttpError("timeout"); };
    CHECK_THROWS_WITH(setupShelly(http, "192.168.1.42", "http://hb", "Shed"), Catch::Matchers::ContainsSubstring("press its button"));

    http.handler = [](const std::string&) -> HttpResponse {
        return {200, R"({"id":"shellyplus1-1","model":"SNSW-001X16EU","gen":2,"auth_en":true})"};
    };
    CHECK_THROWS(setupShelly(http, "192.168.1.42", "http://hb", "Shed"));

    http.handler = nullptr;
    http.requestHandler = [](const HttpRequest& r) -> HttpResponse {
        if (r.url.ends_with("/shelly")) return {200, R"({"id":"shellyplus1-1","model":"SNSW-001X16EU","gen":2,"auth_en":false})"};
        return {200, R"({"id":1,"result":{"switch:0":{"output":false}}})"};
    };
    CHECK_THROWS(setupShelly(http, "192.168.1.42", "http://hb", "Shed"));
}

TEST_CASE("Shelly set up by hand: the scan recognises it and Set up again keeps it the same sensor") {
    DiscoveredShelly d;
    applyShellyHooks(d, R"({"hooks":[{"id":1,"event":"temperature.change",
        "urls":["http://192.168.1.10:8090/api/push/shelly?id=shed&name=The%20shed&temp=${ev.tC}"]}],"rev":2})");
    CHECK(d.pushId == "shed");
    CHECK(d.pushName == "The shed");

    test::FakeHttp http;
    std::vector<json> created;
    http.requestHandler = [&](const HttpRequest& r) -> HttpResponse {
        if (r.url.ends_with("/shelly")) return {200, kHtG3Info};
        const json call = json::parse(r.body);
        if (call["method"] == "Shelly.GetStatus") return {200, json{{"id", 1}, {"result", json::parse(kHtG3Status)}}.dump()};
        if (call["method"] == "Webhook.List") {
            return {200, R"({"id":1,"result":{"hooks":[{"id":1,"event":"temperature.change",
                "urls":["http://192.168.1.10:8090/api/push/shelly?id=shed&name=The%20shed&temp=${ev.tC}"]}]}})"};
        }
        if (call["method"] == "Webhook.Create") created.push_back(call["params"]);
        return {200, R"({"id":1,"result":{}})"};
    };
    const auto r = setupShelly(http, "192.168.1.42", "http://192.168.1.10:8090", "");
    CHECK(r.removed == 1);
    CHECK(r.device.id == "shed");
    CHECK(r.device.name == "The shed");
    REQUIRE(created.size() == 2);
    CHECK(created[0]["urls"][0] == "http://192.168.1.10:8090/api/push/shelly?id=shed&name=The%20shed&temp=${ev.tC}");
}
