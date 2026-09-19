// Month-at-a-glance grid drawn with Direct2D: one cell per day showing
// steps, resting HR, Body Battery range, sleep score and the day's
// activities. Same hosting contract as PlotWidget (rect, hover, render).
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

#include "plot/plot_types.h"

namespace plot {

struct DayCell {
  int day = 0;  // 1..31; 0 = padding cell
  std::optional<int64_t> steps;
  std::optional<int64_t> resting_hr;
  std::optional<int64_t> sleep_score;
  std::optional<double> sleep_hours;
  std::optional<int64_t> bb_high;
  std::optional<int64_t> bb_low;
  std::optional<int64_t> avg_stress;
  std::optional<double> weight_kg;
  std::vector<std::string> activities;  // "ice hockey 90 min"
  bool has_data() const {
    return steps || resting_hr || sleep_score || bb_high || !activities.empty();
  }
};

struct MonthData {
  int year = 0;
  int month = 0;  // 1..12
  std::string title;           // "September 2026"
  std::vector<DayCell> days;   // exactly days-in-month entries, day 1 first
  int first_weekday = 0;       // 0 = Monday ... 6 = Sunday
  int64_t step_goal = 10000;
};

class CalendarWidget {
 public:
  CalendarWidget() = default;
  CalendarWidget(const CalendarWidget&) = delete;
  CalendarWidget& operator=(const CalendarWidget&) = delete;

  bool init(ID2D1Factory* d2d, IDWriteFactory* dwrite);
  void set_dpi_scale(float scale);
  void set_rect(const D2D1_RECT_F& rect) { rect_ = rect; }
  void set_month(MonthData m);
  bool has_month() const { return !month_.days.empty(); }
  void set_hover(float px, float py, bool inside);
  void render(ID2D1RenderTarget* rt);

 private:
  struct CellRect {
    int day = 0;
    D2D1_RECT_F r{};
  };
  void compute_cells();
  int cell_at(float px, float py) const;  // day number or 0
  void draw_cell(ID2D1RenderTarget* rt, const DayCell& d, const D2D1_RECT_F& r, bool weekend,
                 bool hovered);
  void draw_tooltip(ID2D1RenderTarget* rt, const DayCell& d);
  void text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& box,
            DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign, const Color& c,
            bool bold = false);
  float measure(const std::wstring& s, bool bold);
  float s(float css) const { return css * scale_; }
  void ensure_brush(ID2D1RenderTarget* rt);

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_bold_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_small_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  ID2D1RenderTarget* brush_rt_ = nullptr;

  MonthData month_;
  D2D1_RECT_F rect_{};
  float scale_ = 1.0f;
  std::vector<CellRect> cells_;
  bool hover_ = false;
  float hover_px_ = 0.0f;
  float hover_py_ = 0.0f;
};

}  // namespace plot
