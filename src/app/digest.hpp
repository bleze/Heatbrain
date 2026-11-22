#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/price.hpp"
#include "core/time.hpp"
#include "storage/history.hpp"

namespace hb {

// Scheduled informational messages: the weekly cost summary (Monday morning), the monthly report
// (the 1st) and a heads-up when tomorrow's electricity is unusually expensive or cheap.
struct DigestState {
    std::string lastWeekly;   // local date of the Monday the last summary was sent
    std::string lastMonthly;  // local month ("2026-12") the last monthly report was sent in
    std::string lastHeadsUp;  // local date ("tomorrow") the last heads-up was evaluated for
    std::string lastFirmwareCheck;  // local date of the last Tasmota release check
    std::string lastFirmware;       // release tag already announced ("v16.1.0")
};
DigestState loadDigestState(const std::filesystem::path& file);
void saveDigestState(const std::filesystem::path& file, const DigestState& s);

struct DigestMessage {
    std::string title;
    std::string text;
};

// Text for a cost report covering last week.
DigestMessage weeklySummary(const nlohmann::json& report);

// Text for a cost report covering last month (`month` = "November"), compared with the month
// before when that report has data.
DigestMessage monthlySummary(const nlohmann::json& report, const nlohmann::json& previous, const std::string& month);

// Heads-up if tomorrow's average price is >= 20 % above or below the recent average.
// upcoming: price table that includes tomorrow; recent: the last days' slot prices.
std::optional<DigestMessage> priceHeadsUp(const PriceTable& upcoming, const std::vector<PricePoint>& recent,
                                          const TimeZone& tz, Instant now);

struct DigestOptions {
    bool weekly = true;
    bool headsUp = true;
    bool firmware = true;
    bool monthly = true;
};

struct PlugFirmware {
    std::string name;
    std::string version;  // "16.0.0(release-tasmota)"; empty if the plug could not be reached
};

// One message per Tasmota release that is newer than the firmware on at least one plug;
// nothing if that release was announced already (alreadyAnnounced = its tag).
std::optional<DigestMessage> firmwareNotice(const std::string& latestTag, const std::vector<PlugFirmware>& plugs,
                                            const std::string& alreadyAnnounced);

// The daily release check is due: enabled, from 10:00 local time, not done today.
bool firmwareCheckDue(Instant now, const TimeZone& tz, const DigestState& state, const DigestOptions& opt);
DigestOptions parseDigestOptions(const nlohmann::json& cfg);

// Called after every control step: returns the messages due now and updates state.
// report(endInclusive, days) builds the cost report of the `days` local days up to endInclusive.
std::vector<DigestMessage> dueDigests(Instant now, const TimeZone& tz, DigestState& state, const DigestOptions& opt,
                                      const std::function<nlohmann::json(Instant, int)>& report,
                                      const PriceTable& upcoming, const std::vector<PricePoint>& recent);

}  // namespace hb
