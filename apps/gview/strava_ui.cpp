#include "strava_ui.h"

#include <shellapi.h>

#include <vector>

#include "fit/fit_profile.h"
#include "gc/http_client.h"
#include "plot/plot_widget.h"  // widen()
#include "store/db.h"
#include "strava/auth.h"
#include "strava/loopback.h"
#include "strava/push.h"
#include "strava/rules.h"
#include "strava_ui_ids.h"
#include "sync_ui.h"
#include "util/assert.h"
#include "util/time_util.h"

namespace gview {

namespace {

constexpr wchar_t kApiPage[] = L"https://www.strava.com/settings/api";
constexpr int kAuthorizeTimeoutS = 600;
constexpr int kMaxField = 160;
constexpr size_t kMaxSports = 256;
constexpr int kMaxRows = 1000;

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  G_REQUIRE_RET(n > 0, std::string());
  std::string s(static_cast<size_t>(n), '\0');
  const int got =
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  G_REQUIRE_RET(got == n, std::string());
  return s;
}

std::string field(HWND dlg, int id) {
  wchar_t buf[kMaxField + 1] = {};
  const UINT n = GetDlgItemTextW(dlg, id, buf, kMaxField + 1);
  std::string s = narrow(std::wstring(buf, n));
  SecureZeroMemory(buf, sizeof(buf));  // may have been the secret
  const size_t b = s.find_first_not_of(" \t");
  const size_t e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

void open_url(HWND owner, const std::wstring& url) {
  G_ASSERT(!url.empty());
  const HINSTANCE h = ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  (void)h;  // no browser: the progress log shows the address to open by hand
}

// ------------------------------------------------------------- connect

struct Connect {
  std::filesystem::path base;
  std::wstring who;
  strava::Login login;  // client id and secret from the dialog
};

bool connect_ok(HWND dlg, Connect& c) {
  const std::string id = field(dlg, IDC_SC_CLIENT_ID);
  const std::string secret = field(dlg, IDC_SC_SECRET);
  const wchar_t* problem = nullptr;
  if (!strava::valid_client_id(id)) problem = L"The Client ID is the number shown on Strava's API page.";
  if (problem == nullptr && !strava::valid_client_secret(secret)) {
    problem = L"The Client Secret is the long string of letters and digits on Strava's API page (click Show).";
  }
  if (problem != nullptr) {
    MessageBoxW(dlg, problem, L"Connect to Strava", MB_ICONWARNING);
    SetFocus(GetDlgItem(dlg, strava::valid_client_id(id) ? IDC_SC_SECRET : IDC_SC_CLIENT_ID));
    return false;
  }
  c.login.client_id = id;
  c.login.client_secret = secret;
  return true;
}

void connect_init(HWND dlg, const Connect& c) {
  SetDlgItemTextW(dlg, IDC_SC_WHO, (L"Connecting " + c.who + L" to Strava, so chosen sports go there after each sync.").c_str());
  SendDlgItemMessageW(dlg, IDC_SC_CLIENT_ID, EM_LIMITTEXT, 12, 0);
  SendDlgItemMessageW(dlg, IDC_SC_SECRET, EM_LIMITTEXT, 80, 0);
  strava::Login old;
  std::string err;
  if (strava::load_login(strava::login_path(c.base), old, err)) {  // reconnecting: keep what they typed before
    SetDlgItemTextW(dlg, IDC_SC_CLIENT_ID, plot::widen(old.client_id).c_str());
    SetDlgItemTextW(dlg, IDC_SC_SECRET, plot::widen(old.client_secret).c_str());
  }
}

INT_PTR CALLBACK connect_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    connect_init(dlg, *reinterpret_cast<Connect*>(lp));
    return TRUE;
  }
  auto* c = reinterpret_cast<Connect*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg != WM_COMMAND || c == nullptr) return FALSE;
  switch (LOWORD(wp)) {
    case IDC_SC_OPEN_API: open_url(dlg, kApiPage); return TRUE;
    case IDOK:
      if (connect_ok(dlg, *c)) EndDialog(dlg, IDOK);
      return TRUE;
    case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
    default: return FALSE;
  }
}

