#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "plot/decimate.h"
#include "plot/plot_types.h"

TEST_CASE("decimate passes short series through with neighbours") {
  std::vector<double> x = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<double> y = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<plot::Point> out;
  plot::decimate_minmax(x, y, 3.0, 6.0, 100, out);
  REQUIRE(out.size() == 6);  // 3..6 plus one each side
  CHECK(out.front().x == 2.0);
  CHECK(out.back().x == 7.0);
}

TEST_CASE("decimate keeps the envelope of a long series") {
  // 86400 samples (a day at 1 Hz) with a spike buried in the middle.
  std::vector<double> x(86400);
  std::vector<double> y(86400);
  for (size_t i = 0; i < x.size(); ++i) {
    x[i] = static_cast<double>(i);
    y[i] = 60.0 + 10.0 * std::sin(static_cast<double>(i) / 500.0);
  }
  y[43210] = 190.0;
  y[43211] = 30.0;
  std::vector<plot::Point> out;
  plot::decimate_minmax(x, y, 0.0, 86400.0, 800, out);
  CHECK(out.size() <= 2 * 800 + 4);
  double ymax = -1e9;
  double ymin = 1e9;
  for (const plot::Point& p : out) {
    ymax = std::max(ymax, p.y);
    ymin = std::min(ymin, p.y);
  }
  CHECK(ymax == 190.0);
  CHECK(ymin == 30.0);
  for (size_t i = 1; i < out.size(); ++i) CHECK(out[i].x >= out[i - 1].x);
}

TEST_CASE("decimate handles empty and out-of-range windows") {
  std::vector<plot::Point> out;
  plot::decimate_minmax({}, {}, 0.0, 1.0, 10, out);
  CHECK(out.empty());
  plot::decimate_minmax({10.0, 11.0}, {1.0, 2.0}, 100.0, 200.0, 10, out);
  CHECK(out.size() == 1);  // the last neighbour only
}

TEST_CASE("nearest_index") {
  const std::vector<double> x = {0.0, 10.0, 20.0, 30.0};
  CHECK(plot::nearest_index(x, -5.0) == 0);
  CHECK(plot::nearest_index(x, 14.0) == 1);
  CHECK(plot::nearest_index(x, 16.0) == 2);
  CHECK(plot::nearest_index(x, 99.0) == 3);
  CHECK(plot::nearest_index({}, 1.0) == SIZE_MAX);
}

TEST_CASE("figure finalize computes the x extent") {
  plot::Figure f;
  plot::Panel p;
  plot::Series s;
  s.x = {100.0, 200.0};
  s.y = {1.0, 2.0};
  p.series.push_back(s);
  f.panels.push_back(p);
  f.markers.push_back(plot::Marker{300.0, "m"});
  REQUIRE(f.finalize());
  CHECK(f.x_min == 100.0);
  CHECK(f.x_max == 300.0);
  f.extend_x = true;
  f.extend_x_min = 0.0;
  f.extend_x_max = 1000.0;
  REQUIRE(f.finalize());
  CHECK(f.x_min == 0.0);
  CHECK(f.x_max == 1000.0);
  plot::Figure empty;
  CHECK_FALSE(empty.finalize());
}
