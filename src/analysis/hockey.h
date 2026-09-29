// Hockey-specific analysis of a heart-rate trace: shift detection and
// time-in-zone. Pure functions over (seconds, bpm) vectors so they can be
// unit-tested; the queries layer feeds them activity_record rows.
#pragma once
#include <cstddef>
#include <vector>

namespace hockey {

struct Shift {
  double start = 0.0;    // seconds (same axis as the input x)
  double end = 0.0;
  double hr_low = 0.0;   // bpm at the start of the rise
  double hr_peak = 0.0;  // bpm at the end of the rise
};

constexpr size_t kZones = 5;

struct GameStats {
  size_t shifts = 0;
  double avg_on_s = 0.0;      // mean shift length
  double avg_off_s = 0.0;     // mean gap between shifts
  double longest_on_s = 0.0;
  double avg_hr = 0.0;        // time-weighted over the whole recording
  double max_hr = 0.0;
  double zone_s[kZones] = {}; // time below 60/70/80/90 % and at/above 90 % of hr_max
  double duration_s = 0.0;
};

struct DetectParams {
  double smooth_half_window_s = 8.0;  // centred moving average
  double hysteresis_bpm = 12.0;       // reversal needed to flip rising/falling
  double min_rise_bpm = 20.0;         // rise amplitude that counts as a shift
  double min_shift_s = 20.0;
  // Trough-to-peak length. 330 s keeps the real long shifts (p99 of all
  // qualifying rises is ~320 s; 240 s dropped 5 % of them) while the
  // warm-up skate, usually 6-15 min, still fails it.
  double max_shift_s = 330.0;
  // A shift's peak must reach median + peak_fraction * (p95 - median) of the
  // smoothed trace; this rejects the warm-up skate and the walk to the car.
  double peak_fraction = 0.25;
};

constexpr size_t kMaxSamples = 200000;
constexpr size_t kMaxShifts = 400;

// Centred moving average; x must be ascending, x.size() == y.size().
std::vector<double> smooth(const std::vector<double>& x, const std::vector<double>& y,
                           double half_window_s);

std::vector<Shift> detect_shifts(const std::vector<double>& x, const std::vector<double>& y,
                                 const DetectParams& p = DetectParams());

// Zone index 0..4 for a heart rate given hr_max.
size_t zone_of(double hr, double hr_max);

GameStats compute_stats(const std::vector<double>& x, const std::vector<double>& y,
                        const std::vector<Shift>& shifts, double hr_max);

}  // namespace hockey
