// PNG / JPEG tile -> 32-bit BGRA pixels via WIC.
#pragma once
#include <wincodec.h>

#include <cstdint>
#include <vector>

namespace map3d {

// True and `bgra` (width*height*4 bytes) filled on success.
bool decode_bgra(IWICImagingFactory* wic, const std::vector<uint8_t>& encoded,
                 std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height);

}  // namespace map3d
