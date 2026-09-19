#include "map/map_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "plot/plot_widget.h"  // widen()
#include "plot/ticks.h"
#include "util/assert.h"

namespace map {

using Microsoft::WRL::ComPtr;
using plot::Color;
using plot::rgb;

namespace {

constexpr float kFontPx = 12.0f;
constexpr float kTrackWidth = 3.5f;
constexpr float kHaloWidth = 6.5f;
constexpr float kMarkerRadius = 6.0f;
constexpr float kLapRadius = 4.5f;
constexpr float kHoverPickPx = 14.0f;
constexpr float kLine = 16.0f;
constexpr float kPad = 6.0f;
constexpr float kLegendWidth = 140.0f;
constexpr float kScaleBarMax = 140.0f;

constexpr Color kBg = rgb(0xE9E9E9);
constexpr Color kTilePlaceholder = rgb(0xDDDDDD);
constexpr Color kHalo = rgb(0xFFFFFF, 0.85f);
constexpr Color kNoHr = rgb(0x555555);
constexpr Color kStart = rgb(0x2CA02C);
constexpr Color kFinish = rgb(0xD62728);
constexpr Color kLap = rgb(0xFFFFFF);
constexpr Color kText = rgb(0x222222);
constexpr Color kBox = rgb(0xFFFFFF, 0.9f);
constexpr Color kBoxBorder = rgb(0x999999);

// Blue -> cyan -> green -> yellow -> red, sampled into kColorBuckets.
constexpr Color kStops[] = {rgb(0x3B82F6), rgb(0x06B6D4), rgb(0x22C55E), rgb(0xEAB308),
                            rgb(0xDC2626)};

D2D1_COLOR_F to_d2d(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

Color lerp(const Color& a, const Color& b, float t) {
  return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f};
}

std::string pace_label(double speed_mps) {
  if (speed_mps < 0.3) return "-";
  const double s_per_km = 1000.0 / speed_mps;
  const int m = static_cast<int>(s_per_km / 60.0);
  const int sec = static_cast<int>(s_per_km) % 60;
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "%d:%02d /km", m, sec);
  return buf;
}

}  // namespace

bool MapWidget::init(ID2D1Factory* d2d, IDWriteFactory* dwrite, IWICImagingFactory* wic,
                     TileCache* tiles) {
  G_ASSERT(d2d != nullptr && dwrite != nullptr && wic != nullptr && tiles != nullptr);
  d2d_ = d2d;
  dwrite_ = dwrite;
  wic_ = wic;
  tiles_ = tiles;
  set_dpi_scale(scale_);
  return font_ && font_bold_;
}

