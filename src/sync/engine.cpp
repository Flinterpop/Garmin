#include "sync/engine.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <memory>

#include <nlohmann/json.hpp>

#include "gc/gc_client.h"
#include "gc/token_store.h"
#include "store/db.h"
#include "store/importer.h"
#include "strava/auth.h"
#include "strava/push.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"
#include "util/zip_reader.h"

namespace syncer {

using nlohmann::json;

namespace {

constexpr int kActivityPage = 50;
constexpr DWORD kPoliteDelayMs = 300;  // between API calls
constexpr int kWeightChunkDays = 31;
constexpr int kMaxDownloads = 20000;   // files looked at in Downloads

void polite_delay() { Sleep(kPoliteDelayMs); }

bool cancelled(const SyncOptions& o) { return o.cancel != nullptr && o.cancel->load(); }

struct SyncContext {
  const SyncOptions& opt;
  const Report& report;
  gc::GarminClient& client;
  store::Db& db;
  std::string today;
  int failures = 0;
  int64_t rows = 0;
};

void fail(SyncContext& cx, const std::string& line) {
  cx.report(true, line);
  ++cx.failures;
}

// Saves raw JSON next to the database for debugging / re-import.
void save_json(SyncContext& cx, const std::string& kind, const std::string& key, const json& j) {
  const std::filesystem::path p = cx.opt.data_dir / "json" / kind / (key + ".json");
  if (!gutil::write_text_file(p, j.dump(2))) cx.report(true, "warning: could not write " + p.string());
}

using DayFetch = bool (gc::GarminClient::*)(const std::string&, json&, std::string&);
using DayImport = bool (*)(store::Db&, const std::string&, const json&, store::ImportCounts&,
                           std::string&);

// One dated endpoint: fetch, save, import, report.
void sync_day_kind(SyncContext& cx, const char* kind, const std::string& date, DayFetch fetch,
                   DayImport import) {
  if (cx.client.login_required()) return;  // already reported once
  const bool final_day = date < cx.today;  // today's data is still changing
  if (!cx.opt.force && final_day && store::sync_done(cx.db, kind, date)) return;
  json j;
  std::string err;
  if (!(cx.client.*fetch)(date, j, err)) {
    fail(cx, strf("  %s %s: %s", kind, date.c_str(), err.c_str()));
    return;
  }
  polite_delay();
  if (!j.is_null()) save_json(cx, kind, date, j);
  store::ImportCounts c;
  if (!import(cx.db, date, j, c, err)) {
    fail(cx, strf("  %s %s: import failed: %s", kind, date.c_str(), err.c_str()));
    return;
  }
  cx.rows += c.rows;
  if (final_day) store::mark_sync(cx.db, kind, date, true, err);
  cx.report(false, strf("  %-10s %s  %lld rows", kind, date.c_str(), static_cast<long long>(c.rows)));
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
void import_zip(SyncContext& cx, const std::vector<uint8_t>& zip, const std::filesystem::path& dir,
                int64_t activity_id, const char* what) {
  std::vector<gutil::ZipEntry> entries;
  std::string err;
  if (!gutil::zip_extract_all(zip, entries, err)) {
    fail(cx, strf("  %s: %s", what, err.c_str()));
    return;
  }
  for (const gutil::ZipEntry& e : entries) {
    const std::filesystem::path name = std::filesystem::path(e.name).filename();
    std::string ext = name.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".fit") continue;
    const std::filesystem::path p = dir / name;
    if (!gutil::write_file(p, e.data.data(), e.data.size())) {
      fail(cx, strf("  %s: cannot write %s", what, p.string().c_str()));
      continue;
    }
    store::ImportCounts c;
    if (!store::import_fit(cx.db, p.string(), e.data, activity_id, cx.opt.force, c, err)) {
      fail(cx, strf("  %s: %s", what, err.c_str()));
      continue;
    }
    cx.rows += c.rows;
    cx.report(false, strf("  fit        %-40s %lld msgs %lld rows%s", name.string().c_str(),
                          static_cast<long long>(c.messages), static_cast<long long>(c.rows),
                          c.skipped ? " (already imported)" : ""));
  }
}

void sync_wellness_fit(SyncContext& cx, const std::string& date) {
  if (cx.client.login_required()) return;  // already reported once
  const char* kind = "wellness_fit";
  const bool final_day = date < cx.today;
  if (!cx.opt.force && final_day && store::sync_done(cx.db, kind, date)) return;
  std::vector<uint8_t> zip;
  std::string err;
  if (!cx.client.download_wellness_zip(date, zip, err)) {
    // 404 simply means no monitoring files for that day.
    const bool not_found = err.find("HTTP 404") != std::string::npos;
    if (!not_found) {
      fail(cx, strf("  %s %s: %s", kind, date.c_str(), err.c_str()));
    } else if (final_day) {
      store::mark_sync(cx.db, kind, date, true, err);
    }
    polite_delay();
    return;
  }
  polite_delay();
  import_zip(cx, zip, cx.opt.data_dir / "fit" / "wellness" / date, 0, kind);
  if (final_day) store::mark_sync(cx.db, kind, date, true, err);
}

void sync_weight(SyncContext& cx, const std::string& from, const std::string& to) {
  std::string start = from;
  for (int i = 0; i < kMaxDays / kWeightChunkDays + 1 && start <= to && !cancelled(cx.opt); ++i) {
    std::string end = gutil::add_days(start, kWeightChunkDays - 1);
    if (end > to) end = to;
    json j;
    std::string err;
    if (!cx.client.weight_range(start, end, j, err)) {
      fail(cx, strf("  weight %s..%s: %s", start.c_str(), end.c_str(), err.c_str()));
    } else {
      save_json(cx, "weight", start + "_" + end, j);
      store::ImportCounts c;
      if (!store::import_weight(cx.db, j, c, err)) {
        fail(cx, "  weight: import failed: " + err);
      } else {
        cx.rows += c.rows;
        cx.report(false, strf("  %-10s %s..%s  %lld rows", "weight", start.c_str(), end.c_str(),
                              static_cast<long long>(c.rows)));
      }
    }
    polite_delay();
    start = gutil::add_days(end, 1);
  }
}

// Pages through the activity list until it passes `from_ts`; returns the ids in range.
bool list_activities(SyncContext& cx, int64_t from_ts, std::vector<int64_t>& ids) {
  G_ASSERT(from_ts > 0 && cx.opt.max_activities >= 0);
  G_ASSERT(ids.empty());
  const int max_items = std::min(cx.opt.max_activities, kMaxActivities);
  bool more = true;
  for (int start = 0; start < max_items && more && !cancelled(cx.opt); start += kActivityPage) {
    json page;
    std::string err;
    const int limit = std::min(kActivityPage, max_items - start);
    if (!cx.client.activities(start, limit, page, err)) {
      fail(cx, "  activities: " + err);
      return false;
    }
    polite_delay();
    if (!page.is_array() || page.empty()) break;
    store::ImportCounts c;
    if (!store::import_activity_list(cx.db, page, c, err)) {
      fail(cx, "  activities: import failed: " + err);
      return false;
    }
    cx.rows += c.rows;
    for (const json& a : page) {
      int64_t ts = 0;
      if (gutil::parse_datetime(a.value("startTimeGMT", ""), ts) && ts < from_ts) {
        more = false;
        break;
      }
      if (a.contains("activityId") && a["activityId"].is_number_integer()) {
        ids.push_back(a["activityId"].get<int64_t>());
      }
    }
    if (static_cast<int>(page.size()) < limit) more = false;
  }
  G_ASSERT(ids.size() <= static_cast<size_t>(max_items));
  return true;
}

// Downloads (or re-imports from disk) one activity's FIT file.
void fetch_activity(SyncContext& cx, int64_t id, const std::filesystem::path& dir) {
  const std::filesystem::path p = dir / (std::to_string(id) + "_ACTIVITY.fit");
  std::error_code ec;
  if (!cx.opt.force && std::filesystem::exists(p, ec)) {
    // Already on disk; make sure it is imported (cheap if it is).
    std::vector<uint8_t> bytes;
    store::ImportCounts c;
    std::string err;
    if (gutil::read_file(p, bytes) && !store::import_fit(cx.db, p.string(), bytes, id, false, c, err)) {
      fail(cx, strf("  activity %lld: %s", static_cast<long long>(id), err.c_str()));
    }
    return;
  }
  std::vector<uint8_t> zip;
  std::string err;
  if (!cx.client.download_activity_zip(id, zip, err)) {
    fail(cx, strf("  activity %lld: %s", static_cast<long long>(id), err.c_str()));
    polite_delay();
    return;
  }
  polite_delay();
  import_zip(cx, zip, dir, id, "activity");
}

void sync_activities(SyncContext& cx, int64_t from_ts) {
  G_ASSERT(from_ts > 0);
  G_ASSERT(cx.opt.max_activities >= 0);
  std::vector<int64_t> ids;
  if (!list_activities(cx, from_ts, ids)) return;
  cx.report(false, strf("  %-10s %zu in range", "activities", ids.size()));
  if (cx.opt.no_fit) return;
  const std::filesystem::path dir = cx.opt.data_dir / "fit" / "activities";
  for (size_t i = 0; i < ids.size() && i < static_cast<size_t>(kMaxActivities); ++i) {
    if (cancelled(cx.opt)) return;
    fetch_activity(cx, ids[i], dir);
  }
}

void persist_tokens(const SyncOptions& o, gc::GarminClient& client, const Report& report) {
  if (!client.tokens_dirty()) return;
  std::string err;
  if (!gc::save_tokens(gc::token_path(o.profile_base), client.tokens(), err)) {
    report(true, "warning: " + err);
    return;
  }
  client.clear_dirty();
}

// The per-day endpoints for every date in range, then weight, blood pressure and activities.
void sync_range(SyncContext& cx, SyncResult& r) {
  std::string date = cx.opt.from;
  for (int i = 0; i < kMaxDays && date <= cx.opt.to; ++i) {
    if (cancelled(cx.opt)) break;
    sync_day_kind(cx, "summary", date, &gc::GarminClient::daily_summary, &store::import_daily_summary);
    sync_day_kind(cx, "heartrate", date, &gc::GarminClient::daily_heart_rate, &import_hr_adapter);
    sync_day_kind(cx, "sleep", date, &gc::GarminClient::daily_sleep, &store::import_sleep);
    sync_day_kind(cx, "stress", date, &gc::GarminClient::daily_stress, &import_stress_adapter);
    sync_day_kind(cx, "hrv", date, &gc::GarminClient::daily_hrv, &store::import_hrv);
    if (!cx.opt.no_fit) sync_wellness_fit(cx, date);
    persist_tokens(cx.opt, cx.client, cx.report);
    if (cx.client.login_required()) break;
    date = gutil::add_days(date, 1);
  }
  const bool go_on = !cx.client.login_required() && !cancelled(cx.opt);
  if (go_on) sync_weight(cx, cx.opt.from, cx.opt.to);
  cx.failures += import_bp_downloads(cx.db, cx.rows, cx.report);  // local files, no login needed
  int64_t from_ts = 0;
  const bool parsed = gutil::parse_date(cx.opt.from, from_ts);
  G_ASSERT(parsed);
  if (go_on && cx.opt.max_activities > 0) sync_activities(cx, from_ts);
  persist_tokens(cx.opt, cx.client, cx.report);
  r.cancelled = cancelled(cx.opt);
}

}  // namespace

std::string strf(const char* fmt, ...) {
  G_ASSERT(fmt != nullptr);
  char buf[1024] = {};
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  G_REQUIRE_RET(n >= 0, std::string());
  return std::string(buf, static_cast<size_t>(std::min<int>(n, sizeof(buf) - 1)));
}

bool open_db(const std::filesystem::path& data_dir, store::Db& db, const Report& report) {
  G_REQUIRE_RET(!data_dir.empty(), false);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  std::string err;
  if (!db.open(data_dir / "garmin.db", err)) {
    report(true, "error: " + err);
    return false;
  }
  return true;
}

int import_bp_downloads(store::Db& db, int64_t& rows, const Report& report) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) || raw == nullptr) return 0;
  const std::filesystem::path downloads(raw);
  CoTaskMemFree(raw);
  int failures = 0;
  std::error_code ec;
  int seen = 0;
  for (const auto& entry : std::filesystem::directory_iterator(downloads, ec)) {
    if (!entry.is_regular_file() || ++seen > kMaxDownloads) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("readings_", 0) != 0 || entry.path().extension() != ".csv") continue;
    std::string text;
    store::ImportCounts c;
    std::string err;
    if (!gutil::read_text_file(entry.path(), text) || !store::import_bp_csv(db, text, c, err)) {
      report(true, strf("  blood pressure %s: %s", name.c_str(), err.c_str()));
      ++failures;
      continue;
    }
    rows += c.rows;
    if (c.rows > 0) {
      report(false, strf("  %-10s %s  %lld new readings", "bp", name.c_str(), static_cast<long long>(c.rows)));
    }
  }
  return failures;
}

