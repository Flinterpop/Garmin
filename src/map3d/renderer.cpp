#include "map3d/renderer.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>

#include "util/assert.h"

namespace map3d {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kFovDeg = 45.0f;  // must match orbit_pan's assumption in terrain.cpp
constexpr float kSky[4] = {0.86f, 0.90f, 0.94f, 1.0f};
constexpr uint32_t kMaxTextureSide = 4096;
constexpr uint8_t kPlaceholderGrey = 0xDD;

// Terrain: textured, lit by one sun. Track: flat colour. Matrices arrive
// transposed so HLSL's default column-major packing matches DirectXMath.
constexpr char kShaders[] = R"(
cbuffer Frame : register(b0) { float4x4 view_proj; float4 sun; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct TIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };
struct TOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };
TOut vs_terrain(TIn i) { TOut o; o.pos = mul(float4(i.pos, 1), view_proj); o.nrm = i.nrm; o.uv = i.uv; return o; }
float4 ps_terrain(TOut i) : SV_Target {
  float3 c = tex.Sample(samp, i.uv).rgb;
  float d = saturate(dot(normalize(i.nrm), -sun.xyz));
  return float4(saturate(c * (0.55 + 0.6 * d)), 1);
}

struct CIn { float3 pos : POSITION; float4 col : COLOR; };
struct COut { float4 pos : SV_POSITION; float4 col : COLOR; };
COut vs_track(CIn i) { COut o; o.pos = mul(float4(i.pos, 1), view_proj); o.col = i.col; return o; }
float4 ps_track(COut i) : SV_Target { return i.col; }
)";

struct FrameConstants {
  DirectX::XMFLOAT4X4 view_proj;
  DirectX::XMFLOAT4 sun;
};

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "map3d", nullptr, nullptr, entry,
                                target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
  if (FAILED(hr) && errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
  return SUCCEEDED(hr);
}

template <typename T>
bool make_buffer(ID3D11Device* dev, UINT bind, const std::vector<T>& data, ComPtr<ID3D11Buffer>& out) {
  out.Reset();
  if (data.empty()) return true;
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = static_cast<UINT>(data.size() * sizeof(T));
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = bind;
  D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
  return SUCCEEDED(dev->CreateBuffer(&d, &init, &out));
}

}  // namespace

bool Renderer::init(HWND hwnd, ID2D1Factory* d2d, uint32_t width, uint32_t height) {
  G_ASSERT(hwnd != nullptr && d2d != nullptr);
  d2d_ = d2d;
  width_ = std::max<uint32_t>(width, 1);
  height_ = std::max<uint32_t>(height, 1);
  if (!create_device() || !create_swap_chain(hwnd, width_, height_)) return false;
  return create_targets() && create_pipeline() && create_states();
}

void Renderer::reset() {
  release_targets();
  srv_.Reset();
  texture_.Reset();
  terrain_vb_.Reset();
  terrain_ib_.Reset();
  track_vb_.Reset();
  terrain_index_count_ = 0;
  track_vertex_count_ = 0;
  cb_.Reset();
  raster_.Reset();
  depth_.Reset();
  sampler_.Reset();
  il_terrain_.Reset();
  il_track_.Reset();
  vs_terrain_.Reset();
  vs_track_.Reset();
  ps_terrain_.Reset();
  ps_track_.Reset();
  swap_.Reset();
  ctx_.Reset();
  device_.Reset();
  G_ASSERT(!ok());
}

bool Renderer::create_device() {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // needed for the Direct2D overlay
  for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, flags, levels, ARRAYSIZE(levels),
                                    D3D11_SDK_VERSION, &device_, nullptr, &ctx_))) {
      return true;
    }
  }
  return false;
}

bool Renderer::create_swap_chain(HWND hwnd, uint32_t width, uint32_t height) {
  ComPtr<IDXGIDevice> dxgi_dev;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  G_REQUIRE_RET(SUCCEEDED(device_.As(&dxgi_dev)), false);
  G_REQUIRE_RET(SUCCEEDED(dxgi_dev->GetAdapter(&adapter)), false);
  G_REQUIRE_RET(SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))), false);
  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = width;
  d.Height = height;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  d.Scaling = DXGI_SCALING_STRETCH;
  G_REQUIRE_RET(SUCCEEDED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &d, nullptr, nullptr,
                                                          &swap_)),
                false);
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  return true;
}

