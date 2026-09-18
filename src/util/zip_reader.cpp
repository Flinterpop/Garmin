#include "util/zip_reader.h"

#include <zlib.h>

#include <cstring>

#include "util/assert.h"

namespace gutil {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr size_t kEocdSize = 22;
constexpr size_t kCentralFixed = 46;
constexpr size_t kLocalFixed = 30;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflate = 8;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Locates the end-of-central-directory record, scanning back over an
// optional archive comment (bounded to 64 KiB by the format).
bool find_eocd(const std::vector<uint8_t>& a, size_t& eocd_pos) {
  G_REQUIRE_RET(a.size() >= kEocdSize, false);
  const size_t max_back = std::min<size_t>(a.size() - kEocdSize, 65535u);
  for (size_t back = 0; back <= max_back; ++back) {
    const size_t pos = a.size() - kEocdSize - back;
    if (rd32(&a[pos]) == kEocdSig) {
      eocd_pos = pos;
      return true;
    }
  }
  return false;
}

bool inflate_raw(const uint8_t* src, size_t src_len, size_t dst_len,
                 std::vector<uint8_t>& dst) {
  G_REQUIRE_RET(dst_len <= kZipMaxEntryBytes, false);
  dst.assign(dst_len, 0);
  z_stream zs{};
  G_REQUIRE_RET(inflateInit2(&zs, -MAX_WBITS) == Z_OK, false);
  zs.next_in = const_cast<Bytef*>(src);
  zs.avail_in = static_cast<uInt>(src_len);
  zs.next_out = dst.data();
  zs.avail_out = static_cast<uInt>(dst_len);
  const int rc = inflate(&zs, Z_FINISH);
  const bool complete = (rc == Z_STREAM_END) && (zs.total_out == dst_len);
  inflateEnd(&zs);
  return complete;
}

bool extract_one(const std::vector<uint8_t>& a, const uint8_t* cd, ZipEntry& out,
                 std::string& err) {
  const uint16_t method = rd16(cd + 10);
  const uint32_t comp_size = rd32(cd + 20);
  const uint32_t uncomp_size = rd32(cd + 24);
  const uint16_t name_len = rd16(cd + 28);
  const uint32_t local_off = rd32(cd + 42);

  out.name.assign(reinterpret_cast<const char*>(cd + kCentralFixed), name_len);

  if (static_cast<size_t>(local_off) + kLocalFixed > a.size() ||
      rd32(&a[local_off]) != kLocalSig) {
    err = "zip: bad local header for " + out.name;
    return false;
  }
  const uint8_t* lh = &a[local_off];
  const size_t data_off = static_cast<size_t>(local_off) + kLocalFixed + rd16(lh + 26) +
                          rd16(lh + 28);
  if (data_off + comp_size > a.size()) {
    err = "zip: entry data out of range for " + out.name;
    return false;
  }
  if (uncomp_size > kZipMaxEntryBytes) {
    err = "zip: entry too large: " + out.name;
    return false;
  }
  const uint8_t* src = &a[data_off];
  if (method == kMethodStored) {
    if (comp_size != uncomp_size) {
      err = "zip: stored size mismatch for " + out.name;
      return false;
    }
    out.data.assign(src, src + comp_size);
    return true;
  }
  if (method == kMethodDeflate) {
    if (!inflate_raw(src, comp_size, uncomp_size, out.data)) {
      err = "zip: inflate failed for " + out.name;
      return false;
    }
    return true;
  }
  err = "zip: unsupported compression method for " + out.name;
  return false;
}

}  // namespace

bool zip_extract_all(const std::vector<uint8_t>& archive, std::vector<ZipEntry>& out,
                     std::string& err) {
  out.clear();
  size_t eocd = 0;
  if (!find_eocd(archive, eocd)) {
    err = "zip: end-of-central-directory not found";
    return false;
  }
  const uint16_t total = rd16(&archive[eocd + 10]);
  const uint32_t cd_size = rd32(&archive[eocd + 12]);
  const uint32_t cd_off = rd32(&archive[eocd + 16]);
  if (static_cast<size_t>(cd_off) + cd_size > archive.size() || total > kZipMaxEntries) {
    err = "zip: central directory out of range";
    return false;
  }

  size_t pos = cd_off;
  const size_t cd_end = static_cast<size_t>(cd_off) + cd_size;
  for (uint16_t i = 0; i < total; ++i) {
    if (pos + kCentralFixed > cd_end || rd32(&archive[pos]) != kCentralSig) {
      err = "zip: bad central directory entry";
      return false;
    }
    const uint8_t* cd = &archive[pos];
    const size_t rec_len = kCentralFixed + rd16(cd + 28) + rd16(cd + 30) + rd16(cd + 32);
    if (pos + rec_len > cd_end) {
      err = "zip: central directory entry overruns";
      return false;
    }
    const uint32_t ext_attr = rd32(cd + 38);
    const bool is_dir = (rd16(cd + 28) > 0 && cd[kCentralFixed + rd16(cd + 28) - 1] == '/') ||
                        (ext_attr & 0x10u) != 0;
    if (!is_dir) {
      ZipEntry entry;
      if (!extract_one(archive, cd, entry, err)) return false;
      out.push_back(std::move(entry));
    }
    pos += rec_len;
  }
  G_ASSERT(out.size() <= kZipMaxEntries);
  return true;
}

}  // namespace gutil
