// Min/max decimation: reduces a sorted series to at most two points per
// pixel column, which draws identically to the full series but keeps path
// geometry small for long recordings (a day of 1 Hz data is 86k points).
#pragma once
#include <cstddef>
#include <vector>

namespace plot {

struct Point {
  double x = 0.0;
  double y = 0.0;
};

constexpr int kMaxColumns = 16384;

// Emits the points of (x, y) inside [x0, x1] (plus one neighbour on each side
// for line continuity). When more than 2*columns points fall in range, each
// column keeps only its minimum and maximum, in x order.
void decimate_minmax(const std::vector<double>& x, const std::vector<double>& y, double x0,
                     double x1, int columns, std::vector<Point>& out);

// Index of the sample nearest to `xq` in ascending `x`; SIZE_MAX if empty.
size_t nearest_index(const std::vector<double>& x, double xq);

}  // namespace plot
