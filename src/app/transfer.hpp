#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace hb {

// Moving heatbrain to another machine (e.g. from a PC to the NAS): everything in one JSON file.
//
// {
//   "format": "heatbrain-export", "version": 1, "exportedAt": "2026-11-22T10:00:00Z",
//   "config": {...},                         // heatbrain.json, secrets included
//   "files": {"netatmo-tokens.json": {...},  // small state files, by path below the data dir
//             "plugs/bedroom/firmware.json": {...}, ...},
//   "history": "<base64 SQLite database>"     // optional
// }
inline constexpr const char* kExportFormat = "heatbrain-export";
inline constexpr int kExportVersion = 1;

nlohmann::json buildExport(const std::filesystem::path& dataDir, const std::filesystem::path& configFile,
                           bool withHistory);

// The state files an export carries; anything else in an import is refused (no path tricks).
bool isTransferablePath(const std::string& relative);

struct ImportResult {
    int files = 0;
    bool history = false;
    std::string exportedAt;
};

// Validates the bundle (format, config), then backs up and replaces the config, writes the
// state files and replaces the history. Throws std::invalid_argument with a readable reason
// when the bundle is not acceptable; nothing is changed in that case.
ImportResult applyImport(const nlohmann::json& bundle, const std::filesystem::path& dataDir,
                         const std::filesystem::path& configFile);

}  // namespace hb
