#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/tariff.hpp"
#include "providers/energidataservice.hpp"
#include "test_support.hpp"

using namespace hb;
using namespace std::chrono;
using Catch::Approx;

namespace {

LocalTime at(year_month_day d, int hour) { return local_days{d} + hours{hour}; }

}  // namespace

TEST_CASE("Radius C tariff from the real Datahub response") {
    const auto byCode = eds::parseDatahubPricelist(test::readFixture("datahub_radius_c.json"));
    REQUIRE(byCode.contains("DT_C_01"));
    const auto& rows = byCode.at("DT_C_01");

    // Winter 2026: low 00-06, peak 17-21, otherwise high-load.
    CHECK(*tariffAt(rows, at(2026y / 11 / 21, 3)) == Approx(0.106).margin(0.001));
    CHECK(*tariffAt(rows, at(2026y / 11 / 21, 12)) == Approx(0.319).margin(0.001));
    CHECK(*tariffAt(rows, at(2026y / 11 / 21, 17)) == Approx(0.956).margin(0.001));
    CHECK(*tariffAt(rows, at(2026y / 11 / 21, 20)) == Approx(0.956).margin(0.001));
    CHECK(*tariffAt(rows, at(2026y / 11 / 21, 21)) == Approx(0.319).margin(0.001));

    // Rows are published ahead of time: 2027 already has its own prices.
    CHECK(*tariffAt(rows, at(2027y / 2 / 1, 18)) == Approx(1.043).margin(0.001));

    // Before the oldest row in the fixture.
    CHECK_FALSE(tariffAt(rows, at(2025y / 1 / 1, 12)));
}

TEST_CASE("Daily tariffs apply Price1 to every hour") {
    TariffRow daily;
    daily.validFrom = local_days{2026y / 1 / 1};
    daily.hourly[0] = 0.05;
    const std::vector<TariffRow> rows{daily};
    CHECK(*tariffAt(rows, at(2026y / 6 / 1, 0)) == 0.05);
    CHECK(*tariffAt(rows, at(2026y / 6 / 1, 18)) == 0.05);
}

TEST_CASE("Overlapping rows: latest validFrom wins, validTo is exclusive") {
    TariffRow old;
    old.validFrom = local_days{2026y / 1 / 1};
    old.hourly[0] = 1.0;
    TariffRow newer;
    newer.validFrom = local_days{2026y / 6 / 1};
    newer.validTo = local_days{2026y / 7 / 1};
    newer.hourly[0] = 2.0;
    const std::vector<TariffRow> rows{old, newer};
    CHECK(*tariffAt(rows, at(2026y / 5 / 31, 23)) == 1.0);
    CHECK(*tariffAt(rows, at(2026y / 6 / 15, 12)) == 2.0);
    CHECK(*tariffAt(rows, at(2026y / 7 / 1, 0)) == 1.0);
}
