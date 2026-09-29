#include "map3d/view3d.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "map/tile_provider.h"
#include "map3d/image_decode.h"
#include "plot/plot_widget.h"  // widen()
#include "util/assert.h"

namespace map3d {

namespace {

constexpr wchar_t kClass[] = L"GViewMap3D";
constexpr UINT_PTR kRetryTimer = 1;
constexpr UINT kRetryMs = 2000;
constexpr double kMargin = 0.15;               // terrain beyond the track, each side
constexpr float kExaggerations[] = {1.0f, 1.5f, 2.0f, 3.0f};
constexpr size_t kExaggerationCount = sizeof(kExaggerations) / sizeof(kExaggerations[0]);
constexpr uint32_t kStartRgb = 0x2CA02C;
constexpr uint32_t kFinishRgb = 0xD62728;
constexpr float kFontPx = 12.0f;
constexpr float kPad = 6.0f;
constexpr float kLine = 16.0f;

uint32_t to_rgb(const plot::Color& c) {
  auto byte = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* view = reinterpret_cast<View3D*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (view != nullptr && msg != WM_NCCREATE && msg != WM_NCDESTROY) return view->handle(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{};
  if (GetClassInfoExW(inst, kClass, &wc)) return true;
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = child_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
  wc.lpszClassName = kClass;
  return RegisterClassExW(&wc) != 0;
}

}  // namespace

View3D::~View3D() {
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

bool View3D::create(HWND parent, ID2D1Factory* d2d, IDWriteFactory* dwrite,
                    IWICImagingFactory* wic, map::TileCache* tiles) {
  G_ASSERT(parent != nullptr && d2d != nullptr && dwrite != nullptr);
  G_ASSERT(wic != nullptr && tiles != nullptr && hwnd_ == nullptr);
  d2d_ = d2d;
  dwrite_ = dwrite;
  wic_ = wic;
  tiles_ = tiles;
  terrain_layer_ = map::provider_index(map::kTerrainProviderId);
  G_REQUIRE_RET(terrain_layer_ != SIZE_MAX, false);
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  G_REQUIRE_RET(register_class(inst), false);
  hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, parent, nullptr,
                          inst, this);
  G_REQUIRE_RET(hwnd_ != nullptr, false);
  set_dpi_scale(scale_);
  return gpu_.init(hwnd_, d2d_, 16, 16);
}

void View3D::show(bool on) {
  G_REQUIRE_VOID(hwnd_ != nullptr);
  ShowWindow(hwnd_, on ? SW_SHOWNA : SW_HIDE);
  if (on) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool View3D::visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != 0; }

void View3D::set_rect(const RECT& r) {
  G_REQUIRE_VOID(hwnd_ != nullptr);
  const int w = std::max<int>(1, r.right - r.left);
  const int h = std::max<int>(1, r.bottom - r.top);
  MoveWindow(hwnd_, r.left, r.top, w, h, FALSE);
  if (gpu_.ok()) gpu_.resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void View3D::set_dpi_scale(float scale) {
  G_ASSERT(scale > 0.1f && scale < 10.0f);
  scale_ = scale;
  font_.Reset();
  dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s(kFontPx),
                            L"en-us", &font_);
  if (font_) font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void View3D::set_track(map::Track t) {
  G_ASSERT(t.points.size() <= map::kMaxTrackPoints);
  track_ = std::move(t);
  world_.clear();
  for (size_t i = 0; i < track_.points.size() && i < kMaxTrack; ++i) {
    world_.push_back(map::to_world(track_.points[i].lat, track_.points[i].lon));
  }
  hr_ = map::hr_range(track_.points);
  heights_.assign(static_cast<size_t>(kGrid) * kGrid, 0.0f);
  if (world_.empty()) {
    gpu_.set_terrain({}, {});
    gpu_.set_track({});
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  region_ = region_for(world_, kMargin);
  layout_blocks();
  reset_camera();
  rebuild_meshes();
  refresh_tiles();
}

// Chooses the tile blocks for heights and imagery and resets what was loaded.
void View3D::layout_blocks() {
  G_ASSERT(!world_.empty());
  const auto& providers = map::tile_providers();
  G_ASSERT(base_ < providers.size());
  terrain_block_ = block_for(region_, zoom_for(region_, kTerrainTilesAcross, map::kTerrainMaxZoom));
  image_block_ = block_for(region_, zoom_for(region_, kImageTilesAcross, providers[base_].max_zoom));
  terrain_tiles_.assign(static_cast<size_t>(terrain_block_.n) * terrain_block_.n, {});
  image_have_.assign(static_cast<size_t>(image_block_.n) * image_block_.n, false);
  const bool made = gpu_.create_texture(static_cast<uint32_t>(image_block_.n * kTilePx));
  G_ASSERT(made);
}

void View3D::set_base_layer(size_t provider) {
  G_REQUIRE_VOID(provider < map::tile_providers().size());
  if (provider == base_) return;
  base_ = provider;
  if (world_.empty()) return;
  const std::vector<std::vector<float>> keep = terrain_tiles_;  // heights do not depend on the layer
  layout_blocks();
  terrain_tiles_ = keep;
  rebuild_meshes();  // texture coordinates follow the new imagery block
  refresh_tiles();
}

void View3D::on_tiles_ready() {
  if (!world_.empty()) refresh_tiles();
}

// Picks up tiles that have arrived and asks for the rest; a timer retries
// failed ones while anything is still missing.
void View3D::refresh_tiles() {
  G_REQUIRE_VOID(hwnd_ != nullptr);
  const bool new_heights = load_terrain_tiles();
  const bool new_imagery = load_imagery_tiles();
  if (new_heights) {
    rebuild_heights();
    rebuild_meshes();
  }
  const bool missing = missing_tiles() > 0;
  if (missing != timer_on_) {
    timer_on_ = missing;
    if (missing) {
      SetTimer(hwnd_, kRetryTimer, kRetryMs, nullptr);
    } else {
      KillTimer(hwnd_, kRetryTimer);
    }
  }
  if (new_heights || new_imagery || missing) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool View3D::load_terrain_tiles() {
  bool any = false;
  const int n = terrain_block_.n;
  for (int k = 0; k < n * n && k < kImageTilesAcross * kImageTilesAcross; ++k) {
    std::vector<float>& heights = terrain_tiles_[static_cast<size_t>(k)];
    if (!heights.empty()) continue;
    const map::TileKey key{terrain_block_.z, terrain_block_.x0 + k % n, terrain_block_.y0 + k / n,
                           static_cast<int>(terrain_layer_)};
    std::vector<uint8_t> png, bgra;
    uint32_t w = 0, h = 0;
    if (!tiles_->get(key, png) || !decode_bgra(wic_, png, bgra, w, h)) continue;
    if (w != kTilePx || h != kTilePx) continue;
    heights.resize(static_cast<size_t>(kTilePx) * kTilePx);
    for (size_t p = 0; p < heights.size(); ++p) {
      heights[p] = static_cast<float>(terrarium_height(bgra[p * 4 + 2], bgra[p * 4 + 1], bgra[p * 4]));
    }
    any = true;
  }
  return any;
}

bool View3D::load_imagery_tiles() {
  bool any = false;
  const int n = image_block_.n;
  for (int k = 0; k < n * n && k < kImageTilesAcross * kImageTilesAcross; ++k) {
    if (image_have_[static_cast<size_t>(k)]) continue;
    const map::TileKey key{image_block_.z, image_block_.x0 + k % n, image_block_.y0 + k / n,
                           static_cast<int>(base_)};
    std::vector<uint8_t> img, bgra;
    uint32_t w = 0, h = 0;
    if (!tiles_->get(key, img) || !decode_bgra(wic_, img, bgra, w, h)) continue;
    if (w != kTilePx || h != kTilePx) continue;
    gpu_.update_texture(static_cast<uint32_t>((k % n) * kTilePx), static_cast<uint32_t>((k / n) * kTilePx),
                        w, h, bgra.data());
    image_have_[static_cast<size_t>(k)] = true;
    any = true;
  }
  return any;
}

size_t View3D::missing_tiles() const {
  size_t m = 0;
  for (size_t i = 0; i < terrain_tiles_.size(); ++i) m += terrain_tiles_[i].empty() ? 1 : 0;
  for (size_t i = 0; i < image_have_.size(); ++i) m += image_have_[i] ? 0 : 1;
  return m;
}

// Samples the kGrid x kGrid heights from the loaded terrain tiles; points
// in tiles not loaded yet take the mean of those that are.
void View3D::rebuild_heights() {
  const int n = terrain_block_.n;
  const double tiles = static_cast<double>(map::tiles_at(terrain_block_.z));
  std::vector<bool> known(heights_.size(), false);
  double sum = 0.0;
  size_t count = 0;
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) {
      const map::WorldPoint w = grid_point(region_, i, j);
      const int tx = static_cast<int>(std::floor(w.x * tiles)) - terrain_block_.x0;
      const int ty = static_cast<int>(std::floor(w.y * tiles)) - terrain_block_.y0;
      if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;
      const std::vector<float>& tile = terrain_tiles_[static_cast<size_t>(ty) * n + tx];
      if (tile.empty()) continue;
      const int px = std::clamp(static_cast<int>((w.x * tiles - std::floor(w.x * tiles)) * kTilePx), 0, kTilePx - 1);
      const int py = std::clamp(static_cast<int>((w.y * tiles - std::floor(w.y * tiles)) * kTilePx), 0, kTilePx - 1);
      const size_t g = static_cast<size_t>(j) * kGrid + i;
      heights_[g] = tile[static_cast<size_t>(py) * kTilePx + px];
      known[g] = true;
      sum += heights_[g];
      ++count;
    }
  }
  const float fill = count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
  for (size_t g = 0; g < heights_.size(); ++g) {
    if (!known[g]) heights_[g] = fill;
  }
}

void View3D::rebuild_meshes() {
  G_REQUIRE_VOID(!world_.empty());
  const double exag = kExaggerations[exaggeration_idx_];
  std::vector<TerrainVertex> verts;
  std::vector<uint32_t> idx;
  build_terrain(region_, heights_, exag, image_block_, verts, idx);
  gpu_.set_terrain(verts, idx);

  const float half = static_cast<float>(region_.extent_m() * 0.0025);
  const float lift = half * 0.8f + 2.0f;
  const size_t step = std::max<size_t>(1, world_.size() / kMaxRibbonPoints);
  std::vector<Vec3> centre;
  std::vector<uint32_t> rgb;
  for (size_t i = 0; i < world_.size() && centre.size() <= kMaxRibbonPoints; i += step) {
    Vec3 p = to_local(region_, world_[i], height_at(region_, heights_, world_[i]), exag);
    p.y += lift;
    centre.push_back(p);
    rgb.push_back(to_rgb(map::hr_bucket_color(map::hr_bucket(track_.points[i].hr, hr_))));
  }
  std::vector<ColorVertex> ribbon;
  build_ribbon(centre, rgb, half, ribbon);
  build_pin(centre.front(), half * 0.7f, half * 16.0f, kStartRgb, ribbon);
  build_pin(centre.back(), half * 0.7f, half * 16.0f, kFinishRgb, ribbon);
  gpu_.set_track(ribbon);
  const map::WorldPoint c{(region_.min.x + region_.max.x) / 2, (region_.min.y + region_.max.y) / 2};
  orbit_.target.y = static_cast<float>(height_at(region_, heights_, c) * exag);
}

void View3D::reset_camera() {
  orbit_ = Orbit{};
  orbit_.pitch_deg = 40.0f;
  orbit_.distance = static_cast<float>(std::max(region_.extent_m(), 100.0) * 1.1);
  if (!world_.empty()) {
    const map::WorldPoint c{(region_.min.x + region_.max.x) / 2, (region_.min.y + region_.max.y) / 2};
    orbit_.target.y = static_cast<float>(height_at(region_, heights_, c) * kExaggerations[exaggeration_idx_]);
  }
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void View3D::cycle_exaggeration() {
  exaggeration_idx_ = (exaggeration_idx_ + 1) % kExaggerationCount;
  if (!world_.empty()) rebuild_meshes();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void View3D::zoom(int notches) {
  const float extent = static_cast<float>(std::max(region_.extent_m(), 100.0));
  orbit_zoom(orbit_, notches, extent * 0.03f, extent * 5.0f);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void View3D::on_drag(int x, int y) {
  const float dx = static_cast<float>(x - last_.x);
  const float dy = static_cast<float>(y - last_.y);
  last_ = POINT{x, y};
  if (drag_ == 1) {
    orbit_rotate(orbit_, dx, dy);
  } else if (drag_ == 2) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    orbit_pan(orbit_, dx, dy, static_cast<float>(rc.bottom - rc.top));
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void View3D::render() {
  if (!gpu_.ok()) return;
  Camera cam;
  cam.target = orbit_.target;
  cam.eye = eye_of(orbit_);
  cam.near_m = std::max(1.0f, orbit_.distance * 0.005f);
  cam.far_m = orbit_.distance * 4.0f + static_cast<float>(region_.extent_m()) * 4.0f;
  ID2D1RenderTarget* rt = gpu_.draw(cam);
  if (rt != nullptr) draw_overlay(rt);
  if (!gpu_.present()) {
    // Device lost (driver update, GPU reset): start over; tiles come back from the cache.
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    gpu_.reset();
    if (gpu_.init(hwnd_, d2d_, static_cast<uint32_t>(rc.right), static_cast<uint32_t>(rc.bottom)) &&
        !world_.empty()) {
      layout_blocks();
      rebuild_heights();
      rebuild_meshes();
      refresh_tiles();
    }
  }
}

void View3D::text(ID2D1RenderTarget* rt, const std::wstring& str, const D2D1_RECT_F& box,
                  DWRITE_TEXT_ALIGNMENT align) {
  G_REQUIRE_VOID(font_ && brush_);
  font_->SetTextAlignment(align);
  font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  brush_->SetColor(D2D1::ColorF(0x222222));
  rt->DrawTextW(str.c_str(), static_cast<UINT32>(str.size()), font_.Get(), box, brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void View3D::draw_overlay(ID2D1RenderTarget* rt) {
  G_ASSERT(rt != nullptr);
  if (brush_rt_ != rt) {
    brush_.Reset();
    rt->CreateSolidColorBrush(D2D1::ColorF(0x222222), &brush_);
    brush_rt_ = rt;
  }
  G_REQUIRE_VOID(brush_);
  const D2D1_SIZE_F sz = rt->GetSize();
  rt->BeginDraw();
  auto panel = [&](const D2D1_RECT_F& r) {
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
    rt->FillRectangle(r, brush_.Get());
  };
  wchar_t hint[160] = {};
  std::swprintf(hint, 160, L"Drag: orbit    Right-drag: pan    Wheel: zoom    Home: reset    E: height x%.1f",
                kExaggerations[exaggeration_idx_]);
  const D2D1_RECT_F top = D2D1::RectF(0, 0, sz.width, s(kLine + kPad));
  panel(top);
  text(rt, world_.empty() ? L"No GPS track" : hint, D2D1::RectF(s(kPad), 0, sz.width, top.bottom),
       DWRITE_TEXT_ALIGNMENT_LEADING);
  const std::wstring credit =
      plot::widen(map::tile_providers()[base_].attribution + "  |  " +
                  map::tile_providers()[terrain_layer_].attribution);
  const D2D1_RECT_F bottom = D2D1::RectF(0, sz.height - s(kLine + kPad), sz.width, sz.height);
  panel(bottom);
  text(rt, credit, D2D1::RectF(0, bottom.top, sz.width - s(kPad), sz.height), DWRITE_TEXT_ALIGNMENT_TRAILING);
  const size_t missing = world_.empty() ? 0 : missing_tiles();
  if (missing > 0) {
    text(rt, L"Loading " + std::to_wstring(missing) + L" tiles...",
         D2D1::RectF(s(kPad), bottom.top, sz.width / 2, sz.height), DWRITE_TEXT_ALIGNMENT_LEADING);
  }
  if (hr_.valid) {
    const float w = s(140.0f);
    const float x = sz.width - w - s(kPad * 2);
    const float y = top.bottom + s(kPad * 2);
    panel(D2D1::RectF(x - s(kPad), y - s(kPad), x + w + s(kPad), y + s(kLine * 2 + 8.0f)));
    for (size_t b = 0; b < map::kColorBuckets; ++b) {
      const plot::Color c = map::hr_bucket_color(b);
      brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b));
      rt->FillRectangle(D2D1::RectF(x + w * b / map::kColorBuckets, y + s(kLine),
                                    x + w * (b + 1) / map::kColorBuckets, y + s(kLine + 8.0f)),
                        brush_.Get());
    }
    wchar_t lo[16] = {}, hi[16] = {};
    std::swprintf(lo, 16, L"%.0f", hr_.lo);
    std::swprintf(hi, 16, L"%.0f", hr_.hi);
    text(rt, L"Heart rate", D2D1::RectF(x, y, x + w, y + s(kLine)), DWRITE_TEXT_ALIGNMENT_CENTER);
    text(rt, lo, D2D1::RectF(x, y + s(kLine + 8.0f), x + w, y + s(kLine * 2 + 8.0f)), DWRITE_TEXT_ALIGNMENT_LEADING);
    text(rt, hi, D2D1::RectF(x, y + s(kLine + 8.0f), x + w, y + s(kLine * 2 + 8.0f)), DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
  const HRESULT hr = rt->EndDraw();
  G_ASSERT(SUCCEEDED(hr) || hr == D2DERR_RECREATE_TARGET);
}

LRESULT View3D::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (hwnd_ == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);  // still inside CreateWindowExW
  G_ASSERT(hwnd == hwnd_);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      render();
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
      if (wp == kRetryTimer) refresh_tiles();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      drag_ = msg == WM_LBUTTONDOWN ? 1 : 2;
      last_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      SetCapture(hwnd_);
      SetFocus(GetParent(hwnd_));  // keys (Home, E, digits) stay with the main window
      return 0;
    case WM_MOUSEMOVE:
      if (drag_ != 0) on_drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
      if (drag_ != 0) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED: drag_ = 0; return 0;
    case WM_CONTEXTMENU: return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

}  // namespace map3d
