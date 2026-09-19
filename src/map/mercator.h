// Web Mercator (EPSG:3857) helpers in the "world unit" convention used by
// slippy-map tiles: the whole world maps to [0,1) x [0,1), tile (x, y) at
// zoom z covers world [x/2^z, (x+1)/2^z). Pure functions, unit-tested.
#pragma once
#include <cstdint>

namespace map {

constexpr int kTileSize = 256;
constexpr int kMinZoom = 1;
constexpr int kMaxZoom = 18;
constexpr double kMaxLatitude = 85.05112878;
constexpr double kEarthRadiusM = 6371008.8;       // mean radius, for distances
constexpr double kMercatorRadiusM = 6378137.0;    // WGS84 equatorial, tile scale

struct WorldPoint {
  double x = 0.0;  // 0..1, west -> east
  double y = 0.0;  // 0..1, north -> south
};

WorldPoint to_world(double lat_deg, double lon_deg);
void to_lat_lon(const WorldPoint& w, double& lat_deg, double& lon_deg);

// Number of tiles along one axis at `zoom`.
int32_t tiles_at(int zoom);

// Ground metres per world-unit-pixel at a latitude (for the scale bar).
double metres_per_pixel(double lat_deg, int zoom);

// Great-circle distance between two positions in metres.
double haversine_m(double lat1, double lon1, double lat2, double lon2);

// Largest zoom at which a world-unit box of size (dx, dy) fits in
// (width_px, height_px). Clamped to [kMinZoom, kMaxZoom].
int zoom_to_fit(double dx, double dy, double width_px, double height_px);

}  // namespace map
