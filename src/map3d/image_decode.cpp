#include "map3d/image_decode.h"

#include <wrl/client.h>

#include "util/assert.h"

namespace map3d {

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kMaxSide = 1024;  // tiles are 256 or 512 px
}

bool decode_bgra(IWICImagingFactory* wic, const std::vector<uint8_t>& encoded,
                 std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height) {
  G_REQUIRE_RET(wic != nullptr && !encoded.empty(), false);
  ComPtr<IWICStream> stream;
  G_REQUIRE_RET(SUCCEEDED(wic->CreateStream(&stream)), false);
  G_REQUIRE_RET(SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                                      static_cast<DWORD>(encoded.size()))),
                false);
  ComPtr<IWICBitmapDecoder> decoder;
  G_REQUIRE_RET(SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                                       WICDecodeMetadataCacheOnLoad, &decoder)),
                false);
  ComPtr<IWICBitmapFrameDecode> frame;
  G_REQUIRE_RET(SUCCEEDED(decoder->GetFrame(0, &frame)), false);
  ComPtr<IWICFormatConverter> conv;
  G_REQUIRE_RET(SUCCEEDED(wic->CreateFormatConverter(&conv)), false);
  // Straight (not premultiplied) BGRA: terrain tiles encode heights in the
  // colour channels and must come through bit-exact.
  G_REQUIRE_RET(SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom)),
                false);
  G_REQUIRE_RET(SUCCEEDED(conv->GetSize(&width, &height)), false);
  G_REQUIRE_RET(width > 0 && height > 0 && width <= kMaxSide && height <= kMaxSide, false);
  bgra.resize(static_cast<size_t>(width) * height * 4);
  G_REQUIRE_RET(SUCCEEDED(conv->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()),
                                           bgra.data())),
                false);
  return true;
}

}  // namespace map3d
