#pragma once

// Shared by the CLI translation units of the heatbrain executable.

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hbcli {

namespace fs = std::filesystem;

struct Args {
    std::string command = "help";
    std::vector<std::string> positional;
    std::optional<fs::path> dataDir;
    bool slots = false;
    bool force = false;
    bool verbose = false;
    bool all = false;    // history: include unchanged decisions
    int hours = 24;      // history window
};

inline Args parseArgs(int argc, char** argv) {
    Args a;
    bool haveCommand = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            a.dataDir = argv[++i];
        } else if (arg == "--slots") {
            a.slots = true;
        } else if (arg == "--force") {
            a.force = true;
        } else if (arg == "--all") {
            a.all = true;
        } else if (arg == "--hours" && i + 1 < argc) {
            a.hours = std::stoi(argv[++i]);
        } else if (arg == "--verbose" || arg == "-v") {
            a.verbose = true;
        } else if (arg.starts_with("-")) {
            throw std::runtime_error("Unknown option: " + arg);
        } else if (!haveCommand) {
            a.command = arg;
            haveCommand = true;
        } else {
            a.positional.push_back(arg);
        }
    }
    return a;
}

inline fs::path resolveDataDir(const Args& args) {
    if (args.dataDir) return *args.dataDir;
    if (const char* env = std::getenv("HEATBRAIN_DATA_DIR"); env && *env) return env;
#ifdef HEATBRAIN_SOURCE_DIR
    return fs::path(HEATBRAIN_SOURCE_DIR) / "data";  // local dev build
#else
    return "data";
#endif
}

inline std::vector<fs::path> exampleConfigCandidates() {
    std::vector<fs::path> c;
#ifdef HEATBRAIN_SOURCE_DIR
    c.push_back(fs::path(HEATBRAIN_SOURCE_DIR) / "config" / "heatbrain.example.json");
#endif
    c.push_back(fs::path("config") / "heatbrain.example.json");
    c.push_back("/app/config/heatbrain.example.json");
    return c;
}

// Commands implemented in control.cpp
int runPlan(const Args& args);
int runDaemon(const Args& args);
int runHistory(const Args& args);
int runOverride(const Args& args);

}  // namespace hbcli
