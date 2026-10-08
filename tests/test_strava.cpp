#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "store/db.h"
#include "strava/api.h"
#include "strava/auth.h"
#include "strava/loopback.h"
#include "strava/push.h"
#include "strava/rules.h"

namespace {

void exec(store::Db& db, const std::string& sql) {
  std::string err;
  REQUIRE(db.exec(sql.c_str(), err));
}

// One session (and its file) as the importer would leave it; `records` rows of activity_record.
void add_session(store::Db& db, int file_id, int64_t start, int sport, double timer_s, int records) {
  exec(db, "INSERT INTO fit_file(id, path, imported_at, messages) VALUES(" + std::to_string(file_id) +
               ", 'C:\\fit\\" + std::to_string(file_id) + ".fit', 0, 0)");
  exec(db, "INSERT INTO activity_session(fit_file_id, start_ts, sport, timer_s) VALUES(" + std::to_string(file_id) +
               ", " + std::to_string(start) + ", " + std::to_string(sport) + ", " + std::to_string(timer_s) + ")");
  for (int i = 0; i < records; ++i) {
    exec(db, "INSERT INTO activity_record(fit_file_id, ts) VALUES(" + std::to_string(file_id) + ", " +
                 std::to_string(start + i) + ")");
  }
}

strava::Settings hockey_only() {
  strava::Settings s;
  s.since = "2024-01-01";
  strava::SportRule* r = strava::rule_for(s, 73);
  REQUIRE(r != nullptr);
  r->enabled = true;
  r->name = "Old guy hockey";
  strava::SportRule* run = strava::rule_for(s, 1);
  REQUIRE(run != nullptr);  // present but off
  return s;
}

}  // namespace

TEST_CASE("strava settings: format and parse round-trip; bad values fall back", "[strava]") {
  strava::Settings s = hockey_only();
  const strava::Settings back = strava::parse_settings(strava::format_settings(s), "2000-01-01");
  CHECK(back.since == "2024-01-01");
  REQUIRE(back.rules.size() == 2);
  const strava::SportRule* h = strava::find_rule(back, 73);
  REQUIRE(h != nullptr);
  CHECK(h->enabled);
  CHECK(h->sport_type == "IceSkate");  // Strava has no hockey type
  CHECK(h->name == "Old guy hockey");
  CHECK(h->min_minutes == strava::kDefaultMinMinutes);
  CHECK_FALSE(strava::find_rule(back, 1)->enabled);
  CHECK(strava::any_enabled(back));

  const strava::Settings bad = strava::parse_settings(
      "[strava]\nsince = yesterday\n[sport 2]\nenabled = 1\ntype = Bobsled\nmin_minutes = -4\n", "2000-01-01");
  CHECK(bad.since == "2000-01-01");
  REQUIRE(bad.rules.size() == 1);
  CHECK(bad.rules[0].sport_type == "Ride");  // the default for cycling, not the unknown type
  CHECK(bad.rules[0].min_minutes == strava::kDefaultMinMinutes);
}

TEST_CASE("strava settings: titles and types are validated", "[strava]") {
  CHECK(strava::valid_title("Old guy hockey"));
  CHECK_FALSE(strava::valid_title(""));
  CHECK_FALSE(strava::valid_title("two\nlines"));
  CHECK_FALSE(strava::valid_title(std::string(strava::kMaxTitle + 1, 'x')));
  CHECK(strava::valid_sport_type("IceSkate"));
  CHECK_FALSE(strava::valid_sport_type("IceHockey"));
  CHECK(strava::default_sport_type(999) == "Workout");
}

TEST_CASE("strava auth: client id/secret shapes, authorize URL and scopes", "[strava]") {
  CHECK(strava::valid_client_id("123456"));
  CHECK_FALSE(strava::valid_client_id("12a"));
  CHECK(strava::valid_client_secret(std::string(40, 'a')));
  CHECK_FALSE(strava::valid_client_secret("short"));
  const std::string url = strava::authorize_url("123456");
  CHECK(url.find("client_id=123456") != std::string::npos);
  CHECK(url.find("activity%3Awrite") != std::string::npos);
  CHECK(url.find("localhost%3A8765") != std::string::npos);
  CHECK(strava::scope_sufficient("read,activity:write,activity:read_all"));
  CHECK_FALSE(strava::scope_sufficient("read,activity:read_all"));

  strava::Login l;
  std::string err;
  REQUIRE(strava::parse_token_response(
      R"({"access_token":"a1","refresh_token":"r1","expires_at":1760000000,"athlete":{"firstname":"Pat","lastname":"Doe"}})",
      l, err));
  CHECK(l.access_token == "a1");
  CHECK(l.expires_at == 1760000000);
  CHECK(l.athlete == "Pat Doe");
  CHECK_FALSE(strava::parse_token_response(R"({"message":"Bad Request"})", l, err));
}

