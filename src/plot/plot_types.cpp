#include "plot/plot_types.h"

#include <algorithm>

#include "util/assert.h"

namespace plot {

bool Figure::finalize() {
  G_ASSERT(panels.size() <= kMaxPanels);
  bool any = false;
  double lo = 0.0;
  double hi = 0.0;
  auto grow = [&](double v) {
    if (!any) {
      lo = v;
      hi = v;
      any = true;
    } else {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  };
  for (const Panel& p : panels) {
    G_ASSERT(p.series.size() <= kMaxSeriesPerPanel);
    for (const Series& s : p.series) {
      G_ASSERT(s.valid());
      if (s.x.empty()) continue;
      grow(s.x.front());
      grow(s.x.back());
      if (s.style == Style::kBand) grow(s.x2.back());
      if (s.style == Style::kBars) grow(s.bar_end(s.x.size() - 1));
    }
  }
  for (const Marker& m : markers) grow(m.x);
  if (extend_x) {
    grow(extend_x_min);
    grow(extend_x_max);
  }
  if (!any) return false;
  if (hi <= lo) hi = lo + 1.0;
  x_min = lo;
  x_max = hi;
  return true;
}

size_t Figure::point_count() const {
  size_t n = 0;
  for (const Panel& p : panels) {
    for (const Series& s : p.series) n += s.x.size();
  }
  return n;
}

}  // namespace plot
