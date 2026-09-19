// Ski views: day list, per-day runs figure, season overview.
#include <algorithm>
#include <cstdio>

#include "analysis/ski.h"
#include "plot/ticks.h"
#include "queries.h"
#include "util/assert.h"

namespace gview {

using plot::Figure;
using plot::Panel;
using plot::Series;
using plot::Style;
using plot::YAxisSide;
namespace colors = plot::colors;

namespace {

constexpr size_t kMaxDays = 2000;
constexpr double kKmh = 3.6;
constexpr const char* kSkiSql =
    "SELECT s.fit_file_id, s.start_ts, s.timer_s, s.distance_m, a.name FROM activity_session s"
    " LEFT JOIN activity a ON a.fit_file_id = s.fit_file_id"
    " WHERE s.sport = 13 OR a.type IN ('resort_skiing', 'alpine_skiing', 'skiing',"
    " 'resort_skiing_snowboarding_ws')"
    " ORDER BY s.start_ts DESC, (SELECT COUNT(*) FROM activity_record r"
    "   WHERE r.fit_file_id = s.fit_file_id) DESC";

Series make(const char* name, const char* units, plot::Color c, Style style,
            YAxisSide axis = YAxisSide::kLeft) {
  Series s;
  s.name = name;
  s.units = units;
  s.color = c;
  s.style = style;
  s.axis = axis;
  return s;
}

ski::Trace load_trace(store::Db& db, int64_t fit_file_id, int64_t start_ts) {
  ski::Trace tr;
  store::Stmt st(db,
                 "SELECT ts, alt_m, speed_mps, hr, dist_m FROM activity_record"
                 " WHERE fit_file_id = ? AND alt_m IS NOT NULL ORDER BY ts");
  G_REQUIRE_RET(st.ok(), tr);
  st.bind(1, fit_file_id);
  for (size_t i = 0; i < ski::kMaxSamples && st.row(); ++i) {
    tr.t.push_back(static_cast<double>(st.col_int(0) - start_ts));
    tr.alt.push_back(st.col_double(1));
    tr.speed.push_back(st.col_null(2) ? 0.0 : st.col_double(2));
    tr.hr.push_back(st.col_null(3) ? 0.0 : st.col_double(3));
    tr.dist.push_back(st.col_null(4) ? 0.0 : st.col_double(4));
  }
  return tr;
}

std::string day_line(const ski::DayStats& d) {
  char buf[200] = {};
  std::snprintf(buf, sizeof(buf),
                "%zu runs   %.0f m vertical   %.1f km   top %.0f km/h   ski %s / lift %s",
                d.runs, d.vertical_m, d.distance_m / 1000.0, d.max_speed_mps * kKmh,
                plot::format_elapsed(d.ski_time_s).c_str(),
                plot::format_elapsed(d.lift_time_s).c_str());
  return buf;
}

}  // namespace

std::vector<SkiEntry> list_ski_days(store::Db& db) {
  std::vector<SkiEntry> out;
  SkiEntry season;
  season.season = true;
  season.label = "Season overview (all days)";
  out.push_back(season);
  store::Stmt st(db, kSkiSql);
  G_REQUIRE_RET(st.ok(), out);
  int64_t last = -1;
  for (size_t i = 0; i < kMaxDays && st.row(); ++i) {
    SkiEntry e;
    e.fit_file_id = st.col_int(0);
    e.start_ts = st.col_int(1);
    if (e.start_ts == last) continue;
    last = e.start_ts;
    e.label = plot::format_date_local(static_cast<double>(e.start_ts));
    if (!st.col_null(4) && !st.col_text(4).empty()) e.label += "  " + st.col_text(4);
    char buf[64] = {};
    if (!st.col_null(3) && st.col_double(3) > 0.0) {
      std::snprintf(buf, sizeof(buf), "  %.1f km", st.col_double(3) / 1000.0);
      e.label += buf;
    }
    if (!st.col_null(2)) {
      std::snprintf(buf, sizeof(buf), "  %.0f min", st.col_double(2) / 60.0);
      e.label += buf;
    }
    out.push_back(std::move(e));
  }
  return out;
}

Figure load_ski_day(store::Db& db, const SkiEntry& e) {
  Figure fig;
  fig.xmode = plot::XMode::kElapsed;
  G_ASSERT(!e.season);
  const ski::Trace tr = load_trace(db, e.fit_file_id, e.start_ts);
  const std::vector<ski::Run> runs = ski::detect_runs(tr);
  const ski::DayStats d = ski::compute_stats(tr, runs);
  fig.title = "Ski  " + e.label + "    " + day_line(d);

  {
    Panel p;
    p.title = "Altitude  -  " + day_line(d);
    Series band = make("Run", "", plot::rgb(0x1F77B4, 0.16f), Style::kBand);
    for (const ski::Run& r : runs) {
      band.x.push_back(r.start_s);
      band.x2.push_back(r.end_s);
      band.y.push_back(0.0);
    }
    if (!band.x.empty()) p.series.push_back(std::move(band));
    Series alt = make("Altitude", "m", colors::kAltitude, Style::kLine);
    alt.x = tr.t;
    alt.y = tr.alt;
    Series spd = make("Speed", "km/h", colors::kSpeed, Style::kLine, YAxisSide::kRight);
    for (size_t i = 0; i < tr.t.size(); ++i) {
      spd.x.push_back(tr.t[i]);
      spd.y.push_back(tr.speed[i] * kKmh);
    }
    p.right.include_zero = true;
    p.series.push_back(std::move(alt));
    p.series.push_back(std::move(spd));
    p.weight = 1.5f;
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Heart rate";
    Series hr = make("HR", "bpm", colors::kHeartRate, Style::kLine);
    for (size_t i = 0; i < tr.t.size(); ++i) {
      if (tr.hr[i] <= 0.0) continue;
      hr.x.push_back(tr.t[i]);
      hr.y.push_back(tr.hr[i]);
    }
    if (!hr.x.empty()) {
      p.series.push_back(std::move(hr));
      fig.panels.push_back(std::move(p));
    }
  }
  {
    Panel p;
    p.title = "Per run";
    p.left.include_zero = true;
    p.right.include_zero = true;
    Series vert = make("Vertical", "m", colors::kBodyBattery, Style::kBars);
    Series top = make("Top speed", "km/h", colors::kSpeed, Style::kPoints, YAxisSide::kRight);
    for (const ski::Run& r : runs) {
      vert.x.push_back(r.start_s);
      vert.x2.push_back(r.end_s);
      vert.y.push_back(r.vertical_m);
      top.x.push_back((r.start_s + r.end_s) / 2.0);
      top.y.push_back(r.max_speed_mps * kKmh);
    }
    p.series.push_back(std::move(vert));
    if (!top.x.empty()) p.series.push_back(std::move(top));
    fig.panels.push_back(std::move(p));
  }
  return fig;
}

Figure load_ski_season(store::Db& db) {
  Figure fig;
  fig.xmode = plot::XMode::kTime;
  fig.title = "Ski season overview";
  Series vert = make("Vertical", "m", colors::kBodyBattery, Style::kBars);
  Series runs_s = make("Runs", "", colors::kStress, Style::kPoints, YAxisSide::kRight);
  Series top = make("Top speed", "km/h", colors::kSpeed, Style::kPoints);
  Series avg_run = make("Avg run", "m", colors::kCadence, Style::kPoints, YAxisSide::kRight);
  Series dist = make("Distance", "km", colors::kAltitude, Style::kBars);
  Series hr = make("Avg HR skiing", "bpm", colors::kHeartRate, Style::kPoints, YAxisSide::kRight);

  constexpr double kBarHalf = 0.45 * 86400.0;
  const std::vector<SkiEntry> days = list_ski_days(db);
  double season_vertical = 0.0;
  size_t season_runs = 0;
  for (const SkiEntry& e : days) {
    if (e.season) continue;
    const ski::Trace tr = load_trace(db, e.fit_file_id, e.start_ts);
    if (tr.t.size() < 60) continue;
    const std::vector<ski::Run> runs = ski::detect_runs(tr);
    const ski::DayStats d = ski::compute_stats(tr, runs);
    if (d.runs == 0) continue;
    const double t = static_cast<double>(e.start_ts);
    vert.x.push_back(t - kBarHalf);
    vert.x2.push_back(t + kBarHalf);
    vert.y.push_back(d.vertical_m);
    runs_s.x.push_back(t);
    runs_s.y.push_back(static_cast<double>(d.runs));
    top.x.push_back(t);
    top.y.push_back(d.max_speed_mps * kKmh);
    avg_run.x.push_back(t);
    avg_run.y.push_back(d.vertical_m / static_cast<double>(d.runs));
    dist.x.push_back(t - kBarHalf);
    dist.x2.push_back(t + kBarHalf);
    dist.y.push_back(d.distance_m / 1000.0);
    if (d.avg_hr_skiing > 0.0) {
      hr.x.push_back(t);
      hr.y.push_back(d.avg_hr_skiing);
    }
    season_vertical += d.vertical_m;
    season_runs += d.runs;
  }
  auto flip = [](Series& s) {
    std::reverse(s.x.begin(), s.x.end());
    std::reverse(s.y.begin(), s.y.end());
    std::reverse(s.x2.begin(), s.x2.end());
  };
  for (Series* s : {&vert, &runs_s, &top, &avg_run, &dist, &hr}) {
    flip(*s);
    s->gap_break = 21.0 * 86400.0;
  }
  char buf[96] = {};
  std::snprintf(buf, sizeof(buf), "Ski season overview   %zu runs, %.0f m vertical in total",
                season_runs, season_vertical);
  fig.title = buf;

  {
    Panel p;
    p.title = "Vertical per day";
    p.left.include_zero = true;
    p.right.include_zero = true;
    p.series.push_back(std::move(vert));
    p.series.push_back(std::move(runs_s));
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Speed and run size";
    p.left.include_zero = true;
    p.right.include_zero = true;
    p.series.push_back(std::move(top));
    p.series.push_back(std::move(avg_run));
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Distance and effort";
    p.left.include_zero = true;
    p.series.push_back(std::move(dist));
    if (!hr.x.empty()) p.series.push_back(std::move(hr));
    fig.panels.push_back(std::move(p));
  }
  return fig;
}

}  // namespace gview