// The browser half: listen, send the browser to Strava, trade the code for tokens.
bool authorize(Connect& c, const syncer::Report& report, const std::atomic<bool>& cancel) {
  std::string err;
  strava::Listener lis;
  if (!lis.open(strava::kRedirectPort, err)) {
    report(true, "Could not wait for Strava's answer: " + err + ". Close that program and try again.");
    return false;
  }
  const std::string url = strava::authorize_url(c.login.client_id);
  report(false, "Opening your browser at Strava. Click Authorize there (leave both boxes ticked).");
  report(false, "If no browser opens, paste this into one: " + url);
  open_url(nullptr, plot::widen(url));
  strava::Redirect rd;
  if (!lis.wait(kAuthorizeTimeoutS, &cancel, rd, err)) {
    report(true, "Not connected: " + err + ".");
    return false;
  }
  if (!rd.error.empty() || !strava::scope_sufficient(rd.scope)) {
    report(true, rd.error.empty() ? "Not connected: gview needs permission to see and upload your activities. "
                                    "Connect again and leave both boxes ticked."
                                  : "Not connected: Strava said " + rd.error + ".");
    return false;
  }
  gc::HttpClient http(L"gview");
  if (!http.ok() || !strava::exchange_code(http, c.login, rd.code, err) ||
      !strava::save_login(strava::login_path(c.base), c.login, err)) {
    report(true, "Not connected: " + err);
    return false;
  }
  report(false, "Connected to Strava" + (c.login.athlete.empty() ? std::string() : " as " + c.login.athlete) + ".");
  return true;
}

// ------------------------------------------------------------- settings

struct SettingsDlg {
  std::filesystem::path ini;
  std::filesystem::path data_dir;
  strava::Settings s;
  std::vector<int> sports;   // list box order
  std::vector<int> counts;   // activities per sport in the data
  int current = -1;          // list index shown in the fields
  bool saved = false;
};

// Sports in this profile's data, most activities first, then any with a rule but no data.
void load_sports(SettingsDlg& d) {
  store::Db db;
  std::string err;
  const std::filesystem::path p = d.data_dir / "garmin.db";
  std::error_code ec;
  if (std::filesystem::exists(p, ec) && db.open(p, err)) {
    store::Stmt st(db, "SELECT sport, COUNT(DISTINCT start_ts) FROM activity_session WHERE sport IS NOT NULL"
                       " GROUP BY sport ORDER BY 2 DESC");
    for (int n = 0; st.ok() && n < kMaxRows && d.sports.size() < kMaxSports && st.row(); ++n) {
      d.sports.push_back(static_cast<int>(st.col_int(0)));
      d.counts.push_back(static_cast<int>(st.col_int(1)));
    }
  }
  for (size_t i = 0; i < d.s.rules.size() && i < strava::kMaxRules; ++i) {
    bool listed = false;
    for (size_t j = 0; j < d.sports.size(); ++j) listed = listed || d.sports[j] == d.s.rules[i].sport;
    if (!listed && d.sports.size() < kMaxSports) {
      d.sports.push_back(d.s.rules[i].sport);
      d.counts.push_back(0);
    }
  }
  G_ASSERT(d.sports.size() == d.counts.size());
}

std::wstring sport_label(const SettingsDlg& d, size_t i) {
  G_ASSERT(i < d.sports.size());
  const strava::SportRule* r = strava::find_rule(d.s, d.sports[i]);
  const bool on = r != nullptr && r->enabled;
  return std::wstring(on ? L"\x2714 " : L"     ") + plot::widen(fit::sport_name(static_cast<uint8_t>(d.sports[i]))) +
         L"  (" + std::to_wstring(d.counts[i]) + L")";
}