bool Renderer::create_targets() {
  ComPtr<ID3D11Texture2D> back;
  G_REQUIRE_RET(SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back))), false);
  G_REQUIRE_RET(SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_)), false);
  D3D11_TEXTURE2D_DESC dd{};
  dd.Width = width_;
  dd.Height = height_;
  dd.MipLevels = 1;
  dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_D32_FLOAT;
  dd.SampleDesc.Count = 1;
  dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> depth;
  G_REQUIRE_RET(SUCCEEDED(device_->CreateTexture2D(&dd, nullptr, &depth)), false);
  G_REQUIRE_RET(SUCCEEDED(device_->CreateDepthStencilView(depth.Get(), nullptr, &dsv_)), false);
  ComPtr<IDXGISurface> surface;
  G_REQUIRE_RET(SUCCEEDED(back.As(&surface)), false);
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  return SUCCEEDED(d2d_->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &d2d_rt_));
}

void Renderer::release_targets() {
  if (ctx_) ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  d2d_rt_.Reset();
  rtv_.Reset();
  dsv_.Reset();
}

bool Renderer::resize(uint32_t width, uint32_t height) {
  G_REQUIRE_RET(ok(), false);
  width = std::max<uint32_t>(width, 1);
  height = std::max<uint32_t>(height, 1);
  if (width == width_ && height == height_) return true;
  release_targets();
  width_ = width;
  height_ = height;
  G_REQUIRE_RET(SUCCEEDED(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)), false);
  return create_targets();
}

bool Renderer::create_pipeline() {
  ComPtr<ID3DBlob> vt, pt, vc, pc;
  G_REQUIRE_RET(compile("vs_terrain", "vs_4_0", vt) && compile("ps_terrain", "ps_4_0", pt), false);
  G_REQUIRE_RET(compile("vs_track", "vs_4_0", vc) && compile("ps_track", "ps_4_0", pc), false);
  auto* dev = device_.Get();
  G_REQUIRE_RET(SUCCEEDED(dev->CreateVertexShader(vt->GetBufferPointer(), vt->GetBufferSize(), nullptr, &vs_terrain_)), false);
  G_REQUIRE_RET(SUCCEEDED(dev->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, &ps_terrain_)), false);
  G_REQUIRE_RET(SUCCEEDED(dev->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs_track_)), false);
  G_REQUIRE_RET(SUCCEEDED(dev->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps_track_)), false);
  const D3D11_INPUT_ELEMENT_DESC terrain[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(TerrainVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(TerrainVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TerrainVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0}};
  const D3D11_INPUT_ELEMENT_DESC track[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ColorVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ColorVertex, r), D3D11_INPUT_PER_VERTEX_DATA, 0}};
  G_REQUIRE_RET(SUCCEEDED(dev->CreateInputLayout(terrain, ARRAYSIZE(terrain), vt->GetBufferPointer(), vt->GetBufferSize(), &il_terrain_)), false);
  G_REQUIRE_RET(SUCCEEDED(dev->CreateInputLayout(track, ARRAYSIZE(track), vc->GetBufferPointer(), vc->GetBufferSize(), &il_track_)), false);
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(FrameConstants);
  cbd.Usage = D3D11_USAGE_DEFAULT;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  return SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &cb_));
}

bool Renderer::create_states() {
  D3D11_RASTERIZER_DESC r{};
  r.FillMode = D3D11_FILL_SOLID;
  r.CullMode = D3D11_CULL_NONE;  // the ribbon and pins are seen from both sides
  r.DepthClipEnable = TRUE;
  G_REQUIRE_RET(SUCCEEDED(device_->CreateRasterizerState(&r, &raster_)), false);
  D3D11_DEPTH_STENCIL_DESC d{};
  d.DepthEnable = TRUE;
  d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  G_REQUIRE_RET(SUCCEEDED(device_->CreateDepthStencilState(&d, &depth_)), false);
  D3D11_SAMPLER_DESC s{};
  s.Filter = D3D11_FILTER_ANISOTROPIC;  // the ground is seen at grazing angles
  s.MaxAnisotropy = 8;
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  s.MaxLOD = D3D11_FLOAT32_MAX;
  return SUCCEEDED(device_->CreateSamplerState(&s, &sampler_));
}

