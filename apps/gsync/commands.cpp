#include "commands.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <nlohmann/json.hpp>

#include "gc/gc_client.h"
#include "gc/token_store.h"
#include "store/db.h"
#include "store/importer.h"
#include "util/assert.h"
#include "util/console.h"
#include "util/file_util.h"
#include "util/time_util.h"
#include "util/zip_reader.h"

namespace cmd {

using nlohmann::json;

namespace {

constexpr int kMaxDays = 3660;            // 10 years per run
constexpr int kMaxActivities = 5000;
constexpr int kActivityPage = 50;
constexpr DWORD kPoliteDelayMs = 300;     // between API calls
constexpr int kWeightChunkDays = 31;

void usage_text() {
  std::fputs(
      "usage: gsync <command> [options]\n"
      "commands:\n"
      "  login                 sign in to Garmin Connect (prompts; supports MFA)\n"
      "  logout                delete the saved tokens\n"
      "  whoami                show the signed-in account\n"
      "  sync                  pull daily data, activities and FIT files\n"
      "  import <path>...      import FIT files or directories (e.g. from the watch USB)\n"
      "  get <api-path>        raw authenticated GET, prints JSON (debugging)\n"
      "  stats                 row counts in the local database\n"
      "  import-bp <csv>...    import Omron blood-pressure CSV exports\n"
      "options:\n"
      "  --data <dir>          data directory (default %LOCALAPPDATA%\\GarminSync\\data)\n"
      "  --days <n>            sync the last n days (default 7)\n"
      "  --from <YYYY-MM-DD>   sync start date (overrides --days)\n"
      "  --to <YYYY-MM-DD>     sync end date (default today)\n"
      "  --activities <n>      max activities to list (default 50)\n"
      "  --no-fit              skip FIT downloads\n"
      "  --force               re-fetch / re-import even if already done\n"
      "  --out <file>          write `get` output to a file\n"
      "  --log <file>          append all output to a log file (for scheduled runs)\n",
      stderr);
}

std::filesystem::path resolve_data_dir(const Options& o) {
  if (!o.data_dir.empty()) return o.data_dir;
  const std::filesystem::path base = gutil::app_data_dir();
  return base.empty() ? std::filesystem::path("data") : base / "data";
}

bool open_db(const Options& o, store::Db& db, std::filesystem::path& data_dir) {
  data_dir = resolve_data_dir(o);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  std::string err;
  if (!db.open(data_dir / "garmin.db", err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return false;
  }
  return true;
}

// Loads saved tokens and returns a client, or null with a message printed.
std::unique_ptr<gc::GarminClient> make_client(bool require_login) {
  gc::Tokens t;
  std::string err;
  const bool loaded = gc::load_tokens(gc::default_token_path(), t, err);
  if (!loaded && require_login) {
    std::fprintf(stderr, "error: %s\nrun `gsync login` first\n", err.c_str());
    return nullptr;
  }
  return std::make_unique<gc::GarminClient>(t);
}

void persist_tokens(gc::GarminClient& client) {
  if (!client.tokens_dirty()) return;
  std::string err;
  if (!gc::save_tokens(gc::default_token_path(), client.tokens(), err)) {
    std::fprintf(stderr, "warning: %s\n", err.c_str());
    return;
  }
  client.clear_dirty();
}

void polite_delay() { Sleep(kPoliteDelayMs); }

std::string mfa_prompt() {
  std::string code;
  if (!gutil::read_line("MFA code: ", false, code)) return std::string();
  return code;
}

// Saves raw JSON next to the database for debugging / re-import.
void save_json(const std::filesystem::path& data_dir, const std::string& kind,
               const std::string& key, const json& j) {
  const std::filesystem::path p = data_dir / "json" / kind / (key + ".json");
  if (!gutil::write_text_file(p, j.dump(2))) {
    std::fprintf(stderr, "warning: could not write %s\n", p.string().c_str());
  }
}

struct SyncContext {
  const Options& opt;
  gc::GarminClient& client;
  store::Db& db;
  std::filesystem::path data_dir;
  std::string today;
  int failures = 0;
  int64_t rows = 0;
};

using DayFetch = bool (gc::GarminClient::*)(const std::string&, json&, std::string&);
using DayImport = bool (*)(store::Db&, const std::string&, const json&, store::ImportCounts&,
                           std::string&);

// One dated endpoint: fetch, save, import, log.
void sync_day_kind(SyncContext& cx, const char* kind, const std::string& date, DayFetch fetch,
                   DayImport import) {
  const bool final_day = date < cx.today;  // today's data is still changing
  if (!cx.opt.force && final_day && store::sync_done(cx.db, kind, date)) return;
  json j;
  std::string err;
  if (!(cx.client.*fetch)(date, j, err)) {
    std::fprintf(stderr, "  %s %s: %s\n", kind, date.c_str(), err.c_str());
    ++cx.failures;
    return;
  }
  polite_delay();
  if (!j.is_null()) save_json(cx.data_dir, kind, date, j);
  store::ImportCounts c;
  if (!import(cx.db, date, j, c, err)) {
    std::fprintf(stderr, "  %s %s: import failed: %s\n", kind, date.c_str(), err.c_str());
    ++cx.failures;
    return;
  }
  cx.rows += c.rows;
  if (final_day) store::mark_sync(cx.db, kind, date, true, err);
  std::printf("  %-10s %s  %lld rows\n", kind, date.c_str(), static_cast<long long>(c.rows));
}

// Adapters so every import has the (db, date, json, counts, err) shape.
bool import_hr_adapter(store::Db& db, const std::string&, const json& j, store::ImportCounts& c,
                       std::string& err) {
  return store::import_heart_rate(db, j, c, err);
}
bool import_stress_adapter(store::Db& db, const std::string&, const json& j,
                           store::ImportCounts& c, std::string& err) {
  return store::import_stress(db, j, c, err);
}

// Extracts FIT entries of a downloaded ZIP, saves them under `dir`, imports.
void import_zip(SyncContext& cx, const std::vector<uint8_t>& zip,
                const std::filesystem::path& dir, int64_t activity_id, const char* what) {
  std::vector<gutil::ZipEntry> entries;
  std::string err;
  if (!gutil::zip_extract_all(zip, entries, err)) {
    std::fprintf(stderr, "  %s: %s\n", what, err.c_str());
    ++cx.failures;
    return;
  }
  for (const gutil::ZipEntry& e : entries) {
    const std::filesystem::path name = std::filesystem::path(e.name).filename();
    std::string ext = name.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".fit") continue;
    const std::filesystem::path p = dir / name;
    if (!gutil::write_file(p, e.data.data(), e.data.size())) {
      std::fprintf(stderr, "  %s: cannot write %s\n", what, p.string().c_str());
      ++cx.failures;
      continue;
    }
    store::ImportCounts c;
    if (!store::import_fit(cx.db, p.string(), e.data, activity_id, cx.opt.force, c, err)) {
      std::fprintf(stderr, "  %s: %s\n", what, err.c_str());
      ++cx.failures;
      continue;
    }
    cx.rows += c.rows;
    std::printf("  fit        %-40s %lld msgs %lld rows%s\n", name.string().c_str(),
                static_cast<long long>(c.messages), static_cast<long long>(c.rows),
                c.skipped ? " (already imported)" : "");
  }
}

void sync_wellness_fit(SyncContext& cx, const std::string& date) {
  const char* kind = "wellness_fit";
  const bool final_day = date < cx.today;
  if (!cx.opt.force && final_day && store::sync_done(cx.db, kind, date)) return;
  std::vector<uint8_t> zip;
  std::string err;
  if (!cx.client.download_wellness_zip(date, zip, err)) {
    // 404 simply means no monitoring files for that day.
    const bool not_found = err.find("HTTP 404") != std::string::npos;
    if (!not_found) {
      std::fprintf(stderr, "  %s %s: %s\n", kind, date.c_str(), err.c_str());
      ++cx.failures;
    } else if (final_day) {
      store::mark_sync(cx.db, kind, date, true, err);
    }
    polite_delay();
    return;
  }
  polite_delay();
  import_zip(cx, zip, cx.data_dir / "fit" / "wellness" / date, 0, kind);
  if (final_day) store::mark_sync(cx.db, kind, date, true, err);
}

void sync_weight(SyncContext& cx, const std::string& from, const std::string& to) {
  std::string start = from;
  for (int i = 0; i < kMaxDays / kWeightChunkDays + 1 && start <= to; ++i) {
    std::string end = gutil::add_days(start, kWeightChunkDays - 1);
    if (end > to) end = to;
    json j;
    std::string err;
    if (!cx.client.weight_range(start, end, j, err)) {
      std::fprintf(stderr, "  weight %s..%s: %s\n", start.c_str(), end.c_str(), err.c_str());
      ++cx.failures;
    } else {
      save_json(cx.data_dir, "weight", start + "_" + end, j);
      store::ImportCounts c;
      if (!store::import_weight(cx.db, j, c, err)) {
        std::fprintf(stderr, "  weight: import failed: %s\n", err.c_str());
        ++cx.failures;
      } else {
        cx.rows += c.rows;
        std::printf("  %-10s %s..%s  %lld rows\n", "weight", start.c_str(), end.c_str(),
                    static_cast<long long>(c.rows));
      }
    }
    polite_delay();
    start = gutil::add_days(end, 1);
  }
}

void sync_activities(SyncContext& cx, int64_t from_ts) {
  const int max_items = std::min(cx.opt.max_activities, kMaxActivities);
  std::vector<int64_t> ids;
  bool more = true;
  for (int start = 0; start < max_items && more; start += kActivityPage) {
    json page;
    std::string err;
    const int limit = std::min(kActivityPage, max_items - start);
    if (!cx.client.activities(start, limit, page, err)) {
      std::fprintf(stderr, "  activities: %s\n", err.c_str());
      ++cx.failures;
      return;
    }
    polite_delay();
    if (!page.is_array() || page.empty()) break;
    store::ImportCounts c;
    if (!store::import_activity_list(cx.db, page, c, err)) {
      std::fprintf(stderr, "  activities: import failed: %s\n", err.c_str());
      ++cx.failures;
      return;
    }
    cx.rows += c.rows;
    for (const json& a : page) {
      int64_t ts = 0;
      const std::string start_gmt = a.value("startTimeGMT", "");
      if (gutil::parse_datetime(start_gmt, ts) && ts < from_ts) {
        more = false;
        break;
      }
      if (a.contains("activityId") && a["activityId"].is_number_integer()) {
        ids.push_back(a["activityId"].get<int64_t>());
      }
    }
    if (static_cast<int>(page.size()) < limit) more = false;
  }
  std::printf("  %-10s %zu in range\n", "activities", ids.size());
  if (cx.opt.no_fit) return;

  const std::filesystem::path dir = cx.data_dir / "fit" / "activities";
  for (const int64_t id : ids) {
    const std::filesystem::path p = dir / (std::to_string(id) + "_ACTIVITY.fit");
    if (!cx.opt.force && std::filesystem::exists(p)) {
      // Already on disk; make sure it is imported (cheap if it is).
      std::vector<uint8_t> bytes;
      store::ImportCounts c;
      std::string err;
      if (gutil::read_file(p, bytes) &&
          !store::import_fit(cx.db, p.string(), bytes, id, false, c, err)) {
        std::fprintf(stderr, "  activity %lld: %s\n", static_cast<long long>(id), err.c_str());
        ++cx.failures;
      }
      continue;
    }
    std::vector<uint8_t> zip;
    std::string err;
    if (!cx.client.download_activity_zip(id, zip, err)) {
      std::fprintf(stderr, "  activity %lld: %s\n", static_cast<long long>(id), err.c_str());
      ++cx.failures;
      polite_delay();
      continue;
    }
    polite_delay();
    import_zip(cx, zip, dir, id, "activity");
  }
}

// Imports every readings_*.csv in the user's Downloads folder (the Omron
// app exports there). Idempotent, so it runs on every sync.
int import_bp_downloads(store::Db& db, int64_t& rows) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) || raw == nullptr) return 0;
  const std::filesystem::path downloads(raw);
  CoTaskMemFree(raw);
  int failures = 0;
  std::error_code ec;
  int seen = 0;
  for (const auto& entry : std::filesystem::directory_iterator(downloads, ec)) {
    if (!entry.is_regular_file() || ++seen > 20000) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("readings_", 0) != 0 || entry.path().extension() != ".csv") continue;
    std::string text;
    store::ImportCounts c;
    std::string err;
    if (!gutil::read_text_file(entry.path(), text) || !store::import_bp_csv(db, text, c, err)) {
      std::fprintf(stderr, "  blood pressure %s: %s' + chr(92) + 'n", name.c_str(), err.c_str());
      ++failures;
      continue;
    }
    rows += c.rows;
    if (c.rows > 0) {
      std::printf("  %-10s %s  %lld new readings' + chr(92) + 'n", "bp", name.c_str(),
                  static_cast<long long>(c.rows));
    }
  }
  return failures;
}

