#include "plugs/plugs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <future>
#include <set>
#include <stdexcept>

#include "core/time.hpp"
#include "plugs/simulated.hpp"
#include "plugs/tasmota.hpp"

namespace hb {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

FailsafeConfig parseFailsafe(const json& cfg) {
    FailsafeConfig f;
    if (!cfg.is_object()) return f;
    f.pulse = minutes{cfg.value("pulseMinutes", f.pulse.count())};
    f.watchdog = minutes{cfg.value("watchdogMinutes", f.watchdog.count())};
    f.limpOn = minutes{cfg.value("limpOnMinutes", f.limpOn.count())};
    f.limpOff = minutes{cfg.value("limpOffMinutes", f.limpOff.count())};
    validate(f);
    return f;
}

std::vector<Plug> loadPlugs(const json& cfg, HttpClient& lanHttp) {
    std::vector<Plug> plugs;
    if (!cfg.contains("plugs")) return plugs;
    std::set<std::string> ids;
    for (const auto& pj : cfg.at("plugs")) {
        Plug p;
        p.id = pj.at("id").get<std::string>();
        if (!ids.insert(p.id).second) throw std::runtime_error("Duplicate plug id: " + p.id);
        p.name = pj.value("name", p.id);
        p.type = pj.value("type", std::string("tasmota"));
        p.host = pj.value("host", std::string());
        const std::string mode = pj.value("mode", std::string("dryrun"));
        if (mode == "live") {
            p.mode = PlugMode::Live;
        } else if (mode == "dryrun") {
            p.mode = PlugMode::DryRun;
        } else {
            throw std::runtime_error("Plug " + p.id + ": mode must be 'live' or 'dryrun'");
        }
        try {
            p.failsafe = parseFailsafe(pj.value("failsafe", json::object()));
        } catch (const std::exception& e) {
            throw std::runtime_error("Plug " + p.id + ": " + e.what());
        }

        if (p.type == "tasmota") {
            if (p.host.empty()) throw std::runtime_error("Plug " + p.id + ": 'host' is required");
            p.sw = std::make_unique<TasmotaSwitch>(lanHttp, p.host, pj.value("password", std::string()),
                                                   p.failsafe);
        } else if (p.type == "simulated") {
            p.sw = std::make_unique<SimulatedSwitch>(p.name);
        } else {
            throw std::runtime_error("Plug " + p.id + ": unknown type '" + p.type +
                                     "' (known: tasmota, simulated)");
        }
        plugs.push_back(std::move(p));
    }
    return plugs;
}

fs::path saveBackup(const fs::path& dataDir, const Plug& plug, const json& backup) {
    const fs::path dir = dataDir / "plugs" / plug.id;
    fs::create_directories(dir);
    const auto now = floor<seconds>(system_clock::now());
    const fs::path file = dir / std::format("backup-{:%Y%m%dT%H%M%SZ}.json", now);
    std::ofstream(file) << backup.dump(2);
    return file;
}

fs::path latestBackup(const fs::path& dataDir, const Plug& plug) {
    const fs::path dir = dataDir / "plugs" / plug.id;
    fs::path latest;
    if (fs::exists(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            const auto name = e.path().filename().string();
            // Timestamped names sort chronologically.
            if (name.starts_with("backup-") && (latest.empty() || e.path() > latest)) latest = e.path();
        }
    }
    if (latest.empty()) throw std::runtime_error("No backup found for plug " + plug.id);
    return latest;
}

fs::path originalBackup(const fs::path& dataDir, const Plug& plug) {
    const fs::path dir = dataDir / "plugs" / plug.id;
    std::vector<fs::path> files;
    if (fs::exists(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().filename().string().starts_with("backup-")) files.push_back(e.path());
        }
    }
    if (files.empty()) throw std::runtime_error("No backup found for plug " + plug.id);
    std::ranges::sort(files, std::greater<>{});  // timestamped names: newest first
    for (const auto& f : files) {
        try {
            const json b = json::parse(std::ifstream(f));
            if (!backupHasFailsafe(b)) return f;
        } catch (const std::exception&) {
            // unreadable backup: skip it
        }
    }
    return files.back();  // every backup has the fail-safe: the oldest is the best we have
}

bool backupHasFailsafe(const json& backup) {
    // Both variants of heatbrain's watchdog rule start with this (see tasmotaFailsafeRule).
    const std::string rules = backup.contains("rule1") ? backup["rule1"].value("rules", "") : "";
    return rules.find("ON System#Boot DO RuleTimer1 ") != std::string::npos;
}

std::optional<FirmwareRecord> loadFirmwareRecord(const fs::path& dataDir, const std::string& plugId) {
    const fs::path file = dataDir / "plugs" / plugId / "firmware.json";
    if (!fs::exists(file)) return std::nullopt;
    try {
        std::ifstream in(file);
        const json j = json::parse(in);
        return FirmwareRecord{j.value("version", ""), j.value("verified", false), j.value("checkedAt", ""),
                              j.value("detail", "")};
    } catch (const std::exception&) {
        return FirmwareRecord{"", false, "", "unreadable firmware record"};  // fail closed
    }
}

void saveFirmwareRecord(const fs::path& dataDir, const std::string& plugId, const FirmwareRecord& r) {
    const fs::path dir = dataDir / "plugs" / plugId;
    fs::create_directories(dir);
    std::ofstream(dir / "firmware.json")
        << json{{"version", r.version}, {"verified", r.verified}, {"checkedAt", r.checkedAt}, {"detail", r.detail}}.dump(2);
}

std::vector<DiscoveredPlug> discoverTasmota(std::string_view subnet, HttpClient& http) {
    // Accept "a.b.c", "a.b.c.0/24" or "a.b.c.x".
    std::string prefix(subnet.substr(0, subnet.find('/')));
    int dots = static_cast<int>(std::ranges::count(prefix, '.'));
    if (dots == 3) prefix = prefix.substr(0, prefix.rfind('.'));
    else if (dots != 2) throw std::runtime_error("Subnet must look like 192.168.1.0/24");
    if (subnet.find('/') != std::string_view::npos && !subnet.ends_with("/24")) {
        throw std::runtime_error("Only /24 networks are supported");
    }

    auto probe = [&http, &prefix](int i) -> std::optional<DiscoveredPlug> {
        const std::string host = std::format("{}.{}", prefix, i);
        try {
            const auto res = http.get("http://" + host + "/cm?cmnd=Status%200");
            if (res.status != 200) return std::nullopt;
            const json j = json::parse(res.body);
            if (!j.contains("StatusFWR")) return std::nullopt;
            DiscoveredPlug d;
            d.host = host;
            d.deviceName = j.contains("Status") ? j["Status"].value("DeviceName", "") : "";
            d.firmware = j["StatusFWR"].value("Version", "");
            d.hardware = j["StatusFWR"].value("Hardware", "");
            if (j.contains("StatusSTS")) d.on = parseTasmotaPower(j["StatusSTS"]);
            return d;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    std::vector<DiscoveredPlug> found;
    constexpr int kBatch = 64;
    for (int start = 1; start <= 254; start += kBatch) {
        std::vector<std::future<std::optional<DiscoveredPlug>>> batch;
        for (int i = start; i < start + kBatch && i <= 254; ++i) {
            batch.push_back(std::async(std::launch::async, probe, i));
        }
        for (auto& f : batch) {
            if (auto d = f.get()) found.push_back(std::move(*d));
        }
    }
    return found;
}

}  // namespace hb
