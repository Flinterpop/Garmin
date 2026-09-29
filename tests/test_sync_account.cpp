#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "store/db.h"
#include "store/importer.h"
#include "sync/account.h"
#include "sync/engine.h"
#include "util/time_util.h"

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
  const fs::path d = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

std::string today() { return gutil::date_string_local(gutil::now_unix()); }

}  // namespace

TEST_CASE("catch_up_range: a new profile gets the first N days", "[sync]") {
  const fs::path dir = fresh_dir("gview_test_sync_new");
  std::string from, to;
  syncer::catch_up_range(dir, 30, from, to);  // no database at all
  CHECK(to == today());
  CHECK(from == gutil::add_days(today(), -29));
  fs::remove_all(dir);
}

TEST_CASE("catch_up_range: continues from the last completed day", "[sync]") {
  const fs::path dir = fresh_dir("gview_test_sync_resume");
  {
    store::Db db;
    std::string err;
    REQUIRE(db.open(dir / "garmin.db", err));
    const std::string last = gutil::add_days(today(), -5);
    REQUIRE(store::mark_sync(db, "summary", gutil::add_days(today(), -9), true, err));
    REQUIRE(store::mark_sync(db, "summary", last, true, err));
    REQUIRE(store::mark_sync(db, "summary", gutil::add_days(today(), -2), false, err));  // failed: not completed
    REQUIRE(store::mark_sync(db, "heartrate", gutil::add_days(today(), -1), true, err));  // other kinds ignored
  }
  std::string from, to;
  syncer::catch_up_range(dir, 30, from, to);
  CHECK(to == today());
  CHECK(from == gutil::add_days(today(), -5));
  fs::remove_all(dir);
}

TEST_CASE("exit_code: 0 ok, 1 problems, 3 login required", "[sync]") {
  syncer::SyncResult r;
  CHECK(syncer::exit_code(r) == 0);
  r.failures = 2;
  CHECK(syncer::exit_code(r) == 1);
  r.login_required = true;
  CHECK(syncer::exit_code(r) == syncer::kExitLoginRequired);
  syncer::SyncResult n;
  n.not_logged_in = true;
  CHECK(syncer::exit_code(n) == 1);
  syncer::SyncResult c;
  c.cancelled = true;
  CHECK(syncer::exit_code(c) == 1);
}

TEST_CASE("run_sync without a login reports it and touches nothing", "[sync]") {
  const fs::path base = fresh_dir("gview_test_sync_nologin");
  syncer::SyncOptions o;
  o.profile_base = base;
  o.data_dir = base / "data";
  o.from = o.to = today();
  int errors = 0;
  const syncer::SyncResult r = syncer::run_sync(o, [&](bool error, const std::string&) { errors += error ? 1 : 0; });
  CHECK(r.not_logged_in);
  CHECK(errors == 1);
  CHECK_FALSE(fs::exists(base / "data"));  // no database created for a profile that cannot sync
  fs::remove_all(base);
}

TEST_CASE("strf formats and caps", "[sync]") {
  CHECK(syncer::strf("%s %d", "a", 7) == "a 7");
  CHECK(syncer::strf("%s", std::string(5000, 'x').c_str()).size() == 1023);
}
