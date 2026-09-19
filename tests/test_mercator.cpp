#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/mercator.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("world coordinates: origin, equator and round trip") {
  const map::WorldPoint o = map::to_world(0.0, 0.0);
  CHECK_THAT(o.x, WithinAbs(0.5, 1e-12));
  CHECK_THAT(o.y, WithinAbs(0.5, 1e-12));
  const map::WorldPoint w = map::to_world(29.3486, -100.7873);
  double lat = 0.0;
  double lon = 0.0;
  map::to_lat_lon(w, lat, lon);
  CHECK_THAT(lat, WithinAbs(29.3486, 1e-9));
  CHECK_THAT(lon, WithinAbs(-100.7873, 1e-9));
  // North is smaller y.
  CHECK(map::to_world(60.0, 0.0).y < map::to_world(0.0, 0.0).y);
  // Latitude beyond the projection limit is clamped, not NaN.
  const map::WorldPoint pole = map::to_world(89.9, 0.0);
  CHECK(pole.y >= 0.0);
}

TEST_CASE("tile arithmetic") {
  CHECK(map::tiles_at(0) == 1);
  CHECK(map::tiles_at(10) == 1024);
  // Zoom 0 at the equator: one 256 px tile spans the whole circumference.
  CHECK_THAT(map::metres_per_pixel(0.0, 0), WithinRel(156543.03, 1e-3));
  // A half-world box in 512 px: zoom 2 makes the world 1024 px, box 512 px.
  CHECK(map::zoom_to_fit(0.5, 0.5, 512.0, 512.0) == 2);
  CHECK(map::zoom_to_fit(0.5, 0.5, 511.0, 511.0) == 1);
  // A tiny box goes to the max zoom.
  CHECK(map::zoom_to_fit(1e-7, 1e-7, 1000.0, 1000.0) == map::kMaxZoom);
}

TEST_CASE("haversine") {
  // One degree of latitude is about 111.2 km.
  CHECK_THAT(map::haversine_m(0.0, 0.0, 1.0, 0.0), WithinRel(111195.0, 1e-3));
  CHECK(map::haversine_m(45.0, -75.0, 45.0, -75.0) == 0.0);
}
