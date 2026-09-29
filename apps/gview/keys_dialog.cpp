#include "keys_dialog.h"

#include <map>
#include <string>

#include "keys_dialog_ids.h"
#include "map/map_settings.h"
#include "util/assert.h"

namespace gview {

namespace {

struct Field {
  int id;
  const char* name;   // gview.ini [keys] entry
  const wchar_t* label;
};

constexpr Field kFields[] = {{IDC_KEY_GOOGLE, "google", L"Google Map Tiles API"},
                             {IDC_KEY_AZURE, "azure_maps", L"Azure Maps"},
                             {IDC_KEY_THUNDERFOREST, "thunderforest", L"Thunderforest"}};
constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
constexpr int kMaxFieldChars = static_cast<int>(map::kMaxKeyLength);

struct DialogState {
  std::filesystem::path ini;
  WCHAR mask = 0;  // the edit's own password character, restored when unticking Show
  bool saved = false;
};

std::string field_text(HWND dlg, int id) {
  wchar_t buf[map::kMaxKeyLength + 2] = {};
  const UINT n = GetDlgItemTextW(dlg, id, buf, static_cast<int>(map::kMaxKeyLength + 2));
  std::string out;
  for (UINT i = 0; i < n && i <= map::kMaxKeyLength; ++i) {
    const wchar_t c = buf[i];
    if (c == L' ' || c == L'\t') continue;             // pasted keys often carry spaces
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');  // non-ASCII fails validation
  }
  return out;
}

void on_init(HWND dlg, DialogState& st) {
  const map::MapSettings s = map::load_map_settings(st.ini);
  for (size_t i = 0; i < kFieldCount; ++i) {
    SendDlgItemMessageW(dlg, kFields[i].id, EM_LIMITTEXT, kMaxFieldChars, 0);
    const auto it = s.keys.find(kFields[i].name);
    if (it == s.keys.end()) continue;
    const std::wstring w(it->second.begin(), it->second.end());  // ASCII by construction
    const BOOL set = SetDlgItemTextW(dlg, kFields[i].id, w.c_str());
    G_ASSERT(set);
  }
  st.mask = static_cast<WCHAR>(SendDlgItemMessageW(dlg, kFields[0].id, EM_GETPASSWORDCHAR, 0, 0));
  G_ASSERT(st.mask != 0);
}

void on_show_toggled(HWND dlg, const DialogState& st) {
  const bool show = IsDlgButtonChecked(dlg, IDC_SHOW_KEYS) == BST_CHECKED;
  for (size_t i = 0; i < kFieldCount; ++i) {
    SendDlgItemMessageW(dlg, kFields[i].id, EM_SETPASSWORDCHAR, show ? 0 : st.mask, 0);
    InvalidateRect(GetDlgItem(dlg, kFields[i].id), nullptr, TRUE);
  }
}

// Validates every field; on the first bad one, says which and focuses it.
bool on_save(HWND dlg, DialogState& st) {
  std::map<std::string, std::string> keys;
  for (size_t i = 0; i < kFieldCount; ++i) {
    const std::string v = field_text(dlg, kFields[i].id);
    if (v.empty()) continue;
    if (!map::valid_api_key(v)) {
      const std::wstring msg = std::wstring(L"The ") + kFields[i].label +
                               L" key does not look like an API key (letters, digits, - _ . ~ only).";
      MessageBoxW(dlg, msg.c_str(), L"Map API keys", MB_ICONWARNING);
      SetFocus(GetDlgItem(dlg, kFields[i].id));
      return false;
    }
    keys[kFields[i].name] = v;
  }
  if (!map::save_map_keys(st.ini, keys)) {
    MessageBoxW(dlg, (L"Could not write " + st.ini.wstring()).c_str(), L"Map API keys",
                MB_ICONERROR);
    return false;
  }
  st.saved = true;
  return true;
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    on_init(dlg, *reinterpret_cast<DialogState*>(lp));
    return TRUE;
  }
  auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg != WM_COMMAND || st == nullptr) return FALSE;
  switch (LOWORD(wp)) {
    case IDC_SHOW_KEYS: on_show_toggled(dlg, *st); return TRUE;
    case IDOK:
      if (on_save(dlg, *st)) EndDialog(dlg, IDOK);
      return TRUE;
    case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
    default: return FALSE;
  }
}

}  // namespace

bool edit_map_keys(HWND owner, const std::filesystem::path& ini) {
  G_REQUIRE_RET(!ini.empty(), false);
  DialogState st;
  st.ini = ini;
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAP_KEYS),
                                    owner, dialog_proc, reinterpret_cast<LPARAM>(&st));
  G_ASSERT(r != -1);  // -1: the template is missing from the exe
  return r == IDOK && st.saved;
}

}  // namespace gview
