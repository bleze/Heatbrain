#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "app/digest.hpp"

using namespace hb;
using namespace std::chrono;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

const TimeZone& tz() {
    static const TimeZone z("Europe/Copenhagen");
    return z;
}
Instant at(year_month_day d, int hour) { return tz().toInstant(local_days{d} + hours{hour}); }

json sampleReport(bool live) {
    return json{{"currency", "DKK"},
                {"total", {{"cost", 84.3}, {"kWh", 31.2}, {"saved", 12.0}}},
                {"rooms", json::array({{{"name", "Bedroom"}, {"total", {{"cost", 40.0}, {"liveKWh", live ? 10.0 : 0.0}}}},
                                       {{"name", "Shed"}, {"total", {{"cost", 44.3}, {"liveKWh", 0.0}}}},
                                       {{"name", "Basement"}, {"missingWatts", json::array({"basement"})}}})}};
}

// A price table for one local day at a flat price, with an optional peak 17-20.
PriceTable dayPrices(year_month_day d, double base, double peak) {
    PriceTable t;
    t.currency = "DKK";
    for (int q = 0; q < 96; ++q) {
        const double p = (q >= 68 && q < 80) ? peak : base;
        t.slots.push_back({tz().toInstant(local_days{d} + minutes{15 * q}), {p}, p});
    }
    return t;
}

std::vector<PricePoint> recentFlat(year_month_day lastDay, int days, double price) {
    std::vector<PricePoint> out;
    for (int d = days; d >= 1; --d) {
        for (int q = 0; q < 96; ++q) out.push_back({tz().toInstant(local_days{lastDay} - std::chrono::days{d - 1} + minutes{15 * q}), price});
    }
    return out;
}

}  // namespace

TEST_CASE("Weekly summary text") {
    const auto m = weeklySummary(sampleReport(true));
    CHECK(m.title == "heatbrain: last week");
    CHECK_THAT(m.text, ContainsSubstring("84 kr for 31 kWh"));
    CHECK_THAT(m.text, ContainsSubstring("Timing saved 12 kr (12 %)"));
    CHECK_THAT(m.text, ContainsSubstring("Bedroom 40 kr · Shed 44 kr"));
    CHECK_THAT(m.text, ContainsSubstring("No cost for: Basement"));
    CHECK_THAT(weeklySummary(sampleReport(false)).text, ContainsSubstring("would have cost"));
}

TEST_CASE("Price heads-up only for unusual days") {
    const auto today = 2026y / 11 / 23;
    const auto tomorrow = 2026y / 11 / 24;
    const Instant now = at(today, 14);
    const auto recent = recentFlat(today, 7, 2.0);

    const auto dear = priceHeadsUp(dayPrices(tomorrow, 2.6, 3.5), recent, tz(), now);
    REQUIRE(dear);
    CHECK(dear->title == "heatbrain: tomorrow is expensive");
    CHECK_THAT(dear->text, ContainsSubstring("most expensive: 17:00-20:00"));

    REQUIRE(priceHeadsUp(dayPrices(tomorrow, 1.4, 1.5), recent, tz(), now));
    CHECK_FALSE(priceHeadsUp(dayPrices(tomorrow, 2.1, 2.3), recent, tz(), now));  // within 20 %
}

TEST_CASE("Digests are sent once, at the right time") {
    const auto monday = 2026y / 11 / 23;
    DigestState state;
    const DigestOptions opt;
    int reports = 0;
    const auto report = [&](Instant) { ++reports; return sampleReport(true); };
    const PriceTable noTomorrow;
    const std::vector<PricePoint> recent;

    CHECK(dueDigests(at(monday, 7), tz(), state, opt, report, noTomorrow, recent).empty());  // too early
    CHECK(dueDigests(at(monday, 8), tz(), state, opt, report, noTomorrow, recent).size() == 1);
    CHECK(dueDigests(at(monday, 9), tz(), state, opt, report, noTomorrow, recent).empty());  // already sent
    CHECK(reports == 1);
    CHECK(dueDigests(at(2026y / 11 / 24, 8), tz(), state, opt, report, noTomorrow, recent).empty());  // Tuesday

    // Heads-up: evaluated once tomorrow's prices are known, after 14:00.
    const auto prices = dayPrices(2026y / 11 / 25, 3.0, 4.0);
    const auto recentDays = recentFlat(2026y / 11 / 24, 7, 2.0);
    CHECK(dueDigests(at(2026y / 11 / 24, 13), tz(), state, opt, report, prices, recentDays).empty());
    CHECK(dueDigests(at(2026y / 11 / 24, 14), tz(), state, opt, report, prices, recentDays).size() == 1);
    CHECK(dueDigests(at(2026y / 11 / 24, 15), tz(), state, opt, report, prices, recentDays).empty());

    DigestState off;
    CHECK(dueDigests(at(monday, 8), tz(), off, DigestOptions{false, false}, report, prices, recentDays).empty());
}