void MapWidget::set_dpi_scale(float scale) {
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

void MapWidget::set_track(Track t) {
  G_ASSERT(t.points.size() <= kMaxTrackPoints);
  track_ = std::move(t);
  world_.clear();
  world_.reserve(track_.points.size());
  std::vector<double> hrs;
  for (const TrackPoint& p : track_.points) {
    world_.push_back(to_world(p.lat, p.lon));
    if (p.hr > 0.0) hrs.push_back(p.hr);
  }
  has_hr_ = hrs.size() >= 10;
  if (has_hr_) {
    std::sort(hrs.begin(), hrs.end());
    hr_lo_ = hrs[hrs.size() / 20];              // 5th percentile
    hr_hi_ = hrs[hrs.size() - 1 - hrs.size() / 20];  // 95th
    if (hr_hi_ - hr_lo_ < 10.0) hr_hi_ = hr_lo_ + 10.0;
  }
  fit();
}

void MapWidget::fit() {
  G_REQUIRE_VOID(!world_.empty());
  double x0 = world_[0].x, x1 = x0, y0 = world_[0].y, y1 = y0;
  for (const WorldPoint& w : world_) {
    x0 = std::min(x0, w.x);
    x1 = std::max(x1, w.x);
    y0 = std::min(y0, w.y);
    y1 = std::max(y1, w.y);
  }
  center_ = WorldPoint{(x0 + x1) / 2.0, (y0 + y1) / 2.0};
  const float w = std::max(50.0f, rect_.right - rect_.left - s(80.0f));
  const float h = std::max(50.0f, rect_.bottom - rect_.top - s(80.0f));
  zoom_ = zoom_to_fit(x1 - x0, y1 - y0, w, h);
}

double MapWidget::world_px() const {
  return static_cast<double>(kTileSize) * static_cast<double>(tiles_at(zoom_));
}

D2D1_POINT_2F MapWidget::to_px(const WorldPoint& w) const {
  const double cx = (rect_.left + rect_.right) / 2.0;
  const double cy = (rect_.top + rect_.bottom) / 2.0;
  return D2D1::Point2F(static_cast<float>(cx + (w.x - center_.x) * world_px()),
                       static_cast<float>(cy + (w.y - center_.y) * world_px()));
}

WorldPoint MapWidget::to_world_px(float px, float py) const {
  const double cx = (rect_.left + rect_.right) / 2.0;
  const double cy = (rect_.top + rect_.bottom) / 2.0;
  return WorldPoint{center_.x + (px - cx) / world_px(), center_.y + (py - cy) / world_px()};
}

void MapWidget::zoom_step(float px, float py, int delta) {
  const int nz = std::clamp(zoom_ + delta, kMinZoom, kMaxZoom);
  if (nz == zoom_) return;
  const WorldPoint anchor = to_world_px(px, py);
  zoom_ = nz;
  // Keep the geographic point under the cursor fixed.
  const double cx = (rect_.left + rect_.right) / 2.0;
  const double cy = (rect_.top + rect_.bottom) / 2.0;
  center_.x = anchor.x - (px - cx) / world_px();
  center_.y = anchor.y - (py - cy) / world_px();
}

void MapWidget::pan_pixels(float dx, float dy) {
  center_.x -= dx / world_px();
  center_.y -= dy / world_px();
  center_.y = std::clamp(center_.y, 0.0, 1.0);
}

void MapWidget::set_hover(float px, float py, bool inside) {
  hover_ = inside;
  hover_px_ = px;
  hover_py_ = py;
}

void MapWidget::drop_bitmaps() {
  bitmaps_.clear();
  bitmap_rt_ = nullptr;
}

// ----------------------------------------------------------------- render

void MapWidget::ensure_brush(ID2D1RenderTarget* rt) {
  if (brush_ && brush_rt_ == rt) return;
  brush_.Reset();
  rt->CreateSolidColorBrush(to_d2d(kText), &brush_);
  brush_rt_ = rt;
}

void MapWidget::text(ID2D1RenderTarget* rt, const std::wstring& str, const D2D1_RECT_F& box,
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

float MapWidget::measure(const std::wstring& str, bool bold) {
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

void MapWidget::render(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr);
  ensure_brush(rt);
  G_REQUIRE_VOID(brush_);
  if (bitmap_rt_ != rt) drop_bitmaps();
  bitmap_rt_ = rt;
  brush_->SetColor(to_d2d(kBg));
  rt->FillRectangle(rect_, brush_.Get());
  if (!has_track()) {
    text(rt, L"No GPS track", rect_, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText);
    return;
  }
  rt->PushAxisAlignedClip(rect_, D2D1_ANTIALIAS_MODE_ALIASED);
  draw_tiles(rt);
  draw_track(rt);
  draw_markers(rt);
  draw_legend(rt);
  draw_scale_bar(rt);
  draw_attribution(rt);
  if (hover_) draw_hover(rt);
  rt->PopAxisAlignedClip();
}

ID2D1Bitmap* MapWidget::tile_bitmap(ID2D1RenderTarget* rt, const TileKey& key) {
  const auto it = bitmaps_.find(key);
  if (it != bitmaps_.end()) return it->second.Get();
  std::vector<uint8_t> png;
  if (!tiles_->get(key, png)) return nullptr;

  ComPtr<IWICStream> stream;
  G_REQUIRE_RET(SUCCEEDED(wic_->CreateStream(&stream)), nullptr);
  G_REQUIRE_RET(SUCCEEDED(stream->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size()))),
                nullptr);
  ComPtr<IWICBitmapDecoder> decoder;
  G_REQUIRE_RET(SUCCEEDED(wic_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                        WICDecodeMetadataCacheOnLoad, &decoder)),
                nullptr);
  ComPtr<IWICBitmapFrameDecode> frame;
  G_REQUIRE_RET(SUCCEEDED(decoder->GetFrame(0, &frame)), nullptr);
  ComPtr<IWICFormatConverter> conv;
  G_REQUIRE_RET(SUCCEEDED(wic_->CreateFormatConverter(&conv)), nullptr);
  G_REQUIRE_RET(SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeMedianCut)),
                nullptr);
  ComPtr<ID2D1Bitmap> bmp;
  G_REQUIRE_RET(SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp)), nullptr);
  if (bitmaps_.size() >= kMaxBitmaps) bitmaps_.clear();
  ID2D1Bitmap* raw = bmp.Get();
  bitmaps_[key] = std::move(bmp);
  return raw;
}

