#include "util/file_util.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
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

std::filesystem::path exe_dir() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  G_REQUIRE_RET(n > 0 && n < MAX_PATH, std::filesystem::path());
  const std::filesystem::path dir = std::filesystem::path(std::wstring(buf, n)).parent_path();
  G_ASSERT(!dir.empty());
  return dir;
}

std::filesystem::path legacy_appdata_dir() {
  PWSTR raw = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
  G_REQUIRE_RET(SUCCEEDED(hr) && raw != nullptr, std::filesystem::path());
  std::filesystem::path dir(raw);
  CoTaskMemFree(raw);
  return dir / L"GarminSync";  // not created: nothing is written there any more
}

bool valid_profile_name(const std::string& name) {
  if (name.size() > kMaxProfileName) return false;
  for (size_t i = 0; i < name.size() && i < kMaxProfileName; ++i) {
    const char c = name[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::filesystem::path profile_dir(const std::filesystem::path& base, const std::string& profile) {
  G_ASSERT(valid_profile_name(profile));
  if (profile.empty()) return base;
  return base / L"profiles" / std::filesystem::path(profile);
}

std::vector<std::string> list_profiles(const std::filesystem::path& base) {
  std::vector<std::string> out{""};
  std::error_code ec;
  const std::filesystem::path dir = base / L"profiles";
  if (!std::filesystem::is_directory(dir, ec)) return out;
  std::filesystem::directory_iterator it(dir, ec);
  for (size_t i = 0; !ec && it != std::filesystem::directory_iterator() && i < kMaxProfiles; ++i) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec) && !name.empty() && valid_profile_name(name)) out.push_back(name);
    it.increment(ec);
  }
  std::sort(out.begin() + 1, out.end());
  G_ASSERT(!out.empty() && out.front().empty());
  return out;
}

}  // namespace gutil
