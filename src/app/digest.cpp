#include "app/digest.hpp"

#include "core/version.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace {

constexpr int kWeeklyHour = 8;     // Monday 08:00
constexpr int kHeadsUpHour = 14;   // after tomorrow's prices (~13:00)
constexpr double kNotable = 0.20;  // +-20 % vs the recent average

std::string money(double v, const std::string& currency) {
    return std::format("{:.0f} {}", std::round(v), currency == "DKK" ? "kr" : currency);
}

std::string hm(int minute) { return std::format("{:02}:{:02}", minute / 60, minute % 60); }

}  // namespace

DigestState loadDigestState(const fs::path& file) {
    DigestState s;
    try {
        std::ifstream in(file);
        if (!in) return s;
        const json j = json::parse(in);
        s.lastWeekly = j.value("lastWeekly", "");
        s.lastMonthly = j.value("lastMonthly", "");
        s.lastHeadsUp = j.value("lastHeadsUp", "");
        s.lastFirmwareCheck = j.value("lastFirmwareCheck", "");
        s.lastFirmware = j.value("lastFirmware", "");
    } catch (const std::exception&) {
    }
    return s;
}

void saveDigestState(const fs::path& file, const DigestState& s) {
    std::ofstream(file) << json{{"lastWeekly", s.lastWeekly},
                                {"lastMonthly", s.lastMonthly},
                                {"lastHeadsUp", s.lastHeadsUp},
                                {"lastFirmwareCheck", s.lastFirmwareCheck},
                                {"lastFirmware", s.lastFirmware}}
                               .dump(2);
}

DigestOptions parseDigestOptions(const json& cfg) {
    DigestOptions o;
    if (!cfg.contains("notifications")) return o;
    o.weekly = cfg["notifications"].value("weeklySummary", true);
    o.headsUp = cfg["notifications"].value("priceHeadsUp", true);
    o.firmware = cfg["notifications"].value("firmwareUpdates", true);
    o.monthly = cfg["notifications"].value("monthlyReport", true);
    return o;
}

namespace {

// "84 kr for 31 kWh", savings, cost per room and rooms without a known power.
std::string costText(const json& report, const std::string& period) {
    const std::string cur = report.value("currency", std::string("DKK"));
    const auto& total = report.at("total");
    const double cost = total.value("cost", 0.0), kWh = total.value("kWh", 0.0), saved = total.value("saved", 0.0);
    bool anyLive = false;
    std::vector<std::string> perRoom, missing;
    for (const auto& r : report.at("rooms")) {
        if (r.value("excluded", false)) continue;  // left out on purpose
        if (!r.contains("total")) {
            missing.push_back(r.value("name", "?"));
            continue;
        }
        anyLive = anyLive || r["total"].value("liveKWh", 0.0) > 0;
        perRoom.push_back(std::format("{} {}", r.value("name", "?"), money(r["total"].value("cost", 0.0), cur)));
    }
    std::string text = std::format("{} {} {} for {:.0f} kWh (estimated).", period, anyLive ? "cost" : "would have cost",
                                   money(cost, cur), kWh);
    if (cost + saved > 0) {
        const double pct = saved / (cost + saved) * 100;
        text += saved >= 0 ? std::format(" Timing saved {} ({:.0f} %).", money(saved, cur), pct)
                           : std::format(" Timing cost {} extra.", money(-saved, cur));
    }
    if (!perRoom.empty()) {
        text += "\n";
        for (std::size_t i = 0; i < perRoom.size(); ++i) text += (i ? " · " : "") + perRoom[i];
    }
    if (!missing.empty()) {
        text += "\nNo cost for: ";
        for (std::size_t i = 0; i < missing.size(); ++i) text += (i ? ", " : "") + missing[i];
        text += " (set the heating power under Settings → Heating)";
    }
    return text;
}

}  // namespace

DigestMessage weeklySummary(const json& report) { return {"heatbrain: last week", costText(report, "Last week")}; }