int import_one_path(store::Db& db, const std::filesystem::path& p, bool force, int& files,
                    int64_t& rows) {
  int failures = 0;
  std::vector<std::filesystem::path> targets;
  std::error_code ec;
  if (std::filesystem::is_directory(p, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(p, ec)) {
      if (!entry.is_regular_file()) continue;
      std::string ext = entry.path().extension().string();
      for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (ext == ".fit") targets.push_back(entry.path());
      if (targets.size() >= 100000) break;
    }
  } else {
    targets.push_back(p);
  }
  for (const std::filesystem::path& f : targets) {
    std::vector<uint8_t> bytes;
    if (!gutil::read_file(f, bytes)) {
      std::fprintf(stderr, "  cannot read %s\n", f.string().c_str());
      ++failures;
      continue;
    }
    store::ImportCounts c;
    std::string err;
    if (!store::import_fit(db, f.string(), bytes, 0, force, c, err)) {
      std::fprintf(stderr, "  %s\n", err.c_str());
      ++failures;
      continue;
    }
    ++files;
    rows += c.rows;
    std::printf("  %-60s %lld msgs %lld rows%s\n", f.string().c_str(),
                static_cast<long long>(c.messages), static_cast<long long>(c.rows),
                c.skipped ? " (already imported)" : "");
  }
  return failures;
}

}  // namespace