void relabel(HWND dlg, const SettingsDlg& d, int i) {
  G_REQUIRE_VOID(i >= 0 && static_cast<size_t>(i) < d.sports.size());
  const HWND lb = GetDlgItem(dlg, IDC_SV_SPORTS);
  SendMessageW(lb, LB_DELETESTRING, static_cast<WPARAM>(i), 0);
  SendMessageW(lb, LB_INSERTSTRING, static_cast<WPARAM>(i),
               reinterpret_cast<LPARAM>(sport_label(d, static_cast<size_t>(i)).c_str()));
  SendMessageW(lb, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
}

// Fields -> the current sport's rule (created on first touch).
void store_fields(HWND dlg, SettingsDlg& d) {
  if (d.current < 0 || static_cast<size_t>(d.current) >= d.sports.size()) return;
  const bool on = IsDlgButtonChecked(dlg, IDC_SV_ENABLED) == BST_CHECKED;
  const int sport = d.sports[static_cast<size_t>(d.current)];
  strava::SportRule* r = on ? strava::rule_for(d.s, sport) : strava::find_rule(d.s, sport);
  if (r == nullptr) return;  // off and never set: nothing to keep
  r->enabled = on;
  const LRESULT t = SendDlgItemMessageW(dlg, IDC_SV_TYPE, CB_GETCURSEL, 0, 0);
  const std::vector<std::string>& types = strava::sport_types();
  if (t >= 0 && static_cast<size_t>(t) < types.size()) r->sport_type = types[static_cast<size_t>(t)];
  r->name = field(dlg, IDC_SV_TITLE);
  BOOL ok = FALSE;
  const UINT mins = GetDlgItemInt(dlg, IDC_SV_MINUTES, &ok, FALSE);
  if (ok && mins <= static_cast<UINT>(strava::kMaxMinMinutes)) r->min_minutes = static_cast<int>(mins);
}

// The sport at list index `i` -> fields.
void show_sport(HWND dlg, SettingsDlg& d, int i) {
  G_REQUIRE_VOID(i >= 0 && static_cast<size_t>(i) < d.sports.size());
  d.current = i;
  const int sport = d.sports[static_cast<size_t>(i)];
  const strava::SportRule* r = strava::find_rule(d.s, sport);
  const std::string type = r != nullptr ? r->sport_type : strava::default_sport_type(sport);
  CheckDlgButton(dlg, IDC_SV_ENABLED, r != nullptr && r->enabled ? BST_CHECKED : BST_UNCHECKED);
  const std::vector<std::string>& types = strava::sport_types();
  for (size_t k = 0; k < types.size(); ++k) {
    if (types[k] == type) SendDlgItemMessageW(dlg, IDC_SV_TYPE, CB_SETCURSEL, k, 0);
  }
  SetDlgItemTextW(dlg, IDC_SV_TITLE, r != nullptr ? plot::widen(r->name).c_str() : L"");
  SetDlgItemInt(dlg, IDC_SV_MINUTES, static_cast<UINT>(r != nullptr ? r->min_minutes : strava::kDefaultMinMinutes),
                FALSE);
}

void settings_init(HWND dlg, SettingsDlg& d) {
  load_sports(d);
  SetDlgItemTextW(dlg, IDC_SV_SINCE, plot::widen(d.s.since).c_str());
  SendDlgItemMessageW(dlg, IDC_SV_SINCE, EM_LIMITTEXT, 10, 0);
  SendDlgItemMessageW(dlg, IDC_SV_TITLE, EM_LIMITTEXT, strava::kMaxTitle, 0);
  SendDlgItemMessageW(dlg, IDC_SV_MINUTES, EM_LIMITTEXT, 3, 0);
  const std::vector<std::string>& types = strava::sport_types();
  for (size_t k = 0; k < types.size(); ++k) {
    SendDlgItemMessageW(dlg, IDC_SV_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(plot::widen(types[k]).c_str()));
  }
  for (size_t i = 0; i < d.sports.size(); ++i) {
    SendDlgItemMessageW(dlg, IDC_SV_SPORTS, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sport_label(d, i).c_str()));
  }
  if (!d.sports.empty()) {
    SendDlgItemMessageW(dlg, IDC_SV_SPORTS, LB_SETCURSEL, 0, 0);
    show_sport(dlg, d, 0);
  }
}

bool settings_save(HWND dlg, SettingsDlg& d) {
  store_fields(dlg, d);
  const std::string since = field(dlg, IDC_SV_SINCE);
  int64_t ts = 0;
  if (since.size() != 10 || !gutil::parse_date(since, ts)) {
    MessageBoxW(dlg, L"Give the first date as YYYY-MM-DD, e.g. 2026-01-01.", L"Strava settings", MB_ICONWARNING);
    SetFocus(GetDlgItem(dlg, IDC_SV_SINCE));
    return false;
  }
  for (size_t i = 0; i < d.s.rules.size() && i < strava::kMaxRules; ++i) {
    const std::string& name = d.s.rules[i].name;
    if (!name.empty() && !strava::valid_title(name)) {
      const std::wstring msg = L"The title for " + plot::widen(fit::sport_name(static_cast<uint8_t>(d.s.rules[i].sport))) +
                               L" is too long (100 characters at most).";
      MessageBoxW(dlg, msg.c_str(), L"Strava settings", MB_ICONWARNING);
      return false;
    }
  }
  d.s.since = since;
  if (!strava::save_settings(d.ini, d.s)) {
    MessageBoxW(dlg, (L"Could not write " + d.ini.wstring()).c_str(), L"Strava settings", MB_ICONERROR);
    return false;
  }
  d.saved = true;
  return true;
}

