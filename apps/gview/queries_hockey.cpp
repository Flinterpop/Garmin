// Hockey views: game list, per-game shift/zone figure, season overview.
#include <algorithm>
#include <cstdio>

#include "analysis/hockey.h"
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

constexpr double kDefaultHrMax = 190.0;
constexpr size_t kMaxGames = 2000;
constexpr const char* kGameSql =
    "SELECT s.fit_file_id, s.start_ts, s.timer_s, s.avg_hr, s.max_hr FROM activity_session s"
    " LEFT JOIN activity a ON a.fit_file_id = s.fit_file_id"
    " WHERE s.sport = 73 OR a.type = 'ice_hockey'"
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

// HR trace of one session as elapsed seconds + bpm.
void load_hr(store::Db& db, int64_t fit_file_id, int64_t start_ts, std::vector<double>& x,
             std::vector<double>& y) {
  x.clear();
  y.clear();
  store::Stmt st(db,
                 "SELECT ts, hr FROM activity_record WHERE fit_file_id = ? AND hr IS NOT NULL"
                 " ORDER BY ts");
  G_REQUIRE_VOID(st.ok());
  st.bind(1, fit_file_id);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    x.push_back(static_cast<double>(st.col_int(0) - start_ts));
    y.push_back(st.col_double(1));
  }
}

std::string mmss(double s) { return plot::format_elapsed(s); }

std::string stats_line(const hockey::GameStats& g) {
  char buf[160] = {};
  std::snprintf(buf, sizeof(buf), "%zu shifts   avg %s on / %s off   longest %s   Z4+ %.0f min",
                g.shifts, mmss(g.avg_on_s).c_str(), mmss(g.avg_off_s).c_str(),
                mmss(g.longest_on_s).c_str(), (g.zone_s[3] + g.zone_s[4]) / 60.0);
  return buf;
}

void add_zone_lines(Panel& p, double hr_max) {
  static const double kFrac[] = {0.6, 0.7, 0.8, 0.9};
  static const char* kName[] = {"Z2 60%", "Z3 70%", "Z4 80%", "Z5 90%"};
  for (size_t i = 0; i < 4; ++i) {
    p.hlines.push_back(plot::HLine{hr_max * kFrac[i], kName[i], plot::rgb(0x999999, 0.9f),
                                   YAxisSide::kLeft});
  }
}

}  // namespace

double hockey_hr_max(store::Db& db) {
  std::vector<double> maxima;
  store::Stmt st(db, kGameSql);
  G_REQUIRE_RET(st.ok(), kDefaultHrMax);
  for (size_t i = 0; i < kMaxGames && st.row(); ++i) {
    if (!st.col_null(4) && st.col_double(4) > 100.0) maxima.push_back(st.col_double(4));
  }
  G_REQUIRE_RET(!maxima.empty(), kDefaultHrMax);
  std::sort(maxima.begin(), maxima.end());
  // 95th percentile of per-game maxima: ignores a stray optical-HR spike.
  const size_t idx = std::min(maxima.size() - 1, static_cast<size_t>(maxima.size() * 0.95));
  return maxima[idx];
}

std::vector<GameEntry> list_games(store::Db& db) {
  G_ASSERT(db.is_open());
  std::vector<GameEntry> out;
  GameEntry season;
  season.season = true;
  season.label = "Season overview (all games)";
  out.push_back(season);
  store::Stmt st(db, kGameSql);
  G_REQUIRE_RET(st.ok(), out);
  int64_t last = -1;
  for (size_t i = 0; i < kMaxGames && st.row(); ++i) {
    GameEntry g;
    g.fit_file_id = st.col_int(0);
    g.start_ts = st.col_int(1);
    if (g.start_ts == last) continue;  // watch copy and Connect download of the same game
    last = g.start_ts;
    g.label = plot::format_date_local(static_cast<double>(g.start_ts)) + "  " +
              plot::format_clock_local(static_cast<double>(g.start_ts)).substr(0, 5);
    char buf[64] = {};
    if (!st.col_null(2)) {
      std::snprintf(buf, sizeof(buf), "  %.0f min", st.col_double(2) / 60.0);
      g.label += buf;
    }
    if (!st.col_null(3) && !st.col_null(4)) {
      std::snprintf(buf, sizeof(buf), "  HR %lld/%lld", static_cast<long long>(st.col_int(3)),
                    static_cast<long long>(st.col_int(4)));
      g.label += buf;
    }
    out.push_back(std::move(g));
  }
  return out;
}

