#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <string>
#include <vector>

#include "util/zip_reader.h"

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

std::vector<uint8_t> raw_deflate(const std::vector<uint8_t>& in) {
  z_stream zs{};
  REQUIRE(deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                       Z_DEFAULT_STRATEGY) == Z_OK);
  std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(in.size())));
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

struct Entry {
  std::string name;
  std::vector<uint8_t> data;
  bool deflate = false;
};

// Minimal ZIP writer (local headers + central directory + EOCD).
std::vector<uint8_t> make_zip(const std::vector<Entry>& entries) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> cd;
  for (const Entry& e : entries) {
    const std::vector<uint8_t> payload = e.deflate ? raw_deflate(e.data) : e.data;
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0, e.data.data(), static_cast<uInt>(e.data.size())));
    const uint32_t local_off = static_cast<uint32_t>(out.size());
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0);
    put16(out, e.deflate ? 8 : 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, crc);
    put32(out, static_cast<uint32_t>(payload.size()));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);
    out.insert(out.end(), e.name.begin(), e.name.end());
    out.insert(out.end(), payload.begin(), payload.end());

    put32(cd, 0x02014b50);
    put16(cd, 20);
    put16(cd, 20);
    put16(cd, 0);
    put16(cd, e.deflate ? 8 : 0);
    put16(cd, 0);
    put16(cd, 0);
    put32(cd, crc);
    put32(cd, static_cast<uint32_t>(payload.size()));
    put32(cd, static_cast<uint32_t>(e.data.size()));
    put16(cd, static_cast<uint16_t>(e.name.size()));
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put32(cd, 0);
    put32(cd, local_off);
    cd.insert(cd.end(), e.name.begin(), e.name.end());
  }
  const uint32_t cd_off = static_cast<uint32_t>(out.size());
  out.insert(out.end(), cd.begin(), cd.end());
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<uint16_t>(entries.size()));
  put16(out, static_cast<uint16_t>(entries.size()));
  put32(out, static_cast<uint32_t>(cd.size()));
  put32(out, cd_off);
  put16(out, 0);
  return out;
}

}  // namespace

TEST_CASE("zip reader extracts stored and deflated entries") {
  std::vector<uint8_t> big(5000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i % 7);
  const std::vector<Entry> entries = {
      {"123_ACTIVITY.fit", {1, 2, 3, 4}, false},
      {"dir/", {}, false},
      {"2024-01-01/MONITOR.FIT", big, true},
  };
  std::vector<gutil::ZipEntry> out;
  std::string err;
  REQUIRE(gutil::zip_extract_all(make_zip(entries), out, err));
  REQUIRE(out.size() == 2);
  CHECK(out[0].name == "123_ACTIVITY.fit");
  CHECK(out[0].data == std::vector<uint8_t>{1, 2, 3, 4});
  CHECK(out[1].name == "2024-01-01/MONITOR.FIT");
  CHECK(out[1].data == big);
}

TEST_CASE("zip reader rejects garbage") {
  std::vector<gutil::ZipEntry> out;
  std::string err;
  const std::vector<uint8_t> junk(40, 0xAB);
  CHECK_FALSE(gutil::zip_extract_all(junk, out, err));
  CHECK_FALSE(err.empty());
  std::vector<uint8_t> z = make_zip({{"a.fit", {9, 9, 9}, false}});
  z[0] = 0;  // break the local header signature
  CHECK_FALSE(gutil::zip_extract_all(z, out, err));
}
