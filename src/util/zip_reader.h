#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gutil {

struct ZipEntry {
  std::string name;
  std::vector<uint8_t> data;
};

// Extracts every file entry of an in-memory ZIP archive (methods: stored and
// deflate). Garmin download-service responses are small archives holding one
// or a handful of FIT files, so everything is decoded eagerly. Returns false
// with a message on malformed input. kMaxEntries bounds the loop.
constexpr size_t kZipMaxEntries = 512;
constexpr size_t kZipMaxEntryBytes = 64u * 1024u * 1024u;

bool zip_extract_all(const std::vector<uint8_t>& archive, std::vector<ZipEntry>& out,
                     std::string& err);

}  // namespace gutil
