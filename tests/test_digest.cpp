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
    CHECK_THAT(m.text, ContainsSubstring("Last week cost 84 kr for 31 kWh"));
    CHECK_THAT(m.text, ContainsSubstring("Timing saved 12 kr (12 %)"));
    CHECK_THAT(m.text, ContainsSubstring("Bedroom 40 kr · Shed 44 kr"));
    CHECK_THAT(m.text, ContainsSubstring("No cost for: Basement"));
    CHECK_THAT(weeklySummary(sampleReport(false)).text, ContainsSubstring("would have cost"));
}

TEST_CASE("Monthly report: cost, price paid, dearest day, the month before") {
    json report = sampleReport(true);
    report["market"] = {{"2026-11-01", 2.0}, {"2026-11-02", 3.0}};
    report["rooms"][0]["days"] = json::array({{{"date", "2026-11-01"}, {"cost", 10.0}}, {{"date", "2026-11-02"}, {"cost", 30.0}}});
    report["rooms"][1]["days"] = json::array({{{"date", "2026-11-01"}, {"cost", 44.3}}});
    const json previous{{"total", {{"kWh", 39.0}}}};
    const auto m = monthlySummary(report, previous, "November");
    CHECK(m.title == "heatbrain: November");
    CHECK_THAT(m.text, ContainsSubstring("November cost 84 kr for 31 kWh"));
    CHECK_THAT(m.text, ContainsSubstring("Average 2.70 DKK/kWh paid, against 2.50 on the market"));
    CHECK_THAT(m.text, ContainsSubstring("Dearest day: 11-01 (54 kr)"));
    CHECK_THAT(m.text, ContainsSubstring("20 % less energy than the month before"));
    CHECK_THAT(monthlySummary(report, json::object(), "November").text, !ContainsSubstring("month before"));
}

TEST_CASE("Monthly report on the 1st from 08:00, once, covering the whole month before") {
    DigestState state;
    DigestOptions opt;
    opt.weekly = false;
    std::vector<std::pair<Instant, int>> asked;
    const auto report = [&](Instant end, int days) { asked.emplace_back(end, days); return sampleReport(true); };
    const PriceTable none;
    CHECK(dueDigests(at(2026y / 12 / 1, 7), tz(), state, opt, report, none, {}).empty());
    const auto m = dueDigests(at(2026y / 12 / 1, 8), tz(), state, opt, report, none, {});
    REQUIRE(m.size() == 1);
    CHECK(m[0].title == "heatbrain: November");
    REQUIRE(asked.size() == 2);
    CHECK(asked[0].first == at(2026y / 12 / 1, 0) - seconds{1});
    CHECK(asked[0].second == 30);
    CHECK(asked[1].first == at(2026y / 11 / 1, 0) - seconds{1});
    CHECK(asked[1].second == 31);
    CHECK(dueDigests(at(2026y / 12 / 1, 9), tz(), state, opt, report, none, {}).empty());
    CHECK(dueDigests(at(2026y / 12 / 2, 8), tz(), state, opt, report, none, {}).empty());
    CHECK(dueDigests(at(2027y / 1 / 1, 8), tz(), state, opt, report, none, {})[0].title == "heatbrain: December");
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
    const auto report = [&](Instant, int) { ++reports; return sampleReport(true); };
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
    CHECK(dueDigests(at(monday, 8), tz(), off, DigestOptions{false, false, true, false}, report, prices, recentDays).empty());
}

TEST_CASE("Firmware notice: once per release, only when a plug is behind") {
    using hb::PlugFirmware;
    const std::vector<PlugFirmware> plugs{{"Bedroom", "16.0.0(release-tasmota)"},
                                          {"Shed", "16.0.0(release-tasmota)"},
                                          {"Basement", ""}};  // unreachable: ignored
    const auto m = hb::firmwareNotice("v16.1.0", plugs, "");
    REQUIRE(m);
    CHECK(m->title == "Tasmota 16.1.0 is out");
    CHECK(m->text.starts_with("Bedroom, Shed run 16.0.0."));

    CHECK_FALSE(hb::firmwareNotice("v16.1.0", plugs, "v16.1.0"));  // announced already
    CHECK_FALSE(hb::firmwareNotice("v16.0.0", plugs, ""));         // all up to date
    CHECK_FALSE(hb::firmwareNotice("", plugs, ""));                // no release info

    const auto one = hb::firmwareNotice("v16.1.0", {{"Bedroom", "14.6.0(release-tasmota)"}, {"Shed", "16.1.0"}}, "");
    REQUIRE(one);
    CHECK(one->text.starts_with("Bedroom runs 14.6.0."));
    const auto mixed = hb::firmwareNotice("v16.1.0", {{"A", "14.6.0"}, {"B", "16.0.0"}}, "");
    REQUIRE(mixed);
    CHECK(mixed->text.starts_with("A, B run older versions."));
}
