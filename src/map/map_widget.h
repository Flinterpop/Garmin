// Direct2D map: OSM tiles under a GPS track coloured by heart rate, with
// start/finish/lap markers, hover readout, scale bar and attribution.
// Hosting contract matches PlotWidget (rect, mouse events, render).
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>

#include "map/mercator.h"
#include "map/tile_cache.h"
#include "plot/plot_types.h"

namespace map {

struct TrackPoint {
  double lat = 0.0;
  double lon = 0.0;
  double elapsed_s = 0.0;
  double hr = 0.0;         // 0 = unknown
  double speed_mps = 0.0;  // 0 = unknown
  double alt_m = 0.0;
  double dist_m = 0.0;     // cumulative along the track
};

struct Track {
  std::string title;
  std::vector<TrackPoint> points;
  std::vector<size_t> lap_starts;  // indices into points
};

constexpr size_t kMaxTrackPoints = 200000;
constexpr size_t kColorBuckets = 16;
constexpr size_t kMaxBitmaps = 512;

// Index of the point whose elapsed_s is closest to `t`; points must be in
// time order. SIZE_MAX when `points` is empty.
size_t index_at_time(const std::vector<TrackPoint>& points, double t);

class MapWidget {
 public:
  MapWidget() = default;
  MapWidget(const MapWidget&) = delete;
  MapWidget& operator=(const MapWidget&) = delete;

  bool init(ID2D1Factory* d2d, IDWriteFactory* dwrite, IWICImagingFactory* wic,
            TileCache* tiles);
  void set_dpi_scale(float scale);
  void set_rect(const D2D1_RECT_F& rect) { rect_ = rect; }

  // Base provider and overlays (indices into tile_providers()). Zoom is
  // clamped to what the base provider serves.
  void set_layers(size_t base, const std::vector<size_t>& overlays);

  void set_track(Track t);  // fits the view to the track
  bool has_track() const { return !track_.points.empty(); }
  void fit();

  void zoom_step(float px, float py, int delta);  // +1 in, -1 out, around the cursor
  void pan_pixels(float dx, float dy);
  void set_hover(float px, float py, bool inside);

  // Linked cursor for a host that shows the same activity on a time axis:
  // marks where the athlete was at `elapsed_s` (hidden while hovering the map).
  void set_cursor_time(double elapsed_s, bool show);
  // Elapsed time of the track point under the mouse, if there is one.
  bool hovered_time(double& elapsed_s) const;

  // Call when the render target was recreated: cached tile bitmaps die with it.
  void drop_bitmaps();
  void render(ID2D1RenderTarget* rt);

 private:
  double world_px() const;  // pixels per world unit at the current zoom
  D2D1_POINT_2F to_px(const WorldPoint& w) const;
  WorldPoint to_world_px(float px, float py) const;
  void draw_tiles(ID2D1RenderTarget* rt);
  void draw_layer(ID2D1RenderTarget* rt, size_t layer, bool placeholder);
  int max_zoom() const;
  std::wstring attribution() const;
  void draw_track(ID2D1RenderTarget* rt);
  void draw_markers(ID2D1RenderTarget* rt);
  void draw_legend(ID2D1RenderTarget* rt);
  void draw_scale_bar(ID2D1RenderTarget* rt);
  void draw_attribution(ID2D1RenderTarget* rt);
  void draw_hover(ID2D1RenderTarget* rt);
  void draw_cursor(ID2D1RenderTarget* rt);
  size_t nearest_point(float px, float py, float max_dist_px) const;
  size_t bucket_of(double hr) const;
  plot::Color bucket_color(size_t b) const;
  ID2D1Bitmap* tile_bitmap(ID2D1RenderTarget* rt, const TileKey& key);
  void text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& box,
            DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign, const plot::Color& c,
            bool bold = false);
  float measure(const std::wstring& s, bool bold);
  float s(float css) const { return css * scale_; }
  void ensure_brush(ID2D1RenderTarget* rt);

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_bold_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  ID2D1RenderTarget* brush_rt_ = nullptr;
  TileCache* tiles_ = nullptr;

  std::map<TileKey, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
  ID2D1RenderTarget* bitmap_rt_ = nullptr;

  Track track_;
  std::vector<WorldPoint> world_;  // per point
  double hr_lo_ = 0.0;
  double hr_hi_ = 0.0;
  bool has_hr_ = false;

  D2D1_RECT_F rect_{};
  float scale_ = 1.0f;
  WorldPoint center_;
  int zoom_ = 12;
  bool hover_ = false;
  float hover_px_ = 0.0f;
  float hover_py_ = 0.0f;
  bool cursor_ = false;
  double cursor_s_ = 0.0;
  size_t base_ = 0;
  std::vector<size_t> overlays_;
};

}  // namespace map
