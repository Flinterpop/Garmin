// Read-only queries over garmin.db that produce plot::Figure objects for the
// three views (Day, Trends, Activity) and the list entries that drive them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "plot/plot_types.h"
#include "store/db.h"

namespace gview {

constexpr size_t kMaxListEntries = 20000;
constexpr size_t kMaxSamples = 2000000;

struct DayEntry {
  std::string date;   // YYYY-MM-DD (local)
  std::string label;  // "17 Sep 2026   9,780 steps  RHR 49"
};

struct ActivityEntry {
  int64_t fit_file_id = 0;
  int64_t start_ts = 0;
  std::string label;  // "12 Apr 2026  running  2.2 km  45 min"
  std::string title;
};

struct TrendRange {
  std::string label;
  int days;  // 0 = everything
};

std::vector<DayEntry> list_days(store::Db& db);
std::vector<ActivityEntry> list_activities(store::Db& db);
std::vector<TrendRange> trend_ranges();

plot::Figure load_day(store::Db& db, const std::string& date);
plot::Figure load_activity(store::Db& db, const ActivityEntry& a);
plot::Figure load_trends(store::Db& db, int days);

// Local midnight (Unix seconds) for a YYYY-MM-DD date; 0 on bad input.
int64_t local_midnight_of(const std::string& date);

}  // namespace gview
