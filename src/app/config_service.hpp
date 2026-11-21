#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hb {

inline constexpr const char* kSecretMask = "********";

// Copy of the config with secrets (Netatmo client secret / refresh token, plug passwords,
// webhook URLs) replaced by kSecretMask, for sending to the browser.
nlohmann::json maskSecrets(const nlohmann::json& cfg);

// Puts back secrets the browser left masked, taking them from the stored config.
// Plugs and sources are matched by id, notification channels by position.
nlohmann::json unmaskSecrets(nlohmann::json incoming, const nlohmann::json& stored);

// Builds every part of the config the way `heatbrain run` would, without network access.
// Returns human-readable problems; empty = valid.
std::vector<std::string> validateConfig(const nlohmann::json& cfg, const std::filesystem::path& dataDir);

// Writes the config atomically, keeping the previous version in
// <dataDir>/config-backups/ (the newest 20 are kept).
void saveConfig(const std::filesystem::path& configFile, const nlohmann::json& cfg);

}  // namespace hb
