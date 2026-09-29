// The Garmin Connect sync, shared by gsync (command line, nightly task) and
// gview (Data > Sync now, first-run setup). Progress goes to a callback so
// each host decides where lines go: a console, a log file, a window.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace store {
class Db;
}

namespace syncer {

// One line of progress; `error` marks failures (gsync sends those to stderr).
using Report = std::function<void(bool error, const std::string& line)>;

constexpr int kMaxDays = 3660;  // 10 years per run
constexpr int kMaxActivities = 5000;
constexpr int kExitLoginRequired = 3;  // gsync exit code: 1 = some fetches failed, 2 = usage

struct SyncOptions {
  std::filesystem::path profile_base;  // holds tokens.bin
  std::filesystem::path data_dir;      // database and downloads
  std::string from;                    // YYYY-MM-DD, inclusive
  std::string to;                      // YYYY-MM-DD, inclusive, >= from
  int max_activities = 50;
  bool no_fit = false;
  bool force = false;
  const std::atomic<bool>* cancel = nullptr;  // polled between days and between activities
};

struct SyncResult {
  int64_t rows = 0;
  int failures = 0;
  bool not_logged_in = false;   // no saved login for this profile
  bool login_required = false;  // Garmin refused the saved login
  bool cancelled = false;
  bool db_error = false;
};

SyncResult run_sync(const SyncOptions& o, const Report& report);

// gsync's exit code for a result: 0 ok, 1 failures, 3 login required.
int exit_code(const SyncResult& r);

// Imports every readings_*.csv in the user's Downloads folder (Omron exports).
int import_bp_downloads(store::Db& db, int64_t& rows, const Report& report);

// Opens (creating if needed) <data_dir>\garmin.db.
bool open_db(const std::filesystem::path& data_dir, store::Db& db, const Report& report);

// printf-style formatting into a std::string (lines are short; capped at 1 KB).
std::string strf(const char* fmt, ...);

}  // namespace syncer