Figure load_game(store::Db& db, const GameEntry& g, double hr_max) {
  G_ASSERT(db.is_open());
  Figure fig;
  fig.xmode = plot::XMode::kElapsed;
  G_ASSERT(!g.season);
  std::vector<double> x;
  std::vector<double> y;
  load_hr(db, g.fit_file_id, g.start_ts, x, y);
  const std::vector<hockey::Shift> shifts = hockey::detect_shifts(x, y);
  const hockey::GameStats stats = hockey::compute_stats(x, y, shifts, hr_max);
  fig.title = "Hockey  " + g.label + "    " + stats_line(stats);

  {
    Panel p;
    p.title = "Heart rate  -  " + stats_line(stats);
    Series on = make("On ice", "", plot::rgb(0x1F77B4, 0.16f), Style::kBand);
    for (const hockey::Shift& s : shifts) {
      on.x.push_back(s.start);
      on.x2.push_back(s.end);
      on.y.push_back(0.0);
    }
    if (!on.x.empty()) p.series.push_back(std::move(on));
    Series hr = make("HR", "bpm", colors::kHeartRate, Style::kLine);
    hr.x = x;
    hr.y = y;
    p.series.push_back(std::move(hr));
    add_zone_lines(p, hr_max);
    p.weight = 1.6f;
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Shifts";
    p.left.include_zero = true;
    Series len = make("Shift length", "min", colors::kBodyBattery, Style::kBars);
    Series peak = make("Peak HR", "bpm", colors::kHeartRate, Style::kPoints, YAxisSide::kRight);
    for (const hockey::Shift& s : shifts) {
      len.x.push_back(s.start);
      len.x2.push_back(s.end);
      len.y.push_back((s.end - s.start) / 60.0);
      peak.x.push_back((s.start + s.end) / 2.0);
      peak.y.push_back(s.hr_peak);
    }
    p.series.push_back(std::move(len));
    if (!peak.x.empty()) p.series.push_back(std::move(peak));
    fig.panels.push_back(std::move(p));
  }
  return fig;
}

namespace {

// Per-game season series, filled newest-first and flipped once at the end.
struct SeasonSeries {
  Series avg = make("Avg HR", "bpm", plot::rgb(0xE377C2), Style::kPoints);
  Series mx = make("Max HR", "bpm", colors::kHeartRate, Style::kPoints);
  Series count = make("Shifts", "", colors::kBodyBattery, Style::kPoints);
  Series on = make("Avg shift", "s", colors::kSpeed, Style::kPoints, YAxisSide::kRight);
  Series z4 = make("Z4+ time", "min", colors::kStress, Style::kBars);
  Series dur = make("Recording", "min", colors::kAltitude, Style::kPoints, YAxisSide::kRight);

  void add(double t, const hockey::GameStats& s) {
    constexpr double kBarHalf = 0.45 * 86400.0;
    avg.x.push_back(t);
    avg.y.push_back(s.avg_hr);
    mx.x.push_back(t);
    mx.y.push_back(s.max_hr);
    count.x.push_back(t);
    count.y.push_back(static_cast<double>(s.shifts));
    on.x.push_back(t);
    on.y.push_back(s.avg_on_s);
    z4.x.push_back(t - kBarHalf);
    z4.x2.push_back(t + kBarHalf);
    z4.y.push_back((s.zone_s[3] + s.zone_s[4]) / 60.0);
    dur.x.push_back(t);
    dur.y.push_back(s.duration_s / 60.0);
  }

  void finish() {
    for (Series* s : {&avg, &mx, &count, &on, &z4, &dur}) {
      std::reverse(s->x.begin(), s->x.end());
      std::reverse(s->y.begin(), s->y.end());
      std::reverse(s->x2.begin(), s->x2.end());
      s->gap_break = 21.0 * 86400.0;
    }
  }
};

}  // namespace

Figure load_season(store::Db& db, double hr_max) {
  G_ASSERT(db.is_open());
  G_ASSERT(hr_max > 100.0);
  Figure fig;
  fig.xmode = plot::XMode::kTime;
  char buf[96] = {};
  std::snprintf(buf, sizeof(buf), "Hockey season overview   (zones from HR max %.0f)", hr_max);
  fig.title = buf;

  SeasonSeries ss;
  std::vector<double> x;
  std::vector<double> y;
  for (const GameEntry& g : list_games(db)) {
    if (g.season) continue;
    load_hr(db, g.fit_file_id, g.start_ts, x, y);
    if (x.size() < 60) continue;
    const std::vector<hockey::Shift> shifts = hockey::detect_shifts(x, y);
    ss.add(static_cast<double>(g.start_ts), hockey::compute_stats(x, y, shifts, hr_max));
  }
  ss.finish();

  Panel hr;
  hr.title = "Heart rate per game";
  hr.series.push_back(std::move(ss.avg));
  hr.series.push_back(std::move(ss.mx));
  add_zone_lines(hr, hr_max);
  fig.panels.push_back(std::move(hr));

  Panel shifts;
  shifts.title = "Shifts per game";
  shifts.left.include_zero = true;
  shifts.right.include_zero = true;
  shifts.series.push_back(std::move(ss.count));
  shifts.series.push_back(std::move(ss.on));
  fig.panels.push_back(std::move(shifts));

  Panel hard;
  hard.title = "Hard minutes";
  hard.left.include_zero = true;
  hard.right.include_zero = true;
  hard.series.push_back(std::move(ss.z4));
  hard.series.push_back(std::move(ss.dur));
  fig.panels.push_back(std::move(hard));
  return fig;
}

}  // namespace gview
