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

// Appends one long shift shaped like the 28 Sep 2026 one: a gentle slide to
// 94 bpm on the bench (below synth_game's 100 baseline, so the trough sits
// where the climb starts), a fast climb to ~158, a plateau, a push to 172
// ending `rise_s` after the climb began, then recovery on the bench.
void append_long_shift(int rise_s, std::vector<double>& x, std::vector<double>& y) {
  const int t0 = x.empty() ? 0 : static_cast<int>(x.back()) + 1;
  const int push_s = 40;
  const int total = 60 + rise_s + 240;
  for (int k = 0; k < total; ++k) {
    double hr = 0.0;
    const int r = k - 60;  // seconds into the climb
    if (r < 0) {
      hr = 100.0 - 6.0 * k / 60.0;
    } else if (r < 40) {
      hr = 94.0 + 64.0 * r / 40.0;
    } else if (r < rise_s - push_s) {
      hr = 158.0 + 4.0 * (r - 40) / (rise_s - push_s - 40);
    } else if (r < rise_s) {
      hr = 162.0 + 10.0 * (r - (rise_s - push_s)) / push_s;
    } else {
      hr = 100.0 + 72.0 * std::exp(-(r - rise_s) / 50.0);
    }
    x.push_back(static_cast<double>(t0 + k));
    y.push_back(hr + ((k % 7) - 3) * 0.8);
  }
}

}  // namespace

TEST_CASE("detect_shifts keeps a 4.5 min shift and rejects a warm-up-length rise") {
  std::vector<double> x;
  std::vector<double> y;
  synth_game(10, x, y);
  append_long_shift(270, x, y);  // 28 Sep 2026: 257 s, dropped by the old 240 s cap
  const auto shifts = hockey::detect_shifts(x, y);
  REQUIRE(shifts.size() == 11);
  const double len = shifts.back().end - shifts.back().start;
  CHECK(len > 240.0);
  CHECK(len < 300.0);
  CHECK(shifts.back().hr_peak > 165.0);

  synth_game(10, x, y);
  append_long_shift(420, x, y);  // warm-up skate length
  CHECK(hockey::detect_shifts(x, y).size() == 10);
}

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
