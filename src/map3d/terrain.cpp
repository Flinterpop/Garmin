#include "map3d/terrain.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace map3d {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinExtentM = 500.0;      // a stationary recording still gets some terrain
constexpr float kFovDeg = 45.0f;           // must match the renderer's projection
constexpr float kYawDegPerPx = 0.3f;
constexpr float kPitchDegPerPx = 0.25f;
constexpr float kZoomPerNotch = 0.85f;

Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(const Vec3& a, const Vec3& b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalized(const Vec3& v) {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len < 1e-12f) return Vec3{0.0f, 1.0f, 0.0f};
  return Vec3{v.x / len, v.y / len, v.z / len};
}

ColorVertex colored(const Vec3& p, uint32_t rgb) {
  ColorVertex v;
  v.pos = p;
  v.r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
  v.g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
  v.b = static_cast<float>(rgb & 0xFF) / 255.0f;
  return v;
}

// Two triangles for the quad a-b-c-d (a, b one edge; c, d the opposite edge).
void push_quad(std::vector<ColorVertex>& out, const ColorVertex& a, const ColorVertex& b,
               const ColorVertex& c, const ColorVertex& d) {
  out.push_back(a);
  out.push_back(b);
  out.push_back(c);
  out.push_back(b);
  out.push_back(d);
  out.push_back(c);
}

double rad(double deg) { return deg * kPi / 180.0; }

}  // namespace

double terrarium_height(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<double>(r) * 256.0 + static_cast<double>(g) + static_cast<double>(b) / 256.0 -
         32768.0;
}