void MapWidget::draw_tiles(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr && tiles_ != nullptr);
  G_ASSERT(zoom_ >= kMinZoom && zoom_ <= kMaxZoom);
  tiles_->clear_queue();
  const int n = tiles_at(zoom_);
  const double wp = world_px();
  const WorldPoint tl = to_world_px(rect_.left, rect_.top);
  const WorldPoint br = to_world_px(rect_.right, rect_.bottom);
  const int x0 = static_cast<int>(std::floor(tl.x * n));
  const int x1 = static_cast<int>(std::floor(br.x * n));
  const int y0 = std::max(0, static_cast<int>(std::floor(tl.y * n)));
  const int y1 = std::min(n - 1, static_cast<int>(std::floor(br.y * n)));
  // The visible area is at most a few dozen tiles; bound the loops anyway.
  constexpr int kMaxSpan = 64;
  for (int ty = y0; ty <= y1 && ty - y0 < kMaxSpan; ++ty) {
    for (int tx = x0; tx <= x1 && tx - x0 < kMaxSpan; ++tx) {
      const int wrapped = ((tx % n) + n) % n;
      const D2D1_POINT_2F origin =
          to_px(WorldPoint{static_cast<double>(tx) / n, static_cast<double>(ty) / n});
      const float size = static_cast<float>(wp / n);
      const D2D1_RECT_F dst =
          D2D1::RectF(std::round(origin.x), std::round(origin.y), std::round(origin.x) + size,
                      std::round(origin.y) + size);
      ID2D1Bitmap* bmp = tile_bitmap(rt, TileKey{zoom_, wrapped, ty});
      if (bmp != nullptr) {
        rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      } else {
        brush_->SetColor(to_d2d(kTilePlaceholder));
        rt->FillRectangle(dst, brush_.Get());
      }
    }
  }
}

size_t MapWidget::bucket_of(double hr) const {
  if (!has_hr_ || hr <= 0.0) return kColorBuckets;  // sentinel: no HR
  const double f = std::clamp((hr - hr_lo_) / (hr_hi_ - hr_lo_), 0.0, 0.999);
  return static_cast<size_t>(f * static_cast<double>(kColorBuckets));
}

Color MapWidget::bucket_color(size_t b) const {
  if (b >= kColorBuckets) return kNoHr;
  constexpr size_t n = sizeof(kStops) / sizeof(kStops[0]);
  const float pos = (static_cast<float>(b) + 0.5f) / static_cast<float>(kColorBuckets) *
                    static_cast<float>(n - 1);
  const size_t i = std::min(static_cast<size_t>(pos), n - 2);
  return lerp(kStops[i], kStops[i + 1], pos - static_cast<float>(i));
}

