#include "plot/plot_widget.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "plot/decimate.h"
#include "util/assert.h"

namespace plot {

using Microsoft::WRL::ComPtr;

namespace {

// Layout constants in CSS pixels (multiplied by the DPI scale).
constexpr float kFontPx = 12.0f;
constexpr float kOuterPad = 8.0f;
constexpr float kLeftMargin = 58.0f;
constexpr float kRightMargin = 58.0f;
constexpr float kRightMarginNoAxis = 16.0f;
constexpr float kTitleRow = 22.0f;
constexpr float kPanelGap = 10.0f;
constexpr float kBottomAxis = 26.0f;
constexpr float kTickLen = 4.0f;
constexpr float kPointRadius = 3.0f;
constexpr float kTooltipPad = 6.0f;
constexpr float kLineHeight = 16.0f;
constexpr int kYTicksTarget = 5;
constexpr double kMinSpanSeconds = 30.0;
constexpr double kHoverToleranceFrac = 0.02;
constexpr size_t kMaxHoverEntries = 32;

constexpr Color kBackground = rgb(0xFFFFFF);
constexpr Color kPlotBorder = rgb(0xB0B0B0);
constexpr Color kGrid = rgb(0xE8E8E8);
constexpr Color kGridStrong = rgb(0xC8C8C8);
constexpr Color kText = rgb(0x222222);
constexpr Color kTextMuted = rgb(0x666666);
constexpr Color kCrosshair = rgb(0x444444, 0.7f);
constexpr Color kTooltipBg = rgb(0xFFFFFF, 0.96f);

D2D1_COLOR_F to_d2d(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

}  // namespace

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  G_REQUIRE_RET(n > 0, std::wstring());
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

// ------------------------------------------------------------------ setup

bool PlotWidget::init(ID2D1Factory* d2d, IDWriteFactory* dwrite) {
  G_ASSERT(d2d != nullptr && dwrite != nullptr);
  d2d_ = d2d;
  dwrite_ = dwrite;
  D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
  props.dashStyle = D2D1_DASH_STYLE_DASH;
  G_REQUIRE_RET(SUCCEEDED(d2d_->CreateStrokeStyle(props, nullptr, 0, &dashed_)), false);
  set_dpi_scale(scale_);
  return font_ != nullptr && font_bold_ != nullptr;
}

void PlotWidget::set_dpi_scale(float scale) {
  G_ASSERT(scale > 0.1f && scale < 10.0f);
  scale_ = scale;
  G_REQUIRE_VOID(dwrite_ != nullptr);
  font_.Reset();
  font_bold_.Reset();
  dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s(kFontPx),
                            L"en-us", &font_);
  dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s(kFontPx),
                            L"en-us", &font_bold_);
  if (font_) font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (font_bold_) font_bold_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void PlotWidget::set_figure(Figure fig) {
  fig_ = std::move(fig);
  if (!fig_.finalize()) {
    fig_.panels.clear();
    return;
  }
  fit_x();
}

// ------------------------------------------------------------------- view

void PlotWidget::fit_x() {
  x0_ = fig_.x_min;
  x1_ = fig_.x_max;
  if (x1_ - x0_ < kMinSpanSeconds) x1_ = x0_ + kMinSpanSeconds;
}

void PlotWidget::set_x_range(double x0, double x1) {
  G_REQUIRE_VOID(x1 > x0);
  x0_ = x0;
  x1_ = x1;
}

double PlotWidget::px_to_x(float px) const {
  const float w = std::max(1.0f, plot_right_ - plot_left_);
  return x0_ + (static_cast<double>(px - plot_left_) / static_cast<double>(w)) * (x1_ - x0_);
}

float PlotWidget::x_to_px(double x) const {
  const float w = std::max(1.0f, plot_right_ - plot_left_);
  return plot_left_ + static_cast<float>((x - x0_) / (x1_ - x0_)) * w;
}

void PlotWidget::zoom_at(float px, double factor) {
  G_ASSERT(factor > 0.0);
  G_REQUIRE_VOID(has_figure());
  const double anchor = px_to_x(px);
  double span = (x1_ - x0_) * factor;
  const double max_span = (fig_.x_max - fig_.x_min) * 1.05 + kMinSpanSeconds;
  span = std::clamp(span, kMinSpanSeconds, max_span);
  const double frac = (anchor - x0_) / (x1_ - x0_);
  x0_ = anchor - frac * span;
  x1_ = x0_ + span;
}

void PlotWidget::pan_pixels(float dx) {
  G_REQUIRE_VOID(has_figure());
  const float w = std::max(1.0f, plot_right_ - plot_left_);
  const double shift = -static_cast<double>(dx) / static_cast<double>(w) * (x1_ - x0_);
  x0_ += shift;
  x1_ += shift;
}

void PlotWidget::set_hover(float px, float py, bool inside) {
  hover_ = inside;
  hover_px_ = px;
  hover_py_ = py;
}

// ----------------------------------------------------------------- layout

void PlotWidget::compute_layout() {
  layout_.clear();
  const size_t n = fig_.panels.size();
  G_REQUIRE_VOID(n > 0);
  bool any_right = false;
  float total_weight = 0.0f;
  for (const Panel& p : fig_.panels) {
    total_weight += std::max(0.1f, p.weight);
    for (const Series& sr : p.series) any_right = any_right || sr.axis == YAxisSide::kRight;
  }
  plot_left_ = rect_.left + s(kOuterPad + kLeftMargin);
  plot_right_ = rect_.right - s(kOuterPad + (any_right ? kRightMargin : kRightMarginNoAxis));
  if (plot_right_ - plot_left_ < 10.0f) plot_right_ = plot_left_ + 10.0f;

  const float avail = (rect_.bottom - rect_.top) - s(kOuterPad * 2 + kBottomAxis) -
                      s(kPanelGap) * static_cast<float>(n - 1) -
                      s(kTitleRow) * static_cast<float>(n);
  const float plot_total = std::max(10.0f * static_cast<float>(n), avail);
  float y = rect_.top + s(kOuterPad);
  for (const Panel& p : fig_.panels) {
    PanelLayout L;
    const float h = plot_total * std::max(0.1f, p.weight) / total_weight;
    y += s(kTitleRow);
    L.plot = D2D1::RectF(plot_left_, y, plot_right_, y + h);
    auto_range(p, L);
    layout_.push_back(L);
    y += h + s(kPanelGap);
  }
}

void PlotWidget::auto_range(const Panel& panel, PanelLayout& L) const {
  bool any_l = false;
  bool any_r = false;
  double lo_l = 0.0, hi_l = 0.0, lo_r = 0.0, hi_r = 0.0;
  auto grow = [](bool& any, double& lo, double& hi, double v) {
    if (!any) {
      lo = v;
      hi = v;
      any = true;
    } else {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  };
  for (const Series& sr : panel.series) {
    if (sr.style == Style::kBand || sr.x.empty()) continue;
    const bool right = sr.axis == YAxisSide::kRight;
    if (right) {
      L.has_right = true;
    } else {
      L.has_left = true;
    }
    const auto lo_it = std::lower_bound(sr.x.begin(), sr.x.end(), x0_);
    const auto hi_it = std::upper_bound(sr.x.begin(), sr.x.end(), x1_);
    size_t i0 = static_cast<size_t>(lo_it - sr.x.begin());
    size_t i1 = static_cast<size_t>(hi_it - sr.x.begin());
    if (i0 > 0) --i0;
    if (i1 < sr.x.size()) ++i1;
    for (size_t i = i0; i < i1; ++i) {
      if (!std::isfinite(sr.y[i])) continue;
      if (right) {
        grow(any_r, lo_r, hi_r, sr.y[i]);
        if (sr.style == Style::kRange) grow(any_r, lo_r, hi_r, sr.y2[i]);
      } else {
        grow(any_l, lo_l, hi_l, sr.y[i]);
        if (sr.style == Style::kRange) grow(any_l, lo_l, hi_l, sr.y2[i]);
      }
    }
    if (sr.style == Style::kBars) {
      if (right) {
        grow(any_r, lo_r, hi_r, 0.0);
      } else {
        grow(any_l, lo_l, hi_l, 0.0);
      }
    }
  }
  auto finish = [](const AxisSpec& spec, bool any, double lo, double hi, double& out_lo,
                   double& out_hi) {
    if (spec.fixed) {
      out_lo = spec.min;
      out_hi = spec.max;
      return;
    }
    if (!any) {
      lo = 0.0;
      hi = 1.0;
    }
    if (spec.include_zero) lo = std::min(lo, 0.0);
    // Breathing room so lines do not sit on the frame.
    const double pad = (hi - lo) * 0.05;
    lo -= pad;
    hi += pad;
    if (spec.include_zero && lo < 0.0 && hi > 0.0 && lo > -pad * 1.01) lo = 0.0;
    nice_range(lo, hi, kYTicksTarget);
    out_lo = lo;
    out_hi = hi;
  };
  finish(panel.left, any_l, lo_l, hi_l, L.left_lo, L.left_hi);
  finish(panel.right, any_r, lo_r, hi_r, L.right_lo, L.right_hi);
}

float PlotWidget::y_to_px(double y, double lo, double hi, const D2D1_RECT_F& plot) const {
  const double span = hi > lo ? hi - lo : 1.0;
  const double f = (y - lo) / span;
  return plot.bottom - static_cast<float>(f) * (plot.bottom - plot.top);
}

// ----------------------------------------------------------------- render

void PlotWidget::ensure_brush(ID2D1RenderTarget* rt) {
  if (brush_ && brush_rt_ == rt) return;
  brush_.Reset();
  rt->CreateSolidColorBrush(to_d2d(kText), &brush_);
  brush_rt_ = rt;
}

void PlotWidget::text(ID2D1RenderTarget* rt, const std::wstring& str, const D2D1_RECT_F& box,
                      DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign,
                      const Color& c, bool bold) {
  IDWriteTextFormat* f = bold ? font_bold_.Get() : font_.Get();
  G_REQUIRE_VOID(f != nullptr && brush_);
  f->SetTextAlignment(align);
  f->SetParagraphAlignment(valign);
  brush_->SetColor(to_d2d(c));
  rt->DrawTextW(str.c_str(), static_cast<UINT32>(str.size()), f, box, brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float PlotWidget::measure_text_width(const std::wstring& str, bool bold) {
  IDWriteTextFormat* f = bold ? font_bold_.Get() : font_.Get();
  G_REQUIRE_RET(f != nullptr && dwrite_, 0.0f);
  ComPtr<IDWriteTextLayout> layout;
  G_REQUIRE_RET(SUCCEEDED(dwrite_->CreateTextLayout(str.c_str(), static_cast<UINT32>(str.size()),
                                                    f, 4096.0f, 100.0f, &layout)),
                0.0f);
  DWRITE_TEXT_METRICS m{};
  G_REQUIRE_RET(SUCCEEDED(layout->GetMetrics(&m)), 0.0f);
  return m.widthIncludingTrailingWhitespace;
}

void PlotWidget::render(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr);
  ensure_brush(rt);
  G_REQUIRE_VOID(brush_);
  brush_->SetColor(to_d2d(kBackground));
  rt->FillRectangle(rect_, brush_.Get());
  if (!has_figure()) {
    text(rt, L"No data", rect_, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
         kTextMuted);
    return;
  }
  compute_layout();
  G_ASSERT(layout_.size() == fig_.panels.size());
  for (size_t i = 0; i < fig_.panels.size(); ++i) {
    draw_panel(rt, fig_.panels[i], layout_[i], i + 1 == fig_.panels.size());
  }
  draw_markers(rt);
  if (hover_) draw_hover(rt);
}

void PlotWidget::draw_x_axis(ID2D1RenderTarget* rt, const PanelLayout& L) {
  std::vector<Tick> ticks;
  const double width = static_cast<double>(plot_right_ - plot_left_);
  if (fig_.xmode == XMode::kTime) {
    time_ticks(x0_, x1_, width, ticks);
  } else {
    elapsed_ticks(x0_, x1_, width, ticks);
  }
  const float label_w = s(90.0f);
  for (const Tick& t : ticks) {
    const float px = std::round(x_to_px(t.value)) + 0.5f;
    brush_->SetColor(to_d2d(kPlotBorder));
    rt->DrawLine(D2D1::Point2F(px, L.plot.bottom), D2D1::Point2F(px, L.plot.bottom + s(kTickLen)),
                 brush_.Get(), 1.0f);
    const D2D1_RECT_F box = D2D1::RectF(px - label_w / 2, L.plot.bottom + s(kTickLen),
                                        px + label_w / 2, L.plot.bottom + s(kBottomAxis));
    text(rt, widen(t.label), box, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
         t.emphasized ? kText : kTextMuted, t.emphasized);
  }
}

void PlotWidget::draw_panel(ID2D1RenderTarget* rt, const Panel& panel, const PanelLayout& L,
                            bool bottom) {
  const D2D1_RECT_F& P = L.plot;

  // Title + legend row.
  float tx = P.left;
  const float ty = P.top - s(kTitleRow);
  const D2D1_RECT_F title_box = D2D1::RectF(tx, ty, P.right, P.top);
  if (!panel.title.empty()) {
    const std::wstring wt = widen(panel.title);
    text(rt, wt, title_box, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
         kText, true);
    tx += measure_text_width(wt, true) + s(16.0f);
  }
  for (const Series& sr : panel.series) {
    if (!sr.in_legend) continue;
    std::string label = sr.name;
    if (!sr.units.empty()) label += " (" + sr.units + ")";
    const std::wstring wl = widen(label);
    const float w = measure_text_width(wl, false);
    // The legend may run into the right margin; stop only at the widget edge.
    if (tx + s(14.0f) + w > rect_.right - s(4.0f)) break;
    brush_->SetColor(to_d2d(sr.color));
    const float cy = (ty + P.top) / 2.0f;
    if (sr.style == Style::kBand || sr.style == Style::kBars || sr.style == Style::kRange) {
      rt->FillRectangle(D2D1::RectF(tx, cy - s(5.0f), tx + s(10.0f), cy + s(5.0f)), brush_.Get());
    } else {
      rt->DrawLine(D2D1::Point2F(tx, cy), D2D1::Point2F(tx + s(10.0f), cy), brush_.Get(),
                   s(2.5f));
    }
    tx += s(14.0f);
    text(rt, wl, D2D1::RectF(tx, ty, tx + w + s(4.0f), P.top), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kTextMuted);
    tx += w + s(12.0f);
  }

  // Vertical grid at x ticks.
  {
    std::vector<Tick> ticks;
    const double width = static_cast<double>(plot_right_ - plot_left_);
    if (fig_.xmode == XMode::kTime) {
      time_ticks(x0_, x1_, width, ticks);
    } else {
      elapsed_ticks(x0_, x1_, width, ticks);
    }
    for (const Tick& t : ticks) {
      const float px = std::round(x_to_px(t.value)) + 0.5f;
      brush_->SetColor(to_d2d(t.emphasized ? kGridStrong : kGrid));
      rt->DrawLine(D2D1::Point2F(px, P.top), D2D1::Point2F(px, P.bottom), brush_.Get(), 1.0f);
    }
  }

  // Left axis ticks + horizontal grid.
  if (L.has_left) {
    std::vector<Tick> ticks;
    linear_ticks(L.left_lo, L.left_hi, kYTicksTarget, ticks);
    for (const Tick& t : ticks) {
      const float py = std::round(y_to_px(t.value, L.left_lo, L.left_hi, P)) + 0.5f;
      brush_->SetColor(to_d2d(kGrid));
      rt->DrawLine(D2D1::Point2F(P.left, py), D2D1::Point2F(P.right, py), brush_.Get(), 1.0f);
      brush_->SetColor(to_d2d(kPlotBorder));
      rt->DrawLine(D2D1::Point2F(P.left - s(kTickLen), py), D2D1::Point2F(P.left, py),
                   brush_.Get(), 1.0f);
      text(rt, widen(t.label),
           D2D1::RectF(rect_.left, py - s(kLineHeight / 2), P.left - s(kTickLen + 3.0f),
                       py + s(kLineHeight / 2)),
           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kTextMuted);
    }
  }
  if (L.has_right) {
    std::vector<Tick> ticks;
    linear_ticks(L.right_lo, L.right_hi, kYTicksTarget, ticks);
    for (const Tick& t : ticks) {
      const float py = std::round(y_to_px(t.value, L.right_lo, L.right_hi, P)) + 0.5f;
      brush_->SetColor(to_d2d(kPlotBorder));
      rt->DrawLine(D2D1::Point2F(P.right, py), D2D1::Point2F(P.right + s(kTickLen), py),
                   brush_.Get(), 1.0f);
      text(rt, widen(t.label),
           D2D1::RectF(P.right + s(kTickLen + 3.0f), py - s(kLineHeight / 2), rect_.right,
                       py + s(kLineHeight / 2)),
           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kTextMuted);
    }
  }

  // Series, clipped to the plot area. Bands first so lines sit on top.
  rt->PushAxisAlignedClip(P, D2D1_ANTIALIAS_MODE_ALIASED);
  for (const Series& sr : panel.series) {
    if (sr.style == Style::kBand || sr.style == Style::kRange) draw_series(rt, sr, L);
  }
  for (const Series& sr : panel.series) {
    if (sr.style != Style::kBand && sr.style != Style::kRange) draw_series(rt, sr, L);
  }
  for (const HLine& h : panel.hlines) {
    const bool right = h.axis == YAxisSide::kRight;
    const float py = std::round(y_to_px(h.y, right ? L.right_lo : L.left_lo,
                                        right ? L.right_hi : L.left_hi, P)) + 0.5f;
    if (py < P.top || py > P.bottom) continue;
    brush_->SetColor(to_d2d(h.color));
    rt->DrawLine(D2D1::Point2F(P.left, py), D2D1::Point2F(P.right, py), brush_.Get(), 1.0f,
                 dashed_.Get());
    if (!h.label.empty()) {
      text(rt, widen(h.label), D2D1::RectF(P.left, py - s(kLineHeight), P.right - s(4.0f), py),
           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_FAR, h.color);
    }
  }
  rt->PopAxisAlignedClip();

  brush_->SetColor(to_d2d(kPlotBorder));
  rt->DrawRectangle(D2D1::RectF(P.left + 0.5f, P.top + 0.5f, P.right - 0.5f, P.bottom - 0.5f),
                    brush_.Get(), 1.0f);
  if (bottom) draw_x_axis(rt, L);
}

void PlotWidget::draw_series(ID2D1RenderTarget* rt, const Series& sr, const PanelLayout& L) {
  G_ASSERT(sr.valid());
  if (sr.x.empty()) return;
  const bool right = sr.axis == YAxisSide::kRight;
  const double lo = right ? L.right_lo : L.left_lo;
  const double hi = right ? L.right_hi : L.left_hi;
  const D2D1_RECT_F& P = L.plot;
  brush_->SetColor(to_d2d(sr.color));

  if (sr.style == Style::kBand) {
    const size_t n = sr.x.size();
    for (size_t i = 0; i < n; ++i) {
      if (sr.x2[i] < x0_ || sr.x[i] > x1_) continue;
      const float a = x_to_px(std::max(sr.x[i], x0_));
      const float b = x_to_px(std::min(sr.x2[i], x1_));
      rt->FillRectangle(D2D1::RectF(a, P.top, std::max(b, a + 1.0f), P.bottom), brush_.Get());
    }
    return;
  }

  if (sr.style == Style::kRange) {
    const size_t n = sr.x.size();
    size_t i0 = static_cast<size_t>(std::lower_bound(sr.x.begin(), sr.x.end(), x0_) - sr.x.begin());
    size_t i1 = static_cast<size_t>(std::upper_bound(sr.x.begin(), sr.x.end(), x1_) - sr.x.begin());
    if (i0 > 0) --i0;
    if (i1 < n) ++i1;
    if (i1 <= i0 + 1) return;
    ComPtr<ID2D1PathGeometry> geom;
    ComPtr<ID2D1GeometrySink> sink;
    G_REQUIRE_VOID(SUCCEEDED(d2d_->CreatePathGeometry(&geom)) && SUCCEEDED(geom->Open(&sink)));
    sink->BeginFigure(D2D1::Point2F(x_to_px(sr.x[i0]), y_to_px(sr.y2[i0], lo, hi, P)),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = i0 + 1; i < i1; ++i) {
      sink->AddLine(D2D1::Point2F(x_to_px(sr.x[i]), y_to_px(sr.y2[i], lo, hi, P)));
    }
    for (size_t i = i1; i-- > i0;) {
      sink->AddLine(D2D1::Point2F(x_to_px(sr.x[i]), y_to_px(sr.y[i], lo, hi, P)));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    G_REQUIRE_VOID(SUCCEEDED(sink->Close()));
    rt->FillGeometry(geom.Get(), brush_.Get());
    return;
  }

  if (sr.style == Style::kBars) {
    const float zero = y_to_px(std::clamp(0.0, lo, hi), lo, hi, P);
    const size_t n = sr.x.size();
    for (size_t i = 0; i < n; ++i) {
      const double xe = sr.bar_end(i);
      if (xe < x0_ || sr.x[i] > x1_) continue;
      const float a = x_to_px(sr.x[i]) + 1.0f;
      const float b = std::max(x_to_px(xe) - 1.0f, a + 1.0f);
      const float py = y_to_px(sr.y[i], lo, hi, P);
      rt->FillRectangle(D2D1::RectF(a, std::min(py, zero), b, std::max(py, zero)), brush_.Get());
    }
    return;
  }

  const int columns = static_cast<int>(std::max(1.0f, P.right - P.left));
  decimate_minmax(sr.x, sr.y, x0_, x1_, columns, scratch_);
  if (scratch_.empty()) return;

  ComPtr<ID2D1PathGeometry> geom;
  G_REQUIRE_VOID(SUCCEEDED(d2d_->CreatePathGeometry(&geom)));
  ComPtr<ID2D1GeometrySink> sink;
  G_REQUIRE_VOID(SUCCEEDED(geom->Open(&sink)));
  const auto pt = [&](const Point& p) {
    return D2D1::Point2F(x_to_px(p.x), y_to_px(p.y, lo, hi, P));
  };
  sink->BeginFigure(pt(scratch_[0]), D2D1_FIGURE_BEGIN_HOLLOW);
  const size_t n = scratch_.size();
  for (size_t i = 1; i < n; ++i) {
    if (sr.gap_break > 0.0 && scratch_[i].x - scratch_[i - 1].x > sr.gap_break) {
      // Data gap: lift the pen instead of drawing a misleading connector.
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      sink->BeginFigure(pt(scratch_[i]), D2D1_FIGURE_BEGIN_HOLLOW);
      continue;
    }
    if (sr.style == Style::kStep) {
      sink->AddLine(D2D1::Point2F(x_to_px(scratch_[i].x), y_to_px(scratch_[i - 1].y, lo, hi, P)));
    }
    sink->AddLine(pt(scratch_[i]));
  }
  if (sr.style == Style::kStep && n >= 2) {
    // Hold the last value for one more sample interval, not to the edge.
    const double hold = std::min(scratch_[n - 1].x - scratch_[n - 2].x, (x1_ - x0_) * 0.05);
    sink->AddLine(D2D1::Point2F(x_to_px(scratch_[n - 1].x + hold),
                                y_to_px(scratch_[n - 1].y, lo, hi, P)));
  }
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  G_REQUIRE_VOID(SUCCEEDED(sink->Close()));

  const float width = s(sr.style == Style::kPoints ? 1.0f : sr.width);
  rt->DrawGeometry(geom.Get(), brush_.Get(), width);

  if (sr.style == Style::kPoints && n <= 4000) {
    for (size_t i = 0; i < n; ++i) {
      const D2D1_POINT_2F c = pt(scratch_[i]);
      rt->FillEllipse(D2D1::Ellipse(c, s(kPointRadius), s(kPointRadius)), brush_.Get());
    }
  }
}

void PlotWidget::draw_markers(ID2D1RenderTarget* rt) {
  if (fig_.markers.empty() || layout_.empty()) return;
  const float top = layout_.front().plot.top;
  const float bottom = layout_.back().plot.bottom;
  for (const Marker& m : fig_.markers) {
    if (m.x < x0_ || m.x > x1_) continue;
    const float px = std::round(x_to_px(m.x)) + 0.5f;
    brush_->SetColor(to_d2d(colors::kMarker));
    rt->DrawLine(D2D1::Point2F(px, top), D2D1::Point2F(px, bottom), brush_.Get(), 1.0f,
                 dashed_.Get());
    if (!m.label.empty()) {
      text(rt, widen(m.label), D2D1::RectF(px + s(3.0f), top, px + s(120.0f), top + s(kLineHeight)),
           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, kTextMuted);
    }
  }
}

void PlotWidget::collect_hover(double xq, std::vector<HoverEntry>& out) const {
  out.clear();
  const double tol = (x1_ - x0_) * kHoverToleranceFrac;
  for (const Panel& p : fig_.panels) {
    for (const Series& sr : p.series) {
      if (sr.x.empty() || out.size() >= kMaxHoverEntries || sr.style == Style::kRange) continue;
      if (sr.style == Style::kBand) {
        const auto it = std::upper_bound(sr.x.begin(), sr.x.end(), xq);
        if (it == sr.x.begin()) continue;
        const size_t i = static_cast<size_t>(it - sr.x.begin()) - 1;
        if (xq <= sr.x2[i]) out.push_back(HoverEntry{sr.name, sr.color});
        continue;
      }
      size_t i = nearest_index(sr.x, xq);
      if (sr.style == Style::kStep || sr.style == Style::kBars) {
        const auto it = std::upper_bound(sr.x.begin(), sr.x.end(), xq);
        if (it == sr.x.begin()) continue;
        i = static_cast<size_t>(it - sr.x.begin()) - 1;
        const double hold = sr.style == Style::kBars ? sr.bar_end(i) - sr.x[i] : tol * 10.0;
        if (xq - sr.x[i] > hold) continue;
      } else if (std::fabs(sr.x[i] - xq) > tol) {
        continue;
      }
      out.push_back(HoverEntry{sr.name + ": " + format_value(sr.y[i], sr.units), sr.color});
    }
  }
}

void PlotWidget::draw_hover(ID2D1RenderTarget* rt) {
  if (layout_.empty() || hover_px_ < plot_left_ || hover_px_ > plot_right_) return;
  const float top = layout_.front().plot.top;
  const float bottom = layout_.back().plot.bottom;
  if (hover_py_ < top || hover_py_ > bottom) return;
  const double xq = px_to_x(hover_px_);

  brush_->SetColor(to_d2d(kCrosshair));
  const float px = std::round(hover_px_) + 0.5f;
  rt->DrawLine(D2D1::Point2F(px, top), D2D1::Point2F(px, bottom), brush_.Get(), 1.0f);

  std::vector<HoverEntry> entries;
  collect_hover(xq, entries);
  std::string header = fig_.xmode == XMode::kTime
                           ? format_date_local(xq) + "  " + format_clock_local(xq)
                           : format_elapsed(xq);
  std::vector<std::wstring> lines;
  lines.push_back(widen(header));
  float w = measure_text_width(lines[0], true);
  for (const HoverEntry& e : entries) {
    lines.push_back(widen(e.text));
    w = std::max(w, measure_text_width(lines.back(), false) + s(14.0f));
  }
  const float box_w = w + s(kTooltipPad * 2);
  const float box_h = s(kLineHeight) * static_cast<float>(lines.size()) + s(kTooltipPad * 2);
  float bx = hover_px_ + s(14.0f);
  float by = hover_py_ + s(14.0f);
  if (bx + box_w > rect_.right) bx = hover_px_ - s(14.0f) - box_w;
  if (by + box_h > rect_.bottom) by = rect_.bottom - box_h;
  const D2D1_RECT_F box = D2D1::RectF(bx, by, bx + box_w, by + box_h);
  brush_->SetColor(to_d2d(kTooltipBg));
  rt->FillRectangle(box, brush_.Get());
  brush_->SetColor(to_d2d(kPlotBorder));
  rt->DrawRectangle(box, brush_.Get(), 1.0f);

  float ly = by + s(kTooltipPad);
  for (size_t i = 0; i < lines.size(); ++i) {
    float lx = bx + s(kTooltipPad);
    if (i > 0) {
      brush_->SetColor(to_d2d(entries[i - 1].color));
      rt->FillRectangle(D2D1::RectF(lx, ly + s(4.0f), lx + s(8.0f), ly + s(12.0f)), brush_.Get());
      lx += s(14.0f);
    }
    text(rt, lines[i], D2D1::RectF(lx, ly, bx + box_w, ly + s(kLineHeight)),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, i == 0);
    ly += s(kLineHeight);
  }
}

}  // namespace plot
