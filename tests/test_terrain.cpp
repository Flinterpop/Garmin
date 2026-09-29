#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "map3d/terrain.h"

using Catch::Matchers::WithinAbs;
using map3d::kGrid;

namespace {

// A ~2 km track near 45 N.
std::vector<map::WorldPoint> sample_track() {
  return {map::to_world(45.00, -76.00), map::to_world(45.01, -75.99),
          map::to_world(45.005, -75.985)};
}

std::vector<float> flat(float h) { return std::vector<float>(static_cast<size_t>(kGrid) * kGrid, h); }

}  // namespace

TEST_CASE("terrarium decodes the AWS Terrain Tiles encoding", "[3d]") {
  CHECK(map3d::terrarium_height(128, 0, 0) == 0.0);           // sea level
  CHECK(map3d::terrarium_height(128, 100, 128) == 100.5);
  CHECK(map3d::terrarium_height(0, 0, 0) == -32768.0);
  CHECK_THAT(map3d::terrarium_height(131, 125, 0), WithinAbs(893.0, 1e-9));  // 3*256 + 125
}

TEST_CASE("region is square around the track, with margin and ground scale", "[3d]") {
  const auto r = map3d::region_for(sample_track(), 0.1);
  CHECK_THAT(r.max.x - r.min.x, WithinAbs(r.max.y - r.min.y, 1e-15));
  for (const auto& w : sample_track()) {
    CHECK(w.x > r.min.x);
    CHECK(w.x < r.max.x);
    CHECK(w.y > r.min.y);
    CHECK(w.y < r.max.y);
  }
  // Metres per world unit at the centre latitude: 2*pi*6378137*cos(lat).
  double lat = 0, lon = 0;
  map::to_lat_lon(map::WorldPoint{(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2}, lat, lon);
  CHECK_THAT(lat, WithinAbs(45.005, 0.001));
  CHECK_THAT(r.metres_per_world,
             WithinAbs(2 * 3.14159265358979 * 6378137.0 * std::cos(lat * 3.14159265358979 / 180), 1.0));
  // A single point still gets a usable area.
  CHECK(map3d::region_for({map::to_world(45.0, -76.0)}, 0.1).extent_m() >= 500.0);
}

TEST_CASE("zoom_for keeps the tile block within the limit", "[3d]") {
  const auto r = map3d::region_for(sample_track(), 0.1);
  for (int across : {1, 2, 4, 9}) {
    const int z = map3d::zoom_for(r, across, 18);
    CHECK(map3d::block_for(r, z).n <= across);
    if (z < 18) CHECK(map3d::block_for(r, z + 1).n > across);  // and it is the largest such zoom
  }
  CHECK(map3d::zoom_for(r, 9, 12) <= 12);
}

TEST_CASE("block_uv maps the block's corners to 0 and 1", "[3d]") {
  map3d::TileBlock b;
  b.z = 10;
  b.x0 = 300;
  b.y0 = 360;
  b.n = 3;
  const double n = 1024.0;
  float u = 0, v = 0;
  map3d::block_uv(b, map::WorldPoint{300 / n, 360 / n}, u, v);
  CHECK_THAT(u, WithinAbs(0.0, 1e-6));
  CHECK_THAT(v, WithinAbs(0.0, 1e-6));
  map3d::block_uv(b, map::WorldPoint{303 / n, 363 / n}, u, v);
  CHECK_THAT(u, WithinAbs(1.0, 1e-6));
  CHECK_THAT(v, WithinAbs(1.0, 1e-6));
}

TEST_CASE("local frame: x east, y up, z north, centred", "[3d]") {
  const auto r = map3d::region_for(sample_track(), 0.1);
  const map::WorldPoint c{(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
  const auto o = map3d::to_local(r, c, 100.0, 2.0);
  CHECK_THAT(o.x, WithinAbs(0.0, 1e-3));
  CHECK_THAT(o.z, WithinAbs(0.0, 1e-3));
  CHECK_THAT(o.y, WithinAbs(200.0, 1e-3));  // exaggerated
  const auto ne = map3d::to_local(r, r.min, 0.0, 1.0);  // min = north-west corner
  CHECK(ne.x < 0.0f);
  CHECK(ne.z > 0.0f);
  CHECK_THAT(ne.x * -2.0, WithinAbs(r.extent_m(), 1.0));
}

TEST_CASE("flat terrain mesh: counts, heights, normals up, uv in the block", "[3d]") {
  const auto r = map3d::region_for(sample_track(), 0.1);
  const auto tex = map3d::block_for(r, map3d::zoom_for(r, 9, 18));
  std::vector<map3d::TerrainVertex> v;
  std::vector<uint32_t> idx;
  map3d::build_terrain(r, flat(250.0f), 1.5, tex, v, idx);
  REQUIRE(v.size() == static_cast<size_t>(kGrid) * kGrid);
  CHECK(idx.size() == static_cast<size_t>(kGrid - 1) * (kGrid - 1) * 6);
  for (uint32_t i : idx) CHECK(i < v.size());
  for (const auto& p : {v.front(), v[v.size() / 2], v.back()}) {
    CHECK_THAT(p.pos.y, WithinAbs(375.0, 1e-3));
    CHECK_THAT(p.normal.y, WithinAbs(1.0, 1e-5));
    CHECK(p.u >= 0.0f);
    CHECK(p.u <= 1.0f);
    CHECK(p.v >= 0.0f);
    CHECK(p.v <= 1.0f);
  }
}

TEST_CASE("sloped terrain: normals lean downhill, height_at interpolates", "[3d]") {
  const auto r = map3d::region_for(sample_track(), 0.1);
  std::vector<float> h(static_cast<size_t>(kGrid) * kGrid);
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) h[static_cast<size_t>(j) * kGrid + i] = static_cast<float>(i);  // rises east
  }
  std::vector<map3d::TerrainVertex> v;
  std::vector<uint32_t> idx;
  map3d::build_terrain(r, h, 1.0, map3d::block_for(r, 10), v, idx);
  CHECK(v[static_cast<size_t>(64) * kGrid + 64].normal.x < 0.0f);  // faces west, downhill
  CHECK(v[static_cast<size_t>(64) * kGrid + 64].normal.y > 0.0f);
  const map::WorldPoint mid{(map3d::grid_point(r, 10, 5).x + map3d::grid_point(r, 11, 5).x) / 2,
                            map3d::grid_point(r, 10, 5).y};
  CHECK_THAT(map3d::height_at(r, h, mid), WithinAbs(10.5, 1e-6));
  CHECK_THAT(map3d::height_at(r, h, map::WorldPoint{0.0, 0.0}), WithinAbs(0.0, 1e-9));  // clamps
}

TEST_CASE("ribbon: two triangles per segment, skips repeats, carries colour", "[3d]") {
  std::vector<map3d::Vec3> c = {{0, 0, 0}, {10, 0, 0}, {10, 0, 0}, {10, 0, 10}};
  std::vector<uint32_t> rgb = {0xFF0000, 0x00FF00, 0x00FF00, 0x0000FF};
  std::vector<map3d::ColorVertex> out;
  map3d::build_ribbon(c, rgb, 2.0f, out);
  REQUIRE(out.size() == 12);  // the repeated point makes no segment
  CHECK_THAT(out[0].r, WithinAbs(1.0, 1e-6));
  CHECK_THAT(std::fabs(out[0].pos.z), WithinAbs(2.0, 1e-6));  // offset across an east-going segment
  CHECK_THAT(out[11].b, WithinAbs(1.0, 1e-6));
  map3d::build_pin({0, 0, 0}, 1.0f, 20.0f, 0x2CA02C, out);
  CHECK(out.size() == 24);
}

TEST_CASE("orbit camera: yaw 0 looks north from the south, clamps and pans", "[3d]") {
  map3d::Orbit o;
  o.distance = 100.0f;
  o.pitch_deg = 0.0001f;
  auto e = map3d::eye_of(o);
  CHECK_THAT(e.z, WithinAbs(-100.0, 1e-3));  // south of the target
  CHECK_THAT(e.x, WithinAbs(0.0, 1e-3));
  o.pitch_deg = 90.0f;
  CHECK_THAT(map3d::eye_of(o).y, WithinAbs(100.0, 1e-3));

  map3d::Orbit r;
  map3d::orbit_rotate(r, 0.0f, 10000.0f);
  CHECK(r.pitch_deg == map3d::kMaxPitch);
  map3d::orbit_rotate(r, 0.0f, -10000.0f);
  CHECK(r.pitch_deg == map3d::kMinPitch);
  map3d::orbit_rotate(r, -100.0f, 0.0f);  // wraps to 330
  CHECK_THAT(r.yaw_deg, WithinAbs(330.0, 1e-3));

  map3d::Orbit p;  // looking north: dragging right moves the target west
  map3d::orbit_pan(p, 50.0f, 0.0f, 800.0f);
  CHECK(p.target.x < 0.0f);
  CHECK_THAT(p.target.z, WithinAbs(0.0, 1e-3));

  map3d::Orbit z;  // one event is capped at 10 notches: 1000 * 0.85^10 ~ 197
  map3d::orbit_zoom(z, 100, 50.0f, 5000.0f);
  CHECK_THAT(z.distance, WithinAbs(196.87, 0.01));
  map3d::orbit_zoom(z, 10, 50.0f, 5000.0f);  // ~39, clamped to the floor
  CHECK(z.distance == 50.0f);
  map3d::orbit_zoom(z, -1000, 50.0f, 5000.0f);
  CHECK(z.distance > 50.0f);
}