Region region_for(const std::vector<map::WorldPoint>& track, double margin_frac) {
  G_ASSERT(!track.empty());
  G_ASSERT(margin_frac >= 0.0 && margin_frac < 2.0);
  map::WorldPoint lo = track.front();
  map::WorldPoint hi = track.front();
  for (size_t i = 0; i < track.size() && i < kMaxTrack; ++i) {
    lo.x = std::min(lo.x, track[i].x);
    lo.y = std::min(lo.y, track[i].y);
    hi.x = std::max(hi.x, track[i].x);
    hi.y = std::max(hi.y, track[i].y);
  }
  const map::WorldPoint c{(lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0};
  double lat = 0.0;
  double lon = 0.0;
  map::to_lat_lon(c, lat, lon);
  Region r;
  r.metres_per_world = 2.0 * kPi * map::kMercatorRadiusM * std::cos(rad(lat));
  const double min_span = kMinExtentM / r.metres_per_world;
  const double span = std::max(std::max(hi.x - lo.x, hi.y - lo.y), min_span) * (1.0 + 2.0 * margin_frac);
  r.min = map::WorldPoint{c.x - span / 2.0, c.y - span / 2.0};
  r.max = map::WorldPoint{c.x + span / 2.0, c.y + span / 2.0};
  G_ASSERT(r.max.x > r.min.x && r.metres_per_world > 0.0);
  return r;
}

TileBlock block_for(const Region& r, int z) {
  G_ASSERT(z >= 0 && z <= 30);
  const double n = static_cast<double>(map::tiles_at(z));
  TileBlock b;
  b.z = z;
  b.x0 = std::max(0, static_cast<int>(std::floor(r.min.x * n)));
  b.y0 = std::max(0, static_cast<int>(std::floor(r.min.y * n)));
  const int x1 = static_cast<int>(std::floor(r.max.x * n - 1e-9));
  const int y1 = static_cast<int>(std::floor(r.max.y * n - 1e-9));
  b.n = std::max(x1 - b.x0, y1 - b.y0) + 1;
  G_ASSERT(b.n >= 1);
  return b;
}

int zoom_for(const Region& r, int tiles_across, int max_zoom) {
  G_ASSERT(tiles_across >= 1);
  const int top = std::clamp(max_zoom, map::kMinZoom, map::kMaxZoom);
  for (int z = top; z > map::kMinZoom; --z) {
    if (block_for(r, z).n <= tiles_across) return z;
  }
  return map::kMinZoom;
}

void block_uv(const TileBlock& b, const map::WorldPoint& w, float& u, float& v) {
  G_ASSERT(b.n >= 1);
  const double n = static_cast<double>(map::tiles_at(b.z));
  u = static_cast<float>((w.x * n - b.x0) / b.n);
  v = static_cast<float>((w.y * n - b.y0) / b.n);
}

Vec3 to_local(const Region& r, const map::WorldPoint& w, double height_m, double exaggeration) {
  const double cx = (r.min.x + r.max.x) / 2.0;
  const double cy = (r.min.y + r.max.y) / 2.0;
  return Vec3{static_cast<float>((w.x - cx) * r.metres_per_world),
              static_cast<float>(height_m * exaggeration),
              static_cast<float>(-(w.y - cy) * r.metres_per_world)};  // world y grows south
}

map::WorldPoint grid_point(const Region& r, int i, int j) {
  G_ASSERT(i >= 0 && i < kGrid && j >= 0 && j < kGrid);
  const double f = 1.0 / static_cast<double>(kGrid - 1);
  return map::WorldPoint{r.min.x + (r.max.x - r.min.x) * i * f, r.min.y + (r.max.y - r.min.y) * j * f};
}

void build_terrain(const Region& r, const std::vector<float>& heights, double exaggeration,
                   const TileBlock& tex, std::vector<TerrainVertex>& verts,
                   std::vector<uint32_t>& indices) {
  G_ASSERT(heights.size() == static_cast<size_t>(kGrid) * kGrid);
  verts.assign(static_cast<size_t>(kGrid) * kGrid, TerrainVertex{});
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) {
      const map::WorldPoint w = grid_point(r, i, j);
      TerrainVertex& v = verts[static_cast<size_t>(j) * kGrid + i];
      v.pos = to_local(r, w, heights[static_cast<size_t>(j) * kGrid + i], exaggeration);
      block_uv(tex, w, v.u, v.v);
    }
  }
  auto at = [&](int i, int j) -> const Vec3& {
    return verts[static_cast<size_t>(std::clamp(j, 0, kGrid - 1)) * kGrid + std::clamp(i, 0, kGrid - 1)].pos;
  };
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) {
      const Vec3 east = sub(at(i + 1, j), at(i - 1, j));
      const Vec3 north = sub(at(i, j - 1), at(i, j + 1));  // j grows south
      verts[static_cast<size_t>(j) * kGrid + i].normal = normalized(cross(north, east));
    }
  }
  indices.clear();
  indices.reserve(static_cast<size_t>(kGrid - 1) * (kGrid - 1) * 6);
  for (int j = 0; j + 1 < kGrid; ++j) {
    for (int i = 0; i + 1 < kGrid; ++i) {
      const uint32_t a = static_cast<uint32_t>(j * kGrid + i);
      const uint32_t b = a + 1;
      const uint32_t c = a + kGrid;
      const uint32_t d = c + 1;
      indices.insert(indices.end(), {a, b, c, b, d, c});
    }
  }
  G_ASSERT(indices.size() == static_cast<size_t>(kGrid - 1) * (kGrid - 1) * 6);
}

double height_at(const Region& r, const std::vector<float>& heights, const map::WorldPoint& w) {
  G_ASSERT(heights.size() == static_cast<size_t>(kGrid) * kGrid);
  const double span = r.max.x - r.min.x;
  G_ASSERT(span > 0.0);
  const double fx = std::clamp((w.x - r.min.x) / span, 0.0, 1.0) * (kGrid - 1);
  const double fy = std::clamp((w.y - r.min.y) / span, 0.0, 1.0) * (kGrid - 1);
  const int i = std::min(static_cast<int>(fx), kGrid - 2);
  const int j = std::min(static_cast<int>(fy), kGrid - 2);
  const double tx = fx - i;
  const double ty = fy - j;
  auto h = [&](int ii, int jj) { return static_cast<double>(heights[static_cast<size_t>(jj) * kGrid + ii]); };
  const double top = h(i, j) * (1.0 - tx) + h(i + 1, j) * tx;
  const double bottom = h(i, j + 1) * (1.0 - tx) + h(i + 1, j + 1) * tx;
  return top * (1.0 - ty) + bottom * ty;
}

