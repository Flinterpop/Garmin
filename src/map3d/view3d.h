// 3D track view: a child window over gview's plot area showing the terrain
// around an activity (AWS Terrain Tiles heights, the selected map layer
// draped over them) and the GPS track as a heart-rate-coloured ribbon.
// Left-drag orbits, right-drag pans; the host forwards wheel and keys.
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <vector>

#include "map/hr_color.h"
#include "map/map_widget.h"
#include "map/tile_cache.h"
#include "map3d/renderer.h"
#include "map3d/terrain.h"

namespace map3d {

constexpr int kTerrainTilesAcross = 3;   // heights: 129 x 129 samples need no more
constexpr int kImageTilesAcross = 8;     // imagery: texture at most 2048 x 2048
constexpr size_t kMaxRibbonPoints = 20000;
constexpr int kTilePx = 256;

class View3D {
 public:
  View3D() = default;
  ~View3D();
  View3D(const View3D&) = delete;
  View3D& operator=(const View3D&) = delete;

  bool create(HWND parent, ID2D1Factory* d2d, IDWriteFactory* dwrite, IWICImagingFactory* wic,
              map::TileCache* tiles);
  void show(bool on);
  bool visible() const;
  void set_rect(const RECT& r);  // parent client coordinates
  void set_dpi_scale(float scale);

  void set_track(map::Track t);           // resets the camera
  void set_base_layer(size_t provider);   // re-drapes the imagery
  void on_tiles_ready();                  // the tile cache has news
  void reset_camera();
  void cycle_exaggeration();
  void zoom(int notches);

  // From the child's window procedure. Takes the HWND because the first
  // messages (WM_CREATE, ...) arrive before CreateWindowExW has returned it.
  LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

 private:
  void layout_blocks();
  void refresh_tiles();
  bool load_terrain_tiles();
  bool load_imagery_tiles();
  size_t missing_tiles() const;
  void rebuild_heights();
  void rebuild_meshes();
  void render();
  void draw_overlay(ID2D1RenderTarget* rt);
  void text(ID2D1RenderTarget* rt, const std::wstring& s, const D2D1_RECT_F& box,
            DWRITE_TEXT_ALIGNMENT align);
  void on_drag(int x, int y);
  float s(float css) const { return css * scale_; }

  HWND hwnd_ = nullptr;
  Renderer gpu_;
  ID2D1Factory* d2d_ = nullptr;
  IDWriteFactory* dwrite_ = nullptr;
  IWICImagingFactory* wic_ = nullptr;
  map::TileCache* tiles_ = nullptr;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> font_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  ID2D1RenderTarget* brush_rt_ = nullptr;
  float scale_ = 1.0f;

  map::Track track_;
  std::vector<map::WorldPoint> world_;
  map::HrRange hr_;
  Region region_;
  TileBlock terrain_block_;
  TileBlock image_block_;
  size_t base_ = 0;
  size_t terrain_layer_ = 0;
  std::vector<std::vector<float>> terrain_tiles_;  // per block tile: 256 x 256 heights, empty = not yet
  std::vector<bool> image_have_;
  std::vector<float> heights_;
  size_t exaggeration_idx_ = 1;
  Orbit orbit_;

  int drag_ = 0;  // 0 none, 1 orbit, 2 pan
  POINT last_{};
  bool timer_on_ = false;
};

}  // namespace map3d