// ------------------------------------------------------------------- parse

void usage() { usage_text(); }

bool parse(int argc, char** argv, Options& o, std::string& err) {
  if (argc < 2) return false;
  o.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = i + 1 < argc;
    if (a == "--data" && has_next) {
      o.data_dir = argv[++i];
    } else if (a == "--days" && has_next) {
      o.days = std::atoi(argv[++i]);
      if (o.days < 1 || o.days > kMaxDays) {
        err = "--days must be 1.." + std::to_string(kMaxDays);
        return false;
      }
    } else if (a == "--from" && has_next) {
      o.from_date = argv[++i];
    } else if (a == "--to" && has_next) {
      o.to_date = argv[++i];
    } else if (a == "--activities" && has_next) {
      o.max_activities = std::atoi(argv[++i]);
      if (o.max_activities < 0 || o.max_activities > kMaxActivities) {
        err = "--activities out of range";
        return false;
      }
    } else if (a == "--out" && has_next) {
      o.out_file = argv[++i];
    } else if (a == "--log" && has_next) {
      o.log_file = argv[++i];
    } else if (a == "--no-fit") {
      o.no_fit = true;
    } else if (a == "--force") {
      o.force = true;
    } else if (!a.empty() && a[0] == '-') {
      err = "unknown option " + a;
      return false;
    } else {
      o.args.push_back(a);
    }
  }
  int64_t tmp = 0;
  if (!o.from_date.empty() && !gutil::parse_date(o.from_date, tmp)) {
    err = "--from is not YYYY-MM-DD";
    return false;
  }
  if (!o.to_date.empty() && !gutil::parse_date(o.to_date, tmp)) {
    err = "--to is not YYYY-MM-DD";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- commands

int run_login(const Options&) {
  auto client = make_client(false);
  G_ASSERT(client != nullptr);
  std::string email;
  std::string password;
  if (!gutil::read_line("Garmin Connect email: ", false, email)) return 1;
  if (!gutil::read_line("Password: ", true, password)) return 1;
  std::string err;
  if (!client->login(email, password, mfa_prompt, err)) {
    std::fprintf(stderr, "login failed: %s\n", err.c_str());
    return 1;
  }
  SecureZeroMemory(password.data(), password.size());
  persist_tokens(*client);
  std::printf("signed in as %s (%s)\n", client->tokens().full_name.c_str(),
              client->tokens().display_name.c_str());
  std::printf("tokens saved to %s\n", gc::default_token_path().string().c_str());
  return 0;
}

int run_logout(const Options&) {
  std::error_code ec;
  const std::filesystem::path p = gc::default_token_path();
  if (std::filesystem::remove(p, ec)) {
    std::printf("removed %s\n", p.string().c_str());
  } else {
    std::printf("no saved login\n");
  }
  return 0;
}

int run_whoami(const Options&) {
  auto client = make_client(true);
  if (!client) return 1;
  std::string err;
  if (!client->fetch_profile(err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  persist_tokens(*client);
  const gc::Tokens& t = client->tokens();
  std::printf("%s (%s)\n", t.full_name.c_str(), t.display_name.c_str());
  std::printf("bearer token valid until %s\n", gutil::iso8601_utc(t.oauth2.expires_at).c_str());
  return 0;
}

int run_sync(const Options& o) {
  auto client = make_client(true);
  if (!client) return 1;
  store::Db db;
  std::filesystem::path data_dir;
  if (!open_db(o, db, data_dir)) return 1;

  const std::string today = gutil::date_string_local(gutil::now_unix());
  const std::string to = o.to_date.empty() ? today : o.to_date;
  const std::string from = o.from_date.empty() ? gutil::add_days(to, -(o.days - 1)) : o.from_date;
  if (from > to) {
    std::fprintf(stderr, "error: --from is after --to\n");
    return 2;
  }
  std::printf("syncing %s .. %s into %s\n", from.c_str(), to.c_str(), data_dir.string().c_str());

  SyncContext cx{o, *client, db, data_dir, today};
  std::string date = from;
  for (int i = 0; i < kMaxDays && date <= to; ++i) {
    sync_day_kind(cx, "summary", date, &gc::GarminClient::daily_summary,
                  &store::import_daily_summary);
    sync_day_kind(cx, "heartrate", date, &gc::GarminClient::daily_heart_rate, &import_hr_adapter);
    sync_day_kind(cx, "sleep", date, &gc::GarminClient::daily_sleep, &store::import_sleep);
    sync_day_kind(cx, "stress", date, &gc::GarminClient::daily_stress, &import_stress_adapter);
    sync_day_kind(cx, "hrv", date, &gc::GarminClient::daily_hrv, &store::import_hrv);
    if (!o.no_fit) sync_wellness_fit(cx, date);
    persist_tokens(*client);
    date = gutil::add_days(date, 1);
  }
  sync_weight(cx, from, to);
  cx.failures += import_bp_downloads(db, cx.rows);
  int64_t from_ts = 0;
  const bool parsed = gutil::parse_date(from, from_ts);
  G_ASSERT(parsed);
  if (o.max_activities > 0) sync_activities(cx, from_ts);
  persist_tokens(*client);

  std::printf("done: %lld rows written, %d failures\n", static_cast<long long>(cx.rows),
              cx.failures);
  return cx.failures == 0 ? 0 : 1;
}

int run_import(const Options& o) {
  if (o.args.empty()) {
    std::fprintf(stderr, "error: import needs at least one file or directory\n");
    return 2;
  }
  store::Db db;
  std::filesystem::path data_dir;
  if (!open_db(o, db, data_dir)) return 1;
  int failures = 0;
  int files = 0;
  int64_t rows = 0;
  for (const std::string& a : o.args) failures += import_one_path(db, a, o.force, files, rows);
  std::printf("imported %d files, %lld rows, %d failures\n", files, static_cast<long long>(rows),
              failures);
  return failures == 0 ? 0 : 1;
}

int run_get(const Options& o) {
  if (o.args.size() != 1 || o.args[0].empty() || o.args[0][0] != '/') {
    std::fprintf(stderr, "error: get needs an API path starting with '/'\n");
    return 2;
  }
  auto client = make_client(true);
  if (!client) return 1;
  std::string err;
  std::vector<uint8_t> bytes;
  if (!client->get_bytes(o.args[0], bytes, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  persist_tokens(*client);
  if (!o.out_file.empty()) {
    if (!gutil::write_file(o.out_file, bytes.data(), bytes.size())) {
      std::fprintf(stderr, "error: cannot write %s\n", o.out_file.c_str());
      return 1;
    }
    std::printf("wrote %zu bytes to %s\n", bytes.size(), o.out_file.c_str());
    return 0;
  }
  const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (j.is_discarded()) {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
  } else {
    std::printf("%s\n", j.dump(2).c_str());
  }
  return 0;
}

int run_import_bp(const Options& o) {
  store::Db db;
  std::filesystem::path data_dir;
  if (!open_db(o, db, data_dir)) return 1;
  int failures = 0;
  int64_t rows = 0;
  if (o.args.empty()) {
    failures = import_bp_downloads(db, rows);
  } else {
    for (const std::string& a : o.args) {
      std::string text;
      store::ImportCounts c;
      std::string err;
      if (!gutil::read_text_file(a, text) || !store::import_bp_csv(db, text, c, err)) {
        std::fprintf(stderr, "  %s: %s' + chr(92) + 'n", a.c_str(), err.empty() ? "cannot read" : err.c_str());
        ++failures;
        continue;
      }
      rows += c.rows;
      std::printf("  %s  %lld new readings' + chr(92) + 'n", a.c_str(), static_cast<long long>(c.rows));
    }
  }
  std::printf("imported %lld blood pressure readings, %d failures' + chr(92) + 'n",
              static_cast<long long>(rows), failures);
  return failures == 0 ? 0 : 1;
}

int run_stats(const Options& o) {
  store::Db db;
  std::filesystem::path data_dir;
  if (!open_db(o, db, data_dir)) return 1;
  static const char* kTables[] = {
      "daily_summary", "hr_sample",        "stress_sample",      "body_battery_sample",
      "sleep",         "sleep_stage",      "sleep_level_sample", "respiration_sample",
      "spo2_sample",   "hrv_daily",        "hrv_sample",         "weight",
      "activity",      "activity_session", "activity_lap",       "activity_record",
      "activity_hrv",  "fit_file",         "blood_pressure",    "sync_log"};
  std::printf("database: %s\n", (data_dir / "garmin.db").string().c_str());
  for (const char* t : kTables) {
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + t;
    store::Stmt s(db, sql.c_str());
    if (!s.ok() || !s.row()) continue;
    std::printf("  %-22s %10lld\n", t, static_cast<long long>(s.col_int(0)));
  }
  return 0;
}

}  // namespace cmd
