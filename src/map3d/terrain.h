// Geometry for the 3D track view: pure functions, no Direct3D, unit-tested.
//
// Coordinates: a local frame centred on the region, in metres, left-handed
// as Direct3D expects: x east, y up, z north. Over an activity (tens of km at
// most) a flat tangent plane is accurate to well under a pixel.
#pragma once
#include <cstdint>
#include <vector>

#include "map/mercator.h"

namespace map3d {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

constexpr int kGrid = 129;               // terrain vertices per side (128 cells)
constexpr int kMaxTilesAcross = 9;        // per layer; bounds downloads and texture size
constexpr size_t kMaxTrack = 200000;

// Terrarium PNG encoding (AWS Terrain Tiles): metres = R*256 + G + B/256 - 32768.
double terrarium_height(uint8_t r, uint8_t g, uint8_t b);

// Square area around the track in world units, with a margin, plus its scale.
struct Region {
  map::WorldPoint min;
  map::WorldPoint max;          // max.x - min.x == max.y - min.y
  double metres_per_world = 1;  // ground metres per world unit at the centre latitude
  double extent_m() const { return (max.x - min.x) * metres_per_world; }
};

Region region_for(const std::vector<map::WorldPoint>& track, double margin_frac);

// Largest zoom (<= max_zoom) at which the region spans at most `tiles_across`
// tiles once snapped to the tile grid.
int zoom_for(const Region& r, int tiles_across, int max_zoom);

// Tile block covering the region at a zoom.
struct TileBlock {
  int z = 0;
  int x0 = 0, y0 = 0;  // first tile
  int n = 0;           // tiles per side (block is n x n)
};
TileBlock block_for(const Region& r, int z);

// Texture coordinate of a world point inside a block (0..1 across the block).
void block_uv(const TileBlock& b, const map::WorldPoint& w, float& u, float& v);

// World point -> local metres (height already in metres, times exaggeration).
Vec3 to_local(const Region& r, const map::WorldPoint& w, double height_m, double exaggeration);

// World point of grid vertex (i, j), i east, j south.
map::WorldPoint grid_point(const Region& r, int i, int j);

struct TerrainVertex {
  Vec3 pos;
  Vec3 normal;
  float u = 0.0f;
  float v = 0.0f;
};

// Mesh over the region from kGrid x kGrid heights (row-major, j south).
// Texture coordinates address `tex`, the imagery block.
void build_terrain(const Region& r, const std::vector<float>& heights, double exaggeration,
                   const TileBlock& tex, std::vector<TerrainVertex>& verts,
                   std::vector<uint32_t>& indices);

// Bilinear height at a world point from the kGrid x kGrid heights.
double height_at(const Region& r, const std::vector<float>& heights, const map::WorldPoint& w);

struct ColorVertex {
  Vec3 pos;
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// Flat ribbon (triangle list) along `centre`, `half_width` metres each side,
// one colour per centre point. Degenerate (zero-length) segments are skipped.
void build_ribbon(const std::vector<Vec3>& centre, const std::vector<uint32_t>& rgb,
                  float half_width, std::vector<ColorVertex>& out);

// Vertical marker pin: two crossed quads from `base` up `height`.
void build_pin(const Vec3& base, float half_width, float height, uint32_t rgb,
               std::vector<ColorVertex>& out);

// Orbit camera around a target. Yaw 0 looks north; pitch is the angle above
// the horizon.
struct Orbit {
  Vec3 target;
  float yaw_deg = 0.0f;
  float pitch_deg = 35.0f;
  float distance = 1000.0f;
};

constexpr float kMinPitch = 5.0f;
constexpr float kMaxPitch = 89.0f;

Vec3 eye_of(const Orbit& o);
void orbit_rotate(Orbit& o, float dx_px, float dy_px);
void orbit_pan(Orbit& o, float dx_px, float dy_px, float viewport_h_px);
void orbit_zoom(Orbit& o, int notches, float min_dist, float max_dist);

}  // namespace map3d
