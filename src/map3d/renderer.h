// Direct3D 11 renderer for the 3D track view: a textured, lit terrain mesh
// and unlit coloured triangles (track ribbon, pins), on a flip-model swap
// chain, with a Direct2D target on the same back buffer for the overlay.
// Knows nothing about tiles or tracks; the view feeds it vertices and pixels.
#pragma once
#include <d2d1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "map3d/terrain.h"

namespace map3d {

struct Camera {
  Vec3 eye;
  Vec3 target;
  float near_m = 1.0f;
  float far_m = 10000.0f;
};

class Renderer {
 public:
  Renderer() = default;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool init(HWND hwnd, ID2D1Factory* d2d, uint32_t width, uint32_t height);
  bool ok() const { return device_ != nullptr && swap_ != nullptr; }
  void reset();  // releases everything, e.g. after the device was lost; init() again after
  bool resize(uint32_t width, uint32_t height);

  void set_terrain(const std::vector<TerrainVertex>& v, const std::vector<uint32_t>& idx);
  void set_track(const std::vector<ColorVertex>& v);
  // (Re)creates the terrain texture, side x side BGRA, grey until tiles arrive.
  bool create_texture(uint32_t side);
  // Copies one 256 x 256 tile into the texture at (x, y) pixels.
  void update_texture(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* bgra);

  // Draws the scene; returns the D2D target for the overlay (valid until present()).
  ID2D1RenderTarget* draw(const Camera& cam);
  // False when the device was lost; the caller re-inits and re-uploads.
  bool present();

 private:
  bool create_device();
  bool create_swap_chain(HWND hwnd, uint32_t width, uint32_t height);
  bool create_targets();
  void release_targets();
  bool create_pipeline();
  bool create_states();
  void upload_mips();

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> d2d_rt_;
  ID2D1Factory* d2d_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_terrain_, vs_track_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_terrain_, ps_track_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> il_terrain_, il_track_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> terrain_vb_, terrain_ib_, track_vb_;
  uint32_t terrain_index_count_ = 0;
  uint32_t track_vertex_count_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
  bool mips_dirty_ = false;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace map3d
