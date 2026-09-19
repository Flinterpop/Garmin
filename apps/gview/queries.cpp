#include "queries.h"

#include <cmath>
#include <cstdio>
#include <ctime>

#include "fit/fit_profile.h"
#include "plot/ticks.h"
#include "util/assert.h"
#include "util/time_util.h"

namespace gview {

using plot::Figure;
using plot::Panel;
using plot::Series;
using plot::Style;
using plot::YAxisSide;
namespace colors = plot::colors;

namespace {

constexpr double kDay = 86400.0;
constexpr double kDayViewStartHour = -6.0;  // 18:00 the evening before
constexpr double kDayViewEndHour = 24.0;
constexpr double kLbPerKg = 2.20462262;  // weight is stored in kg, shown in lb

std::string fmt(const char* f, double a) {
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), f, a);
  return buf;
}

std::string with_commas(int64_t v) {
  std::string s = std::to_string(v);
  std::string out;
  int count = 0;
  for (size_t i = s.size(); i > 0; --i) {
    out.insert(out.begin(), s[i - 1]);
    if (++count % 3 == 0 && i > 1) out.insert(out.begin(), ',');
  }
  return out;
}

// Fills (x, y) from a two-column query: ts, value. Bounded by kMaxSamples.
void load_xy(store::Db& db, const char* sql, int64_t p1, int64_t p2, Series& s) {
  store::Stmt st(db, sql);
  G_REQUIRE_VOID(st.ok());
  st.bind(1, p1).bind(2, p2);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    if (st.col_null(1)) continue;
    s.x.push_back(static_cast<double>(st.col_int(0)));
    s.y.push_back(st.col_double(1));
  }
}

Series make_series(const char* name, const char* units, plot::Color c, Style style,
                   YAxisSide axis = YAxisSide::kLeft, float width = 1.5f) {
  Series s;
  s.name = name;
  s.units = units;
  s.color = c;
  s.style = style;
  s.axis = axis;
  s.width = width;
  // Daily points and baselines should not be joined across missing weeks.
  if (style == Style::kPoints || style == Style::kLine) s.gap_break = 0.0;
  return s;
}

bool has_rows(store::Db& db, const char* sql, int64_t p1, int64_t p2) {
  store::Stmt st(db, sql);
  G_REQUIRE_RET(st.ok(), false);
  st.bind(1, p1).bind(2, p2);
  return st.row() && st.col_int(0) > 0;
}

}  // namespace

