#include <catch2/catch_test_macros.hpp>

#include "util/time_util.h"

TEST_CASE("fit epoch conversion") {
  CHECK(gutil::fit_to_unix(0) == 631065600);
  CHECK(gutil::unix_to_fit(631065600) == 0);
  // 2024-01-01T00:00:00Z = 1704067200 unix
  CHECK(gutil::fit_to_unix(1704067200 - 631065600) == 1704067200);
}

TEST_CASE("date parsing and formatting round trip") {
  int64_t t = 0;
  REQUIRE(gutil::parse_date("2024-01-01", t));
  CHECK(t == 1704067200);
  CHECK(gutil::date_string_utc(t) == "2024-01-01");
  CHECK(gutil::iso8601_utc(t + 3661) == "2024-01-01T01:01:01Z");
  CHECK_FALSE(gutil::parse_date("2024-1-1", t));
  CHECK_FALSE(gutil::parse_date("2024-13-01", t));
  CHECK_FALSE(gutil::parse_date("hello", t));
}

TEST_CASE("datetime parsing accepts Garmin's two formats") {
  int64_t t = 0;
  REQUIRE(gutil::parse_datetime("2024-01-01T01:01:01.0", t));
  CHECK(t == 1704067200 + 3661);
  REQUIRE(gutil::parse_datetime("2024-01-01 01:01:01", t));
  CHECK(t == 1704067200 + 3661);
  CHECK_FALSE(gutil::parse_datetime("2024-01-01", t));
  CHECK_FALSE(gutil::parse_datetime("2024-01-01X01:01:01", t));
}

TEST_CASE("add_days crosses month and year boundaries") {
  CHECK(gutil::add_days("2024-02-28", 2) == "2024-03-01");  // leap year
  CHECK(gutil::add_days("2023-12-31", 1) == "2024-01-01");
  CHECK(gutil::add_days("2024-01-01", -1) == "2023-12-31");
  CHECK(gutil::add_days("bad", 1).empty());
}
