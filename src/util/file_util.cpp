#include "util/file_util.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>

#include "util/assert.h"

namespace gutil {

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  G_REQUIRE_RET(f.is_open(), false);
  const std::streamsize size = f.tellg();
  G_REQUIRE_RET(size >= 0 && static_cast<size_t>(size) <= kMaxFileBytes, false);
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (size > 0) {
    G_REQUIRE_RET(f.read(reinterpret_cast<char*>(out.data()), size).good() || f.eof(),
                  false);
  }
  return true;
}

bool read_text_file(const std::filesystem::path& p, std::string& out) {
  std::vector<uint8_t> bytes;
  G_REQUIRE_RET(read_file(p, bytes), false);
  out.assign(bytes.begin(), bytes.end());
  return true;
}

bool write_file(const std::filesystem::path& p, const void* data, size_t len) {
  G_ASSERT(data != nullptr || len == 0);
  std::error_code ec;
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  G_REQUIRE_RET(f.is_open(), false);
  if (len > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
  return f.good();
}

bool write_text_file(const std::filesystem::path& p, const std::string& text) {
  return write_file(p, text.data(), text.size());
}

std::filesystem::path app_data_dir() {
  PWSTR raw = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
  G_REQUIRE_RET(SUCCEEDED(hr) && raw != nullptr, std::filesystem::path());
  std::filesystem::path dir(raw);
  CoTaskMemFree(raw);
  dir /= L"GarminSync";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  G_REQUIRE_RET(!ec || std::filesystem::exists(dir), std::filesystem::path());
  return dir;
}

}  // namespace gutil
