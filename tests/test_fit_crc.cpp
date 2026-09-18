#include <catch2/catch_test_macros.hpp>

#include "fit/fit_crc.h"

// The FIT CRC is CRC-16/ARC (poly 0x8005 reflected = 0xA001, init 0,
// no xor-out). Its standard check value over "123456789" is 0xBB3D.
TEST_CASE("fit crc matches CRC-16/ARC check value") {
  const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  REQUIRE(fit::crc_compute(data, sizeof(data)) == 0xBB3D);
}

TEST_CASE("fit crc of empty input is zero") { REQUIRE(fit::crc_compute(nullptr, 0) == 0); }

TEST_CASE("fit crc incremental equals one-shot") {
  const uint8_t data[] = {0x0E, 0x10, 0x8B, 0x08, 0x12, 0x00, 0x00, 0x00, '.', 'F', 'I', 'T'};
  uint16_t crc = 0;
  for (const uint8_t b : data) crc = fit::crc_update(crc, b);
  REQUIRE(crc == fit::crc_compute(data, sizeof(data)));
}