TEST_CASE("strava loopback: the redirect's query is decoded; other requests are ignored", "[strava]") {
  strava::Redirect r;
  REQUIRE(strava::parse_redirect_request(
      "GET /?state=&code=abc123&scope=read,activity:write%2Cactivity:read_all HTTP/1.1\r\nHost: x\r\n\r\n", r));
  CHECK(r.code == "abc123");
  CHECK(r.scope == "read,activity:write,activity:read_all");
  strava::Redirect denied;
  REQUIRE(strava::parse_redirect_request("GET /?state=&error=access_denied HTTP/1.1\r\n\r\n", denied));
  CHECK(denied.error == "access_denied");
  CHECK(denied.code.empty());
  strava::Redirect none;
  CHECK_FALSE(strava::parse_redirect_request("GET /favicon.ico HTTP/1.1\r\n\r\n", none));
}

TEST_CASE("strava api: activity list, matching by start time, uploads and duplicates", "[strava]") {
  std::vector<strava::Activity> acts;
  REQUIRE(strava::parse_activities(
      R"([{"id":11,"start_date":"2024-01-04T11:17:49Z","sport_type":"Workout","name":"Morning Workout"},
          {"id":12,"start_date":"2024-01-08T11:20:53Z","sport_type":"IceSkate","name":"Old guy hockey"},
          {"name":"no id"}])",
      acts));
  REQUIRE(acts.size() == 2);
  CHECK(acts[0].start_ts == 1704367069);
  CHECK(strava::find_match(acts, 1704367069 + 90) == &acts[0]);
  CHECK(strava::find_match(acts, 1704367069 + strava::kMatchToleranceS + 1) == nullptr);

  strava::UploadState u;
  REQUIRE(strava::parse_upload(R"({"id":555,"status":"Your activity is still being processed.","error":null})", u));
  CHECK(u.upload_id == 555);
  CHECK(u.activity_id == 0);
  CHECK(u.error.empty());
  REQUIRE(strava::parse_upload(
      R"({"id":556,"error":"hockey.fit duplicate of <a href='/activities/20497305009' target='_blank'>Morning</a>"})",
      u));
  CHECK(u.duplicate_of == 20497305009);
  CHECK(strava::duplicate_activity_id("Improperly formatted data.") == 0);

  const std::string body = strava::multipart_body("BND", {{"data_type", "fit"}}, "a.fit", std::string("\x0e\x10\0x", 4));
  CHECK(body.find("--BND\r\nContent-Disposition: form-data; name=\"data_type\"\r\n\r\nfit\r\n") == 0);
  CHECK(body.find(std::string("\x0e\x10\0x", 4)) != std::string::npos);  // binary kept, NUL and all
  CHECK(body.size() >= 6);
  CHECK(body.substr(body.size() - 9) == "--BND--\r\n");
}

TEST_CASE("strava push: candidates follow the rules, skip done ones and prefer the copy with records", "[strava]") {
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  const int64_t day = 86400;
  const int64_t t0 = 1704367069;  // 2024-01-04
  add_session(db, 1, t0, 73, 4500, 0);           // record-less SUMMARY twin
  add_session(db, 2, t0, 73, 4500, 3);           // the real copy
  add_session(db, 3, t0 + day, 73, 300, 3);      // 5 min: an accidental start
  add_session(db, 4, t0 + 2 * day, 1, 1800, 3);  // a run: rule present but off
  add_session(db, 5, t0 + 3 * day, 73, 4000, 3);
  add_session(db, 6, t0 - 30 * day, 73, 4000, 3);  // before `since`
  exec(db, "INSERT INTO strava_push(start_ts, state, at) VALUES(" + std::to_string(t0 + 3 * day) + ", 'done', 0)");

  std::vector<strava::Candidate> c = strava::select_candidates(db, hockey_only());
  REQUIRE(c.size() == 1);
  CHECK(c[0].start_ts == t0);
  CHECK(c[0].fit.filename() == "2.fit");

  exec(db, "INSERT INTO strava_push(start_ts, state, attempts, at) VALUES(" + std::to_string(t0) + ", 'failed', " +
               std::to_string(strava::kMaxAttempts) + ", 0)");
  CHECK(strava::select_candidates(db, hockey_only()).empty());  // given up on after kMaxAttempts
}

TEST_CASE("strava push: without a login nothing happens", "[strava]") {
  strava::PushOptions o;
  o.profile_base = std::filesystem::temp_directory_path() / "gview_test_no_strava";
  o.data_dir = o.profile_base / "data";
  int lines = 0;
  const strava::PushResult r = strava::run_push(o, [&](bool, const std::string&) { ++lines; });
  CHECK(r.not_connected);
  CHECK(lines == 0);  // silent: most people never connect Strava
  CHECK(strava::summary(r).find("0 uploaded") != std::string::npos);
}
