#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <functional>

#include "app/cameras.hpp"
#include "app/config_service.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;

namespace {
const std::string kJpeg = std::string("\xFF\xD8\xFF\xE0", 4) + "picture";
}

TEST_CASE("Cameras: parsed from the config, with readable errors") {
    const auto cams = parseCameras(json::parse(R"({"cameras": [
        {"id": "garden", "name": "Garden", "host": "192.168.1.61"},
        {"id": "drive", "host": "192.168.1.63:8080", "user": "viewer", "password": "pw"}]})"));
    REQUIRE(cams.size() == 2);
    CHECK(cams[1].name == "drive");
    CHECK(snapshotUrl(cams[1]) == "http://192.168.1.63:8080/cgi-bin/viewer/video.jpg");
    CHECK(parseCameras(json::object()).empty());
    CHECK_THROWS(parseCameras(json::parse(R"({"cameras": [{"id": "a", "host": "x"}, {"id": "a", "host": "y"}]})")));
    CHECK_THROWS(parseCameras(json::parse(R"({"cameras": [{"id": "a", "host": ""}]})")));
    CHECK_THROWS(parseCameras(json::parse(R"({"cameras": [{"id": "a", "host": "http://192.168.1.61/"}]})")));
}

TEST_CASE("Cameras: the password is masked for the browser and kept on save") {
    const json stored = json::parse(R"({"cameras": [{"id": "drive", "host": "h", "user": "viewer", "password": "secret"}]})");
    const json masked = maskSecrets(stored);
    CHECK(masked["cameras"][0]["password"] != "secret");
    CHECK(unmaskSecrets(masked, stored)["cameras"][0]["password"] == "secret");
}

TEST_CASE("Cameras: pictures are fetched with the login, shared while fresh, and failures explained") {
    test::FakeHttp http;
    Instant now = sys_days{2026y / 11 / 28} + hours{12};
    CameraHub hub(http, [&] { return now; });
    hub.configure({{"garden", "Garden", "192.168.1.61", "", ""}, {"drive", "Drive", "192.168.1.63", "viewer", "pw"}});

    http.requestHandler = [&](const HttpRequest& req) {
        for (const auto& [k, v] : req.headers) {
            if (k == "Authorization") CHECK(v == "Basic dmlld2VyOnB3");  // viewer:pw
        }
        return HttpResponse{200, kJpeg, std::nullopt};
    };
    CHECK(*hub.picture("drive", seconds{30}).jpeg == kJpeg);
    now += seconds{10};
    hub.picture("drive", seconds{30});
    CHECK(http.calls == 1);  // still fresh: the same picture
    now += seconds{30};
    hub.picture("drive", seconds{30});
    CHECK(http.calls == 2);
    CHECK_THROWS_AS(hub.picture("nope", seconds{30}), std::out_of_range);

    http.requestHandler = [](const HttpRequest&) { return HttpResponse{401, "", std::nullopt}; };
    CHECK_THROWS_WITH(hub.picture("garden", seconds{0}), "asks for a login: set user and password");
    http.requestHandler = [](const HttpRequest&) { return HttpResponse{200, "<html>", std::nullopt}; };
    CHECK_THROWS_WITH(hub.picture("garden", seconds{0}), "did not send a picture");
    http.requestHandler = [](const HttpRequest&) -> HttpResponse { throw HttpError("timed out"); };
    CHECK_THROWS_WITH(hub.picture("drive", seconds{0}), "not answering (timed out)");

    const auto st = hub.status();
    REQUIRE(st.size() == 2);
    CHECK_FALSE(st[1].ok);
    CHECK(st[1].lastPicture == now);  // the last good one is still there
    // Same camera after a config reload: its state stays; a new login starts afresh.
    hub.configure({{"drive", "Driveway", "192.168.1.63", "viewer", "pw"}});
    CHECK(hub.status()[0].name == "Driveway");
    CHECK(hub.status()[0].lastPicture);
    hub.configure({{"drive", "Driveway", "192.168.1.63", "viewer", "new"}});
    CHECK_FALSE(hub.status()[0].lastPicture);
}

namespace {
// The scan asks 64 addresses at once: a fake that may be called from many threads.
struct ParallelFake final : HttpClient {
    std::function<HttpResponse(const std::string&)> handler;
    HttpResponse send(const HttpRequest& req) override { return handler(req.url); }
};
}  // namespace

TEST_CASE("Cameras: found on the network by VIVOTEK's own answers") {
    ParallelFake http;
    http.handler = [](const std::string& url) {
        if (url.starts_with("http://10.0.0.61/cgi-bin/viewer/")) {
            return HttpResponse{200, "system_info_modelname='FD8134'\r\nsystem_info_firmwareversion='FD8134-VVTK-0301c'\r\nsystem_hostname='Garden'\r\n", std::nullopt};
        }
        if (url.starts_with("http://10.0.0.61/cgi-bin/admin/")) return HttpResponse{200, "system_hostname='Garden'\r\n", std::nullopt};
        if (url.starts_with("http://10.0.0.63/")) {
            return HttpResponse{401, "Your client does not have permission to get URL /cgi-bin/viewer/getparam.cgi from this server.", std::nullopt};
        }
        if (url.starts_with("http://10.0.0.7/")) return HttpResponse{401, "Unauthorized", std::nullopt};  // a router
        return HttpResponse{404, "", std::nullopt};
    };
    auto found = discoverCameras("10.0.0.0/24", http);
    std::ranges::sort(found, {}, &DiscoveredCamera::host);
    REQUIRE(found.size() == 2);
    CHECK(found[0].host == "10.0.0.61");
    CHECK(found[0].model == "FD8134");
    CHECK(found[0].firmware == "FD8134-VVTK-0301c");
    CHECK(found[0].hostName == "Garden");
    CHECK(found[0].openAdmin);
    CHECK_FALSE(found[0].needsLogin);
    CHECK(found[1].host == "10.0.0.63");
    CHECK(found[1].needsLogin);
    CHECK_THROWS(discoverCameras("10.0.0.0/16", http));
}
