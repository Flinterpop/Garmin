#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "analysis/hockey.h"

using Catch::Matchers::WithinAbs;

namespace {

// Synthetic game: 1 Hz HR, baseline 100 bpm, a shift every 240 s made of a
// 60 s linear rise to 170 followed by an exponential decay on the bench.
void synth_game(int shifts, std::vector<double>& x, std::vector<double>& y) {
  x.clear();
  y.clear();
  const int total = shifts * 240 + 120;
  for (int t = 0; t < total; ++t) {
    double hr = 100.0;
    const int phase = t % 240;
    const int shift_idx = t / 240;
    if (shift_idx < shifts && t >= 60) {
      if (phase < 60) {
        hr = 100.0 + 70.0 * phase / 60.0;
      } else {
        hr = 100.0 + 70.0 * std::exp(-(phase - 60) / 50.0);
      }
    }
    x.push_back(static_cast<double>(t));
    y.push_back(hr + ((t % 7) - 3) * 0.8);  // a little jitter
  }
}

}  // namespace

TEST_CASE("smooth is a centred moving average that preserves constants") {
  std::vector<double> x;
  std::vector<double> y;
  for (int i = 0; i < 100; ++i) {
    x.push_back(i);
    y.push_back(120.0);
  }
  const auto s = hockey::smooth(x, y, 5.0);
  REQUIRE(s.size() == 100);
  for (const double v : s) CHECK_THAT(v, WithinAbs(120.0, 1e-9));
  // A single spike is spread over the window.
  y[50] = 220.0;
  const auto s2 = hockey::smooth(x, y, 5.0);
  CHECK_THAT(s2[50], WithinAbs(120.0 + 100.0 / 11.0, 1e-9));
  CHECK_THAT(s2[56], WithinAbs(120.0, 1e-9));
}

TEST_CASE("detect_shifts finds each rise of a synthetic game") {
  std::vector<double> x;
  std::vector<double> y;
  synth_game(15, x, y);
  const auto shifts = hockey::detect_shifts(x, y);
  REQUIRE(shifts.size() == 15);
  for (const hockey::Shift& s : shifts) {
    const double len = s.end - s.start;
    CHECK(len > 40.0);
    CHECK(len < 90.0);
    CHECK(s.hr_peak - s.hr_low > 50.0);
  }
  // Shifts are in order and do not overlap.
  for (size_t i = 1; i < shifts.size(); ++i) CHECK(shifts[i].start >= shifts[i - 1].end);
}

TEST_CASE("detect_shifts ignores flat and noisy-but-small traces") {
  std::vector<double> x;
  std::vector<double> y;
  for (int i = 0; i < 3600; ++i) {
    x.push_back(i);
    y.push_back(95.0 + 5.0 * std::sin(i / 30.0));  // +-5 bpm wobble
  }
  CHECK(hockey::detect_shifts(x, y).empty());
  CHECK(hockey::detect_shifts({}, {}).empty());
}

TEST_CASE("zones and game stats") {
  CHECK(hockey::zone_of(100.0, 190.0) == 0);
  CHECK(hockey::zone_of(120.0, 190.0) == 1);
  CHECK(hockey::zone_of(140.0, 190.0) == 2);
  CHECK(hockey::zone_of(160.0, 190.0) == 3);
  CHECK(hockey::zone_of(175.0, 190.0) == 4);

  std::vector<double> x;
  std::vector<double> y;
  synth_game(10, x, y);
  const auto shifts = hockey::detect_shifts(x, y);
  const hockey::GameStats g = hockey::compute_stats(x, y, shifts, 190.0);
  CHECK(g.shifts == 10);
  CHECK(g.avg_on_s > 40.0);
  CHECK(g.avg_on_s < 90.0);
  CHECK(g.avg_off_s > 150.0);
  CHECK(g.avg_off_s < 200.0);
  CHECK(g.max_hr > 165.0);
  CHECK(g.avg_hr > 100.0);
  CHECK(g.avg_hr < 140.0);
  double zone_total = 0.0;
  for (const double z : g.zone_s) zone_total += z;
  CHECK_THAT(zone_total, WithinAbs(static_cast<double>(x.size()), 1.0));
}