void build_ribbon(const std::vector<Vec3>& centre, const std::vector<uint32_t>& rgb,
                  float half_width, std::vector<ColorVertex>& out) {
  G_ASSERT(centre.size() == rgb.size());
  G_ASSERT(half_width > 0.0f);
  out.clear();
  const size_t n = std::min(centre.size(), kMaxTrack);
  out.reserve(n * 6);
  for (size_t k = 0; k + 1 < n; ++k) {
    const Vec3& a = centre[k];
    const Vec3& b = centre[k + 1];
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-3f) continue;
    const float px = -dz / len * half_width;  // horizontal perpendicular
    const float pz = dx / len * half_width;
    push_quad(out, colored(Vec3{a.x + px, a.y, a.z + pz}, rgb[k]), colored(Vec3{a.x - px, a.y, a.z - pz}, rgb[k]),
              colored(Vec3{b.x + px, b.y, b.z + pz}, rgb[k + 1]),
              colored(Vec3{b.x - px, b.y, b.z - pz}, rgb[k + 1]));
  }
  G_ASSERT(out.size() % 3 == 0);
}

void build_pin(const Vec3& base, float half_width, float height, uint32_t rgb,
               std::vector<ColorVertex>& out) {
  G_ASSERT(half_width > 0.0f && height > 0.0f);
  const float top = base.y + height;
  push_quad(out, colored(Vec3{base.x - half_width, base.y, base.z}, rgb),
            colored(Vec3{base.x + half_width, base.y, base.z}, rgb),
            colored(Vec3{base.x - half_width, top, base.z}, rgb),
            colored(Vec3{base.x + half_width, top, base.z}, rgb));
  push_quad(out, colored(Vec3{base.x, base.y, base.z - half_width}, rgb),
            colored(Vec3{base.x, base.y, base.z + half_width}, rgb),
            colored(Vec3{base.x, top, base.z - half_width}, rgb),
            colored(Vec3{base.x, top, base.z + half_width}, rgb));
}

Vec3 eye_of(const Orbit& o) {
  G_ASSERT(o.distance > 0.0f);
  const double p = rad(o.pitch_deg);
  const double y = rad(o.yaw_deg);
  const double d = o.distance;
  return Vec3{o.target.x - static_cast<float>(d * std::cos(p) * std::sin(y)),
              o.target.y + static_cast<float>(d * std::sin(p)),
              o.target.z - static_cast<float>(d * std::cos(p) * std::cos(y))};
}

void orbit_rotate(Orbit& o, float dx_px, float dy_px) {
  o.yaw_deg = std::fmod(o.yaw_deg + dx_px * kYawDegPerPx, 360.0f);
  if (o.yaw_deg < 0.0f) o.yaw_deg += 360.0f;
  o.pitch_deg = std::clamp(o.pitch_deg + dy_px * kPitchDegPerPx, kMinPitch, kMaxPitch);
  G_ASSERT(o.yaw_deg >= 0.0f && o.yaw_deg < 360.0f);
}

void orbit_pan(Orbit& o, float dx_px, float dy_px, float viewport_h_px) {
  G_REQUIRE_VOID(viewport_h_px > 1.0f);
  // Ground metres per pixel at the target's distance.
  const float k = 2.0f * o.distance * std::tan(static_cast<float>(rad(kFovDeg / 2.0f))) / viewport_h_px;
  const float s = static_cast<float>(std::sin(rad(o.yaw_deg)));
  const float c = static_cast<float>(std::cos(rad(o.yaw_deg)));
  // right = (c, 0, -s), forward = (s, 0, c); the ground follows the mouse.
  o.target.x += (-dx_px * c + dy_px * s) * k;
  o.target.z += (dx_px * s + dy_px * c) * k;
}

void orbit_zoom(Orbit& o, int notches, float min_dist, float max_dist) {
  G_ASSERT(min_dist > 0.0f && max_dist > min_dist);
  const int steps = std::clamp(notches, -10, 10);
  o.distance *= std::pow(kZoomPerNotch, static_cast<float>(steps));
  o.distance = std::clamp(o.distance, min_dist, max_dist);
}

}  // namespace map3d
