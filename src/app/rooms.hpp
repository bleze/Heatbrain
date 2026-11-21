#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/planner.hpp"
#include "core/room.hpp"
#include "plugs/plugs.hpp"

namespace hb {

// Parses and validates the config's "rooms" array against the configured plugs.
std::vector<RoomConfig> parseRooms(const nlohmann::json& cfg, const std::vector<Plug>& plugs);

DayWindow parseWindow(const nlohmann::json& j);

// Rooms written before daily programs existed have "schedule" bands (+ "noHeat" windows).
// Returns the room with an equivalent "program" of switch points instead; rooms that
// already have a program (or are price-only) are returned unchanged.
nlohmann::json roomWithProgram(const nlohmann::json& room);

// The whole config with every room in program form (what the settings UI edits).
nlohmann::json withPrograms(nlohmann::json cfg);

// Manual overrides, stored in <dataDir>/overrides.json so the CLI can set them while
// `heatbrain run` is running (it re-reads the file every tick).
class Overrides {
public:
    explicit Overrides(std::filesystem::path file) : file_(std::move(file)) {}

    std::map<std::string, ManualOverride> load() const;
    void set(const std::string& roomId, ManualOverride o) const;
    void clear(const std::string& roomId) const;

private:
    void save(const std::map<std::string, ManualOverride>& all) const;
    std::filesystem::path file_;
};

}  // namespace hb
