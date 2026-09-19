#include "analysis/hockey.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace hockey {

std::vector<double> smooth(const std::vector<double>& x, const std::vector<double>& y,
                           double half_window_s) {
  G_ASSERT(x.size() == y.size());
  G_ASSERT(half_window_s >= 0.0);
  const size_t n = std::min(x.size(), kMaxSamples);
  std::vector<double> out(n);
  size_t lo = 0;
  size_t hi = 0;  // window is [lo, hi)
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    // Both window edges only ever move forward, so the whole pass is O(n).
    while (hi < n && x[hi] <= x[i] + half_window_s) sum += y[hi++];
    while (lo < hi && x[lo] < x[i] - half_window_s) sum -= y[lo++];
    G_ASSERT(hi > lo);
    out[i] = sum / static_cast<double>(hi - lo);
  }
  return out;
}

std::vector<Shift> detect_shifts(const std::vector<double>& x, const std::vector<double>& y,
                                 const DetectParams& p) {
  std::vector<Shift> shifts;
  G_ASSERT(x.size() == y.size());
  const size_t n = std::min(x.size(), kMaxSamples);
  G_REQUIRE_RET(n >= 3, shifts);
  const std::vector<double> s = smooth(x, y, p.smooth_half_window_s);

  // Peak threshold from the distribution of the smoothed trace.
  double peak_min = 0.0;
  {
    std::vector<double> sorted(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[n / 2];
    const double p95 = sorted[std::min(n - 1, static_cast<size_t>(n * 0.95))];
    peak_min = median + p.peak_fraction * (p95 - median);
  }

  // Zig-zag extrema with hysteresis: track the running extreme in the
  // current direction and flip when the trace reverses by more than the
  // hysteresis. Each rising leg large enough is a shift.
  bool rising = s[1] >= s[0];
  size_t ext_i = 0;  // index of the current extreme (min while rising)
  size_t last_min_i = 0;
  for (size_t i = 1; i < n; ++i) {
    if (rising) {
      if (s[i] >= s[ext_i]) {
        ext_i = i;  // new high
      } else if (s[ext_i] - s[i] >= p.hysteresis_bpm) {
        // Peak confirmed at ext_i; the rise ran from last_min_i to ext_i.
        const double rise = s[ext_i] - s[last_min_i];
        const double len = x[ext_i] - x[last_min_i];
        if (rise >= p.min_rise_bpm && len >= p.min_shift_s && len <= p.max_shift_s &&
            s[ext_i] >= peak_min && shifts.size() < kMaxShifts) {
          shifts.push_back(Shift{x[last_min_i], x[ext_i], s[last_min_i], s[ext_i]});
        }
        rising = false;
        ext_i = i;
      }
    } else {
      if (s[i] <= s[ext_i]) {
        ext_i = i;  // new low
      } else if (s[i] - s[ext_i] >= p.hysteresis_bpm) {
        last_min_i = ext_i;
        rising = true;
        ext_i = i;
      }
    }
  }
  // A rise still in progress at the end counts if it already qualifies.
  if (rising && shifts.size() < kMaxShifts) {
    const double rise = s[ext_i] - s[last_min_i];
    const double len = x[ext_i] - x[last_min_i];
    if (rise >= p.min_rise_bpm && len >= p.min_shift_s && len <= p.max_shift_s &&
        s[ext_i] >= peak_min) {
      shifts.push_back(Shift{x[last_min_i], x[ext_i], s[last_min_i], s[ext_i]});
    }
  }
  return shifts;
}

size_t zone_of(double hr, double hr_max) {
  G_ASSERT(hr_max > 0.0);
  const double f = hr / hr_max;
  if (f < 0.6) return 0;
  if (f < 0.7) return 1;
  if (f < 0.8) return 2;
  if (f < 0.9) return 3;
  return 4;
}

GameStats compute_stats(const std::vector<double>& x, const std::vector<double>& y,
                        const std::vector<Shift>& shifts, double hr_max) {
  GameStats g;
  G_ASSERT(x.size() == y.size());
  G_REQUIRE_RET(!x.empty() && hr_max > 0.0, g);
  const size_t n = std::min(x.size(), kMaxSamples);
  constexpr double kMaxSampleGap = 10.0;  // seconds credited to one sample
  double weighted = 0.0;
  double total = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double dt = (i + 1 < n) ? std::min(x[i + 1] - x[i], kMaxSampleGap) : 1.0;
    g.zone_s[zone_of(y[i], hr_max)] += dt;
    weighted += y[i] * dt;
    total += dt;
    g.max_hr = std::max(g.max_hr, y[i]);
  }
  g.avg_hr = total > 0.0 ? weighted / total : 0.0;
  g.duration_s = x[n - 1] - x[0];
  g.shifts = shifts.size();
  double on = 0.0;
  double off = 0.0;
  for (size_t i = 0; i < shifts.size(); ++i) {
    const double len = shifts[i].end - shifts[i].start;
    on += len;
    g.longest_on_s = std::max(g.longest_on_s, len);
    if (i > 0) off += shifts[i].start - shifts[i - 1].end;
  }
  if (!shifts.empty()) g.avg_on_s = on / static_cast<double>(shifts.size());
  if (shifts.size() > 1) g.avg_off_s = off / static_cast<double>(shifts.size() - 1);
  return g;
}

}  // namespace hockey
