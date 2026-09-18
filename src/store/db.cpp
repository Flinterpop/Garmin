#include "store/db.h"

#include <sqlite3.h>

#include "util/assert.h"

namespace store {

namespace {

constexpr const char* kSchema[] = {
    "PRAGMA journal_mode=WAL",
    "PRAGMA synchronous=NORMAL",
    "CREATE TABLE IF NOT EXISTS fit_file("
    " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, file_type INTEGER,"
    " time_created INTEGER, serial_number INTEGER, product_name TEXT,"
    " imported_at INTEGER NOT NULL, messages INTEGER NOT NULL)",
    "CREATE TABLE IF NOT EXISTS daily_summary("
    " date TEXT PRIMARY KEY, steps INTEGER, distance_m REAL, floors_up REAL,"
    " resting_hr INTEGER, min_hr INTEGER, max_hr INTEGER, avg_stress INTEGER,"
    " body_battery_high INTEGER, body_battery_low INTEGER, total_kcal INTEGER,"
    " active_kcal INTEGER, sleep_seconds INTEGER, json TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS hr_sample("
    " ts INTEGER NOT NULL, source TEXT NOT NULL, bpm INTEGER NOT NULL,"
    " PRIMARY KEY(ts, source)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS stress_sample("
    " ts INTEGER NOT NULL, source TEXT NOT NULL, level INTEGER NOT NULL,"
    " PRIMARY KEY(ts, source)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS body_battery_sample("
    " ts INTEGER PRIMARY KEY, level INTEGER NOT NULL) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS sleep("
    " date TEXT PRIMARY KEY, start_ts INTEGER, end_ts INTEGER, deep_s INTEGER,"
    " light_s INTEGER, rem_s INTEGER, awake_s INTEGER, score INTEGER,"
    " avg_spo2 REAL, avg_resp REAL, avg_hrv REAL, json TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS sleep_stage("
    " start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL, stage TEXT NOT NULL,"
    " source TEXT NOT NULL, PRIMARY KEY(start_ts, source)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS sleep_level_sample("
    " ts INTEGER PRIMARY KEY, level INTEGER NOT NULL) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS respiration_sample("
    " ts INTEGER PRIMARY KEY, brpm REAL NOT NULL) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS spo2_sample("
    " ts INTEGER PRIMARY KEY, pct INTEGER NOT NULL, confidence INTEGER) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS hrv_daily("
    " date TEXT PRIMARY KEY, weekly_avg REAL, last_night_avg REAL,"
    " last_night_5min_high REAL, status TEXT, baseline_low_upper REAL,"
    " baseline_balanced_lower REAL, baseline_balanced_upper REAL, json TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS hrv_sample("
    " ts INTEGER NOT NULL, source TEXT NOT NULL, rmssd_ms REAL NOT NULL,"
    " PRIMARY KEY(ts, source)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS weight("
    " ts INTEGER PRIMARY KEY, weight_kg REAL NOT NULL, bmi REAL, body_fat_pct REAL,"
    " body_water_pct REAL, bone_mass_kg REAL, muscle_mass_kg REAL, visceral_fat REAL,"
    " metabolic_age INTEGER, source TEXT, json TEXT)",
    "CREATE TABLE IF NOT EXISTS activity("
    " id INTEGER PRIMARY KEY, name TEXT, type TEXT, start_ts INTEGER, duration_s REAL,"
    " distance_m REAL, avg_hr INTEGER, max_hr INTEGER, calories INTEGER,"
    " elevation_gain_m REAL, avg_speed_mps REAL, training_effect REAL,"
    " anaerobic_effect REAL, fit_file_id INTEGER, json TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS activity_session("
    " fit_file_id INTEGER NOT NULL, start_ts INTEGER NOT NULL, sport INTEGER,"
    " sub_sport INTEGER, elapsed_s REAL, timer_s REAL, distance_m REAL, calories INTEGER,"
    " avg_hr INTEGER, max_hr INTEGER, avg_speed_mps REAL, max_speed_mps REAL,"
    " ascent_m REAL, descent_m REAL, avg_cadence INTEGER, avg_power INTEGER,"
    " training_effect REAL, anaerobic_effect REAL,"
    " PRIMARY KEY(fit_file_id, start_ts)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS activity_lap("
    " fit_file_id INTEGER NOT NULL, start_ts INTEGER NOT NULL, elapsed_s REAL,"
    " timer_s REAL, distance_m REAL, avg_hr INTEGER, max_hr INTEGER, avg_speed_mps REAL,"
    " calories INTEGER, PRIMARY KEY(fit_file_id, start_ts)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS activity_record("
    " fit_file_id INTEGER NOT NULL, ts INTEGER NOT NULL, lat REAL, lon REAL, alt_m REAL,"
    " hr INTEGER, cadence INTEGER, dist_m REAL, speed_mps REAL, power_w INTEGER,"
    " temp_c REAL, PRIMARY KEY(fit_file_id, ts)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS activity_hrv("
    " fit_file_id INTEGER NOT NULL, seq INTEGER NOT NULL, rr_ms REAL NOT NULL,"
    " PRIMARY KEY(fit_file_id, seq)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS sync_log("
    " kind TEXT NOT NULL, key TEXT NOT NULL, fetched_at INTEGER NOT NULL,"
    " ok INTEGER NOT NULL, PRIMARY KEY(kind, key)) WITHOUT ROWID",
    "CREATE INDEX IF NOT EXISTS idx_activity_start ON activity(start_ts)",
    "CREATE INDEX IF NOT EXISTS idx_fit_file_type ON fit_file(file_type, time_created)",
};

}  // namespace

Db::~Db() { close(); }

bool Db::open(const std::filesystem::path& path, std::string& err) {
  G_ASSERT(db_ == nullptr);
  const std::string utf8 = path.string();
  const int rc = sqlite3_open_v2(utf8.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    err = "sqlite open failed: " + last_error();
    close();
    return false;
  }
  sqlite3_busy_timeout(db_, 5000);
  for (const char* sql : kSchema) {
    if (!exec(sql, err)) {
      close();
      return false;
    }
  }
  return true;
}

void Db::close() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool Db::exec(const char* sql, std::string& err) {
  G_ASSERT(db_ != nullptr);
  G_ASSERT(sql != nullptr);
  char* msg = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    err = std::string("sqlite: ") + (msg != nullptr ? msg : "error") + " in: " + sql;
    sqlite3_free(msg);
    return false;
  }
  return true;
}

