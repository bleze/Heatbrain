#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/price.hpp"
#include "providers/price_model.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

TEST_CASE("computeTotal sums parts then applies surcharges in order") {
    const std::vector<Surcharge> vat{{"Moms", 25}};
    CHECK(*computeTotal({1.0, 0.5}, vat) == Approx(1.875));
    CHECK(*computeTotal({1.0, 0.5}, {}) == Approx(1.5));
    CHECK(*computeTotal({1.0}, {{"a", 10}, {"b", 10}}) == Approx(1.21));
    CHECK_FALSE(computeTotal({1.0, std::nullopt}, vat));
}

TEST_CASE("PriceModel combines components from config") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());
    const TimeZone tz("Europe/Copenhagen");
    ProviderContext ctx{cache, tz, "DKK", [] { return Instant{}; }};

    const auto cfg = nlohmann::json::parse(R"({
      "components": [
        {"type": "fixed", "name": "Tax", "amount": 0.8},
        {"type": "schedule", "name": "Grid", "seasons": [
          {"bands": [{"from":"00:00","to":"17:00","amount":0.2},
                     {"from":"17:00","to":"24:00","amount":1.2}]}]}
      ],
      "surcharges": [{"name": "VAT", "percent": 25}]
    })");
    PriceModel model(cfg, ctx);

    const Instant from = tz.toInstant(local_days{2026y / 11 / 21} + 16h + 30min);
    const PriceTable t = model.build(from, from + 1h);
    REQUIRE(t.slots.size() == 4);
    CHECK(t.componentNames == std::vector<std::string>{"Tax", "Grid"});
    CHECK(*t.slots[0].total == Approx((0.8 + 0.2) * 1.25));
    CHECK(*t.slots[2].total == Approx((0.8 + 1.2) * 1.25));
    CHECK(t.errors.empty());
    CHECK(http.calls == 0);
}

TEST_CASE("Unknown component types give a helpful error") {
    test::TempDir dir;
    test::FakeHttp http;
    CachingHttpClient cache(http, dir.path());
    const TimeZone tz("Europe/Copenhagen");
    ProviderContext ctx{cache, tz, "DKK", [] { return Instant{}; }};
    const auto cfg = nlohmann::json::parse(R"({"components":[{"type":"nope","name":"x"}]})");
    CHECK_THROWS_WITH(PriceModel(cfg, ctx), Catch::Matchers::ContainsSubstring("fixed"));
}
