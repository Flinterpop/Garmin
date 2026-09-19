#include "analysis/ski.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace ski {

namespace {

// Vertical speed at each sample from the smoothed altitude over a forward
// window of `span_s` seconds (falls back to backward at the end).
std::vector<double> vertical_speed(const std::vector<double>& t, const std::vector<double>& alt,
                                   double span_s) {
  const size_t n = t.size();
  std::vector<double> vs(n, 0.0);
  size_t j = 0;
  for (size_t i = 0; i < n; ++i) {
    if (j < i) j = i;
    while (j + 1 < n && t[j] - t[i] < span_s) ++j;
    if (j > i && t[j] > t[i]) {
      vs[i] = (alt[j] - alt[i]) / (t[j] - t[i]);
    } else if (i > 0 && t[i] > t[i - 1]) {
      vs[i] = vs[i - 1];
    }
  }
  return vs;
}

}  // namespace

std::vector<double> smooth_altitude(const Trace& tr, double half_window_s) {
  G_ASSERT(tr.valid());
  const size_t n = std::min(tr.t.size(), kMaxSamples);
  std::vector<double> out(n);
  size_t lo = 0;
  size_t hi = 0;
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    while (hi < n && tr.t[hi] <= tr.t[i] + half_window_s) sum += tr.alt[hi++];
    while (lo < hi && tr.t[lo] < tr.t[i] - half_window_s) sum -= tr.alt[lo++];
    G_ASSERT(hi > lo);
    out[i] = sum / static_cast<double>(hi - lo);
  }
  return out;
}

std::vector<Run> detect_runs(const Trace& tr, const Params& p) {
  std::vector<Run> runs;
  G_ASSERT(tr.valid());
  const size_t n = std::min(tr.t.size(), kMaxSamples);
  G_REQUIRE_RET(n >= 3, runs);
  const std::vector<double> alt = smooth_altitude(tr, p.smooth_half_window_s);
  const std::vector<double> vs = vertical_speed(tr.t, alt, p.slope_window_s);

  auto close_run = [&](size_t start, size_t end) {
    if (end <= start || runs.size() >= kMaxRuns) return;
    Run r;
    r.start_idx = start;
    r.end_idx = end;
    r.start_s = tr.t[start];
    r.end_s = tr.t[end];
    r.top_alt_m = alt[start];
    r.bottom_alt_m = alt[end];
    for (size_t i = start; i <= end; ++i) {
      r.top_alt_m = std::max(r.top_alt_m, alt[i]);
      r.bottom_alt_m = std::min(r.bottom_alt_m, alt[i]);
    }
    r.vertical_m = alt[start] - alt[end];
    const double dur = r.end_s - r.start_s;
    if (r.vertical_m < p.min_vertical_m || dur < p.min_duration_s) return;
    double hr_sum = 0.0;
    double hr_w = 0.0;
    double integrated = 0.0;
    for (size_t i = start; i < end; ++i) {
      const double dt = tr.t[i + 1] - tr.t[i];
      r.max_speed_mps = std::max(r.max_speed_mps, tr.speed[i]);
      integrated += tr.speed[i] * dt;
      if (tr.hr[i] > 0.0) {
        hr_sum += tr.hr[i] * dt;
        hr_w += dt;
        r.max_hr = std::max(r.max_hr, tr.hr[i]);
      }
    }
    const double by_dist = tr.dist[end] - tr.dist[start];
    r.distance_m = by_dist > 0.0 ? by_dist : integrated;
    r.avg_speed_mps = dur > 0.0 ? r.distance_m / dur : 0.0;
    r.avg_hr = hr_w > 0.0 ? hr_sum / hr_w : 0.0;
    runs.push_back(r);
  };

  bool in_run = false;
  size_t start = 0;
  size_t last_desc = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool desc = vs[i] < p.descend_mps;
    const bool asc = vs[i] > p.ascend_mps;
    if (desc) {
      if (!in_run) {
        in_run = true;
        start = i;
      }
      last_desc = i;
    } else if (in_run && (asc || tr.t[i] - tr.t[last_desc] > p.flat_gap_s)) {
      close_run(start, last_desc);
      in_run = false;
    }
  }
  if (in_run) close_run(start, last_desc);
  return runs;
}

DayStats compute_stats(const Trace& tr, const std::vector<Run>& runs, const Params& p) {
  DayStats d;
  G_ASSERT(tr.valid());
  const size_t n = std::min(tr.t.size(), kMaxSamples);
  G_REQUIRE_RET(n >= 2, d);
  d.total_time_s = tr.t[n - 1] - tr.t[0];
  const std::vector<double> alt = smooth_altitude(tr, p.smooth_half_window_s);
  const std::vector<double> vs = vertical_speed(tr.t, alt, p.slope_window_s);
  for (size_t i = 0; i + 1 < n; ++i) {
    if (vs[i] > p.ascend_mps) d.lift_time_s += std::min(tr.t[i + 1] - tr.t[i], 10.0);
  }
  double hr_sum = 0.0;
  double hr_w = 0.0;
  for (const Run& r : runs) {
    ++d.runs;
    d.vertical_m += r.vertical_m;
    d.distance_m += r.distance_m;
    const double dur = r.end_s - r.start_s;
    d.ski_time_s += dur;
    d.max_speed_mps = std::max(d.max_speed_mps, r.max_speed_mps);
    d.longest_run_m = std::max(d.longest_run_m, r.vertical_m);
    if (r.avg_hr > 0.0) {
      hr_sum += r.avg_hr * dur;
      hr_w += dur;
    }
  }
  d.avg_hr_skiing = hr_w > 0.0 ? hr_sum / hr_w : 0.0;
  return d;
}

}  // namespace ski
