#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "analysis/ski.h"

using Catch::Matchers::WithinAbs;

namespace {

// Synthetic resort day at 1 Hz: `runs` cycles of 30 s at the top, a 120 s
// descent of 300 m at 2.5 m/s vertical, 40 s at the bottom, and a 300 s lift
// back up. Speed 12 m/s while skiing, 3 m/s on the lift, 0 when waiting.
ski::Trace synth_day(int runs) {
  ski::Trace tr;
  double alt = 1300.0;
  double dist = 0.0;
  double t = 0.0;
  auto push = [&](double vs, double speed, double hr) {
    alt += vs;
    dist += speed;
    tr.t.push_back(t);
    tr.alt.push_back(alt + 0.4 * std::sin(t));  // barometric jitter
    tr.speed.push_back(speed);
    tr.hr.push_back(hr);
    tr.dist.push_back(dist);
    t += 1.0;
  };
  for (int r = 0; r < runs; ++r) {
    for (int i = 0; i < 30; ++i) push(0.0, 0.0, 95.0);
    for (int i = 0; i < 120; ++i) push(-2.5, 12.0, 140.0);
    for (int i = 0; i < 40; ++i) push(0.0, 0.0, 120.0);
    for (int i = 0; i < 300; ++i) push(1.0, 3.0, 100.0);
  }
  return tr;
}

}  // namespace

TEST_CASE("ski runs are found on a synthetic day") {
  const ski::Trace tr = synth_day(8);
  const auto runs = ski::detect_runs(tr);
  REQUIRE(runs.size() == 8);
  for (const ski::Run& r : runs) {
    CHECK(r.vertical_m > 270.0);
    CHECK(r.vertical_m < 330.0);
    CHECK(r.end_s - r.start_s > 90.0);
    CHECK(r.end_s - r.start_s < 160.0);
    CHECK_THAT(r.max_speed_mps, WithinAbs(12.0, 1e-9));
    CHECK(r.avg_hr > 130.0);
  }
  for (size_t i = 1; i < runs.size(); ++i) CHECK(runs[i].start_s > runs[i - 1].end_s);

  const ski::DayStats d = ski::compute_stats(tr, runs);
  CHECK(d.runs == 8);
  CHECK(d.vertical_m > 2200.0);
  CHECK(d.lift_time_s > 8 * 250.0);
  CHECK(d.lift_time_s < 8 * 320.0);
  CHECK(d.ski_time_s > 8 * 90.0);
  CHECK_THAT(d.max_speed_mps, WithinAbs(12.0, 1e-9));
  CHECK(d.avg_hr_skiing > 130.0);
}

TEST_CASE("ski detection ignores flat and short bumps") {
  ski::Trace tr;
  for (int i = 0; i < 1800; ++i) {
    tr.t.push_back(i);
    tr.alt.push_back(500.0 + 3.0 * std::sin(i / 20.0));  // +-3 m wobble
    tr.speed.push_back(1.0);
    tr.hr.push_back(80.0);
    tr.dist.push_back(i);
  }
  CHECK(ski::detect_runs(tr).empty());
  CHECK(ski::detect_runs(ski::Trace{}).empty());
}
