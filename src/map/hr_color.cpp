#include "map/hr_color.h"

#include <algorithm>

#include "map/map_widget.h"
#include "util/assert.h"

namespace map {

namespace {

constexpr plot::Color kNoHr = plot::rgb(0x555555);
constexpr plot::Color kStops[] = {plot::rgb(0x3B82F6), plot::rgb(0x06B6D4), plot::rgb(0x22C55E),
                                  plot::rgb(0xEAB308), plot::rgb(0xDC2626)};

plot::Color lerp(const plot::Color& a, const plot::Color& b, float t) {
  return plot::Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f};
}

}  // namespace

HrRange hr_range(const std::vector<TrackPoint>& points) {
  HrRange r;
  std::vector<double> hrs;
  for (size_t i = 0; i < points.size() && i < kMaxTrackPoints; ++i) {
    if (points[i].hr > 0.0) hrs.push_back(points[i].hr);
  }
  if (hrs.size() < 10) return r;
  std::sort(hrs.begin(), hrs.end());
  r.lo = hrs[hrs.size() / 20];                  // 5th percentile
  r.hi = hrs[hrs.size() - 1 - hrs.size() / 20];  // 95th
  if (r.hi - r.lo < 10.0) r.hi = r.lo + 10.0;
  r.valid = true;
  G_ASSERT(r.hi > r.lo);
  return r;
}

size_t hr_bucket(double hr, const HrRange& r) {
  if (!r.valid || hr <= 0.0) return kColorBuckets;  // sentinel: no HR
  G_ASSERT(r.hi > r.lo);
  const double f = std::clamp((hr - r.lo) / (r.hi - r.lo), 0.0, 0.999);
  return static_cast<size_t>(f * static_cast<double>(kColorBuckets));
}

plot::Color hr_bucket_color(size_t b) {
  if (b >= kColorBuckets) return kNoHr;
  constexpr size_t n = sizeof(kStops) / sizeof(kStops[0]);
  const float pos = (static_cast<float>(b) + 0.5f) / static_cast<float>(kColorBuckets) *
                    static_cast<float>(n - 1);
  const size_t i = std::min(static_cast<size_t>(pos), n - 2);
  return lerp(kStops[i], kStops[i + 1], pos - static_cast<float>(i));
}

}  // namespace map
