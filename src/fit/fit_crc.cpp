#include "fit/fit_crc.h"

#include "util/assert.h"

namespace fit {

namespace {
constexpr uint16_t kCrcTable[16] = {0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00,
                                    0x2800, 0xE401, 0xA001, 0x6C00, 0x7800, 0xB401,
                                    0x5000, 0x9C01, 0x8801, 0x4400};
}  // namespace

uint16_t crc_update(uint16_t crc, uint8_t byte) {
  // Low nibble.
  uint16_t tmp = kCrcTable[crc & 0xF];
  crc = static_cast<uint16_t>((crc >> 4) & 0x0FFF);
  crc = static_cast<uint16_t>(crc ^ tmp ^ kCrcTable[byte & 0xF]);
  // High nibble.
  tmp = kCrcTable[crc & 0xF];
  crc = static_cast<uint16_t>((crc >> 4) & 0x0FFF);
  crc = static_cast<uint16_t>(crc ^ tmp ^ kCrcTable[(byte >> 4) & 0xF]);
  return crc;
}

uint16_t crc_compute(const uint8_t* data, size_t len) {
  G_ASSERT(data != nullptr || len == 0);
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) crc = crc_update(crc, data[i]);
  return crc;
}

}  // namespace fit
