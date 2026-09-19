// Sleep view: one night in detail.
#include <algorithm>
#include <cstdio>

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

constexpr int64_t kMargin = 30 * 60;  // half an hour either side of the night

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

void load_xy(store::Db& db, const char* sql, int64_t t0, int64_t t1, Series& s) {
  store::Stmt st(db, sql);
  G_REQUIRE_VOID(st.ok());
  st.bind(1, t0).bind(2, t1);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    if (st.col_null(1)) continue;
    s.x.push_back(static_cast<double>(st.col_int(0)));
    s.y.push_back(st.col_double(1));
  }
}

}  // namespace

std::vector<NightEntry> list_nights(store::Db& db) {
  std::vector<NightEntry> out;
  store::Stmt st(db,
                 "SELECT date, start_ts, end_ts, score, deep_s, light_s, rem_s FROM sleep"
                 " WHERE start_ts IS NOT NULL AND end_ts IS NOT NULL ORDER BY date DESC");
  G_REQUIRE_RET(st.ok(), out);
  for (size_t i = 0; i < kMaxListEntries && st.row(); ++i) {
    NightEntry e;
    e.date = st.col_text(0);
    e.start_ts = st.col_int(1);
    e.end_ts = st.col_int(2);
    const double hours = (st.col_int(4) + st.col_int(5) + st.col_int(6)) / 3600.0;
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "  %.1f h", hours);
    e.label = plot::format_date_local(static_cast<double>(local_midnight_of(e.date)) + 43200.0) +
              buf;
    if (!st.col_null(3)) e.label += "  score " + std::to_string(st.col_int(3));
    out.push_back(std::move(e));
  }
  return out;
}

Figure load_night(store::Db& db, const NightEntry& n) {
  Figure fig;
  fig.xmode = plot::XMode::kTime;
  const int64_t t0 = n.start_ts - kMargin;
  const int64_t t1 = n.end_ts + kMargin;
  fig.extend_x = true;
  fig.extend_x_min = static_cast<double>(t0);
  fig.extend_x_max = static_cast<double>(t1);

  // Summary line from the sleep row.
  {
    store::Stmt st(db,
                   "SELECT deep_s, light_s, rem_s, awake_s, score, avg_spo2, avg_resp, avg_hrv"
                   " FROM sleep WHERE date = ?");
    if (st.ok()) {
      st.bind(1, n.date);
      if (st.row()) {
        char buf[240] = {};
        std::snprintf(buf, sizeof(buf),
                      "Sleep  %s    %s in bed   deep %lld  light %lld  rem %lld  awake %lld min",
                      n.date.c_str(), plot::format_elapsed(static_cast<double>(n.end_ts - n.start_ts)).c_str(),
                      static_cast<long long>(st.col_int(0) / 60), static_cast<long long>(st.col_int(1) / 60),
                      static_cast<long long>(st.col_int(2) / 60), static_cast<long long>(st.col_int(3) / 60));
        fig.title = buf;
        if (!st.col_null(4)) fig.title += "   score " + std::to_string(st.col_int(4));
      }
    }
  }
  if (fig.title.empty()) fig.title = "Sleep  " + n.date;

  // Hypnogram: stages as bands plus a step trace (deep 0 .. awake 3).
  {
    Panel p;
    p.title = "Stages";
    p.left.fixed = true;
    p.left.min = -0.5;
    p.left.max = 3.5;
    struct StageDef {
      const char* stage;
      const char* name;
      double level;
      plot::Color color;
    };
    const StageDef stages[] = {{"deep", "Deep", 0.0, colors::kDeep},
                               {"light", "Light", 1.0, colors::kLight},
                               {"rem", "REM", 2.0, colors::kRem},
                               {"awake", "Awake", 3.0, colors::kAwake}};
    Series step = make("Stage", "", plot::rgb(0x333333), Style::kStep);
    std::vector<std::pair<double, double>> timeline;  // (start, level)
    for (const StageDef& sd : stages) {
      Series band = make(sd.name, "", sd.color, Style::kBand);
      store::Stmt st(db,
                     "SELECT start_ts, end_ts FROM sleep_stage WHERE stage = ? AND source = 'api'"
                     " AND end_ts >= ? AND start_ts <= ? ORDER BY start_ts");
      if (!st.ok()) continue;
      st.bind(1, std::string(sd.stage)).bind(2, t0).bind(3, t1);
      for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
        band.x.push_back(static_cast<double>(st.col_int(0)));
        band.x2.push_back(static_cast<double>(st.col_int(1)));
        band.y.push_back(0.0);
        timeline.emplace_back(static_cast<double>(st.col_int(0)), sd.level);
      }
      if (!band.x.empty()) p.series.push_back(std::move(band));
      p.hlines.push_back(plot::HLine{sd.level, sd.name, plot::rgb(0x888888), YAxisSide::kLeft});
    }
    std::sort(timeline.begin(), timeline.end());
    for (const auto& [x, level] : timeline) {
      step.x.push_back(x);
      step.y.push_back(level);
    }
    if (!step.x.empty()) p.series.push_back(std::move(step));
    p.weight = 0.9f;
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Heart rate / HRV";
    Series hr = make("HR", "bpm", colors::kHeartRate, Style::kLine);
    load_xy(db, "SELECT ts, bpm FROM hr_sample WHERE source='fit' AND ts BETWEEN ? AND ? ORDER BY ts",
            t0, t1, hr);
    if (hr.x.empty()) {
      load_xy(db, "SELECT ts, bpm FROM hr_sample WHERE source='api' AND ts BETWEEN ? AND ? ORDER BY ts",
              t0, t1, hr);
    }
    Series hrv = make("HRV", "ms", colors::kHrv, Style::kPoints, YAxisSide::kRight);
    load_xy(db, "SELECT ts, rmssd_ms FROM hrv_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0, t1, hrv);
    p.series.push_back(std::move(hr));
    if (!hrv.x.empty()) p.series.push_back(std::move(hrv));
    p.weight = 1.2f;
    fig.panels.push_back(std::move(p));
  }
  {
    Panel p;
    p.title = "Respiration / SpO2 / Stress";
    Series resp = make("Respiration", "brpm", colors::kRespiration, Style::kLine);
    load_xy(db, "SELECT ts, brpm FROM respiration_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0,
            t1, resp);
    Series spo2 = make("SpO2", "%", colors::kBodyBattery, Style::kPoints, YAxisSide::kRight);
    load_xy(db, "SELECT ts, pct FROM spo2_sample WHERE ts BETWEEN ? AND ? ORDER BY ts", t0, t1, spo2);
    Series stress = make("Stress", "", colors::kStress, Style::kStep, YAxisSide::kRight);
    load_xy(db, "SELECT ts, level FROM stress_sample WHERE source='fit' AND ts BETWEEN ? AND ? ORDER BY ts",
            t0, t1, stress);
    if (!resp.x.empty()) p.series.push_back(std::move(resp));
    if (!spo2.x.empty()) p.series.push_back(std::move(spo2));
    if (!stress.x.empty()) p.series.push_back(std::move(stress));
    if (!p.series.empty()) fig.panels.push_back(std::move(p));
  }
  fig.markers.push_back(plot::Marker{static_cast<double>(n.start_ts), "asleep"});
  fig.markers.push_back(plot::Marker{static_cast<double>(n.end_ts), ""});
  return fig;
}

}  // namespace gview
