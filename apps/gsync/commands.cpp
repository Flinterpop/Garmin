#include "commands.h"

#include <windows.h>

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
#include "sync/account.h"
#include "sync/engine.h"
#include "util/assert.h"
#include "util/console.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace cmd {

using nlohmann::json;
using syncer::kMaxActivities;
using syncer::kMaxDays;

namespace {

constexpr char kUsage[] =
      "usage: gsync [options] <command> [options]\n"
      "commands:\n"
      "  login                 sign in to Garmin Connect (prompts; supports MFA)\n"
      "  logout                delete the saved tokens\n"
      "  whoami                show the signed-in account\n"
      "  sync                  pull daily data, activities and FIT files\n"
      "  import <path>...      import FIT files or directories (e.g. from the watch USB)\n"
      "  get <api-path>        raw authenticated GET, prints JSON (debugging)\n"
      "  stats                 row counts in the local database\n"
      "  import-bp <csv>...    import Omron blood-pressure CSV exports\n"
      "  profiles              list profiles and whether each is logged in\n"
      "  migrate-appdata       copy login, data and tiles from %LOCALAPPDATA%\\GarminSync\n"
      "                        (v0.1.3 and earlier) to the folder beside gsync.exe\n"
      "options:\n"
      "  --profile <name>      use this person's login and data (profiles\\<name> beside\n"
      "                        gsync.exe); without it, the default profile\n"
      "  --data <dir>          data directory (default: data\\ in the profile folder)\n"
      "  --days <n>            sync the last n days (default 7)\n"
      "  --from <YYYY-MM-DD>   sync start date (overrides --days)\n"
      "  --to <YYYY-MM-DD>     sync end date (default today)\n"
      "  --activities <n>      max activities to list (default 50)\n"
      "  --no-fit              skip FIT downloads\n"
      "  --force               re-fetch / re-import even if already done\n"
      "  --out <file>          write `get` output to a file\n"
      "  --log <file>          append all output to a log file (for scheduled runs)\n"
      "exit codes: 0 ok, 1 some fetches failed, 2 usage error, 3 login required\n";

void usage_text() { std::fputs(kUsage, stderr); }

// Progress lines from the shared sync library go to the console (errors to stderr).
void console_report(bool error, const std::string& line) {
  std::fprintf(error ? stderr : stdout, "%s\n", line.c_str());
}

// The profile's folder beside the exe: logins and data never go to AppData.
std::filesystem::path profile_base(const Options& o) {
  G_ASSERT(gutil::valid_profile_name(o.profile));
  const std::filesystem::path exe = gutil::exe_dir();
  G_REQUIRE_RET(!exe.empty(), std::filesystem::path("."));
  return gutil::profile_dir(exe, o.profile);
}

std::filesystem::path resolve_data_dir(const Options& o) {
  if (!o.data_dir.empty()) return o.data_dir;
  return profile_base(o) / "data";
}

std::string profile_label(const std::string& profile) {
  return profile.empty() ? std::string("(default)") : profile;
}

std::string profile_flag(const Options& o) {
  return o.profile.empty() ? std::string() : " --profile " + o.profile;
}

bool open_db(const Options& o, store::Db& db, std::filesystem::path& data_dir) {
  data_dir = resolve_data_dir(o);
  return syncer::open_db(data_dir, db, console_report);
}

// Loads saved tokens and returns a client, or null with a message printed.
std::unique_ptr<gc::GarminClient> make_client(const Options& o) {
  gc::Tokens t;
  std::string err;
  if (!gc::load_tokens(gc::token_path(profile_base(o)), t, err)) {
    std::fprintf(stderr, "error: %s\nrun `gsync%s login` first\n", err.c_str(), profile_flag(o).c_str());
    return nullptr;
  }
  return std::make_unique<gc::GarminClient>(t);
}

void persist_tokens(const Options& o, gc::GarminClient& client) {
  if (!client.tokens_dirty()) return;
  std::string err;
  if (!gc::save_tokens(gc::token_path(profile_base(o)), client.tokens(), err)) {
    std::fprintf(stderr, "warning: %s\n", err.c_str());
    return;
  }
  client.clear_dirty();
}

std::string mfa_prompt() {
  std::string code;
  if (!gutil::read_line("MFA code: ", false, code)) return std::string();
  return code;
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
  G_ASSERT(argv != nullptr);
  if (argc < 2) return false;
  // Options may come before or after the command: `gsync --profile ann login`.
  for (int i = 1; i < argc; ++i) {
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
    } else if (a == "--profile" && has_next) {
      o.profile = argv[++i];
      if (o.profile.empty() || !gutil::valid_profile_name(o.profile)) {
        err = "--profile: 1-32 characters of letters, digits, _ and -";
        return false;
      }
    } else if (a == "--no-fit") {
      o.no_fit = true;
    } else if (a == "--force") {
      o.force = true;
    } else if (!a.empty() && a[0] == '-') {
      err = "unknown option " + a;
      return false;
    } else if (o.command.empty()) {
      o.command = a;
    } else {
      o.args.push_back(a);
    }
  }
  if (o.command.empty()) {
    err = "no command";
    return false;
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

int run_login(const Options& o) {
  std::printf("profile: %s\n", profile_label(o.profile).c_str());
  std::string email;
  std::string password;
  if (!gutil::read_line("Garmin Connect email: ", false, email)) return 1;
  if (!gutil::read_line("Password: ", true, password)) return 1;
  const syncer::LoginResult r = syncer::login(profile_base(o), email, password, mfa_prompt);
  G_ASSERT(password.empty());  // zeroed by login()
  if (!r.ok) {
    std::fprintf(stderr, "login failed: %s\n", r.error.c_str());
    return 1;
  }
  std::printf("signed in as %s\n", r.who.c_str());
  std::printf("tokens saved to %s\n", gc::token_path(profile_base(o)).string().c_str());
  return 0;
}

int run_logout(const Options& o) {
  std::error_code ec;
  const std::filesystem::path p = gc::token_path(profile_base(o));
  if (std::filesystem::remove(p, ec)) {
    std::printf("removed %s\n", p.string().c_str());
  } else {
    std::printf("no saved login\n");
  }
  return 0;
}

int run_whoami(const Options& o) {
  auto client = make_client(o);
  if (!client) return 1;
  std::string err;
  if (!client->fetch_profile(err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  persist_tokens(o, *client);
  const gc::Tokens& t = client->tokens();
  std::printf("%s (%s)\n", t.full_name.c_str(), t.display_name.c_str());
  std::printf("bearer token valid until %s\n", gutil::iso8601_utc(t.oauth2.expires_at).c_str());
  return 0;
}

int run_sync(const Options& o) {
  syncer::SyncOptions so;
  so.profile_base = profile_base(o);
  so.data_dir = resolve_data_dir(o);
  const std::string today = gutil::date_string_local(gutil::now_unix());
  so.to = o.to_date.empty() ? today : o.to_date;
  so.from = o.from_date.empty() ? gutil::add_days(so.to, -(o.days - 1)) : o.from_date;
  if (so.from > so.to) {
    std::fprintf(stderr, "error: --from is after --to\n");
    return 2;
  }
  so.max_activities = o.max_activities;
  so.no_fit = o.no_fit;
  so.force = o.force;
  const syncer::SyncResult r = syncer::run_sync(so, console_report);
  if (r.not_logged_in) std::fprintf(stderr, "run `gsync%s login` first\n", profile_flag(o).c_str());
  if (r.login_required) {
    std::printf("LOGIN REQUIRED: Garmin refused the saved login; run `gsync%s login`, then\n"
                "`gsync sync --from <last good day>` to fill the gap (exit %d)\n",
                profile_flag(o).c_str(), syncer::kExitLoginRequired);
  }
  return syncer::exit_code(r);
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
  G_ASSERT(o.command == "get");
  if (o.args.size() != 1 || o.args[0].empty() || o.args[0][0] != '/') {
    std::fprintf(stderr, "error: get needs an API path starting with '/'\n");
    return 2;
  }
  auto client = make_client(o);
  if (!client) return 1;
  std::string err;
  std::vector<uint8_t> bytes;
  if (!client->get_bytes(o.args[0], bytes, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  persist_tokens(o, *client);
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
    failures = syncer::import_bp_downloads(db, rows, console_report);
  } else {
    for (const std::string& a : o.args) {
      std::string text;
      store::ImportCounts c;
      std::string err;
      if (!gutil::read_text_file(a, text) || !store::import_bp_csv(db, text, c, err)) {
        std::fprintf(stderr, "  %s: %s\n", a.c_str(), err.empty() ? "cannot read" : err.c_str());
        ++failures;
        continue;
      }
      rows += c.rows;
      std::printf("  %s  %lld new readings\n", a.c_str(), static_cast<long long>(c.rows));
    }
  }
  std::printf("imported %lld blood pressure readings, %d failures\n",
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

int run_profiles(const Options&) {
  const std::filesystem::path exe = gutil::exe_dir();
  G_REQUIRE_RET(!exe.empty(), 1);
  const std::vector<std::string> names = gutil::list_profiles(exe);
  std::printf("profiles in %s\n", exe.string().c_str());
  for (size_t i = 0; i < names.size() && i < gutil::kMaxProfiles + 1; ++i) {
    const std::filesystem::path base = gutil::profile_dir(exe, names[i]);
    std::error_code ec;
    const bool data = std::filesystem::exists(base / "data" / "garmin.db", ec);
    std::printf("  %-20s login: %-3s  database: %s\n", profile_label(names[i]).c_str(),
                syncer::has_login(base) ? "yes" : "no", data ? "yes" : "no");
  }
  std::printf("add one with: gsync --profile <name> login   (or Data > Profile in gview)\n");
  return 0;
}

// One-time move off AppData: copies (never moves) the v0.1.3 layout into the
// profile beside the exe. The original stays until the user deletes it.
int run_migrate_appdata(const Options& o) {
  G_ASSERT(o.profile.empty() || gutil::valid_profile_name(o.profile));
  return syncer::migrate_appdata(profile_base(o), console_report) ? 0 : 1;
}

}  // namespace cmd
