#include "sync/account.h"

#include <windows.h>

#include <algorithm>

#include "gc/token_store.h"
#include "store/db.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace syncer {

namespace {

// Copies `from` to `to` unless `to` already exists; reports what happened.
bool copy_if_absent(const std::filesystem::path& from, const std::filesystem::path& to,
                    const Report& report) {
  std::error_code ec;
  const std::string what = from.filename().string();
  if (!std::filesystem::exists(from, ec)) {
    report(false, strf("  %-10s nothing to copy", what.c_str()));
    return true;
  }
  if (std::filesystem::exists(to, ec)) {
    report(false, strf("  %-10s already at %s, left alone", what.c_str(), to.string().c_str()));
    return true;
  }
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::copy_symlinks,
                        ec);
  if (ec) {
    report(true, strf("  %-10s copy failed: %s", what.c_str(), ec.message().c_str()));
    return false;
  }
  report(false, strf("  %-10s copied to %s", what.c_str(), to.string().c_str()));
  return true;
}

// Newest day the summary endpoint completed, or "" (no database / never synced).
std::string last_completed_day(const std::filesystem::path& data_dir) {
  std::error_code ec;
  const std::filesystem::path p = data_dir / "garmin.db";
  if (!std::filesystem::exists(p, ec)) return std::string();
  store::Db db;
  std::string err;
  if (!db.open(p, err)) return std::string();
  store::Stmt st(db, "SELECT MAX(key) FROM sync_log WHERE kind='summary' AND ok=1");
  if (!st.ok() || !st.row() || st.col_null(0)) return std::string();
  return st.col_text(0);
}

}  // namespace

LoginResult login(const std::filesystem::path& profile_base, const std::string& email,
                  std::string& password, const gc::MfaPrompt& mfa) {
  LoginResult r;
  G_REQUIRE_RET(!profile_base.empty(), r);
  gc::GarminClient client{gc::Tokens{}};
  std::string err;
  const bool ok = client.login(email, password, mfa, err);
  SecureZeroMemory(password.data(), password.size());
  password.clear();
  if (!ok) {
    r.error = err;
    return r;
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_base, ec);
  if (!gc::save_tokens(gc::token_path(profile_base), client.tokens(), err)) {
    r.error = err;
    return r;
  }
  r.ok = true;
  r.who = client.tokens().full_name;
  return r;
}

bool has_login(const std::filesystem::path& profile_base) {
  std::error_code ec;
  return std::filesystem::exists(gc::token_path(profile_base), ec);
}

bool legacy_install_present() {
  const std::filesystem::path dir = gutil::legacy_appdata_dir();
  std::error_code ec;
  return !dir.empty() && std::filesystem::exists(dir / "data" / "garmin.db", ec);
}

bool migrate_appdata(const std::filesystem::path& profile_base, const Report& report) {
  const std::filesystem::path from = gutil::legacy_appdata_dir();
  G_REQUIRE_RET(!from.empty() && !profile_base.empty(), false);
  report(false, strf("copying %s -> %s", from.string().c_str(), profile_base.string().c_str()));
  std::error_code ec;
  std::filesystem::create_directories(profile_base, ec);
  bool ok = copy_if_absent(from / "tokens.bin", gc::token_path(profile_base), report);
  ok = copy_if_absent(from / "data", profile_base / "data", report) && ok;
  ok = copy_if_absent(from / "tiles", gutil::exe_dir() / "tiles", report) && ok;  // shared
  report(!ok, ok ? "done; the AppData copy is untouched and can be deleted once you are happy"
                 : "some items failed; nothing was removed from AppData");
  return ok;
}

void catch_up_range(const std::filesystem::path& data_dir, int first_days, std::string& from,
                    std::string& to) {
  G_ASSERT(first_days >= 1 && first_days <= kMaxDays);
  to = gutil::date_string_local(gutil::now_unix());
  const std::string last = last_completed_day(data_dir);
  const std::string earliest = gutil::add_days(to, -(kMaxDays - 1));
  from = last.empty() ? gutil::add_days(to, -(first_days - 1)) : std::min(last, to);
  if (from < earliest) from = earliest;
  G_ASSERT(from <= to);
}

}  // namespace syncer
