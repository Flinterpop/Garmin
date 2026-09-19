// Window-independent plot model. A Figure is a column of Panels sharing one
// x axis; each Panel holds Series drawn against a left and/or right y axis.
// x is seconds: Unix time (XMode::Time) or elapsed seconds (XMode::Elapsed).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plot {

struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

constexpr Color rgb(uint32_t hex, float alpha = 1.0f) {
  return Color{static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
               static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
               static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}

// A restrained palette; series pick from it by role, not by index.
namespace colors {
constexpr Color kHeartRate = rgb(0xD62728);
constexpr Color kStress = rgb(0xFF7F0E);
constexpr Color kBodyBattery = rgb(0x1F77B4);
constexpr Color kRespiration = rgb(0x17BECF);
constexpr Color kSpeed = rgb(0x2CA02C);
constexpr Color kAltitude = rgb(0x7F7F7F);
constexpr Color kCadence = rgb(0x9467BD);
constexpr Color kPower = rgb(0x8C564B);
constexpr Color kWeight = rgb(0x2CA02C);
constexpr Color kHrv = rgb(0x9467BD);
constexpr Color kSleep = rgb(0x1F77B4);
constexpr Color kSteps = rgb(0xBCBD22);
constexpr Color kDeep = rgb(0x3F51B5, 0.55f);
constexpr Color kLight = rgb(0x7986CB, 0.45f);
constexpr Color kRem = rgb(0x9C27B0, 0.45f);
constexpr Color kAwake = rgb(0xFFB74D, 0.55f);
constexpr Color kMarker = rgb(0x999999, 0.8f);
}  // namespace colors

enum class Style : uint8_t {
  kLine,    // polyline through (x, y)
  kStep,    // horizontal steps (value holds until next x)
  kPoints,  // discrete markers (plus a faint line)
  kBars,    // vertical bars from 0 to y, bar width = bar_width seconds
  kBand,    // filled rectangles from x[i] to x2[i], full panel height
};

enum class YAxisSide : uint8_t { kLeft, kRight };

struct Series {
  std::string name;
  std::string units;
  std::vector<double> x;   // ascending
  std::vector<double> y;   // same length as x
  std::vector<double> x2;  // kBand only: interval end per point
  Color color;
  Style style = Style::kLine;
  YAxisSide axis = YAxisSide::kLeft;
  float width = 1.5f;
  double bar_width = 0.0;  // kBars: seconds
  bool valid() const { return x.size() == y.size() && (style != Style::kBand || x2.size() == x.size()); }
};

struct AxisSpec {
  std::string label;
  bool fixed = false;  // false: auto-range from the visible data
  double min = 0.0;
  double max = 1.0;
  bool include_zero = false;
};

struct Panel {
  std::string title;
  std::vector<Series> series;
  AxisSpec left;
  AxisSpec right;
  float weight = 1.0f;  // relative height within the figure
};

enum class XMode : uint8_t { kTime, kElapsed };

// Vertical reference lines (laps, midnight, ...).
struct Marker {
  double x = 0.0;
  std::string label;
};

struct Figure {
  std::string title;
  XMode xmode = XMode::kTime;
  std::vector<Panel> panels;
  std::vector<Marker> markers;
  double x_min = 0.0;  // data extent, set by finalize()
  double x_max = 1.0;
  // Optional extent the figure must at least cover (e.g. a whole day even
  // when samples stop early). Applied by finalize() when extend_x is true.
  bool extend_x = false;
  double extend_x_min = 0.0;
  double extend_x_max = 0.0;

  // Computes x_min/x_max from every series (and markers). Returns false if
  // there is no data at all.
  bool finalize();
  size_t point_count() const;
};

constexpr size_t kMaxPanels = 8;
constexpr size_t kMaxSeriesPerPanel = 16;

}  // namespace plot
