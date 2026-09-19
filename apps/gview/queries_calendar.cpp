// Calendar view: months with data and the per-day cells of one month.
#include <cstdio>
#include <ctime>

#include "fit/fit_profile.h"
#include "queries.h"
#include "util/assert.h"
#include "util/time_util.h"

namespace gview {

namespace {

const char* kMonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

int days_in_month(int year, int month) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  G_ASSERT(month >= 1 && month <= 12);
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return kDays[month - 1] + ((month == 2 && leap) ? 1 : 0);
}

std::string ymd(int year, int month, int day) {
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return buf;
}

std::string minutes_label(double seconds) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "%.0f min", seconds / 60.0);
  return buf;
}

}  // namespace

std::vector<MonthEntry> list_months(store::Db& db) {
  std::vector<MonthEntry> out;
  store::Stmt st(db,
                 "SELECT DISTINCT substr(d, 1, 7) FROM"
                 " (SELECT date AS d FROM daily_summary UNION"
                 "  SELECT date(start_ts, 'unixepoch', 'localtime') FROM activity_session)"
                 " ORDER BY 1 DESC");
  G_REQUIRE_RET(st.ok(), out);
  for (size_t i = 0; i < kMaxListEntries && st.row(); ++i) {
    const std::string ym = st.col_text(0);
    if (ym.size() != 7) continue;
    MonthEntry e;
    e.year = std::atoi(ym.substr(0, 4).c_str());
    e.month = std::atoi(ym.substr(5, 2).c_str());
    if (e.month < 1 || e.month > 12) continue;
    e.label = std::string(kMonthNames[e.month - 1]) + " " + std::to_string(e.year);
    out.push_back(e);
  }
  return out;
}

plot::MonthData load_month(store::Db& db, int year, int month) {
  plot::MonthData m;
  G_REQUIRE_RET(month >= 1 && month <= 12, m);
  m.year = year;
  m.month = month;
  m.title = std::string(kMonthNames[month - 1]) + " " + std::to_string(year);
  const int n = days_in_month(year, month);
  m.days.resize(static_cast<size_t>(n));
  for (int d = 1; d <= n; ++d) m.days[static_cast<size_t>(d - 1)].day = d;

  // Weekday of the 1st, Monday = 0.
  {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    if (mktime(&tm) != static_cast<time_t>(-1)) m.first_weekday = (tm.tm_wday + 6) % 7;
  }

  const std::string first = ymd(year, month, 1);
  const std::string last = ymd(year, month, n);
  auto day_index = [&](const std::string& date) -> int {
    if (date.size() != 10) return -1;
    const int d = std::atoi(date.substr(8, 2).c_str());
    return (d >= 1 && d <= n) ? d - 1 : -1;
  };

  {
    store::Stmt st(db,
                   "SELECT date, steps, resting_hr, body_battery_high, body_battery_low,"
                   " avg_stress FROM daily_summary WHERE date BETWEEN ? AND ?");
    if (st.ok()) {
      st.bind(1, first).bind(2, last);
      for (size_t i = 0; i < 64 && st.row(); ++i) {
        const int idx = day_index(st.col_text(0));
        if (idx < 0) continue;
        plot::DayCell& c = m.days[static_cast<size_t>(idx)];
        if (!st.col_null(1)) c.steps = st.col_int(1);
        if (!st.col_null(2)) c.resting_hr = st.col_int(2);
        if (!st.col_null(3)) c.bb_high = st.col_int(3);
        if (!st.col_null(4)) c.bb_low = st.col_int(4);
        if (!st.col_null(5)) c.avg_stress = st.col_int(5);
      }
    }
  }
  {
    store::Stmt st(db,
                   "SELECT date, score,"
                   " (COALESCE(deep_s,0)+COALESCE(light_s,0)+COALESCE(rem_s,0))/3600.0"
                   " FROM sleep WHERE date BETWEEN ? AND ?");
    if (st.ok()) {
      st.bind(1, first).bind(2, last);
      for (size_t i = 0; i < 64 && st.row(); ++i) {
        const int idx = day_index(st.col_text(0));
        if (idx < 0) continue;
        plot::DayCell& c = m.days[static_cast<size_t>(idx)];
        if (!st.col_null(1)) c.sleep_score = st.col_int(1);
        if (!st.col_null(2) && st.col_double(2) > 0.0) c.sleep_hours = st.col_double(2);
      }
    }
  }
  {
    store::Stmt st(db,
                   "SELECT date(ts, 'unixepoch', 'localtime'), weight_kg FROM weight"
                   " WHERE date(ts, 'unixepoch', 'localtime') BETWEEN ? AND ? ORDER BY ts");
    if (st.ok()) {
      st.bind(1, first).bind(2, last);
      for (size_t i = 0; i < 256 && st.row(); ++i) {
        const int idx = day_index(st.col_text(0));
        if (idx >= 0) m.days[static_cast<size_t>(idx)].weight_kg = st.col_double(1);
      }
    }
  }
  {
    store::Stmt st(db,
                   "SELECT date(s.start_ts, 'unixepoch', 'localtime'), s.start_ts, s.sport,"
                   " s.timer_s, s.distance_m, a.type FROM activity_session s"
                   " LEFT JOIN activity a ON a.fit_file_id = s.fit_file_id"
                   " WHERE date(s.start_ts, 'unixepoch', 'localtime') BETWEEN ? AND ?"
                   " ORDER BY s.start_ts");
    if (st.ok()) {
      st.bind(1, first).bind(2, last);
      int64_t last_start = -1;
      for (size_t i = 0; i < 512 && st.row(); ++i) {
        const int idx = day_index(st.col_text(0));
        const int64_t start = st.col_int(1);
        if (idx < 0 || start == last_start) continue;
        last_start = start;
        std::string name = st.col_null(5) ? std::string() : st.col_text(5);
        if (name.empty()) {
          name = st.col_null(2) ? "activity"
                                : fit::sport_name(static_cast<uint8_t>(st.col_int(2)));
        }
        for (char& ch : name) {
          if (ch == '_') ch = ' ';
        }
        std::string label = name;
        if (!st.col_null(4) && st.col_double(4) > 0.0) {
          char buf[32] = {};
          std::snprintf(buf, sizeof(buf), " %.1f km", st.col_double(4) / 1000.0);
          label += buf;
        }
        if (!st.col_null(3)) label += " " + minutes_label(st.col_double(3));
        m.days[static_cast<size_t>(idx)].activities.push_back(label);
      }
    }
  }
  return m;
}

}  // namespace gview