SyncResult run_sync(const SyncOptions& o, const Report& report) {
  G_ASSERT(report != nullptr);
  G_ASSERT(!o.profile_base.empty() && !o.data_dir.empty());
  SyncResult r;
  gc::Tokens t;
  std::string err;
  if (!gc::load_tokens(gc::token_path(o.profile_base), t, err)) {
    report(true, "error: " + err);
    r.not_logged_in = true;
    return r;
  }
  G_REQUIRE_RET(!o.from.empty() && o.from <= o.to, r);
  gc::GarminClient client(t);
  store::Db db;
  if (!open_db(o.data_dir, db, report)) {
    r.db_error = true;
    return r;
  }
  report(false, strf("syncing %s .. %s into %s", o.from.c_str(), o.to.c_str(), o.data_dir.string().c_str()));
  SyncContext cx{o, report, client, db, gutil::date_string_local(gutil::now_unix())};
  sync_range(cx, r);
  r.rows = cx.rows;
  r.failures = cx.failures;
  r.login_required = client.login_required();
  report(false, strf("done: %lld rows written, %d failures%s", static_cast<long long>(r.rows),
                     r.failures, r.cancelled ? " (stopped early)" : ""));
  if (!r.cancelled && strava::connected(o.profile_base)) {
    const strava::PushOptions po{o.profile_base, o.data_dir, o.cancel};
    const strava::PushResult pr = strava::run_push(po, report);
    r.strava_failures = pr.failures + (pr.login_required ? 1 : 0);
  }
  return r;
}

int exit_code(const SyncResult& r) {
  if (r.login_required) return kExitLoginRequired;
  return (r.failures == 0 && !r.not_logged_in && !r.db_error && !r.cancelled) ? 0 : 1;
}

}  // namespace syncer