void MapWidget::draw_track(ID2D1RenderTarget* rt) {
  const size_t n = world_.size();
  G_REQUIRE_VOID(n >= 2);
  std::vector<D2D1_POINT_2F> px(n);
  for (size_t i = 0; i < n; ++i) px[i] = to_px(world_[i]);

  // Halo: one geometry through everything for contrast against the map.
  {
    ComPtr<ID2D1PathGeometry> geom;
    ComPtr<ID2D1GeometrySink> sink;
    G_REQUIRE_VOID(SUCCEEDED(d2d_->CreatePathGeometry(&geom)) && SUCCEEDED(geom->Open(&sink)));
    sink->BeginFigure(px[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(px.data() + 1, static_cast<UINT32>(n - 1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    brush_->SetColor(to_d2d(kHalo));
    rt->DrawGeometry(geom.Get(), brush_.Get(), s(kHaloWidth));
  }

  // One geometry per colour bucket; each run extends one point past its
  // end so consecutive runs join without gaps.
  for (size_t b = 0; b <= kColorBuckets; ++b) {
    ComPtr<ID2D1PathGeometry> geom;
    ComPtr<ID2D1GeometrySink> sink;
    G_REQUIRE_VOID(SUCCEEDED(d2d_->CreatePathGeometry(&geom)) && SUCCEEDED(geom->Open(&sink)));
    bool any = false;
    bool open = false;
    for (size_t i = 0; i + 1 < n; ++i) {
      const bool mine = bucket_of(track_.points[i].hr) == b;
      if (mine && !open) {
        sink->BeginFigure(px[i], D2D1_FIGURE_BEGIN_HOLLOW);
        open = true;
        any = true;
      }
      if (open) {
        sink->AddLine(px[i + 1]);
        if (!mine || i + 2 >= n) {
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          open = false;
        }
      }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    if (!any) continue;
    brush_->SetColor(to_d2d(bucket_color(b)));
    rt->DrawGeometry(geom.Get(), brush_.Get(), s(kTrackWidth));
  }
}

void MapWidget::draw_markers(ID2D1RenderTarget* rt) {
  const size_t n = world_.size();
  G_REQUIRE_VOID(n >= 1);
  auto dot = [&](const D2D1_POINT_2F& p, const Color& fill, float radius) {
    brush_->SetColor(to_d2d(kHalo));
    rt->FillEllipse(D2D1::Ellipse(p, radius + s(2.0f), radius + s(2.0f)), brush_.Get());
    brush_->SetColor(to_d2d(fill));
    rt->FillEllipse(D2D1::Ellipse(p, radius, radius), brush_.Get());
  };
  // Ski days have a lap per run and they all start at the lift: labels
  // only when there are a few laps, dots up to a few dozen, nothing beyond.
  constexpr size_t kLabelledLaps = 8;
  constexpr size_t kMarkedLaps = 30;
  const size_t laps = track_.lap_starts.size();
  int lap_no = 1;
  for (const size_t idx : track_.lap_starts) {
    if (idx == 0 || idx >= n || laps > kMarkedLaps) continue;
    ++lap_no;
    const D2D1_POINT_2F p = to_px(world_[idx]);
    dot(p, kLap, s(kLapRadius));
    brush_->SetColor(to_d2d(kText));
    rt->DrawEllipse(D2D1::Ellipse(p, s(kLapRadius), s(kLapRadius)), brush_.Get(), 1.0f);
    if (laps <= kLabelledLaps) {
      text(rt, L"L" + std::to_wstring(lap_no),
           D2D1::RectF(p.x + s(7.0f), p.y - s(kLine / 2), p.x + s(60.0f), p.y + s(kLine / 2)),
           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, true);
    }
  }
  dot(to_px(world_[0]), kStart, s(kMarkerRadius));
  dot(to_px(world_[n - 1]), kFinish, s(kMarkerRadius));
}

void MapWidget::draw_legend(ID2D1RenderTarget* rt) {
  if (!has_hr_) return;
  const float w = s(kLegendWidth);
  const float x = rect_.right - w - s(kPad * 2);
  const float y = rect_.top + s(kPad * 2);
  const D2D1_RECT_F box = D2D1::RectF(x - s(kPad), y - s(kPad), x + w + s(kPad),
                                      y + s(kLine + 12.0f + kPad));
  brush_->SetColor(to_d2d(kBox));
  rt->FillRectangle(box, brush_.Get());
  text(rt, L"Heart rate", D2D1::RectF(x, y, x + w, y + s(kLine)), DWRITE_TEXT_ALIGNMENT_CENTER,
       DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, true);
  const float bar_y = y + s(kLine);
  for (size_t b = 0; b < kColorBuckets; ++b) {
    const float a = x + w * static_cast<float>(b) / kColorBuckets;
    const float c = x + w * static_cast<float>(b + 1) / kColorBuckets;
    brush_->SetColor(to_d2d(bucket_color(b)));
    rt->FillRectangle(D2D1::RectF(a, bar_y, c, bar_y + s(8.0f)), brush_.Get());
  }
  char lo[16] = {};
  char hi[16] = {};
  std::snprintf(lo, sizeof(lo), "%.0f", hr_lo_);
  std::snprintf(hi, sizeof(hi), "%.0f", hr_hi_);
  text(rt, plot::widen(lo), D2D1::RectF(x, bar_y + s(8.0f), x + w / 2, bar_y + s(8.0f + kLine)),
       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, kText);
  text(rt, plot::widen(hi), D2D1::RectF(x + w / 2, bar_y + s(8.0f), x + w, bar_y + s(8.0f + kLine)),
       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, kText);
}

void MapWidget::draw_scale_bar(ID2D1RenderTarget* rt) {
  double lat = 0.0;
  double lon = 0.0;
  to_lat_lon(center_, lat, lon);
  const double mpp = metres_per_pixel(lat, zoom_);
  const double max_m = mpp * static_cast<double>(s(kScaleBarMax));
  const double step = plot::nice_step(max_m, 1);
  const double metres = step > max_m ? step / 2.0 : step;
  const float width = static_cast<float>(metres / mpp);
  const float x = rect_.left + s(kPad * 2);
  const float y = rect_.bottom - s(kPad * 2 + 10.0f);
  brush_->SetColor(to_d2d(kBox));
  rt->FillRectangle(D2D1::RectF(x - s(4.0f), y - s(kLine + 4.0f), x + width + s(4.0f), y + s(6.0f)),
                    brush_.Get());
  brush_->SetColor(to_d2d(kText));
  rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + width, y), brush_.Get(), 2.0f);
  rt->DrawLine(D2D1::Point2F(x, y - s(5.0f)), D2D1::Point2F(x, y + s(3.0f)), brush_.Get(), 2.0f);
  rt->DrawLine(D2D1::Point2F(x + width, y - s(5.0f)), D2D1::Point2F(x + width, y + s(3.0f)),
               brush_.Get(), 2.0f);
  char buf[32] = {};
  if (metres >= 1000.0) {
    std::snprintf(buf, sizeof(buf), "%g km", metres / 1000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%g m", metres);
  }
  text(rt, plot::widen(buf), D2D1::RectF(x, y - s(kLine + 3.0f), x + width, y - s(3.0f)),
       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText);
}

void MapWidget::draw_attribution(ID2D1RenderTarget* rt) {
  const std::wstring attribution = L"© OpenStreetMap contributors";
  const float w = measure(attribution, false) + s(kPad * 2);
  const D2D1_RECT_F box = D2D1::RectF(rect_.right - w, rect_.bottom - s(kLine + 4.0f),
                                      rect_.right, rect_.bottom);
  brush_->SetColor(to_d2d(kBox));
  rt->FillRectangle(box, brush_.Get());
  text(rt, attribution, box, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
       kText);
}

size_t MapWidget::nearest_point(float px, float py, float max_dist_px) const {
  size_t best = SIZE_MAX;
  float best_d2 = max_dist_px * max_dist_px;
  const size_t n = world_.size();
  for (size_t i = 0; i < n; ++i) {
    const D2D1_POINT_2F p = to_px(world_[i]);
    const float dx = p.x - px;
    const float dy = p.y - py;
    const float d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = i;
    }
  }
  return best;
}

void MapWidget::draw_hover(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr && hover_);
  G_ASSERT(world_.size() == track_.points.size());
  const size_t i = nearest_point(hover_px_, hover_py_, s(kHoverPickPx));
  if (i == SIZE_MAX) return;
  const TrackPoint& p = track_.points[i];
  const D2D1_POINT_2F at = to_px(world_[i]);
  brush_->SetColor(to_d2d(kText));
  rt->DrawEllipse(D2D1::Ellipse(at, s(6.0f), s(6.0f)), brush_.Get(), 2.0f);

  std::vector<std::wstring> lines;
  char buf[96] = {};
  lines.push_back(plot::widen(plot::format_elapsed(p.elapsed_s)));
  std::snprintf(buf, sizeof(buf), "%.2f km", p.dist_m / 1000.0);
  lines.push_back(plot::widen(buf));
  if (p.hr > 0.0) {
    std::snprintf(buf, sizeof(buf), "HR %.0f bpm", p.hr);
    lines.push_back(plot::widen(buf));
  }
  if (p.speed_mps > 0.0) {
    std::snprintf(buf, sizeof(buf), "%.1f km/h   %s", p.speed_mps * 3.6,
                  pace_label(p.speed_mps).c_str());
    lines.push_back(plot::widen(buf));
  }
  std::snprintf(buf, sizeof(buf), "alt %.0f m", p.alt_m);
  lines.push_back(plot::widen(buf));

  float w = 0.0f;
  for (size_t k = 0; k < lines.size(); ++k) w = std::max(w, measure(lines[k], k == 0));
  const float bw = w + s(kPad * 2);
  const float bh = s(kLine) * static_cast<float>(lines.size()) + s(kPad * 2);
  float bx = hover_px_ + s(16.0f);
  float by = hover_py_ + s(16.0f);
  if (bx + bw > rect_.right) bx = hover_px_ - s(16.0f) - bw;
  if (by + bh > rect_.bottom) by = rect_.bottom - bh;
  const D2D1_RECT_F box = D2D1::RectF(bx, by, bx + bw, by + bh);
  brush_->SetColor(to_d2d(kBox));
  rt->FillRectangle(box, brush_.Get());
  brush_->SetColor(to_d2d(kBoxBorder));
  rt->DrawRectangle(box, brush_.Get(), 1.0f);
  float y = by + s(kPad);
  for (size_t k = 0; k < lines.size(); ++k) {
    text(rt, lines[k], D2D1::RectF(bx + s(kPad), y, bx + bw, y + s(kLine)),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, kText, k == 0);
    y += s(kLine);
  }
}

}  // namespace map
