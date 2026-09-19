#include "plot/ticks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "util/assert.h"

namespace plot {

namespace {

constexpr double kMinute = 60.0;
constexpr double kHour = 3600.0;
constexpr double kDay = 86400.0;
constexpr double kMinTickSpacingPx = 70.0;
constexpr int kMaxLabel = 32;

const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool to_local(double unix_seconds, std::tm& out) {
  const time_t t = static_cast<time_t>(std::floor(unix_seconds));
  return localtime_s(&out, &t) == 0;
}

double from_local(std::tm tm) {
  tm.tm_isdst = -1;
  const time_t t = mktime(&tm);
  return t == static_cast<time_t>(-1) ? 0.0 : static_cast<double>(t);
}

// Local midnight at or before `unix_seconds`.
double local_midnight(double unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_local(unix_seconds, tm), unix_seconds);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  return from_local(tm);
}

// First of the local month at or before `unix_seconds`, shifted by `months`.
double local_month_start(double unix_seconds, int months) {
  std::tm tm{};
  G_REQUIRE_RET(to_local(unix_seconds, tm), unix_seconds);
  tm.tm_mday = 1;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  int total = tm.tm_year * 12 + tm.tm_mon + months;
  tm.tm_year = total / 12;
  tm.tm_mon = total % 12;
  return from_local(tm);
}

std::string fmt(const char* f, int a, int b = 0, int c = 0) {
  char buf[kMaxLabel] = {};
  std::snprintf(buf, sizeof(buf), f, a, b, c);
  return buf;
}

std::string label_hhmm(const std::tm& tm) { return fmt("%02d:%02d", tm.tm_hour, tm.tm_min); }
std::string label_day(const std::tm& tm) {
  char buf[kMaxLabel] = {};
  std::snprintf(buf, sizeof(buf), "%d %s", tm.tm_mday, kMonthNames[tm.tm_mon]);
  return buf;
}
std::string label_month(const std::tm& tm) {
  char buf[kMaxLabel] = {};
  std::snprintf(buf, sizeof(buf), "%s %d", kMonthNames[tm.tm_mon], tm.tm_year + 1900);
  return buf;
}

void push(std::vector<Tick>& out, double v, std::string label, bool emph) {
  if (out.size() >= kMaxTicks) return;
  out.push_back(Tick{v, std::move(label), emph});
}

// Sub-day intervals: align to multiples of `step` in local time.
void subday_ticks(double x0, double x1, double step, std::vector<Tick>& out) {
  const double base = local_midnight(x0);
  double t = base + std::floor((x0 - base) / step) * step;
  for (size_t i = 0; i < kMaxTicks && t <= x1; ++i) {
    if (t >= x0) {
      std::tm lt{};
      if (to_local(t, lt)) {
        const bool midnight = lt.tm_hour == 0 && lt.tm_min == 0;
        push(out, t, midnight ? label_day(lt) : label_hhmm(lt), midnight);
      }
    }
    t += step;
  }
}

void day_ticks(double x0, double x1, int days, std::vector<Tick>& out) {
  double t = local_midnight(x0);
  // Align multi-day steps to a fixed grid of days since the epoch.
  if (days > 1) {
    const long long day_index = static_cast<long long>(std::floor(t / kDay));
    const long long aligned = (day_index / days) * days;
    t = local_midnight(static_cast<double>(aligned) * kDay + kDay / 2);
  }
  for (size_t i = 0; i < kMaxTicks && t <= x1; ++i) {
    std::tm lt{};
    if (t >= x0 && to_local(t, lt)) {
      const bool first_of_month = lt.tm_mday == 1;
      push(out, t, first_of_month ? label_month(lt) : label_day(lt), first_of_month);
    }
    t = local_midnight(t + static_cast<double>(days) * kDay + kHour * 2);
  }
}

void month_ticks(double x0, double x1, int months, std::vector<Tick>& out) {
  double t = local_month_start(x0, 0);
  if (months > 1) {
    std::tm tm{};
    if (to_local(t, tm)) {
      const int aligned = (tm.tm_mon / months) * months;
      t = local_month_start(t, aligned - tm.tm_mon);
    }
  }
  for (size_t i = 0; i < kMaxTicks && t <= x1; ++i) {
    std::tm lt{};
    if (t >= x0 && to_local(t, lt)) {
      const bool january = lt.tm_mon == 0;
      push(out, t, months >= 12 || january ? fmt("%d", lt.tm_year + 1900) : label_month(lt),
           january);
    }
    t = local_month_start(t + kDay, months);
  }
}

}  // namespace

double nice_step(double range, int target) {
  G_ASSERT(target > 0);
  if (!(range > 0.0)) return 1.0;
  const double raw = range / static_cast<double>(target);
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double norm = raw / mag;
  double step = 10.0;
  if (norm <= 1.0) {
    step = 1.0;
  } else if (norm <= 2.0) {
    step = 2.0;
  } else if (norm <= 5.0) {
    step = 5.0;
  }
  return step * mag;
}