void Db::rollback() {
  std::string ignored;
  if (db_ != nullptr) exec("ROLLBACK", ignored);
}

int64_t Db::last_insert_rowid() const {
  G_ASSERT(db_ != nullptr);
  return sqlite3_last_insert_rowid(db_);
}

int Db::changes() const {
  G_ASSERT(db_ != nullptr);
  return sqlite3_changes(db_);
}

std::string Db::last_error() const {
  return db_ != nullptr ? sqlite3_errmsg(db_) : "database not open";
}

// ---------------------------------------------------------------------- Stmt

Stmt::Stmt(Db& db, const char* sql) : db_(db) {
  G_ASSERT(db.is_open());
  G_ASSERT(sql != nullptr);
  if (sqlite3_prepare_v2(db.raw(), sql, -1, &stmt_, nullptr) != SQLITE_OK) {
    stmt_ = nullptr;
  }
}

Stmt::~Stmt() {
  if (stmt_ != nullptr) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind(int idx, int64_t v) {
  G_ASSERT(stmt_ != nullptr);
  sqlite3_bind_int64(stmt_, idx, v);
  return *this;
}

Stmt& Stmt::bind(int idx, double v) {
  G_ASSERT(stmt_ != nullptr);
  sqlite3_bind_double(stmt_, idx, v);
  return *this;
}

Stmt& Stmt::bind(int idx, const std::string& v) {
  G_ASSERT(stmt_ != nullptr);
  sqlite3_bind_text(stmt_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}

Stmt& Stmt::bind_null(int idx) {
  G_ASSERT(stmt_ != nullptr);
  sqlite3_bind_null(stmt_, idx);
  return *this;
}

Stmt& Stmt::bind(int idx, const std::optional<int64_t>& v) {
  return v.has_value() ? bind(idx, *v) : bind_null(idx);
}

Stmt& Stmt::bind(int idx, const std::optional<double>& v) {
  return v.has_value() ? bind(idx, *v) : bind_null(idx);
}

Stmt& Stmt::bind(int idx, const std::optional<std::string>& v) {
  return v.has_value() ? bind(idx, *v) : bind_null(idx);
}

bool Stmt::run(std::string& err) {
  G_ASSERT(stmt_ != nullptr);
  const int rc = sqlite3_step(stmt_);
  const bool ok = (rc == SQLITE_DONE || rc == SQLITE_ROW);
  if (!ok) err = "sqlite: " + db_.last_error();
  reset();
  return ok;
}

bool Stmt::row() {
  G_ASSERT(stmt_ != nullptr);
  return sqlite3_step(stmt_) == SQLITE_ROW;
}

void Stmt::reset() {
  if (stmt_ != nullptr) {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }
}

int64_t Stmt::col_int(int i) const { return sqlite3_column_int64(stmt_, i); }
double Stmt::col_double(int i) const { return sqlite3_column_double(stmt_, i); }
bool Stmt::col_null(int i) const { return sqlite3_column_type(stmt_, i) == SQLITE_NULL; }
std::string Stmt::col_text(int i) const {
  const unsigned char* t = sqlite3_column_text(stmt_, i);
  return t != nullptr ? reinterpret_cast<const char*>(t) : std::string();
}

}  // namespace store
