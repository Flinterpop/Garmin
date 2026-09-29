#include "map/map_settings.h"

#include <windows.h>

#include <algorithm>

#include "map/tile_provider.h"
#include "util/assert.h"

namespace map {

namespace {

constexpr DWORD kBufChars = 1024;

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  G_REQUIRE_RET(n > 0, std::string());
  std::string s(static_cast<size_t>(n), '\0');
  const int got = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(),
                                      n, nullptr, nullptr);
  G_REQUIRE_RET(got == n, std::string());
  return s;
}

std::string read_value(const std::filesystem::path& ini, const wchar_t* section,
                       const wchar_t* name) {
  G_ASSERT(section != nullptr && name != nullptr);
  wchar_t buf[kBufChars] = {};
  const DWORD n = GetPrivateProfileStringW(section, name, L"", buf, kBufChars, ini.c_str());
  G_ASSERT(n < kBufChars);
  return narrow(std::wstring(buf, n));
}

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

void read_keys(const std::filesystem::path& ini, MapSettings& out) {
  const std::vector<TileProvider>& t = tile_providers();
  for (size_t i = 0; i < t.size() && i < kMaxProviders; ++i) {
    const std::string& name = t[i].key_name;
    if (name.empty() || out.keys.count(name) != 0) continue;
    const std::string v = trim(read_value(ini, L"keys", std::wstring(name.begin(), name.end()).c_str()));
    if (!v.empty() && v.size() <= kMaxKeyLength) out.keys[name] = v;
  }
}

void read_overlays(const std::string& csv, MapSettings& out) {
  size_t start = 0;
  for (size_t i = 0; i < kMaxOverlays && start <= csv.size(); ++i) {
    const size_t comma = csv.find(',', start);
    const std::string id = trim(csv.substr(start, comma == std::string::npos ? std::string::npos
                                                                               : comma - start));
    const size_t idx = provider_index(id);
    if (idx != SIZE_MAX && tile_providers()[idx].overlay) out.overlays.push_back(idx);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
}

}  // namespace

MapSettings load_map_settings(const std::filesystem::path& ini) {
  MapSettings s;
  std::error_code ec;
  if (ini.empty() || !std::filesystem::exists(ini, ec)) return s;
  read_keys(ini, s);
  const size_t base = provider_index(trim(read_value(ini, L"map", L"base")));
  if (base != SIZE_MAX && !tile_providers()[base].overlay && provider_available(s, base)) {
    s.base = base;
  }
  read_overlays(read_value(ini, L"map", L"overlays"), s);
  G_ASSERT(s.base < tile_providers().size());
  return s;
}

bool save_map_layers(const std::filesystem::path& ini, const MapSettings& s) {
  G_REQUIRE_RET(!ini.empty(), false);
  const std::vector<TileProvider>& t = tile_providers();
  G_REQUIRE_RET(s.base < t.size(), false);
  std::string csv;
  for (size_t i = 0; i < s.overlays.size() && i < kMaxOverlays; ++i) {
    G_REQUIRE_RET(s.overlays[i] < t.size(), false);
    if (!csv.empty()) csv += ",";
    csv += t[s.overlays[i]].id;
  }
  const std::wstring base(t[s.base].id.begin(), t[s.base].id.end());
  const std::wstring ov(csv.begin(), csv.end());
  return WritePrivateProfileStringW(L"map", L"base", base.c_str(), ini.c_str()) != 0 &&
         WritePrivateProfileStringW(L"map", L"overlays", ov.c_str(), ini.c_str()) != 0;
}

bool provider_available(const MapSettings& s, size_t provider) {
  const std::vector<TileProvider>& t = tile_providers();
  G_REQUIRE_RET(provider < t.size(), false);
  return t[provider].key_name.empty() || s.keys.count(t[provider].key_name) != 0;
}

bool choose_layer(MapSettings& s, size_t provider) {
  const std::vector<TileProvider>& t = tile_providers();
  G_REQUIRE_RET(provider < t.size(), false);
  if (!provider_available(s, provider)) return false;
  if (!t[provider].overlay) {
    if (s.base == provider) return false;
    s.base = provider;
    return true;
  }
  const auto it = std::find(s.overlays.begin(), s.overlays.end(), provider);
  if (it != s.overlays.end()) {
    s.overlays.erase(it);
  } else if (s.overlays.size() < kMaxOverlays) {
    s.overlays.push_back(provider);
  }
  G_ASSERT(s.overlays.size() <= kMaxOverlays);
  return true;
}

size_t next_base(const MapSettings& s) {
  const std::vector<TileProvider>& t = tile_providers();
  G_ASSERT(s.base < t.size());
  for (size_t step = 1; step <= t.size() && step <= kMaxProviders; ++step) {
    const size_t i = (s.base + step) % t.size();
    if (!t[i].overlay && provider_available(s, i)) return i;
  }
  return s.base;
}

std::filesystem::path sidecar_ini_path() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  G_REQUIRE_RET(n > 0 && n < MAX_PATH, std::filesystem::path());
  std::filesystem::path p(std::wstring(buf, n));
  p.replace_extension(L".ini");
  return p;
}

}  // namespace map
