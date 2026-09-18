#include "util/time_util.h"

#include <cstdio>
#include <ctime>

#include "util/assert.h"

namespace gutil {

int64_t fit_to_unix(uint32_t fit_seconds) {
  return static_cast<int64_t>(fit_seconds) + kFitEpochOffset;
}

uint32_t unix_to_fit(int64_t unix_seconds) {
  G_ASSERT(unix_seconds >= kFitEpochOffset);
  return static_cast<uint32_t>(unix_seconds - kFitEpochOffset);
}

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

namespace {

bool to_tm_utc(int64_t unix_seconds, std::tm& out) {
  const time_t t = static_cast<time_t>(unix_seconds);
  return gmtime_s(&out, &t) == 0;
}

bool to_tm_local(int64_t unix_seconds, std::tm& out) {
  const time_t t = static_cast<time_t>(unix_seconds);
  return localtime_s(&out, &t) == 0;
}

std::string format_date(const std::tm& tm) {
  char buf[16] = {};
  const int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                              tm.tm_mon + 1, tm.tm_mday);
  G_ASSERT(n == 10);
  return std::string(buf, static_cast<size_t>(n));
}

bool valid_ymd(int y, int m, int d) {
  return y >= 1970 && y <= 2200 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

}  // namespace

std::string date_string_utc(int64_t unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_tm_utc(unix_seconds, tm), std::string());
  return format_date(tm);
}

std::string date_string_local(int64_t unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_tm_local(unix_seconds, tm), std::string());
  return format_date(tm);
}

std::string iso8601_utc(int64_t unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_tm_utc(unix_seconds, tm), std::string());
  char buf[32] = {};
  const int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                              tm.tm_min, tm.tm_sec);
  G_ASSERT(n == 20);
  return std::string(buf, static_cast<size_t>(n));
}

bool parse_date(const std::string& ymd, int64_t& out_unix) {
  int y = 0;
  int m = 0;
  int d = 0;
  G_REQUIRE_RET(ymd.size() == 10, false);
  G_REQUIRE_RET(std::sscanf(ymd.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3, false);
  G_REQUIRE_RET(valid_ymd(y, m, d), false);
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  const time_t t = _mkgmtime(&tm);
  G_REQUIRE_RET(t != static_cast<time_t>(-1), false);
  out_unix = static_cast<int64_t>(t);
  return true;
}

bool parse_datetime(const std::string& s, int64_t& out_unix) {
  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int sec = 0;
  G_REQUIRE_RET(s.size() >= 19, false);
  // Accept either 'T' or ' ' between date and time.
  G_REQUIRE_RET(s[10] == 'T' || s[10] == ' ', false);
  const int n = std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &mo, &d);
  G_REQUIRE_RET(n == 3, false);
  const int m2 = std::sscanf(s.c_str() + 11, "%2d:%2d:%2d", &h, &mi, &sec);
  G_REQUIRE_RET(m2 == 3, false);
  G_REQUIRE_RET(valid_ymd(y, mo, d), false);
  G_REQUIRE_RET(h >= 0 && h < 24 && mi >= 0 && mi < 60 && sec >= 0 && sec <= 60, false);
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  const time_t t = _mkgmtime(&tm);
  G_REQUIRE_RET(t != static_cast<time_t>(-1), false);
  out_unix = static_cast<int64_t>(t);
  return true;
}

std::string add_days(const std::string& ymd, int days) {
  int64_t base = 0;
  G_REQUIRE_RET(parse_date(ymd, base), std::string());
  return date_string_utc(base + static_cast<int64_t>(days) * 86400);
}

}  // namespace gutil
