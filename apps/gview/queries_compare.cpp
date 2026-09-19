// Compare view: two activities overlaid on elapsed time.
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

namespace {

struct Traces {
  Series hr;
  Series speed;
  Series alt;
  Series cadence;
  double distance_m = 0.0;
  double duration_s = 0.0;
};

Series make(const std::string& name, const char* units, plot::Color c) {
  Series s;
  s.name = name;
  s.units = units;
  s.color = c;
  s.style = Style::kLine;
  return s;
}

Traces load_traces(store::Db& db, const ActivityEntry& a, const char* tag, plot::Color c) {
  Traces t;
  t.hr = make(std::string(tag) + " HR", "bpm", c);
  t.speed = make(std::string(tag) + " speed", "km/h", c);
  t.alt = make(std::string(tag) + " altitude", "m", c);
  t.cadence = make(std::string(tag) + " cadence", "rpm", c);
  store::Stmt st(db,
                 "SELECT ts, hr, speed_mps, alt_m, cadence, dist_m FROM activity_record"
                 " WHERE fit_file_id = ? ORDER BY ts");
  G_REQUIRE_RET(st.ok(), t);
  st.bind(1, a.fit_file_id);
  for (size_t i = 0; i < kMaxSamples && st.row(); ++i) {
    const double x = static_cast<double>(st.col_int(0) - a.start_ts);
    t.duration_s = x;
    if (!st.col_null(1)) {
      t.hr.x.push_back(x);
      t.hr.y.push_back(st.col_double(1));
    }
    if (!st.col_null(2)) {
      t.speed.x.push_back(x);
      t.speed.y.push_back(st.col_double(2) * 3.6);
    }
    if (!st.col_null(3)) {
      t.alt.x.push_back(x);
      t.alt.y.push_back(st.col_double(3));
    }
    if (!st.col_null(4)) {
      t.cadence.x.push_back(x);
      t.cadence.y.push_back(st.col_double(4));
    }
    if (!st.col_null(5)) t.distance_m = st.col_double(5);
  }
  return t;
}

std::string summary(const char* tag, const ActivityEntry& e, const Traces& t) {
  char buf[200] = {};
  double sum = 0.0;
  for (const double v : t.hr.y) sum += v;
  const double avg_hr = t.hr.y.empty() ? 0.0 : sum / static_cast<double>(t.hr.y.size());
  std::snprintf(buf, sizeof(buf), "%s: %s  |  %s, %.1f km, avg HR %.0f", tag, e.label.c_str(),
                plot::format_elapsed(t.duration_s).c_str(), t.distance_m / 1000.0, avg_hr);
  return buf;
}

}  // namespace

Figure load_compare(store::Db& db, const ActivityEntry& a, const ActivityEntry& b) {
  Figure fig;
  fig.xmode = plot::XMode::kElapsed;
  const plot::Color ca = plot::rgb(0xD62728);
  const plot::Color cb = plot::rgb(0x1F77B4);
  const Traces ta = load_traces(db, a, "A", ca);
  const Traces tb = load_traces(db, b, "B", cb);
  fig.title = "Compare   " + summary("A", a, ta) + "     " + summary("B", b, tb);

  {
    Panel p;
    p.title = "Heart rate";
    if (!ta.hr.x.empty()) p.series.push_back(ta.hr);
    if (!tb.hr.x.empty()) p.series.push_back(tb.hr);
    p.weight = 1.4f;
    if (!p.series.empty()) fig.panels.push_back(std::move(p));
  }
  if (!ta.speed.x.empty() || !tb.speed.x.empty()) {
    Panel p;
    p.title = "Speed";
    p.left.include_zero = true;
    if (!ta.speed.x.empty()) p.series.push_back(ta.speed);
    if (!tb.speed.x.empty()) p.series.push_back(tb.speed);
    fig.panels.push_back(std::move(p));
  }
  if (!ta.alt.x.empty() || !tb.alt.x.empty()) {
    Panel p;
    p.title = "Altitude";
    if (!ta.alt.x.empty()) p.series.push_back(ta.alt);
    if (!tb.alt.x.empty()) p.series.push_back(tb.alt);
    fig.panels.push_back(std::move(p));
  }
  if (!ta.cadence.x.empty() || !tb.cadence.x.empty()) {
    Panel p;
    p.title = "Cadence";
    p.left.include_zero = true;
    if (!ta.cadence.x.empty()) p.series.push_back(ta.cadence);
    if (!tb.cadence.x.empty()) p.series.push_back(tb.cadence);
    fig.panels.push_back(std::move(p));
  }
  return fig;
}

}  // namespace gview
