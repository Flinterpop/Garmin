#include "sync/watch.h"

#include <windows.h>
#include <shellapi.h>  // FOF_NO_UI
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <string>
#include <vector>

#include "util/assert.h"

namespace syncer {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kMaxDevices = 64;     // children of This PC
constexpr int kMaxStorages = 16;    // storage areas on one device
constexpr int kMaxEntries = 512;    // entries looked at in one folder
constexpr const wchar_t* kDataFolders[] = {L"Activity", L"Monitor", L"SUMMARY", L"Sleep", L"Metrics", L"HRVStatus"};

std::wstring display_name(IShellItem* item) {
  PWSTR raw = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) || raw == nullptr) return std::wstring();
  std::wstring s(raw);
  CoTaskMemFree(raw);
  return s;
}

bool same_name(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

std::string utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  G_REQUIRE_RET(n > 0, std::string());
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// Children of a folder item, at most `cap`.
std::vector<ComPtr<IShellItem>> children(IShellItem* folder, int cap) {
  std::vector<ComPtr<IShellItem>> out;
  ComPtr<IEnumShellItems> e;
  if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&e)))) return out;
  for (int i = 0; i < cap; ++i) {
    ComPtr<IShellItem> c;
    ULONG got = 0;
    if (e->Next(1, &c, &got) != S_OK || got == 0) break;
    out.push_back(c);
  }
  return out;
}

ComPtr<IShellItem> child_named(IShellItem* folder, const wchar_t* name) {
  std::vector<ComPtr<IShellItem>> kids = children(folder, kMaxEntries);
  for (size_t i = 0; i < kids.size(); ++i) {
    if (same_name(display_name(kids[i].Get()), name)) return kids[i];
  }
  return nullptr;
}

// GARMIN folder of a This PC entry: in the root of a drive, or one storage
// level down on an MTP device ("Internal Storage\GARMIN").
ComPtr<IShellItem> find_garmin(IShellItem* device) {
  SFGAOF attrs = 0;
  if (SUCCEEDED(device->GetAttributes(SFGAO_FILESYSTEM, &attrs)) && (attrs & SFGAO_FILESYSTEM) != 0) {
    // A drive: check for its GARMIN folder directly rather than listing the whole drive.
    PWSTR path = nullptr;
    if (FAILED(device->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) return nullptr;
    const std::filesystem::path g = std::filesystem::path(path) / L"GARMIN";
    CoTaskMemFree(path);
    std::error_code ec;
    ComPtr<IShellItem> item;
    if (!std::filesystem::is_directory(g, ec) ||
        FAILED(SHCreateItemFromParsingName(g.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
      return nullptr;
    }
    return item;
  }
  std::vector<ComPtr<IShellItem>> storages = children(device, kMaxStorages);
  for (size_t i = 0; i < storages.size(); ++i) {
    ComPtr<IShellItem> g = child_named(storages[i].Get(), L"GARMIN");
    if (g) return g;
  }
  return nullptr;
}

// Copies the data folders under `garmin` into `dest` with the Shell; returns how many.
int copy_data_folders(IShellItem* garmin, const std::filesystem::path& dest, const Report& report) {
  std::error_code ec;
  std::filesystem::create_directories(dest, ec);
  ComPtr<IShellItem> to;
  ComPtr<IFileOperation> op;
  if (FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&to))) ||
      FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) ||
      FAILED(op->SetOperationFlags(FOF_NO_UI))) {
    report(true, "  cannot prepare the copy to " + dest.string());
    return 0;
  }
  int queued = 0;
  for (const wchar_t* name : kDataFolders) {
    ComPtr<IShellItem> src = child_named(garmin, name);
    if (!src || FAILED(op->CopyItem(src.Get(), to.Get(), nullptr, nullptr))) continue;
    ++queued;
  }
  BOOL aborted = FALSE;
  if (queued > 0 && (FAILED(op->PerformOperations()) || FAILED(op->GetAnyOperationsAborted(&aborted)) || aborted)) {
    report(true, "  copying from the watch did not finish (was it unplugged?)");
  }
  return queued;
}

}  // namespace

std::wstring device_folder_name(const std::wstring& display_name) {
  std::wstring out;
  for (size_t i = 0; i < display_name.size() && i < 64; ++i) {
    const wchar_t c = display_name[i];
    out += (std::iswalnum(c) || c == L'-' || c == L'_') ? c : L'_';
  }
  return out.empty() ? std::wstring(L"watch") : out;
}

WatchImport import_from_watches(store::Db& db, const std::filesystem::path& data_dir, const Report& report,
                                const std::atomic<bool>* cancel) {
  WatchImport w;
  G_REQUIRE_RET(!data_dir.empty(), w);
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  ComPtr<IShellItem> pc;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&pc)))) {
    report(false, "looking for a connected Garmin watch...");
    std::vector<ComPtr<IShellItem>> devices = children(pc.Get(), kMaxDevices);
    for (size_t i = 0; i < devices.size() && !(cancel != nullptr && cancel->load()); ++i) {
      ComPtr<IShellItem> garmin = find_garmin(devices[i].Get());
      if (!garmin) continue;
      const std::wstring name = display_name(devices[i].Get());
      ++w.watches;
      const std::filesystem::path dest = data_dir / L"fit" / L"watch" / device_folder_name(name);
      report(false, "copying from " + utf8(name) + " (this can take a minute)...");
      w.folders += copy_data_folders(garmin.Get(), dest, report);
      import_fit_path(db, dest, false, w.totals, report);
    }
  }
  pc.Reset();
  if (SUCCEEDED(init)) CoUninitialize();
  G_ASSERT(w.folders >= 0 && w.watches >= 0);
  return w;
}

}  // namespace syncer
