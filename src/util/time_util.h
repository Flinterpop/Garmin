#pragma once
#include <cstdint>
#include <string>

namespace gutil {

// Seconds between the Unix epoch and the FIT epoch (1989-12-31T00:00:00Z).
constexpr int64_t kFitEpochOffset = 631065600;

int64_t fit_to_unix(uint32_t fit_seconds);
uint32_t unix_to_fit(int64_t unix_seconds);

// Current wall-clock time as Unix seconds.
int64_t now_unix();

// "YYYY-MM-DD" for a Unix time, in UTC or local time.
std::string date_string_utc(int64_t unix_seconds);
std::string date_string_local(int64_t unix_seconds);

// "YYYY-MM-DDTHH:MM:SSZ" in UTC.
std::string iso8601_utc(int64_t unix_seconds);

// Parses "YYYY-MM-DD" into Unix seconds at 00:00 UTC. Returns false on bad input.
bool parse_date(const std::string& ymd, int64_t& out_unix);

// Parses "YYYY-MM-DDTHH:MM:SS[.fff]" or "YYYY-MM-DD HH:MM:SS" (treated as UTC).
bool parse_datetime(const std::string& s, int64_t& out_unix);

// Date arithmetic on "YYYY-MM-DD" strings (UTC midnight based).
std::string add_days(const std::string& ymd, int days);

}  // namespace gutil