void Renderer::set_terrain(const std::vector<TerrainVertex>& v, const std::vector<uint32_t>& idx) {
  G_REQUIRE_VOID(ok());
  const bool built = make_buffer(device_.Get(), D3D11_BIND_VERTEX_BUFFER, v, terrain_vb_) &&
                     make_buffer(device_.Get(), D3D11_BIND_INDEX_BUFFER, idx, terrain_ib_);
  terrain_index_count_ = built && terrain_vb_ ? static_cast<uint32_t>(idx.size()) : 0;
}

void Renderer::set_track(const std::vector<ColorVertex>& v) {
  G_REQUIRE_VOID(ok());
  const bool built = make_buffer(device_.Get(), D3D11_BIND_VERTEX_BUFFER, v, track_vb_);
  track_vertex_count_ = built && track_vb_ ? static_cast<uint32_t>(v.size()) : 0;
}

bool Renderer::create_texture(uint32_t side) {
  G_REQUIRE_RET(ok() && side > 0 && side <= kMaxTextureSide, false);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = d.Height = side;
  d.MipLevels = 0;  // full chain, generated after each tile
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // RT: GenerateMips needs it
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  srv_.Reset();
  texture_.Reset();
  G_REQUIRE_RET(SUCCEEDED(device_->CreateTexture2D(&d, nullptr, &texture_)), false);
  G_REQUIRE_RET(SUCCEEDED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_)), false);
  const std::vector<uint8_t> grey(static_cast<size_t>(side) * side * 4, kPlaceholderGrey);
  ctx_->UpdateSubresource(texture_.Get(), 0, nullptr, grey.data(), side * 4, 0);
  mips_dirty_ = true;
  return true;
}

void Renderer::update_texture(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* bgra) {
  G_REQUIRE_VOID(texture_ && bgra != nullptr);
  const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
  ctx_->UpdateSubresource(texture_.Get(), 0, &box, bgra, w * 4, 0);
  mips_dirty_ = true;
}

void Renderer::upload_mips() {
  if (!mips_dirty_ || !srv_) return;
  ctx_->GenerateMips(srv_.Get());
  mips_dirty_ = false;
}

ID2D1RenderTarget* Renderer::draw(const Camera& cam) {
  G_REQUIRE_RET(ok() && rtv_ && dsv_, nullptr);
  using namespace DirectX;
  upload_mips();
  const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
  ctx_->RSSetViewports(1, &vp);
  ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
  ctx_->ClearRenderTargetView(rtv_.Get(), kSky);
  ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

  const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(cam.eye.x, cam.eye.y, cam.eye.z, 1),
                                         XMVectorSet(cam.target.x, cam.target.y, cam.target.z, 1),
                                         XMVectorSet(0, 1, 0, 0));
  const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(kFovDeg),
                                                 static_cast<float>(width_) / height_, cam.near_m, cam.far_m);
  FrameConstants fc{};
  XMStoreFloat4x4(&fc.view_proj, XMMatrixTranspose(view * proj));
  XMStoreFloat4(&fc.sun, XMVector3Normalize(XMVectorSet(-0.45f, -1.0f, 0.35f, 0)));
  ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &fc, 0, 0);
  ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetDepthStencilState(depth_.Get(), 0);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (terrain_index_count_ > 0 && srv_) {
    const UINT stride = sizeof(TerrainVertex), offset = 0;
    ctx_->IASetInputLayout(il_terrain_.Get());
    ctx_->IASetVertexBuffers(0, 1, terrain_vb_.GetAddressOf(), &stride, &offset);
    ctx_->IASetIndexBuffer(terrain_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx_->VSSetShader(vs_terrain_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_terrain_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->DrawIndexed(terrain_index_count_, 0, 0);
  }
  if (track_vertex_count_ > 0) {
    const UINT stride = sizeof(ColorVertex), offset = 0;
    ctx_->IASetInputLayout(il_track_.Get());
    ctx_->IASetVertexBuffers(0, 1, track_vb_.GetAddressOf(), &stride, &offset);
    ctx_->VSSetShader(vs_track_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_track_.Get(), nullptr, 0);
    ctx_->Draw(track_vertex_count_, 0);
  }
  return d2d_rt_.Get();
}

bool Renderer::present() {
  G_REQUIRE_RET(ok(), false);
  const HRESULT hr = swap_->Present(1, 0);
  return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

}  // namespace map3d
