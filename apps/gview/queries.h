// Read-only queries over garmin.db that produce plot::Figure objects for the
// three views (Day, Trends, Activity) and the list entries that drive them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "plot/calendar_widget.h"
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

struct GameEntry {
  int64_t fit_file_id = 0;
  int64_t start_ts = 0;
  std::string label;
  bool season = false;  // the synthetic "all games" entry
};

struct MonthEntry {
  int year = 0;
  int month = 0;
  std::string label;  // "September 2026"
};

struct TrendRange {
  std::string label;
  int days;  // 0 = everything
};

std::vector<DayEntry> list_days(store::Db& db);
std::vector<ActivityEntry> list_activities(store::Db& db);
std::vector<TrendRange> trend_ranges();
std::vector<GameEntry> list_games(store::Db& db);  // first entry is the season overview

std::vector<MonthEntry> list_months(store::Db& db);

// Robust HR max across all hockey sessions (for zone boundaries).
double hockey_hr_max(store::Db& db);

plot::Figure load_day(store::Db& db, const std::string& date);
plot::Figure load_activity(store::Db& db, const ActivityEntry& a);
plot::Figure load_trends(store::Db& db, int days);
plot::Figure load_game(store::Db& db, const GameEntry& g, double hr_max);
plot::Figure load_season(store::Db& db, double hr_max);
plot::MonthData load_month(store::Db& db, int year, int month);

// Local midnight (Unix seconds) for a YYYY-MM-DD date; 0 on bad input.
int64_t local_midnight_of(const std::string& date);

}  // namespace gview