DigestMessage monthlySummary(const json& report, const json& previous, const std::string& month) {
    const std::string cur = report.value("currency", std::string("DKK"));
    std::string text = costText(report, month);
    const double kWh = report.at("total").value("kWh", 0.0), cost = report.at("total").value("cost", 0.0);
    // What a kWh cost on average, against the market average over the same days: the effect of timing.
    if (kWh > 0 && report.contains("market") && !report["market"].empty()) {
        double market = 0;
        for (const auto& [date, avg] : report["market"].items()) market += avg.get<double>();
        market /= static_cast<double>(report["market"].size());
        text += std::format("\nAverage {:.2f} {}/kWh paid, against {:.2f} on the market.", cost / kWh, cur, market);
    }
    // The dearest day for heating, over all rooms.
    std::map<std::string, double> byDay;
    for (const auto& r : report.at("rooms")) {
        for (const auto& d : r.value("days", json::array())) byDay[d.value("date", "")] += d.value("cost", 0.0);
    }
    if (const auto dear = std::ranges::max_element(byDay, {}, [](const auto& p) { return p.second; });
        dear != byDay.end() && dear->second > 0) {
        text += std::format(" Dearest day: {} ({}).", dear->first.substr(5), money(dear->second, cur));
    }
    if (previous.contains("total")) {
        const double before = previous["total"].value("kWh", 0.0);
        if (before > 1 && kWh > 0) {
            const double pct = (kWh / before - 1) * 100;
            text += std::abs(pct) < 1 ? std::string(" About the same energy as the month before.")
                                      : std::format(" {:.0f} % {} energy than the month before.", std::abs(pct),
                                                    pct > 0 ? "more" : "less");
        }
    }
    return {"heatbrain: " + month, text};
}

std::optional<DigestMessage> priceHeadsUp(const PriceTable& upcoming, const std::vector<PricePoint>& recent,
                                          const TimeZone& tz, Instant now) {
    const auto tomorrow = floor<days>(tz.toLocal(now)) + days{1};
    const std::string tomorrowDate = formatLocalDate(tomorrow);
    std::vector<std::pair<int, double>> slots;  // (minute of day, price)
    for (const auto& s : upcoming.slots) {
        const auto local = tz.toLocal(s.start);
        if (s.total && formatLocalDate(local) == tomorrowDate) slots.emplace_back(minuteOfDay(local), *s.total);
    }
    if (slots.size() < 80 || recent.size() < 96 * 2) return std::nullopt;  // need a full day and some history

    double tomorrowAvg = 0, recentAvg = 0;
    for (const auto& [m, p] : slots) tomorrowAvg += p;
    tomorrowAvg /= static_cast<double>(slots.size());
    for (const auto& p : recent) recentAvg += p.total;
    recentAvg /= static_cast<double>(recent.size());
    const double ratio = tomorrowAvg / recentAvg - 1;
    if (std::abs(ratio) < kNotable) return std::nullopt;

    // Cheapest and most expensive 3-hour stretch tomorrow (12 consecutive slots).
    std::ranges::sort(slots);
    double bestCheap = 1e18, bestDear = -1e18;
    int cheapAt = 0, dearAt = 0;
    for (std::size_t i = 0; i + 12 <= slots.size(); ++i) {
        double sum = 0;
        for (std::size_t k = i; k < i + 12; ++k) sum += slots[k].second;
        if (sum < bestCheap) { bestCheap = sum; cheapAt = slots[i].first; }
        if (sum > bestDear) { bestDear = sum; dearAt = slots[i].first; }
    }
    const std::string cur = upcoming.currency.empty() ? "DKK" : upcoming.currency;
    const bool expensive = ratio > 0;
    return DigestMessage{
        expensive ? "heatbrain: tomorrow is expensive" : "heatbrain: tomorrow is cheap",
        std::format("Tomorrow averages {:.2f} {}/kWh, {:.0f} % {} than the past week ({:.2f}). "
                    "Cheapest 3 hours: {}-{} ({:.2f}); most expensive: {}-{} ({:.2f}).",
                    tomorrowAvg, cur, std::abs(ratio) * 100, expensive ? "more" : "less", recentAvg, hm(cheapAt),
                    hm((cheapAt + 180) % 1440), bestCheap / 12, hm(dearAt), hm((dearAt + 180) % 1440), bestDear / 12)};
}

