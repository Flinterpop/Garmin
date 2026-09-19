#include "map/mercator.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace map {

namespace {
constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
double rad2deg(double r) { return r * 180.0 / kPi; }
}  // namespace

WorldPoint to_world(double lat_deg, double lon_deg) {
  const double lat = std::clamp(lat_deg, -kMaxLatitude, kMaxLatitude);
  const double lon = std::clamp(lon_deg, -180.0, 180.0);
  WorldPoint w;
  w.x = (lon + 180.0) / 360.0;
  const double s = std::sin(deg2rad(lat));
  w.y = 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
  // Rounding at the clamped poles can land a hair outside [0, 1].
  w.y = std::clamp(w.y, 0.0, 1.0);
  G_ASSERT(w.x >= 0.0 && w.x <= 1.0);
  return w;
}

void to_lat_lon(const WorldPoint& w, double& lat_deg, double& lon_deg) {
  lon_deg = w.x * 360.0 - 180.0;
  const double n = kPi - 2.0 * kPi * w.y;
  lat_deg = rad2deg(std::atan(0.5 * (std::exp(n) - std::exp(-n))));
}

int32_t tiles_at(int zoom) {
  G_ASSERT(zoom >= 0 && zoom <= 30);
  return static_cast<int32_t>(1) << zoom;
}

double metres_per_pixel(double lat_deg, int zoom) {
  G_ASSERT(zoom >= 0 && zoom <= 30);
  const double circumference = 2.0 * kPi * kMercatorRadiusM;
  return circumference * std::cos(deg2rad(lat_deg)) /
         (static_cast<double>(kTileSize) * static_cast<double>(tiles_at(zoom)));
}

double haversine_m(double lat1, double lon1, double lat2, double lon2) {
  const double p1 = deg2rad(lat1);
  const double p2 = deg2rad(lat2);
  const double dp = p2 - p1;
  const double dl = deg2rad(lon2 - lon1);
  const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                   std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
  return 2.0 * kEarthRadiusM * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
}

int zoom_to_fit(double dx, double dy, double width_px, double height_px) {
  G_ASSERT(width_px > 0.0 && height_px > 0.0);
  const double ex = std::max(dx, 1e-9);
  const double ey = std::max(dy, 1e-9);
  // World pixels at zoom z = 256 * 2^z; need 256 * 2^z * ex <= width_px.
  const double zx = std::log2(width_px / (kTileSize * ex));
  const double zy = std::log2(height_px / (kTileSize * ey));
  const int z = static_cast<int>(std::floor(std::min(zx, zy)));
  return std::clamp(z, kMinZoom, kMaxZoom);
}

}  // namespace map
