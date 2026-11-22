#include "app/digest.hpp"

#include "core/version.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

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
        s.lastHeadsUp = j.value("lastHeadsUp", "");
        s.lastFirmwareCheck = j.value("lastFirmwareCheck", "");
        s.lastFirmware = j.value("lastFirmware", "");
    } catch (const std::exception&) {
    }
    return s;
}

void saveDigestState(const fs::path& file, const DigestState& s) {
    std::ofstream(file) << json{{"lastWeekly", s.lastWeekly},
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
    return o;
}

DigestMessage weeklySummary(const json& report) {
    const std::string cur = report.value("currency", std::string("DKK"));
    const auto& total = report.at("total");
    const double cost = total.value("cost", 0.0), kWh = total.value("kWh", 0.0), saved = total.value("saved", 0.0);
    bool anyLive = false;
    std::vector<std::string> perRoom, missing;
    for (const auto& r : report.at("rooms")) {
        if (!r.contains("total")) {
            missing.push_back(r.value("name", "?"));
            continue;
        }
        anyLive = anyLive || r["total"].value("liveKWh", 0.0) > 0;
        perRoom.push_back(std::format("{} {}", r.value("name", "?"), money(r["total"].value("cost", 0.0), cur)));
    }
    std::string text = std::format("{} {} for {:.0f} kWh (estimated).", anyLive ? "Last week:" : "Last week would have cost",
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
        text += " (set the radiator power under Settings → Heating)";
    }
    return {"heatbrain: last week", text};
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
                                      const std::function<json(Instant)>& reportForLastWeek, const PriceTable& upcoming,
                                      const std::vector<PricePoint>& recent) {
    std::vector<DigestMessage> out;
    const auto local = tz.toLocal(now);
    const std::string today = formatLocalDate(local);

    if (opt.weekly && isoWeekday(local) == 1 && hourOfDay(local) >= kWeeklyHour && state.lastWeekly != today) {
        // Last week = Monday 00:00 .. Sunday 23:59:59, i.e. up to just before today.
        out.push_back(weeklySummary(reportForLastWeek(startOfLocalDay(tz, now) - seconds{1})));
        state.lastWeekly = today;
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
