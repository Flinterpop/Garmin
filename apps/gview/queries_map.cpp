// Map view: activities that carry GPS, and their tracks.
#include <cstdio>

#include "fit/fit_profile.h"
#include "map/mercator.h"
#include "plot/ticks.h"
#include "queries.h"
#include "util/assert.h"

namespace gview {

std::vector<ActivityEntry> list_gps_activities(store::Db& db) {
  std::vector<ActivityEntry> out;
  store::Stmt st(db,
                 "SELECT s.fit_file_id, s.start_ts, s.sport, s.distance_m, s.timer_s, a.name, a.type"
                 " FROM activity_session s"
                 " LEFT JOIN activity a ON a.fit_file_id = s.fit_file_id"
                 " WHERE EXISTS (SELECT 1 FROM activity_record r"
                 "               WHERE r.fit_file_id = s.fit_file_id AND r.lat IS NOT NULL)"
                 " ORDER BY s.start_ts DESC, (SELECT COUNT(*) FROM activity_record r"
                 "   WHERE r.fit_file_id = s.fit_file_id) DESC");
  G_REQUIRE_RET(st.ok(), out);
  int64_t last = -1;
  for (size_t i = 0; i < kMaxListEntries && st.row(); ++i) {
    ActivityEntry e;
    e.fit_file_id = st.col_int(0);
    e.start_ts = st.col_int(1);
    if (e.start_ts == last) continue;
    last = e.start_ts;
    std::string sport = st.col_null(6) ? std::string() : st.col_text(6);
    if (sport.empty()) {
      sport = st.col_null(2) ? "activity" : fit::sport_name(static_cast<uint8_t>(st.col_int(2)));
    }
    for (char& ch : sport) {
      if (ch == '_') ch = ' ';
    }
    e.label = plot::format_date_local(static_cast<double>(e.start_ts)) + "  " + sport;
    char buf[48] = {};
    if (!st.col_null(3) && st.col_double(3) > 0.0) {
      std::snprintf(buf, sizeof(buf), "  %.1f km", st.col_double(3) / 1000.0);
      e.label += buf;
    }
    if (!st.col_null(4)) {
      std::snprintf(buf, sizeof(buf), "  %.0f min", st.col_double(4) / 60.0);
      e.label += buf;
    }
    e.title = e.label;
    if (!st.col_null(5) && !st.col_text(5).empty()) e.title = st.col_text(5) + "  -  " + e.label;
    out.push_back(std::move(e));
  }
  return out;
}

map::Track load_track(store::Db& db, const ActivityEntry& a) {
  map::Track t;
  t.title = a.title;
  store::Stmt st(db,
                 "SELECT ts, lat, lon, hr, speed_mps, alt_m FROM activity_record"
                 " WHERE fit_file_id = ? AND lat IS NOT NULL AND lon IS NOT NULL ORDER BY ts");
  G_REQUIRE_RET(st.ok(), t);
  st.bind(1, a.fit_file_id);
  double dist = 0.0;
  for (size_t i = 0; i < map::kMaxTrackPoints && st.row(); ++i) {
    map::TrackPoint p;
    p.elapsed_s = static_cast<double>(st.col_int(0) - a.start_ts);
    p.lat = st.col_double(1);
    p.lon = st.col_double(2);
    if (!st.col_null(3)) p.hr = st.col_double(3);
    if (!st.col_null(4)) p.speed_mps = st.col_double(4);
    if (!st.col_null(5)) p.alt_m = st.col_double(5);
    if (!t.points.empty()) {
      const map::TrackPoint& q = t.points.back();
      const double d = map::haversine_m(q.lat, q.lon, p.lat, p.lon);
      // A jump of more than 200 m in one sample is a GPS glitch; skip it.
      if (d > 200.0 && p.elapsed_s - q.elapsed_s < 5.0) continue;
      dist += d;
    }
    p.dist_m = dist;
    t.points.push_back(p);
  }

  // Lap starts -> nearest track index (points are time-ordered).
  store::Stmt laps(db, "SELECT start_ts FROM activity_lap WHERE fit_file_id = ? ORDER BY start_ts");
  if (laps.ok() && !t.points.empty()) {
    laps.bind(1, a.fit_file_id);
    size_t idx = 0;
    for (size_t i = 0; i < 1000 && laps.row(); ++i) {
      const double x = static_cast<double>(laps.col_int(0) - a.start_ts);
      for (size_t guard = 0; idx + 1 < t.points.size() && t.points[idx].elapsed_s < x &&
                             guard < map::kMaxTrackPoints;
           ++guard) {
        ++idx;
      }
      t.lap_starts.push_back(idx);
    }
  }
  return t;
}

}  // namespace gview
