#include "storage/history.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace hb {

using namespace std::chrono;

namespace {

// Small RAII wrapper around a prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, long long v) { check(sqlite3_bind_int64(s_, i, v)); return *this; }
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(s_, i, v)); return *this; }
    Stmt& bind(int i, const std::string& v) {
        check(sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }
    Stmt& bind(int i, std::optional<double> v) {
        check(v ? sqlite3_bind_double(s_, i, *v) : sqlite3_bind_null(s_, i));
        return *this;
    }
    Stmt& bind(int i, Instant t) { return bind(i, static_cast<long long>(t.time_since_epoch().count())); }

    void reset() {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }

    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("SQLite step: ") + sqlite3_errmsg(db_));
    }
    long long i64(int c) const { return sqlite3_column_int64(s_, c); }
    double dbl(int c) const { return sqlite3_column_double(s_, c); }
    bool isNull(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        const auto* p = sqlite3_column_text(s_, c);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    Instant instant(int c) const { return Instant{seconds{i64(c)}}; }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error(std::string("SQLite bind: ") + sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

DecisionRecord readDecision(const Stmt& s) {
    DecisionRecord d;
    d.at = s.instant(0);
    d.roomId = s.text(1);
    d.on = s.i64(2) != 0;
    d.changed = s.i64(3) != 0;
    d.reason = s.text(4);
    d.detail = s.text(5);
    d.inputs = nlohmann::json::parse(s.text(6), nullptr, false);
    d.plugs = nlohmann::json::parse(s.text(7), nullptr, false);
    return d;
}

constexpr const char* kDecisionColumns = "ts, room_id, action_on, changed, reason, detail, inputs, plugs";

}  // namespace

History::History(const std::filesystem::path& dbFile) {
    std::filesystem::create_directories(dbFile.parent_path());
    if (sqlite3_open(dbFile.string().c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        throw std::runtime_error("Cannot open " + dbFile.string() + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec(R"(
        CREATE TABLE IF NOT EXISTS decisions (
            ts INTEGER NOT NULL, room_id TEXT NOT NULL, action_on INTEGER NOT NULL,
            changed INTEGER NOT NULL, reason TEXT NOT NULL, detail TEXT NOT NULL,
            inputs TEXT NOT NULL, plugs TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS decisions_room_ts ON decisions(room_id, ts);
        CREATE TABLE IF NOT EXISTS temperatures (
            ts INTEGER NOT NULL, sensor_key TEXT NOT NULL, room_id TEXT NOT NULL,
            temperature REAL NOT NULL, humidity REAL,
            PRIMARY KEY (sensor_key, ts));
        CREATE INDEX IF NOT EXISTS temperatures_room_ts ON temperatures(room_id, ts);
        CREATE TABLE IF NOT EXISTS prices (slot INTEGER PRIMARY KEY, total REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS meter (start INTEGER PRIMARY KEY, seconds INTEGER NOT NULL, kwh REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS sensor_days (
            sensor_key TEXT NOT NULL, day INTEGER NOT NULL, n INTEGER NOT NULL,
            t_sum REAL NOT NULL, t_min REAL NOT NULL, t_max REAL NOT NULL, h_n INTEGER NOT NULL, h_sum REAL NOT NULL,
            PRIMARY KEY (sensor_key, day));
    )");
}

History::~History() { sqlite3_close(db_); }

void History::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQLite: " + msg);
    }
}

void History::recordDecision(const DecisionRecord& d) {
    Stmt s(db_, "INSERT INTO decisions VALUES (?,?,?,?,?,?,?,?)");
    s.bind(1, d.at).bind(2, d.roomId).bind(3, static_cast<long long>(d.on)).bind(4, static_cast<long long>(d.changed));
    s.bind(5, d.reason).bind(6, d.detail).bind(7, d.inputs.dump()).bind(8, d.plugs.dump());
    s.step();
}

namespace {
// Adds one reading to its sensor's day.
void addToDay(sqlite3* db, const std::string& key, Instant day, double temp, std::optional<double> humidity) {
    Stmt d(db, "INSERT INTO sensor_days VALUES (?,?,1,?,?,?,?,?) ON CONFLICT(sensor_key, day) DO UPDATE SET "
               "n=n+1, t_sum=t_sum+excluded.t_sum, t_min=min(t_min, excluded.t_min), t_max=max(t_max, excluded.t_max), "
               "h_n=h_n+excluded.h_n, h_sum=h_sum+excluded.h_sum");
    d.bind(1, key).bind(2, day).bind(3, temp).bind(4, temp).bind(5, temp)
        .bind(6, static_cast<long long>(humidity ? 1 : 0)).bind(7, humidity.value_or(0.0));
    d.step();
}
}  // namespace

void History::recordTemperature(const TemperatureRecord& t) {
    Stmt s(db_, "INSERT OR IGNORE INTO temperatures VALUES (?,?,?,?,?)");
    s.bind(1, t.at).bind(2, t.sensorKey).bind(3, t.roomId).bind(4, t.temperature).bind(5, t.humidity);
    s.step();
    // A new reading (not one seen before): also into the day's summary.
    if (t.day && sqlite3_changes(db_) > 0) addToDay(db_, t.sensorKey, *t.day, t.temperature, t.humidity);
}

void History::recordPrices(const PriceTable& table) {
    exec("BEGIN");
    try {
        Stmt s(db_, "INSERT OR REPLACE INTO prices VALUES (?,?)");
        for (const auto& slot : table.slots) {
            if (!slot.total) continue;
            s.reset();
            s.bind(1, slot.start).bind(2, *slot.total);
            s.step();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::vector<DecisionRecord> History::decisions(const std::string& roomId, Instant from, Instant to,
                                               bool changesOnly) const {
    const std::string sql = std::string("SELECT ") + kDecisionColumns +
                            " FROM decisions WHERE room_id=? AND ts>=? AND ts<?" +
                            (changesOnly ? " AND changed=1" : "") + " ORDER BY ts";
    Stmt s(db_, sql.c_str());
    s.bind(1, roomId).bind(2, from).bind(3, to);
    std::vector<DecisionRecord> out;
    while (s.step()) out.push_back(readDecision(s));
    return out;
}

std::optional<DecisionRecord> History::lastDecision(const std::string& roomId) const {
    const std::string sql =
        std::string("SELECT ") + kDecisionColumns + " FROM decisions WHERE room_id=? ORDER BY ts DESC LIMIT 1";
    Stmt s(db_, sql.c_str());
    s.bind(1, roomId);
    if (!s.step()) return std::nullopt;
    return readDecision(s);
}

std::vector<TemperatureRecord> History::temperatures(const std::string& roomId, Instant from, Instant to) const {
    Stmt s(db_, "SELECT ts, sensor_key, room_id, temperature, humidity FROM temperatures "
                "WHERE room_id=? AND ts>=? AND ts<? ORDER BY ts");
    s.bind(1, roomId).bind(2, from).bind(3, to);
    std::vector<TemperatureRecord> out;
    while (s.step()) {
        TemperatureRecord t{s.instant(0), s.text(1), s.text(2), s.dbl(3), std::nullopt};
        if (!s.isNull(4)) t.humidity = s.dbl(4);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<TemperatureRecord> History::sensorTemperatures(const std::string& sensorKey, Instant from, Instant to) const {
    Stmt s(db_, "SELECT ts, sensor_key, room_id, temperature, humidity FROM temperatures "
                "WHERE sensor_key=? AND ts>=? AND ts<? ORDER BY ts");
    s.bind(1, sensorKey).bind(2, from).bind(3, to);
    std::vector<TemperatureRecord> out;
    while (s.step()) {
        TemperatureRecord t{s.instant(0), s.text(1), s.text(2), s.dbl(3), std::nullopt};
        if (!s.isNull(4)) t.humidity = s.dbl(4);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<SensorDay> History::sensorDays(const std::string& sensorKey, Instant from, Instant to) const {
    Stmt s(db_, "SELECT day, n, t_sum, t_min, t_max, h_n, h_sum FROM sensor_days WHERE sensor_key=? AND day>=? AND day<? ORDER BY day");
    s.bind(1, sensorKey).bind(2, from).bind(3, to);
    std::vector<SensorDay> out;
    while (s.step()) {
        SensorDay d{s.instant(0), static_cast<int>(s.i64(1))};
        d.avgTemp = d.readings ? s.dbl(2) / d.readings : 0;
        d.minTemp = s.dbl(3);
        d.maxTemp = s.dbl(4);
        if (const auto hn = s.i64(5); hn > 0) d.avgHumidity = s.dbl(6) / static_cast<double>(hn);
        out.push_back(d);
    }
    return out;
}

void History::fillSensorDays(const std::function<Instant(Instant)>& dayOf) {
    {
        Stmt c(db_, "SELECT count(*) FROM sensor_days");
        if (c.step() && c.i64(0) > 0) return;  // done before
    }
    exec("BEGIN");
    try {
        Stmt s(db_, "SELECT ts, sensor_key, temperature, humidity FROM temperatures ORDER BY ts");
        while (s.step()) {
            std::optional<double> h;
            if (!s.isNull(3)) h = s.dbl(3);
            addToDay(db_, s.text(1), dayOf(s.instant(0)), s.dbl(2), h);
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

void History::recordMeter(const std::vector<MeterReading>& readings) {
    exec("BEGIN");
    try {
        Stmt s(db_, "INSERT OR REPLACE INTO meter VALUES (?,?,?)");
        for (const auto& r : readings) {
            s.reset();
            s.bind(1, r.start).bind(2, static_cast<long long>(duration_cast<seconds>(r.length).count())).bind(3, r.kWh);
            s.step();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::vector<MeterReading> History::meterReadings(Instant from, Instant to) const {
    Stmt s(db_, "SELECT start, seconds, kwh FROM meter WHERE start>=? AND start<? ORDER BY start");
    s.bind(1, from).bind(2, to);
    std::vector<MeterReading> out;
    while (s.step()) out.push_back({s.instant(0), duration_cast<minutes>(seconds{s.i64(1)}), s.dbl(2)});
    return out;
}

std::optional<MeterReading> History::firstMeterReading() const {
    Stmt s(db_, "SELECT start, seconds, kwh FROM meter ORDER BY start LIMIT 1");
    if (!s.step()) return std::nullopt;
    return MeterReading{s.instant(0), duration_cast<minutes>(seconds{s.i64(1)}), s.dbl(2)};
}

std::optional<MeterReading> History::lastMeterReading() const {
    Stmt s(db_, "SELECT start, seconds, kwh FROM meter ORDER BY start DESC LIMIT 1");
    if (!s.step()) return std::nullopt;
    return MeterReading{s.instant(0), duration_cast<minutes>(seconds{s.i64(1)}), s.dbl(2)};
}

std::vector<Instant> History::readingTimes(Instant from, Instant to) const {
    Stmt s(db_, "SELECT DISTINCT ts FROM temperatures WHERE ts>=? AND ts<? ORDER BY ts");
    s.bind(1, from).bind(2, to);
    std::vector<Instant> out;
    while (s.step()) out.push_back(s.instant(0));
    return out;
}

std::vector<PricePoint> History::prices(Instant from, Instant to) const {
    Stmt s(db_, "SELECT slot, total FROM prices WHERE slot>=? AND slot<? ORDER BY slot");
    s.bind(1, from).bind(2, to);
    std::vector<PricePoint> out;
    while (s.step()) out.push_back({s.instant(0), s.dbl(1)});
    return out;
}

void History::snapshotTo(const std::filesystem::path& file) const {
    Stmt s(db_, "VACUUM INTO ?");
    s.bind(1, file.string());
    s.step();
}

void History::replaceFrom(const std::filesystem::path& file) {
    {
        Stmt a(db_, "ATTACH DATABASE ? AS src");
        a.bind(1, file.string());
        a.step();
    }
    try {
        exec("BEGIN");
        try {
            for (const char* table : {"decisions", "temperatures", "prices", "meter", "sensor_days"}) {
                // Older exports have no meter table: keep what is here then.
                Stmt has(db_, "SELECT count(*) FROM src.sqlite_master WHERE type='table' AND name=?");
                has.bind(1, std::string(table));
                if (!has.step() || has.i64(0) == 0) continue;
                exec((std::string("DELETE FROM main.") + table).c_str());
                exec((std::string("INSERT INTO main.") + table + " SELECT * FROM src." + table).c_str());
            }
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
    } catch (...) {
        exec("DETACH DATABASE src");
        throw;
    }
    exec("DETACH DATABASE src");
}

void History::prune(Instant olderThan) {
    for (const char* table : {"decisions", "temperatures"}) {
        Stmt s(db_, (std::string("DELETE FROM ") + table + " WHERE ts<?").c_str());
        s.bind(1, olderThan);
        s.step();
    }
    // Meter readings are never pruned: one row an hour (~9000 a year) is worth keeping for
    // month-by-month and year-by-year comparisons.
    Stmt s(db_, "DELETE FROM prices WHERE slot<?");
    s.bind(1, olderThan);
    s.step();
}

}  // namespace hb
