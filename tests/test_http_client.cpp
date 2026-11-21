#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <format>
#include <thread>

#include "net/http.hpp"

using namespace hb;

// Regression: cpp-httplib normalizes query strings by default, turning %3B into ';'.
// Tasmota treats a raw ';' as a command separator, which silently truncated rules.
TEST_CASE("HttplibClient sends pre-encoded URLs byte for byte") {
    httplib::Server server;
    std::string target;
    server.Get("/cm", [&](const httplib::Request& req, httplib::Response& res) {
        target = req.target;
        res.set_content("{}", "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread t([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    HttplibClient client;
    const std::string cmnd = "Rule1 ON x DO Backlog a; b ENDON";
    const auto res = client.get(std::format("http://127.0.0.1:{}/cm?cmnd={}", port, urlEncode(cmnd)));
    server.stop();
    t.join();

    CHECK(res.status == 200);
    CHECK(target == "/cm?cmnd=Rule1%20ON%20x%20DO%20Backlog%20a%3B%20b%20ENDON");
}
