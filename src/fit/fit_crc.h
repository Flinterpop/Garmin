#pragma once
#include <cstddef>
#include <cstdint>

namespace fit {

// FIT CRC-16 (reflected polynomial 0xA001, init 0) as specified by Garmin.
uint16_t crc_update(uint16_t crc, uint8_t byte);
uint16_t crc_compute(const uint8_t* data, size_t len);

}  // namespace fit