bool settings_command(HWND dlg, SettingsDlg& d, WPARAM wp) {
  switch (LOWORD(wp)) {
    case IDC_SV_SPORTS:
      if (HIWORD(wp) == LBN_SELCHANGE) {
        store_fields(dlg, d);
        const int prev = d.current;
        const int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_SV_SPORTS, LB_GETCURSEL, 0, 0));
        relabel(dlg, d, prev);
        SendDlgItemMessageW(dlg, IDC_SV_SPORTS, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
        show_sport(dlg, d, sel);
      }
      return true;
    case IDC_SV_ENABLED:
      store_fields(dlg, d);
      relabel(dlg, d, d.current);
      return true;
    case IDOK:
      if (settings_save(dlg, d)) EndDialog(dlg, IDOK);
      return true;
    case IDCANCEL: EndDialog(dlg, IDCANCEL); return true;
    default: return false;
  }
}

INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    settings_init(dlg, *reinterpret_cast<SettingsDlg*>(lp));
    return TRUE;
  }
  auto* d = reinterpret_cast<SettingsDlg*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg != WM_COMMAND || d == nullptr) return FALSE;
  return settings_command(dlg, *d, wp) ? TRUE : FALSE;
}

}  // namespace

bool strava_connect(HWND owner, const std::filesystem::path& profile_base, const std::wstring& who) {
  G_REQUIRE_RET(!profile_base.empty(), false);
  Connect c;
  c.base = profile_base;
  c.who = who;
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_STRAVA_CONNECT), owner,
                                    connect_proc, reinterpret_cast<LPARAM>(&c));
  G_ASSERT(r != -1);  // -1: the template is missing from the exe
  if (r != IDOK) return false;
  const Job job = [&c](const syncer::Report& report, const std::atomic<bool>& cancel) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // ShellExecute
    const bool ok = authorize(c, report, cancel);
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
  };
  return run_progress(owner, L"Connecting to Strava", job);
}

bool strava_settings(HWND owner, const std::filesystem::path& profile_base, const std::filesystem::path& data_dir) {
  G_REQUIRE_RET(!profile_base.empty() && !data_dir.empty(), false);
  SettingsDlg d;
  d.ini = strava::settings_path(profile_base);
  d.data_dir = data_dir;
  d.s = strava::load_settings(d.ini);
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_STRAVA_SETTINGS), owner,
                                    settings_proc, reinterpret_cast<LPARAM>(&d));
  G_ASSERT(r != -1);
  return r == IDOK && d.saved;
}

bool strava_send_now(HWND owner, const std::filesystem::path& profile_base, const std::filesystem::path& data_dir) {
  G_REQUIRE_RET(!profile_base.empty() && !data_dir.empty(), false);
  const Job job = [&](const syncer::Report& report, const std::atomic<bool>& cancel) {
    const strava::PushOptions o{profile_base, data_dir, &cancel};
    const strava::PushResult r = strava::run_push(o, report);
    if (r.not_connected) report(true, "Not connected to Strava: use Data > Strava > Connect to Strava.");
    return !r.not_connected && !r.login_required && r.failures == 0;
  };
  return run_progress(owner, L"Sending to Strava", job);
}

bool strava_disconnect(HWND owner, const std::filesystem::path& profile_base) {
  G_REQUIRE_RET(!profile_base.empty(), false);
  if (MessageBoxW(owner, L"Disconnect from Strava? gview stops sending activities there and Strava forgets its "
                         L"access. Activities already on Strava stay.",
                  L"Strava", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
    return false;
  }
  const Job job = [&](const syncer::Report& report, const std::atomic<bool>&) {
    strava::Login l;
    std::string err;
    gc::HttpClient http(L"gview");
    bool changed = false;
    if (strava::load_login(strava::login_path(profile_base), l, err) && http.ok() &&
        strava::ensure_fresh(http, l, changed, err) && strava::deauthorize(http, l, err)) {
      report(false, "Strava has revoked gview's access.");
    } else {
      report(true, "Could not reach Strava to revoke access (" + err +
                       "); remove gview under My Apps in Strava's settings if you want to.");
    }
    std::error_code ec;
    std::filesystem::remove(strava::login_path(profile_base), ec);
    report(static_cast<bool>(ec), ec ? "Could not delete the saved Strava login." : "Disconnected on this PC.");
    return !ec;
  };
  return run_progress(owner, L"Disconnecting from Strava", job);
}

}  // namespace gview
