// Direct2D renderer + interaction state for a Figure. Owns no window: the
// host gives it a rectangle (device pixels), forwards mouse events, and
// calls render() with its render target inside BeginDraw/EndDraw.
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "plot/decimate.h"
#include "plot/plot_types.h"
#include "plot/ticks.h"

namespace plot {

struct PanelLayout {
  D2D1_RECT_F plot{};   // data area
  double left_lo = 0.0, left_hi = 1.0;
  double right_lo = 0.0, right_hi = 1.0;
  bool has_left = false;
  bool has_right = false;
};

struct HoverEntry {
  std::string text;
  Color color;
};

class PlotWidget {
 public:
  PlotWidget() = default;
  PlotWidget(const PlotWidget&) = delete;
  PlotWidget& operator=(const PlotWidget&) = delete;

  bool init(ID2D1Factory* d2d, IDWriteFactory* dwrite);
  void set_dpi_scale(float scale);

  // Replaces the figure and resets the view to its full x extent.
  void set_figure(Figure fig);
  const Figure& figure() const { return fig_; }
  bool has_figure() const { return !fig_.panels.empty(); }

  void set_rect(const D2D1_RECT_F& rect) { rect_ = rect; }
  const D2D1_RECT_F& rect() const { return rect_; }

  // View manipulation. Pixels are in the same space as rect().
  void zoom_at(float px, double factor);
  void pan_pixels(float dx);
  void fit_x();
  void set_x_range(double x0, double x1);
  double x0() const { return x0_; }
  double x1() const { return x1_; }

  void set_hover(float px, float py, bool inside);

  void render(ID2D1RenderTarget* rt);

  // Exposed for tests / hosts: current pixel <-> x mapping.
  double px_to_x(float px) const;
  float x_to_px(double x) const;

 private:
  void compute_layout();
  void auto_range(const Panel& panel, PanelLayout& L) const;
  void draw_panel(ID2D1RenderTarget* rt, const Panel& panel, const PanelLayout& L,
                  bool bottom);
  void draw_series(ID2D1RenderTarget* rt, const Series& s, const PanelLayout& L);
  void draw_x_axis(ID2D1RenderTarget* rt, const PanelLayout& L);
  void draw_markers(ID2D1RenderTarget* rt);
  void draw_hover(ID2D1RenderTarget* rt);
  void collect_hover(double xq, std::vector<HoverEntry>& out) const;
  void text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& box,
            DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign, const Color& c,
            bool bold = false);
  float measure_text_width(const std::wstring& s, bool bold);
  float y_to_px(double y, double lo, double hi, const D2D1_RECT_F& plot) const;
  float s(float css_px) const { return css_px * scale_; }
  void ensure_brush(ID2D1RenderTarget* rt);

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_bold_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed_;
  ID2D1RenderTarget* brush_rt_ = nullptr;

  Figure fig_;
  D2D1_RECT_F rect_{};
  float scale_ = 1.0f;
  double x0_ = 0.0;
  double x1_ = 1.0;
  float plot_left_ = 0.0f;   // shared x extent of every panel's plot rect
  float plot_right_ = 0.0f;
  std::vector<PanelLayout> layout_;
  bool hover_ = false;
  float hover_px_ = 0.0f;
  float hover_py_ = 0.0f;
  std::vector<Point> scratch_;
};

std::wstring widen(const std::string& s);

}  // namespace plot
