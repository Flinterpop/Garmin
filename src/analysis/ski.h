// Resort-skiing analysis from a 1 Hz activity trace: splits the day into
// runs (sustained descents) and lifts (sustained ascents) using the
// barometric altitude, then summarises each run and the day.
#pragma once
#include <cstddef>
#include <vector>

namespace ski {

struct Trace {
  std::vector<double> t;      // elapsed seconds, ascending
  std::vector<double> alt;    // metres
  std::vector<double> speed;  // m/s (0 = unknown)
  std::vector<double> hr;     // bpm (0 = unknown)
  std::vector<double> dist;   // cumulative metres (0 = unknown)
  bool valid() const {
    return t.size() == alt.size() && t.size() == speed.size() && t.size() == hr.size() &&
           t.size() == dist.size();
  }
};

struct Run {
  size_t start_idx = 0;
  size_t end_idx = 0;
  double start_s = 0.0;
  double end_s = 0.0;
  double top_alt_m = 0.0;
  double bottom_alt_m = 0.0;
  double vertical_m = 0.0;
  double distance_m = 0.0;
  double max_speed_mps = 0.0;
  double avg_speed_mps = 0.0;
  double avg_hr = 0.0;
  double max_hr = 0.0;
};

struct DayStats {
  size_t runs = 0;
  double vertical_m = 0.0;
  double distance_m = 0.0;
  double ski_time_s = 0.0;
  double lift_time_s = 0.0;
  double max_speed_mps = 0.0;
  double longest_run_m = 0.0;   // vertical
  double avg_hr_skiing = 0.0;   // time-weighted over runs
  double total_time_s = 0.0;
};

struct Params {
  double smooth_half_window_s = 8.0;
  double slope_window_s = 12.0;     // span for the vertical-speed estimate
  double descend_mps = -0.5;        // vertical speed that means "skiing down"
  double ascend_mps = 0.2;          // vertical speed that means "on a lift" (small-hill chairs ~0.25)
  double flat_gap_s = 40.0;         // pause at the bottom that ends a run
  double min_vertical_m = 25.0;
  double min_duration_s = 20.0;
};

constexpr size_t kMaxSamples = 200000;
constexpr size_t kMaxRuns = 500;

std::vector<double> smooth_altitude(const Trace& tr, double half_window_s);
std::vector<Run> detect_runs(const Trace& tr, const Params& p = Params());
DayStats compute_stats(const Trace& tr, const std::vector<Run>& runs, const Params& p = Params());

}  // namespace ski
