// Heart-rate colour ramp shared by the 2D map and the 3D view: blue ->
// cyan -> green -> yellow -> red over the track's 5th..95th percentile.
#pragma once
#include <cstddef>
#include <vector>

#include "plot/plot_types.h"

namespace map {

struct TrackPoint;

constexpr size_t kColorBuckets = 16;

struct HrRange {
  double lo = 0.0;
  double hi = 0.0;
  bool valid = false;  // at least 10 points with a heart rate
};

HrRange hr_range(const std::vector<TrackPoint>& points);

// Bucket 0..kColorBuckets-1, or kColorBuckets when the point has no HR.
size_t hr_bucket(double hr, const HrRange& r);

// Colour of a bucket; kColorBuckets gives the neutral no-HR grey.
plot::Color hr_bucket_color(size_t bucket);

}  // namespace map
