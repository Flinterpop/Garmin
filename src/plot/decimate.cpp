#include "plot/decimate.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace plot {

void decimate_minmax(const std::vector<double>& x, const std::vector<double>& y, double x0,
                     double x1, int columns, std::vector<Point>& out) {
  out.clear();
  G_ASSERT(x.size() == y.size());
  G_REQUIRE_VOID(!x.empty() && x1 > x0);
  columns = std::clamp(columns, 1, kMaxColumns);

  // Visible index range, widened by one sample each side.
  size_t i0 = static_cast<size_t>(std::lower_bound(x.begin(), x.end(), x0) - x.begin());
  size_t i1 = static_cast<size_t>(std::upper_bound(x.begin(), x.end(), x1) - x.begin());
  if (i0 > 0) --i0;
  if (i1 < x.size()) ++i1;
  const size_t n = i1 - i0;
  if (n == 0) return;

  if (n <= 2 * static_cast<size_t>(columns)) {
    out.reserve(n);
    for (size_t i = i0; i < i1; ++i) out.push_back(Point{x[i], y[i]});
    return;
  }

  out.reserve(2 * static_cast<size_t>(columns) + 4);
  const double scale = static_cast<double>(columns) / (x1 - x0);
  int cur_col = INT32_MIN;
  size_t imin = 0;
  size_t imax = 0;
  auto flush = [&]() {
    if (cur_col == INT32_MIN) return;
    const size_t a = std::min(imin, imax);
    const size_t b = std::max(imin, imax);
    out.push_back(Point{x[a], y[a]});
    if (b != a) out.push_back(Point{x[b], y[b]});
  };
  for (size_t i = i0; i < i1; ++i) {
    const double c = std::floor((x[i] - x0) * scale);
    const int col = static_cast<int>(std::clamp(c, -1.0, static_cast<double>(columns)));
    if (col != cur_col) {
      flush();
      cur_col = col;
      imin = i;
      imax = i;
      continue;
    }
    if (y[i] < y[imin]) imin = i;
    if (y[i] > y[imax]) imax = i;
  }
  flush();
}

size_t nearest_index(const std::vector<double>& x, double xq) {
  G_REQUIRE_RET(!x.empty(), SIZE_MAX);
  const auto it = std::lower_bound(x.begin(), x.end(), xq);
  if (it == x.begin()) return 0;
  if (it == x.end()) return x.size() - 1;
  const size_t hi = static_cast<size_t>(it - x.begin());
  const size_t lo = hi - 1;
  return (xq - x[lo]) <= (x[hi] - xq) ? lo : hi;
}

}  // namespace plot