std::vector<DigestMessage> dueDigests(Instant now, const TimeZone& tz, DigestState& state, const DigestOptions& opt,
                                      const std::function<json(Instant, int)>& report, const PriceTable& upcoming,
                                      const std::vector<PricePoint>& recent) {
    std::vector<DigestMessage> out;
    const auto local = tz.toLocal(now);
    const std::string today = formatLocalDate(local);

    if (opt.weekly && isoWeekday(local) == 1 && hourOfDay(local) >= kWeeklyHour && state.lastWeekly != today) {
        // Last week = Monday 00:00 .. Sunday 23:59:59, i.e. up to just before today.
        out.push_back(weeklySummary(report(startOfLocalDay(tz, now) - seconds{1}, 7)));
        state.lastWeekly = today;
    }
    const year_month_day ymd{floor<days>(local)};
    const std::string thisMonth = today.substr(0, 7);
    if (opt.monthly && ymd.day() == day{1} && hourOfDay(local) >= kWeeklyHour && state.lastMonthly != thisMonth) {
        // Last month, and the month before for comparison: each ends just before the next starts.
        const auto lastMonth = ymd.year() / ymd.month() - months{1};
        const auto monthBefore = lastMonth - months{1};
        const auto length = [](year_month m) {
            return static_cast<int>(static_cast<unsigned>(year_month_day_last{m / last}.day()));
        };
        const Instant end = startOfLocalDay(tz, now) - seconds{1};
        const Instant endBefore = tz.toInstant(local_days{lastMonth / 1}) - seconds{1};
        static const char* names[] = {"January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December"};
        const json last = report(end, length(lastMonth));
        const json before = report(endBefore, length(monthBefore));
        out.push_back(monthlySummary(last, before, names[static_cast<unsigned>(lastMonth.month()) - 1]));
        state.lastMonthly = thisMonth;
    }
    const std::string tomorrow = formatLocalDate(floor<days>(local) + days{1});
    if (opt.headsUp && hourOfDay(local) >= kHeadsUpHour && state.lastHeadsUp != tomorrow) {
        const bool known = std::ranges::count_if(upcoming.slots, [&](const PriceSlot& s) {
                               return s.total && formatLocalDate(tz.toLocal(s.start)) == tomorrow;
                           }) >= 80;
        if (known) {  // evaluate once per day, as soon as tomorrow's prices are in
            if (auto msg = priceHeadsUp(upcoming, recent, tz, now)) out.push_back(*msg);
            state.lastHeadsUp = tomorrow;
        }
    }
    return out;
}

std::optional<DigestMessage> firmwareNotice(const std::string& latestTag, const std::vector<PlugFirmware>& plugs,
                                            const std::string& alreadyAnnounced) {
    if (latestTag.empty() || parseVersion(latestTag).empty() || latestTag == alreadyAnnounced) return std::nullopt;
    std::string behind;
    std::string current;
    int count = 0;
    for (const auto& p : plugs) {
        if (p.version.empty() || compareVersions(p.version, latestTag) >= 0) continue;
        behind += (behind.empty() ? "" : ", ") + p.name;
        ++count;
        const std::string v = p.version.substr(0, p.version.find('('));
        if (current.empty()) current = v;
        else if (current != v) current = "older versions";
    }
    if (behind.empty()) return std::nullopt;
    const std::string latest = latestTag.starts_with('v') ? latestTag.substr(1) : latestTag;
    return DigestMessage{std::format("Tasmota {} is out", latest),
                         std::format("{} run{} {}. Upgrade from Settings -> Heating (one plug at a time; the fail-safe is "
                                     "tested afterwards).",
                                     behind, count == 1 ? "s" : "", current)};
}

bool firmwareCheckDue(Instant now, const TimeZone& tz, const DigestState& state, const DigestOptions& opt) {
    const auto local = tz.toLocal(now);
    return opt.firmware && hourOfDay(local) >= 10 && state.lastFirmwareCheck != formatLocalDate(local);
}

}  // namespace hb
