#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "map/map_widget.h"

namespace {

std::vector<map::TrackPoint> track_at(const std::vector<double>& times) {
  std::vector<map::TrackPoint> pts;
  for (const double t : times) {
    map::TrackPoint p;
    p.elapsed_s = t;
    pts.push_back(p);
  }
  return pts;
}

}  // namespace

TEST_CASE("index_at_time picks the nearest point in time", "[map]") {
  const auto pts = track_at({0.0, 1.0, 2.0, 10.0, 11.0});
  CHECK(map::index_at_time(pts, 0.0) == 0);
  CHECK(map::index_at_time(pts, 1.4) == 1);
  CHECK(map::index_at_time(pts, 1.6) == 2);
  CHECK(map::index_at_time(pts, 5.9) == 2);   // inside a recording gap: nearer to 2 s
  CHECK(map::index_at_time(pts, 6.1) == 3);   // nearer to 10 s
  CHECK(map::index_at_time(pts, 10.0) == 3);  // exact hit
}

TEST_CASE("index_at_time clamps outside the track", "[map]") {
  const auto pts = track_at({5.0, 6.0, 7.0});
  CHECK(map::index_at_time(pts, -100.0) == 0);
  CHECK(map::index_at_time(pts, 1e9) == 2);
  CHECK(map::index_at_time(track_at({42.0}), 0.0) == 0);
  CHECK(map::index_at_time({}, 3.0) == SIZE_MAX);
}
