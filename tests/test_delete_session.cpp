// Deleting a session in gview: rows go, the file stays known, and no later
// import (same path, forced, or another copy of the file) restores it.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fit/fit_crc.h"
#include "fit/fit_types.h"
#include "store/db.h"
#include "store/importer.h"
#include "util/time_util.h"

namespace {

void push32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A minimal activity FIT: file_id, three records and one session starting at `start_unix`.
std::vector<uint8_t> activity_fit(int64_t start_unix) {
  const uint32_t t0 = static_cast<uint32_t>(start_unix - gutil::kFitEpochOffset);
  std::vector<uint8_t> body;
  auto def = [&body](uint8_t local, uint16_t global, const std::vector<std::array<uint8_t, 3>>& fields) {
    body.insert(body.end(), {static_cast<uint8_t>(0x40 | local), 0, 0, static_cast<uint8_t>(global),
                             static_cast<uint8_t>(global >> 8), static_cast<uint8_t>(fields.size())});
    for (const auto& f : fields) body.insert(body.end(), f.begin(), f.end());
  };
  def(0, fit::kMesgFileId, {{{0, 1, fit::kEnum}}});
  body.insert(body.end(), {0, fit::kFileActivity});
  def(1, fit::kMesgRecord, {{{253, 4, fit::kUint32}}});
  for (uint32_t i = 0; i < 3; ++i) {
    body.push_back(1);
    push32(body, t0 + i);
  }
  def(2, fit::kMesgSession, {{{253, 4, fit::kUint32}, {2, 4, fit::kUint32}, {5, 1, fit::kEnum}}});
  body.push_back(2);
  push32(body, t0 + 3);
  push32(body, t0);
  body.push_back(73);  // sport: hockey as the watch records it
  std::vector<uint8_t> out = {14, 0x20, 0x54, 0x08};
  push32(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), {'.', 'F', 'I', 'T'});
  const uint16_t hcrc = fit::crc_compute(out.data(), 12);
  out.push_back(static_cast<uint8_t>(hcrc));
  out.push_back(static_cast<uint8_t>(hcrc >> 8));
  out.insert(out.end(), body.begin(), body.end());
  const uint16_t crc = fit::crc_compute(out.data(), out.size());
  out.push_back(static_cast<uint8_t>(crc));
  out.push_back(static_cast<uint8_t>(crc >> 8));
  return out;
}

int64_t count(store::Db& db, const std::string& sql) {
  store::Stmt s(db, sql.c_str());
  REQUIRE(s.ok());
  REQUIRE(s.row());
  return s.col_int(0);
}

void import(store::Db& db, const std::string& path, int64_t start, bool force, store::ImportCounts& c) {
  std::string err;
  REQUIRE(store::import_fit(db, path, activity_fit(start), 0, force, c, err));
}

}  // namespace

TEST_CASE("delete_session: every copy goes, the files stay known, and imports do not restore it", "[store]") {
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  constexpr int64_t kTest = 1794018904;    // the session to delete
  constexpr int64_t kGame = 1793618024;    // a real game that must survive
  store::ImportCounts c;
  import(db, "data/fit/activities/1_ACTIVITY.fit", kTest, false, c);
  import(db, "data/fit/watch/fenix7/Activity/test.fit", kTest, false, c);  // the watch's copy
  import(db, "data/fit/activities/2_ACTIVITY.fit", kGame, false, c);
  REQUIRE(db.exec("INSERT INTO activity(id, fit_file_id, json) VALUES(1, 1, '{}')", err));
  REQUIRE(count(db, "SELECT COUNT(*) FROM activity_session") == 3);
  REQUIRE(count(db, "SELECT COUNT(*) FROM activity_record") == 9);

  std::vector<std::string> paths;
  int64_t sessions = 0;
  REQUIRE(store::delete_session(db, kTest, "6 Nov 2026  hockey", paths, sessions, err));
  CHECK(sessions == 2);
  CHECK(paths.size() == 2);
  const std::string left = "SELECT COUNT(*) FROM activity_session WHERE start_ts = " + std::to_string(kTest);
  CHECK(count(db, left) == 0);
  CHECK(count(db, "SELECT COUNT(*) FROM activity_record") == 3);  // only the game's
  CHECK(count(db, "SELECT COUNT(*) FROM activity") == 0);
  CHECK(count(db, "SELECT COUNT(*) FROM deleted_session") == 1);
  CHECK(store::fit_file_known(db, "data/fit/activities/1_ACTIVITY.fit"));  // so the sync does not fetch it again

  SECTION("the same path is skipped") {
    store::ImportCounts again;
    import(db, "data/fit/activities/1_ACTIVITY.fit", kTest, false, again);
    CHECK(again.skipped);
    CHECK(count(db, left) == 0);
  }
  SECTION("a forced re-import drops it again") {
    store::ImportCounts again;
    import(db, "data/fit/activities/1_ACTIVITY.fit", kTest, true, again);
    CHECK(again.skipped);
    CHECK(again.rows == 0);
    CHECK(count(db, left) == 0);
  }
  SECTION("another copy of the file under a new path is dropped") {
    store::ImportCounts again;
    import(db, "C:/Users/someone/Downloads/test.fit", kTest, false, again);
    CHECK(again.skipped);
    CHECK(count(db, left) == 0);
    CHECK(store::fit_file_known(db, "C:/Users/someone/Downloads/test.fit"));
  }
  SECTION("the other session is untouched and still imports normally") {
    CHECK(count(db, "SELECT COUNT(*) FROM activity_session WHERE start_ts = " + std::to_string(kGame)) == 1);
    store::ImportCounts fresh;
    import(db, "data/fit/activities/3_ACTIVITY.fit", kGame + 86400, false, fresh);
    CHECK_FALSE(fresh.skipped);
    CHECK(fresh.rows > 0);
  }
}

TEST_CASE("delete_session: a start time with no session is still remembered", "[store]") {
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  std::vector<std::string> paths;
  int64_t sessions = -1;
  REQUIRE(store::delete_session(db, 1794018904, "", paths, sessions, err));
  CHECK(sessions == 0);
  CHECK(paths.empty());
  CHECK(count(db, "SELECT COUNT(*) FROM deleted_session") == 1);
  CHECK_FALSE(store::fit_file_known(db, "nowhere.fit"));
}