int64_t local_midnight_of(const std::string& date) {
  int y = 0;
  int m = 0;
  int d = 0;
  G_REQUIRE_RET(date.size() == 10, 0);
  G_REQUIRE_RET(std::sscanf(date.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3, 0);
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  tm.tm_isdst = -1;
  const time_t t = mktime(&tm);
  return t == static_cast<time_t>(-1) ? 0 : static_cast<int64_t>(t);
}

// ------------------------------------------------------------------ lists

std::vector<DayEntry> list_days(store::Db& db) {
  std::vector<DayEntry> out;
  store::Stmt st(db,
                 "SELECT d.date, s.steps, s.resting_hr, sl.score FROM"
                 " (SELECT date FROM daily_summary UNION"
                 "  SELECT DISTINCT date(ts, 'unixepoch', 'localtime') FROM hr_sample) d"
                 " LEFT JOIN daily_summary s ON s.date = d.date"
                 " LEFT JOIN sleep sl ON sl.date = d.date"
                 " ORDER BY d.date DESC");
  G_REQUIRE_RET(st.ok(), out);
  for (size_t i = 0; i < kMaxListEntries && st.row(); ++i) {
    DayEntry e;
    e.date = st.col_text(0);
    const int64_t midnight = local_midnight_of(e.date);
    e.label = plot::format_date_local(static_cast<double>(midnight) + 43200.0);
    if (!st.col_null(1)) e.label += "   " + with_commas(st.col_int(1)) + " steps";
    if (!st.col_null(2)) e.label += "   RHR " + std::to_string(st.col_int(2));
    if (!st.col_null(3)) e.label += "   sleep " + std::to_string(st.col_int(3));
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<ActivityEntry> list_activities(store::Db& db) {
  std::vector<ActivityEntry> out;
  store::Stmt st(db,
                 "SELECT s.fit_file_id, s.start_ts, s.sport, s.distance_m, s.timer_s,"
                 " a.name, a.type FROM activity_session s"
                 " LEFT JOIN activity a ON a.fit_file_id = s.fit_file_id"
                 " ORDER BY s.start_ts DESC, (SELECT COUNT(*) FROM activity_record r"
                 "   WHERE r.fit_file_id = s.fit_file_id) DESC");
  G_REQUIRE_RET(st.ok(), out);
  int64_t last_start = -1;
  for (size_t i = 0; i < kMaxListEntries && st.row(); ++i) {
    ActivityEntry e;
    e.fit_file_id = st.col_int(0);
    e.start_ts = st.col_int(1);
    // The same session can exist twice (watch copy + Connect download).
    if (e.start_ts == last_start) continue;
    last_start = e.start_ts;
    std::string sport = st.col_null(6) ? std::string() : st.col_text(6);
    if (sport.empty()) {
      sport = st.col_null(2) ? "activity"
                             : fit::sport_name(static_cast<uint8_t>(st.col_int(2)));
    }
    for (char& ch : sport) {
      if (ch == '_') ch = ' ';
    }
    e.label = plot::format_date_local(static_cast<double>(e.start_ts)) + "  " +
              plot::format_clock_local(static_cast<double>(e.start_ts)).substr(0, 5) + "  " +
              sport;
    if (!st.col_null(3) && st.col_double(3) > 0.0) {
      e.label += "  " + fmt("%.1f km", st.col_double(3) / 1000.0);
    }
    if (!st.col_null(4)) e.label += "  " + fmt("%.0f min", st.col_double(4) / 60.0);
    e.title = e.label;
    if (!st.col_null(5) && !st.col_text(5).empty()) e.title = st.col_text(5) + "  -  " + e.label;
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<TrendRange> trend_ranges() {
  return {{"Last 30 days", 30}, {"Last 90 days", 90}, {"Last 6 months", 183},
          {"Last year", 365},   {"Last 2 years", 730}, {"Everything", 0}};
}

// -------------------------------------------------------------- baseline

void add_baseline(plot::Panel& panel, const Series& daily, int window_days, YAxisSide axis) {
  G_ASSERT(window_days > 0);
  constexpr size_t kMinSamples = 7;
  const size_t n = daily.x.size();
  if (n < kMinSamples) return;
  Series band = make_series((daily.name + " 30d band").c_str(), "", plot::rgb(0x999999, 0.18f),
                            Style::kRange, axis);
  Series mean = make_series((daily.name + " 30d mean").c_str(), daily.units.c_str(),
                            plot::rgb(0x777777), Style::kLine, axis, 1.0f);
  mean.gap_break = 4.0 * kDay;
  Series odd = make_series("Unusual (>2 sd)", daily.units.c_str(), plot::rgb(0x111111),
                           Style::kPoints, axis);
  band.in_legend = false;
  mean.in_legend = false;
  odd.gap_break = 0.5 * kDay;  // never join the flagged days
  const double window = static_cast<double>(window_days) * kDay;
  size_t lo = 0;
  double sum = 0.0;
  double sum2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum += daily.y[i];
    sum2 += daily.y[i] * daily.y[i];
    while (lo < i && daily.x[i] - daily.x[lo] > window) {
      sum -= daily.y[lo];
      sum2 -= daily.y[lo] * daily.y[lo];
      ++lo;
    }
    const size_t count = i - lo + 1;
    if (count < kMinSamples) continue;
    const double m = sum / static_cast<double>(count);
    const double var = std::max(0.0, sum2 / static_cast<double>(count) - m * m);
    const double sd = std::sqrt(var);
    band.x.push_back(daily.x[i]);
    band.y.push_back(m - sd);
    band.y2.push_back(m + sd);
    mean.x.push_back(daily.x[i]);
    mean.y.push_back(m);
    if (std::fabs(daily.y[i] - m) > 2.0 * sd && sd > 0.0) {
      odd.x.push_back(daily.x[i]);
      odd.y.push_back(daily.y[i]);
    }
  }
  if (band.x.size() < 2) return;
  panel.series.insert(panel.series.begin(), std::move(band));
  panel.series.push_back(std::move(mean));
  if (!odd.x.empty()) panel.series.push_back(std::move(odd));
}

// -------------------------------------------------------------------- day

namespace {

// Picks the FIT-sourced samples when the window has any, else the API ones.
const char* source_for(store::Db& db, const char* table, int64_t t0, int64_t t1) {
  const std::string sql = std::string("SELECT COUNT(*) FROM ") + table +
                          " WHERE source='fit' AND ts BETWEEN ? AND ?";
  return has_rows(db, sql.c_str(), t0, t1) ? "fit" : "api";
}

Panel day_heart_rate_panel(store::Db& db, int64_t t0, int64_t t1) {
  Panel p;
  p.title = "Heart rate";
  const std::string sql = std::string("SELECT ts, bpm FROM hr_sample WHERE source='") +
                          source_for(db, "hr_sample", t0, t1) + "' AND ts BETWEEN ? AND ? ORDER BY ts";
  Series hr = make_series("HR", "bpm", colors::kHeartRate, Style::kLine);
  load_xy(db, sql.c_str(), t0, t1, hr);
  p.series.push_back(std::move(hr));
  p.weight = 1.3f;
  return p;
}

Panel day_stress_panel(store::Db& db, int64_t t0, int64_t t1) {
  Panel p;
  p.title = "Stress / Body Battery";
  p.left.fixed = true;
  p.left.min = 0.0;
  p.left.max = 100.0;
  p.right = p.left;
  const std::string sql = std::string("SELECT ts, level FROM stress_sample WHERE source='") +
                          source_for(db, "stress_sample", t0, t1) +
                          "' AND ts BETWEEN ? AND ? ORDER BY ts";
  Series stress = make_series("Stress", "", colors::kStress, Style::kStep);
  load_xy(db, sql.c_str(), t0, t1, stress);
  Series bb = make_series("Body Battery", "", colors::kBodyBattery, Style::kLine,
                          YAxisSide::kRight, 2.0f);
  load_xy(db, "SELECT ts, level FROM body_battery_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0,
          t1, bb);
  p.series.push_back(std::move(stress));
  p.series.push_back(std::move(bb));
  return p;
}

// Sleep stages as bands (API intervals) into `panel`.
void add_sleep_stage_bands(store::Db& db, int64_t t0, int64_t t1, Panel& panel) {
  struct StageDef {
    const char* stage;
    const char* name;
    plot::Color color;
  };
  const StageDef stages[] = {{"deep", "Deep", colors::kDeep},
                             {"light", "Light", colors::kLight},
                             {"rem", "REM", colors::kRem},
                             {"awake", "Awake", colors::kAwake}};
  for (const StageDef& sd : stages) {
    Series band = make_series(sd.name, "", sd.color, Style::kBand);
    store::Stmt st(db,
                   "SELECT start_ts, end_ts FROM sleep_stage WHERE stage=? AND source='api'"
                   " AND end_ts >= ? AND start_ts <= ? ORDER BY start_ts");
    G_REQUIRE_VOID(st.ok());
    st.bind(1, std::string(sd.stage)).bind(2, t0).bind(3, t1);
    for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
      band.x.push_back(static_cast<double>(st.col_int(0)));
      band.x2.push_back(static_cast<double>(st.col_int(1)));
      band.y.push_back(0.0);
    }
    if (!band.x.empty()) panel.series.push_back(std::move(band));
  }
}

Panel day_sleep_panel(store::Db& db, int64_t t0, int64_t t1) {
  Panel p;
  p.title = "Sleep";
  add_sleep_stage_bands(db, t0, t1, p);
  Series resp = make_series("Respiration", "brpm", colors::kRespiration, Style::kLine);
  load_xy(db, "SELECT ts, brpm FROM respiration_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0,
          t1, resp);
  Series hrv = make_series("HRV", "ms", colors::kHrv, Style::kPoints, YAxisSide::kRight);
  load_xy(db, "SELECT ts, rmssd_ms FROM hrv_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0, t1,
          hrv);
  if (!resp.x.empty()) p.series.push_back(std::move(resp));
  if (!hrv.x.empty()) p.series.push_back(std::move(hrv));
  p.weight = 0.9f;
  return p;
}

}  // namespace

Figure load_day(store::Db& db, const std::string& date) {
  G_ASSERT(db.is_open());
  Figure fig;
  fig.xmode = plot::XMode::kTime;
  fig.title = "Day  " + date;
  const int64_t midnight = local_midnight_of(date);
  G_REQUIRE_RET(midnight != 0, fig);
  const int64_t t0 = midnight + static_cast<int64_t>(kDayViewStartHour * 3600.0);
  const int64_t t1 = midnight + static_cast<int64_t>(kDayViewEndHour * 3600.0);
  fig.panels.push_back(day_heart_rate_panel(db, t0, t1));
  fig.panels.push_back(day_stress_panel(db, t0, t1));
  fig.panels.push_back(day_sleep_panel(db, t0, t1));
  fig.markers.push_back(plot::Marker{static_cast<double>(midnight),
                                     plot::format_date_local(static_cast<double>(midnight) + 1)});
  fig.extend_x = true;
  fig.extend_x_min = static_cast<double>(t0);
  fig.extend_x_max = static_cast<double>(t1);
  G_ASSERT(fig.panels.size() == 3);
  return fig;
}

// --------------------------------------------------------------- activity

namespace {

struct ActivityTraces {
  Series hr = make_series("HR", "bpm", colors::kHeartRate, Style::kLine);
  Series speed = make_series("Speed", "km/h", colors::kSpeed, Style::kLine);
  Series alt = make_series("Altitude", "m", colors::kAltitude, Style::kLine, YAxisSide::kRight);
  Series cad = make_series("Cadence", "rpm", colors::kCadence, Style::kLine);
  Series pwr = make_series("Power", "W", colors::kPower, Style::kLine, YAxisSide::kRight);
  Series temp = make_series("Temperature", "C", colors::kAltitude, Style::kLine, YAxisSide::kRight);
};

// Appends (x, value) to `s` when the column is not NULL.
void take(store::Stmt& st, int col, double x, Series& s, double scale = 1.0) {
  if (st.col_null(col)) return;
  s.x.push_back(x);
  s.y.push_back(st.col_double(col) * scale);
}

ActivityTraces load_activity_traces(store::Db& db, const ActivityEntry& a) {
  ActivityTraces t;
  store::Stmt st(db,
                 "SELECT ts, hr, speed_mps, alt_m, cadence, power_w, temp_c FROM activity_record"
                 " WHERE fit_file_id = ? ORDER BY ts");
  G_REQUIRE_RET(st.ok(), t);
  st.bind(1, a.fit_file_id);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    const double x = static_cast<double>(st.col_int(0) - a.start_ts);
    take(st, 1, x, t.hr);
    take(st, 2, x, t.speed, 3.6);
    take(st, 3, x, t.alt);
    take(st, 4, x, t.cad);
    if (!st.col_null(5) && st.col_double(5) > 0.0) take(st, 5, x, t.pwr);
    take(st, 6, x, t.temp);
  }
  return t;
}

// Cadence with power, or cadence with temperature, or whichever exists.
Panel activity_third_panel(ActivityTraces& t) {
  Panel p;
  std::vector<std::string> names;
  if (!t.cad.x.empty()) {
    names.push_back("Cadence");
    p.left.include_zero = true;
    p.series.push_back(std::move(t.cad));
  }
  if (!t.pwr.x.empty()) {
    names.push_back("Power");
    p.series.push_back(std::move(t.pwr));
  } else if (!t.temp.x.empty()) {
    names.push_back("Temperature");
    if (p.series.empty()) t.temp.axis = YAxisSide::kLeft;  // alone, it reads better on the left
    p.series.push_back(std::move(t.temp));
  }
  for (size_t i = 0; i < names.size(); ++i) p.title += (i > 0 ? " / " : "") + names[i];
  return p;
}

void add_lap_markers(store::Db& db, const ActivityEntry& a, Figure& fig) {
  store::Stmt laps(db, "SELECT start_ts FROM activity_lap WHERE fit_file_id = ? ORDER BY start_ts");
  G_REQUIRE_VOID(laps.ok());
  laps.bind(1, a.fit_file_id);
  int n = 0;
  for (size_t i = 0; i < 1000 && laps.row(); ++i) {
    ++n;
    const double x = static_cast<double>(laps.col_int(0) - a.start_ts);
    if (x > 1.0) fig.markers.push_back(plot::Marker{x, "L" + std::to_string(n)});
  }
}

}  // namespace

Figure load_activity(store::Db& db, const ActivityEntry& a) {
  G_ASSERT(db.is_open());
  G_ASSERT(a.fit_file_id > 0);
  Figure fig;
  fig.xmode = plot::XMode::kElapsed;
  fig.title = a.title;
  ActivityTraces t = load_activity_traces(db, a);
  {
    Panel p;
    p.title = "Heart rate";
    p.series.push_back(std::move(t.hr));
    p.weight = 1.3f;
    fig.panels.push_back(std::move(p));
  }
  if (!t.speed.x.empty() || !t.alt.x.empty()) {
    Panel p;
    p.title = "Speed / Altitude";
    p.left.include_zero = true;
    if (!t.speed.x.empty()) p.series.push_back(std::move(t.speed));
    if (!t.alt.x.empty()) p.series.push_back(std::move(t.alt));
    fig.panels.push_back(std::move(p));
  }
  if (!t.cad.x.empty() || !t.pwr.x.empty() || !t.temp.x.empty()) {
    fig.panels.push_back(activity_third_panel(t));
  }
  add_lap_markers(db, a, fig);
  return fig;
}

// ----------------------------------------------------------------- trends

namespace {

struct TrendWindow {
  int64_t t0 = 0;
  int64_t t1 = 0;
  std::string from;  // YYYY-MM-DD
  std::string to;
};

// Daily values are placed at local noon; bars start at local midnight.
void load_daily(store::Db& db, const TrendWindow& w, const char* sql, Series& s, double x_offset) {
  store::Stmt st(db, sql);
  G_REQUIRE_VOID(st.ok());
  st.bind(1, w.from).bind(2, w.to);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    if (st.col_null(1)) continue;
    const int64_t m = local_midnight_of(st.col_text(0));
    if (m == 0) continue;
    s.x.push_back(static_cast<double>(m) + x_offset);
    s.y.push_back(st.col_double(1));
  }
}

Panel trends_rhr_panel(store::Db& db, const TrendWindow& w) {
  Panel p;
  p.title = "Resting HR / HRV";
  Series rhr = make_series("Resting HR", "bpm", colors::kHeartRate, Style::kPoints);
  rhr.gap_break = 4.0 * kDay;
  load_daily(db, w, "SELECT date, resting_hr FROM daily_summary WHERE date BETWEEN ? AND ? ORDER BY date",
             rhr, 43200.0);
  Series hrv = make_series("HRV (night avg)", "ms", colors::kHrv, Style::kPoints, YAxisSide::kRight);
  hrv.gap_break = 4.0 * kDay;
  load_daily(db, w, "SELECT date, last_night_avg FROM hrv_daily WHERE date BETWEEN ? AND ? ORDER BY date",
             hrv, 43200.0);
  const Series rhr_copy = rhr;
  p.series.push_back(std::move(rhr));
  add_baseline(p, rhr_copy, 30, YAxisSide::kLeft);
  if (!hrv.x.empty()) {
    const Series hrv_copy = hrv;
    p.series.push_back(std::move(hrv));
    add_baseline(p, hrv_copy, 30, YAxisSide::kRight);
  }
  return p;
}

Panel trends_sleep_panel(store::Db& db, const TrendWindow& w) {
  Panel p;
  p.title = "Sleep";
  p.left.include_zero = true;
  p.right.fixed = true;
  p.right.min = 0.0;
  p.right.max = 100.0;
  Series hours = make_series("Sleep", "h", colors::kSleep, Style::kBars);
  hours.bar_width = kDay;
  load_daily(db, w,
             "SELECT date, (COALESCE(deep_s,0)+COALESCE(light_s,0)+COALESCE(rem_s,0))/3600.0"
             " FROM sleep WHERE date BETWEEN ? AND ? ORDER BY date",
             hours, 0.0);
  Series score = make_series("Score", "", colors::kStress, Style::kPoints, YAxisSide::kRight);
  load_daily(db, w, "SELECT date, score FROM sleep WHERE date BETWEEN ? AND ? ORDER BY date", score,
             43200.0);
  // The baseline is computed on bar starts shifted to noon so it sits over the bars.
  Series hours_pts = hours;
  for (double& x : hours_pts.x) x += 43200.0;
  p.series.push_back(std::move(hours));
  if (!score.x.empty()) p.series.push_back(std::move(score));
  add_baseline(p, hours_pts, 30, YAxisSide::kLeft);
  return p;
}

Panel trends_weight_panel(store::Db& db, const TrendWindow& w) {
  Panel p;
  p.title = "Weight";
  Series lb = make_series("Weight", "lb", colors::kWeight, Style::kPoints);
  load_xy(db, "SELECT ts, weight_kg FROM weight WHERE ts BETWEEN ? AND ? ORDER BY ts", w.t0, w.t1, lb);
  for (double& v : lb.y) v *= kLbPerKg;
  Series fat = make_series("Body fat", "%", colors::kPower, Style::kPoints, YAxisSide::kRight);
  load_xy(db,
          "SELECT ts, body_fat_pct FROM weight WHERE body_fat_pct IS NOT NULL AND ts BETWEEN ? AND ?"
          " ORDER BY ts",
          w.t0, w.t1, fat);
  p.series.push_back(std::move(lb));
  if (!fat.x.empty()) p.series.push_back(std::move(fat));
  return p;
}

// Returns false when there are no readings in the window.
bool trends_bp_panel(store::Db& db, const TrendWindow& w, Panel& p) {
  p.title = "Blood pressure";
  Series sys1 = make_series("Systolic", "mmHg", colors::kHeartRate, Style::kPoints);
  Series dia1 = make_series("Diastolic", "mmHg", colors::kBodyBattery, Style::kPoints);
  Series pulse = make_series("Pulse", "bpm", colors::kAltitude, Style::kPoints, YAxisSide::kRight);
  Series sys2 = make_series("Systolic (cuff user 2)", "mmHg", plot::rgb(0xE377C2), Style::kPoints);
  Series dia2 = make_series("Diastolic (cuff user 2)", "mmHg", plot::rgb(0x17BECF), Style::kPoints);
  store::Stmt st(db,
                 "SELECT ts, cuff_user, systolic, diastolic, pulse FROM blood_pressure"
                 " WHERE ts BETWEEN ? AND ? ORDER BY ts");
  G_REQUIRE_RET(st.ok(), false);
  st.bind(1, w.t0).bind(2, w.t1);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    const double x = static_cast<double>(st.col_int(0));
    const bool second = st.col_int(1) == 2;
    (second ? sys2 : sys1).x.push_back(x);
    (second ? sys2 : sys1).y.push_back(st.col_double(2));
    (second ? dia2 : dia1).x.push_back(x);
    (second ? dia2 : dia1).y.push_back(st.col_double(3));
    take(st, 4, x, pulse);
  }
  if (sys1.x.empty() && sys2.x.empty()) return false;
  for (Series* s : {&sys1, &dia1, &sys2, &dia2, &pulse}) s->gap_break = 4.0 * kDay;
  p.hlines.push_back(plot::HLine{140.0, "140", plot::rgb(0xD62728, 0.6f), YAxisSide::kLeft});
  p.hlines.push_back(plot::HLine{120.0, "120", plot::rgb(0x999999, 0.8f), YAxisSide::kLeft});
  p.hlines.push_back(plot::HLine{90.0, "90", plot::rgb(0xD62728, 0.6f), YAxisSide::kLeft});
  p.hlines.push_back(plot::HLine{80.0, "80", plot::rgb(0x999999, 0.8f), YAxisSide::kLeft});
  for (Series* s : {&sys1, &dia1, &sys2, &dia2, &pulse}) {
    if (!s->x.empty()) p.series.push_back(std::move(*s));
  }
  return true;
}

Panel trends_steps_panel(store::Db& db, const TrendWindow& w) {
  Panel p;
  p.title = "Steps";
  p.left.include_zero = true;
  Series steps = make_series("Steps", "", colors::kSteps, Style::kBars);
  steps.bar_width = kDay;
  load_daily(db, w, "SELECT date, steps FROM daily_summary WHERE date BETWEEN ? AND ? ORDER BY date",
             steps, 0.0);
  p.series.push_back(std::move(steps));
  p.weight = 0.8f;
  return p;
}

}  // namespace

Figure load_trends(store::Db& db, int days) {
  G_ASSERT(db.is_open());
  G_ASSERT(days >= 0);
  Figure fig;
  fig.xmode = plot::XMode::kTime;
  fig.title = days > 0 ? "Trends  last " + std::to_string(days) + " days" : "Trends  everything";
  TrendWindow w;
  w.t1 = local_midnight_of(gutil::date_string_local(gutil::now_unix())) + 86400;
  w.t0 = days > 0 ? w.t1 - static_cast<int64_t>(days) * 86400 : 0;
  w.from = gutil::date_string_local(w.t0);
  w.to = gutil::date_string_local(w.t1);
  if (days > 0) {
    fig.extend_x = true;
    fig.extend_x_min = static_cast<double>(w.t0);
    fig.extend_x_max = static_cast<double>(w.t1);
  }
  fig.panels.push_back(trends_rhr_panel(db, w));
  fig.panels.push_back(trends_sleep_panel(db, w));
  fig.panels.push_back(trends_weight_panel(db, w));
  Panel bp;
  if (trends_bp_panel(db, w, bp)) fig.panels.push_back(std::move(bp));
  fig.panels.push_back(trends_steps_panel(db, w));
  return fig;
}

}  // namespace gview
