// Axis tick generation. Pure functions so they can be unit-tested without a
// window: numeric "nice number" ticks, wall-clock ticks aligned to local
// time boundaries, and elapsed-time ticks.
#pragma once
#include <string>
#include <vector>

namespace plot {

struct Tick {
  double value = 0.0;
  std::string label;
  bool emphasized = false;  // e.g. local midnight on a time axis
};

constexpr size_t kMaxTicks = 200;

// Nice step (1, 2, 5 x 10^n) for roughly `target` divisions of the range.
double nice_step(double range, int target);

// Expands [lo, hi] outward to tick-aligned bounds. Degenerate ranges are
// widened around their value.
void nice_range(double& lo, double& hi, int target);

// Ticks for a numeric axis within [lo, hi].
void linear_ticks(double lo, double hi, int target, std::vector<Tick>& out);

// Ticks for a Unix-time axis spanning [x0, x1] drawn over `px_width` pixels.
// Labels are local time: "HH:MM" within a day, "17 Sep" at day boundaries,
// "Sep 2026" for month steps, "2026" for year steps.
void time_ticks(double x0, double x1, double px_width, std::vector<Tick>& out);

// Ticks for an elapsed-seconds axis. Labels "MM:SS" or "H:MM:SS".
void elapsed_ticks(double x0, double x1, double px_width, std::vector<Tick>& out);

// Formatting helpers shared with hover readouts.
std::string format_clock_local(double unix_seconds);       // "HH:MM:SS"
std::string format_date_local(double unix_seconds);        // "17 Sep 2026"
std::string format_elapsed(double seconds);                // "H:MM:SS" / "MM:SS"
std::string format_value(double v, const std::string& units);

}  // namespace plot
