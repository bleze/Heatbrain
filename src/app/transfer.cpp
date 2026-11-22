#include "app/transfer.hpp"

#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "app/config_service.hpp"
#include "core/base64.hpp"
#include "core/time.hpp"
#include "storage/history.hpp"

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

namespace {

std::string readBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

json readJsonFile(const fs::path& file) {
    std::ifstream in(file);
    return json::parse(in);
}

// Write next to the target, then rename: a crash never leaves a half-written file.
void writeAtomically(const fs::path& file, const std::string& content, bool binary = false) {
    fs::create_directories(file.parent_path());
    const fs::path tmp = file.string() + ".importing";
    {
        std::ofstream out(tmp, binary ? std::ios::binary | std::ios::trunc : std::ios::trunc);
        out << content;
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, file);
}

std::string nowIso() { return formatIsoUtc(floor<seconds>(system_clock::now())); }

}  // namespace

bool isTransferablePath(const std::string& relative) {
    static const std::regex allowed(
        R"(^(netatmo-tokens|overrides|away|digest|devi)\.json$|^plugs/[A-Za-z0-9_-]+/(backup-\d{8}T\d{6}Z|firmware)\.json$)");
    return std::regex_match(relative, allowed);
}

json buildExport(const fs::path& dataDir, const fs::path& configFile, bool withHistory) {
    json out{{"format", kExportFormat}, {"version", kExportVersion}, {"exportedAt", nowIso()}};
    out["config"] = readJsonFile(configFile);

    json files = json::object();
    if (fs::exists(dataDir)) {
        for (auto it = fs::recursive_directory_iterator(dataDir); it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            const std::string rel = fs::relative(it->path(), dataDir).generic_string();
            if (!isTransferablePath(rel)) continue;
            try {
                files[rel] = readJsonFile(it->path());
            } catch (const std::exception&) {
                // an unreadable state file is left behind rather than failing the export
            }
        }
    }
    out["files"] = files;

    if (withHistory && fs::exists(dataDir / "history.db")) {
        // VACUUM INTO gives a consistent copy while heatbrain keeps writing.
        const fs::path snap = dataDir / "history-export.tmp.db";
        fs::remove(snap);
        {
            History history(dataDir / "history.db");
            history.snapshotTo(snap);
        }
        out["history"] = base64Encode(readBytes(snap));
        fs::remove(snap);
    }
    return out;
}

ImportResult applyImport(const json& bundle, const fs::path& dataDir, const fs::path& configFile) {
    // --- check everything before changing anything
    if (!bundle.is_object() || bundle.value("format", "") != kExportFormat) {
        throw std::invalid_argument("This is not a heatbrain export file.");
    }
    if (bundle.value("version", 0) > kExportVersion) {
        throw std::invalid_argument("This export comes from a newer heatbrain; update this one first.");
    }
    if (!bundle.contains("config") || !bundle["config"].is_object()) {
        throw std::invalid_argument("The export has no configuration.");
    }
    const json& cfg = bundle["config"];
    if (const auto errors = validateConfig(cfg, dataDir); !errors.empty()) {
        std::string msg = "The configuration in the export is not valid here:";
        for (const auto& e : errors) msg += "\n- " + e;
        throw std::invalid_argument(msg);
    }
    const json files = bundle.value("files", json::object());
    if (!files.is_object()) throw std::invalid_argument("'files' must be an object.");
    for (const auto& [rel, content] : files.items()) {
        if (!isTransferablePath(rel)) throw std::invalid_argument("Unexpected file in the export: " + rel);
    }
    std::string historyBytes;
    if (bundle.contains("history") && bundle["history"].is_string()) {
        try {
            historyBytes = base64Decode(bundle["history"].get<std::string>());
        } catch (const std::exception&) {
            throw std::invalid_argument("The history in the export is damaged.");
        }
        if (!historyBytes.starts_with("SQLite format 3")) throw std::invalid_argument("The history in the export is damaged.");
    }

    // --- apply
    ImportResult r;
    r.exportedAt = bundle.value("exportedAt", "");
    if (!historyBytes.empty()) {
        // Copy the rows into the open database (works while heatbrain runs; WAL mode).
        const fs::path incoming = dataDir / "history-import.tmp.db";
        writeAtomically(incoming, historyBytes, true);
        try {
            History history(dataDir / "history.db");
            history.replaceFrom(incoming);
        } catch (...) {
            fs::remove(incoming);
            throw;
        }
        fs::remove(incoming);
        r.history = true;
    }
    for (const auto& [rel, content] : files.items()) {
        writeAtomically(dataDir / fs::path(rel), content.dump(2));
        ++r.files;
    }
    saveConfig(configFile, cfg);  // last: it backs up the old config and triggers nothing by itself
    return r;
}

}  // namespace hb
