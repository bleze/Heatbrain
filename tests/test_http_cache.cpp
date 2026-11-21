#include <catch2/catch_test_macros.hpp>

#include "net/http.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;

TEST_CASE("Fresh cache entries are served without a request") {
    test::TempDir dir;
    test::FakeHttp http;
    http.handler = [](const std::string&) { return HttpResponse{200, "v1"}; };
    CachingHttpClient cache(http, dir.path());

    CHECK(cache.get("https://x/a", hours{1}).body == "v1");
    CHECK(cache.get("https://x/a", hours{1}).body == "v1");
    CHECK(http.calls == 1);
    cache.get("https://x/b", hours{1});
    CHECK(http.calls == 2);  // different URL, different entry
}

TEST_CASE("Expired entries are refetched") {
    test::TempDir dir;
    test::FakeHttp http;
    int n = 0;
    http.handler = [&](const std::string&) { return HttpResponse{200, "v" + std::to_string(++n)}; };
    CachingHttpClient cache(http, dir.path());

    CHECK(cache.get("https://x/a", seconds{0}).body == "v1");
    CHECK(cache.get("https://x/a", seconds{0}).body == "v2");
}

TEST_CASE("Failures fall back to a stale copy, or throw if there is none") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());

    http.handler = [](const std::string&) { return HttpResponse{429, "slow down"}; };
    CHECK_THROWS_AS(cache.get("https://x/a", seconds{0}), HttpError);

    http.handler = [](const std::string&) { return HttpResponse{200, "good"}; };
    cache.get("https://x/a", seconds{0});

    http.handler = [](const std::string&) -> HttpResponse { throw HttpError("network down"); };
    const auto r = cache.get("https://x/a", seconds{0});
    CHECK(r.body == "good");
    CHECK(r.stale);
}

TEST_CASE("Rate limiting without a cached copy: wait as asked, then retry") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());
    std::vector<seconds> waits;
    cache.setSleeper([&](seconds s) { waits.push_back(s); });

    http.handler = [&](const std::string&) {
        if (http.calls == 1) {
            return HttpResponse{429, R"({ "statusCode": 429, "message": "Rate limit is exceeded. Try again in 42 seconds." })"};
        }
        return HttpResponse{200, "ok"};
    };
    CHECK(cache.get("https://x/a", hours{1}).body == "ok");
    CHECK(waits == std::vector<seconds>{42s});
}

TEST_CASE("Rate limiting gives up after a few attempts") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());
    cache.setSleeper([](seconds) {});
    http.handler = [](const std::string&) { return HttpResponse{429, "", 5}; };
    CHECK_THROWS_AS(cache.get("https://x/a", hours{1}), HttpError);
    CHECK(http.calls == 3);
}

TEST_CASE("Rate limiting with a stale copy serves it immediately") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());
    int sleeps = 0;
    cache.setSleeper([&](seconds) { ++sleeps; });
    http.handler = [](const std::string&) { return HttpResponse{200, "old"}; };
    cache.get("https://x/a", seconds{0});
    http.handler = [](const std::string&) { return HttpResponse{429, "", 60}; };
    const auto r = cache.get("https://x/a", seconds{0});
    CHECK(r.body == "old");
    CHECK(r.stale);
    CHECK(sleeps == 0);
}

TEST_CASE("urlEncode") {
    CHECK(urlEncode(R"({"PriceArea":["DK2"]})") == "%7B%22PriceArea%22%3A%5B%22DK2%22%5D%7D");
    CHECK(urlEncode("ValidFrom desc") == "ValidFrom%20desc");
}
