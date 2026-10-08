#include "strava/push.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "fit/fit_profile.h"
#include "gc/http_client.h"
#include "store/db.h"
#include "strava/api.h"
#include "strava/auth.h"
#include "util/assert.h"
#include "util/time_util.h"

namespace strava {

namespace {

constexpr int kMaxRateWaits = 2;        // per run; the rest waits for the next sync
constexpr int64_t kRateWindowS = 900;   // Strava's short limit resets every 15 minutes
constexpr int kPollTries = 30;
constexpr DWORD kPollMs = 2000;
constexpr DWORD kSleepSliceMs = 250;
constexpr int64_t kListMarginS = 86400;
constexpr int kMaxRows = 100000;

constexpr char kCandidateSql[] =
    "SELECT s.start_ts, s.sport, s.timer_s, f.path,"
    " (SELECT COUNT(*) FROM activity_record r WHERE r.fit_file_id = s.fit_file_id) AS n"
    " FROM activity_session s JOIN fit_file f ON f.id = s.fit_file_id"
    " LEFT JOIN strava_push p ON p.start_ts = s.start_ts"
    " WHERE s.start_ts >= ?1 AND s.sport IS NOT NULL"
    "   AND (p.state IS NULL OR (p.state = 'failed' AND p.attempts < ?2))"
    " ORDER BY s.start_ts, n DESC";

constexpr char kRecordSql[] =
    "INSERT INTO strava_push(start_ts, state, strava_id, attempts, note, at) VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
    " ON CONFLICT(start_ts) DO UPDATE SET state = excluded.state, strava_id = excluded.strava_id,"
    " attempts = strava_push.attempts + excluded.attempts, note = excluded.note, at = excluded.at";

enum class Outcome { kDone, kLeft, kFailed, kStop };

struct Ctx {
  const PushOptions& opt;
  const Report& report;
  store::Db& db;
  Client& client;
  PushResult& r;
  int rate_waits = 0;
};

bool cancelled(const PushOptions& o) { return o.cancel != nullptr && o.cancel->load(); }

std::string fmt(const char* f, ...) {
  G_ASSERT(f != nullptr);
  char buf[512] = {};
  va_list args;
  va_start(args, f);
  const int n = std::vsnprintf(buf, sizeof(buf), f, args);
  va_end(args);
  G_REQUIRE_RET(n >= 0, std::string());
  return std::string(buf);
}

// "2024-01-04 06:17" in local time.
std::string when(int64_t ts) {
  G_ASSERT(ts > 0);
  const std::time_t t = static_cast<std::time_t>(ts);
  std::tm lt{};
  if (localtime_s(&lt, &t) != 0) return gutil::date_string_utc(ts);
  char buf[32] = {};
  const size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
  G_ASSERT(n > 0);
  return std::string(buf, n);
}

// Waits for Strava's next 15-minute window; false when stopped or out of waits.
bool wait_rate(Ctx& cx) {
  if (cx.rate_waits >= kMaxRateWaits) return false;
  ++cx.rate_waits;
  const int64_t wait_s = kRateWindowS - gutil::now_unix() % kRateWindowS + 5;
  cx.report(false, fmt("  Strava rate limit reached: waiting %lld s", static_cast<long long>(wait_s)));
  const int64_t slices = wait_s * 1000 / kSleepSliceMs;
  for (int64_t i = 0; i < slices && i < kRateWindowS * 8; ++i) {
    if (cancelled(cx.opt)) return false;
    Sleep(kSleepSliceMs);
  }
  return true;
}

// Repeats `fn` while Strava says "rate limited", as long as waits remain.
template <typename F>
Status with_retry(Ctx& cx, const F& fn) {
  Status s = fn();
  for (int i = 0; i < kMaxRateWaits && s == Status::kRateLimited; ++i) {
    if (!wait_rate(cx)) break;
    s = fn();
  }
  return s;
}

void record(store::Db& db, int64_t start_ts, const char* state, int64_t strava_id, const std::string& note) {
  G_ASSERT(start_ts > 0 && state != nullptr);
  store::Stmt st(db, kRecordSql);
  G_REQUIRE_VOID(st.ok());
  const bool failed = std::string(state) == "failed";
  st.bind(1, start_ts).bind(2, std::string(state)).bind(4, failed ? 1 : 0).bind(5, note).bind(6, gutil::now_unix());
  if (strava_id > 0) {
    st.bind(3, strava_id);
  } else {
    st.bind_null(3);
  }
  std::string err;
  const bool ok = st.run(err);
  (void)ok;  // a lost record only means the activity is checked again next time
}

// A non-OK status from Strava: stop the run (login gone, rate limit), or count a failure.
Outcome on_error(Ctx& cx, const Candidate& c, Status s, const std::string& err) {
  G_ASSERT(s != Status::kOk);
  if (s == Status::kUnauthorized) {
    cx.r.login_required = true;
    return Outcome::kStop;
  }
  if (s == Status::kRateLimited) return Outcome::kStop;
  cx.report(true, fmt("  %s  failed: %s", when(c.start_ts).c_str(), err.c_str()));
  record(cx.db, c.start_ts, "failed", 0, err);
  return Outcome::kFailed;
}

Status set_type(Ctx& cx, int64_t id, const SportRule& rule, std::string& err) {
  G_ASSERT(id > 0 && !rule.sport_type.empty());
  return with_retry(cx, [&] { return cx.client.update(id, rule.sport_type, rule.name, err); });
}

// Already on Strava: set the type and title if they differ, then mark done.
Outcome fix_existing(Ctx& cx, const Candidate& c, const SportRule& rule, const Activity& a) {
  G_ASSERT(a.id > 0);
  const bool same = a.sport_type == rule.sport_type && (rule.name.empty() || a.name == rule.name);
  if (!same) {
    std::string err;
    const Status s = set_type(cx, a.id, rule, err);
    if (s != Status::kOk) return on_error(cx, c, s, err);
    ++cx.r.updated;
    cx.report(false, fmt("  %s  %-12s on Strava, set to %s '%s'", when(c.start_ts).c_str(),
                         fit::sport_name(static_cast<uint8_t>(c.sport)), rule.sport_type.c_str(), rule.name.c_str()));
  } else {
    ++cx.r.unchanged;
  }
  record(cx.db, c.start_ts, "done", a.id, std::string());
  return Outcome::kDone;
}

// Waits for Strava to finish processing an upload (it takes a few seconds).
Status poll_upload(Ctx& cx, UploadState& u, std::string& err) {
  G_ASSERT(u.upload_id > 0);
  Status s = Status::kOk;
  for (int i = 0; i < kPollTries && u.activity_id == 0 && u.error.empty(); ++i) {
    if (cancelled(cx.opt)) break;
    Sleep(kPollMs);
    s = with_retry(cx, [&] { return cx.client.upload_status(u.upload_id, u, err); });
    if (s != Status::kOk) break;
  }
  return s;
}

// Not on Strava: upload the FIT file, then set the type and title.
Outcome upload_new(Ctx& cx, const Candidate& c, const SportRule& rule) {
  std::string err;
  UploadState u;
  const std::string ext_id = "gview-" + std::to_string(c.start_ts);
  Status s = with_retry(cx, [&] { return cx.client.upload(c.fit, ext_id, u, err); });
  if (s == Status::kOk) s = poll_upload(cx, u, err);
  if (s != Status::kOk) return on_error(cx, c, s, err);
  if (u.duplicate_of > 0) {
    Activity a;
    a.id = u.duplicate_of;
    return fix_existing(cx, c, rule, a);  // the empty type/name makes it set both
  }
  if (!u.error.empty()) return on_error(cx, c, Status::kFailed, "Strava rejected the file: " + u.error);
  if (u.activity_id == 0) return Outcome::kLeft;  // still processing: matched by start time next run
  s = set_type(cx, u.activity_id, rule, err);
  if (s != Status::kOk) return on_error(cx, c, s, err);
  ++cx.r.uploaded;
  record(cx.db, c.start_ts, "done", u.activity_id, std::string());
  cx.report(false, fmt("  %s  %-12s uploaded as %s '%s'", when(c.start_ts).c_str(),
                       fit::sport_name(static_cast<uint8_t>(c.sport)), rule.sport_type.c_str(), rule.name.c_str()));
  return Outcome::kDone;
}

Outcome push_one(Ctx& cx, const Candidate& c, const Settings& st, const std::vector<Activity>& acts) {
  const SportRule* rule = find_rule(st, c.sport);
  G_REQUIRE_RET(rule != nullptr && rule->enabled, Outcome::kDone);
  const Activity* m = find_match(acts, c.start_ts);
  return m != nullptr ? fix_existing(cx, c, *rule, *m) : upload_new(cx, c, *rule);
}

// Login from strava.bin, renewed if due; false (with the reason reported) when unusable.
bool fresh_login(const PushOptions& o, const Report& report, gc::HttpClient& http, Login& l, PushResult& r) {
  std::string err;
  if (!load_login(login_path(o.profile_base), l, err)) {
    report(true, "Strava: " + err);
    ++r.failures;
    return false;
  }
  bool changed = false;
  if (!ensure_fresh(http, l, changed, err)) {
    r.login_required = true;
    report(true, "Strava: " + err + ". Connect again with Data > Strava > Connect to Strava.");
    return false;
  }
  if (changed && !save_login(login_path(o.profile_base), l, err)) report(true, "Strava: warning: " + err);
  return true;
}

void push_all(Ctx& cx, const Settings& st, const std::vector<Candidate>& cands, const std::vector<Activity>& acts) {
  G_ASSERT(cands.size() <= static_cast<size_t>(kMaxPerRun));
  for (size_t i = 0; i < cands.size(); ++i) {
    if (cancelled(cx.opt)) {
      cx.r.left += static_cast<int>(cands.size() - i);
      return;
    }
    const Outcome out = push_one(cx, cands[i], st, acts);
    if (out == Outcome::kFailed) ++cx.r.failures;
    if (out == Outcome::kLeft) ++cx.r.left;
    if (out == Outcome::kStop) {
      cx.r.left += static_cast<int>(cands.size() - i);
      return;
    }
  }
}

}  // namespace

std::vector<Candidate> select_candidates(store::Db& db, const Settings& s) {
  std::vector<Candidate> out;
  int64_t since = 0;
  G_REQUIRE_RET(gutil::parse_date(s.since, since), out);
  store::Stmt st(db, kCandidateSql);
  G_REQUIRE_RET(st.ok(), out);
  st.bind(1, since).bind(2, kMaxAttempts);
  int64_t last = 0;
  for (int n = 0; n < kMaxRows && out.size() < static_cast<size_t>(kMaxPerRun) && st.row(); ++n) {
    const int64_t ts = st.col_int(0);
    if (ts == last) continue;  // a record-less twin of the row just taken
    last = ts;
    const SportRule* rule = find_rule(s, static_cast<int>(st.col_int(1)));
    if (rule == nullptr || !rule->enabled) continue;
    if (st.col_null(2) || st.col_double(2) < rule->min_minutes * 60.0) continue;
    out.push_back(Candidate{ts, rule->sport, std::filesystem::path(st.col_text(3))});  // stored with path::string()
  }
  G_ASSERT(out.size() <= static_cast<size_t>(kMaxPerRun));
  return out;
}

std::string summary(const PushResult& r) {
  std::string s = fmt("Strava: %d uploaded, %d updated, %d already right, %d failed", r.uploaded, r.updated,
                      r.unchanged, r.failures);
  if (r.left > 0) s += fmt(", %d left for next time", r.left);
  return s;
}

PushResult run_push(const PushOptions& o, const Report& report) {
  G_ASSERT(report != nullptr);
  G_REQUIRE_RET(!o.profile_base.empty() && !o.data_dir.empty(), PushResult{});
  PushResult r;
  if (!connected(o.profile_base)) {
    r.not_connected = true;
    return r;
  }
  const Settings st = load_settings(settings_path(o.profile_base));
  if (!any_enabled(st)) {
    r.nothing_chosen = true;
    report(false, "Strava: no sport chosen yet (Data > Strava > Settings)");
    return r;
  }
  store::Db db;
  std::string err;
  if (!db.open(o.data_dir / "garmin.db", err)) {
    report(true, "Strava: " + err);
    ++r.failures;
    return r;
  }
  const std::vector<Candidate> cands = select_candidates(db, st);
  if (cands.empty()) {
    report(false, "Strava: nothing new to send");
    return r;
  }
  gc::HttpClient http(L"gview");
  Login l;
  if (!http.ok() || !fresh_login(o, report, http, l, r)) {
    r.left = static_cast<int>(cands.size());
    return r;
  }
  Client client(http, l.access_token);
  Ctx cx{o, report, db, client, r};
  std::vector<Activity> acts;
  const Status s = with_retry(cx, [&] {
    return client.list(cands.front().start_ts - kListMarginS, cands.back().start_ts + kListMarginS, acts, err);
  });
  if (s != Status::kOk) {
    r.login_required = s == Status::kUnauthorized;
    report(true, "Strava: could not list activities: " + err);
    r.left = static_cast<int>(cands.size());
    return r;
  }
  report(false, fmt("Strava: %zu activities to check against %zu on Strava", cands.size(), acts.size()));
  push_all(cx, st, cands, acts);
  if (r.login_required) report(true, "Strava: the saved login was refused. Connect again with Data > Strava.");
  report(r.failures > 0, summary(r));
  return r;
}

}  // namespace strava