void nice_range(double& lo, double& hi, int target) {
  if (hi < lo) std::swap(lo, hi);
  if (hi - lo < 1e-9) {
    const double pad = std::fabs(lo) > 1e-9 ? std::fabs(lo) * 0.1 : 1.0;
    lo -= pad;
    hi += pad;
  }
  // Round to a finer grid than the tick step so the data fills the axis
  // instead of floating in a 0..200 box when it spans 47..173.
  const double step = nice_step(hi - lo, target * 2);
  lo = std::floor(lo / step) * step;
  hi = std::ceil(hi / step) * step;
  G_ASSERT(hi > lo);
}

void linear_ticks(double lo, double hi, int target, std::vector<Tick>& out) {
  out.clear();
  G_REQUIRE_VOID(hi > lo);
  const double step = nice_step(hi - lo, target);
  const int decimals = static_cast<int>(std::max(0.0, -std::floor(std::log10(step))));
  const double first = std::ceil(lo / step - 1e-9) * step;
  for (size_t i = 0; i < kMaxTicks; ++i) {
    const double v = first + static_cast<double>(i) * step;
    if (v > hi + step * 1e-6) break;
    char buf[kMaxLabel] = {};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::fabs(v) < step * 1e-6 ? 0.0 : v);
    push(out, v, buf, false);
  }
}

void time_ticks(double x0, double x1, double px_width, std::vector<Tick>& out) {
  out.clear();
  G_REQUIRE_VOID(x1 > x0 && px_width > 0.0);
  const double span = x1 - x0;
  const double min_span_per_tick = span * kMinTickSpacingPx / px_width;

  static const double kSubDay[] = {kMinute,     2 * kMinute, 5 * kMinute,  10 * kMinute,
                                   15 * kMinute, 30 * kMinute, kHour,        2 * kHour,
                                   3 * kHour,   6 * kHour,   12 * kHour};
  for (const double step : kSubDay) {
    if (step >= min_span_per_tick) {
      subday_ticks(x0, x1, step, out);
      return;
    }
  }
  static const int kDays[] = {1, 2, 7, 14};
  for (const int d : kDays) {
    if (d * kDay >= min_span_per_tick) {
      day_ticks(x0, x1, d, out);
      return;
    }
  }
  static const int kMonths[] = {1, 3, 6, 12, 24, 60};
  for (const int m : kMonths) {
    if (m * 30.4 * kDay >= min_span_per_tick) {
      month_ticks(x0, x1, m, out);
      return;
    }
  }
  month_ticks(x0, x1, 120, out);
}

void elapsed_ticks(double x0, double x1, double px_width, std::vector<Tick>& out) {
  out.clear();
  G_REQUIRE_VOID(x1 > x0 && px_width > 0.0);
  const double min_span_per_tick = (x1 - x0) * kMinTickSpacingPx / px_width;
  static const double kSteps[] = {1,    2,    5,    10,   15,   30,    60,    120,   300,
                                  600,  900,  1800, 3600, 7200, 10800, 21600, 43200, 86400};
  double step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
  for (const double s : kSteps) {
    if (s >= min_span_per_tick) {
      step = s;
      break;
    }
  }
  const double first = std::ceil(x0 / step) * step;
  for (size_t i = 0; i < kMaxTicks; ++i) {
    const double v = first + static_cast<double>(i) * step;
    if (v > x1) break;
    const long long total = static_cast<long long>(std::llround(v));
    std::string label;
    if (step >= kMinute) {
      // Minute-level steps read as H:MM (0:10, 1:20) like a sports watch.
      label = fmt("%d:%02d", static_cast<int>(total / 3600),
                  static_cast<int>((total % 3600) / 60));
    } else {
      label = format_elapsed(v);
    }
    push(out, v, label, false);
  }
}

std::string format_clock_local(double unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_local(unix_seconds, tm), std::string());
  return fmt("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string format_date_local(double unix_seconds) {
  std::tm tm{};
  G_REQUIRE_RET(to_local(unix_seconds, tm), std::string());
  char buf[kMaxLabel] = {};
  std::snprintf(buf, sizeof(buf), "%d %s %d", tm.tm_mday, kMonthNames[tm.tm_mon],
                tm.tm_year + 1900);
  return buf;
}

std::string format_elapsed(double seconds) {
  const long long total = static_cast<long long>(std::llround(std::fabs(seconds)));
  const int h = static_cast<int>(total / 3600);
  const int m = static_cast<int>((total % 3600) / 60);
  const int s = static_cast<int>(total % 60);
  const char* sign = seconds < 0 ? "-" : "";
  if (h > 0) return std::string(sign) + fmt("%d:%02d:%02d", h, m, s);
  return std::string(sign) + fmt("%02d:%02d", m, s);
}

std::string format_value(double v, const std::string& units) {
  char buf[64] = {};
  const double av = std::fabs(v);
  if (av >= 100.0 || std::fabs(v - std::round(v)) < 1e-9) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  } else if (av >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%.1f", v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f", v);
  }
  std::string s = buf;
  if (!units.empty()) s += " " + units;
  return s;
}

}  // namespace plot
