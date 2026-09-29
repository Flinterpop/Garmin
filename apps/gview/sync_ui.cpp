#include "sync_ui.h"

#include <deque>
#include <mutex>
#include <thread>

#include "plot/plot_widget.h"  // widen()
#include "sync/account.h"
#include "sync_ui_ids.h"
#include "util/assert.h"
#include "util/file_util.h"

namespace gview {

namespace {

constexpr UINT kMsgLines = WM_APP + 40;       // worker -> progress window: lines queued
constexpr UINT kMsgJobDone = WM_APP + 41;     // worker -> progress window: job finished
constexpr UINT kMsgMfa = WM_APP + 42;         // login worker -> login dialog (sent): ask for code
constexpr UINT kMsgLoginDone = WM_APP + 43;   // login worker -> login dialog: finished
constexpr int kMaxLogChars = 400000;          // progress log is cleared past this
constexpr int kMaxField = 320;                // email / password / code / name length
constexpr int kFirstSyncDays = 30;

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  G_REQUIRE_RET(n > 0, std::string());
  std::string s(static_cast<size_t>(n), '\0');
  const int got = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                                      nullptr, nullptr);
  G_REQUIRE_RET(got == n, std::string());
  return s;
}

std::wstring field(HWND dlg, int id) {
  wchar_t buf[kMaxField + 1] = {};
  const UINT n = GetDlgItemTextW(dlg, id, buf, kMaxField + 1);
  std::wstring w(buf, n);
  SecureZeroMemory(buf, sizeof(buf));  // may have been a password
  return w;
}

// ------------------------------------------------------------- progress

struct Progress {
  const Job* job = nullptr;
  std::wstring title;
  std::atomic<bool> cancel{false};
  std::mutex mu;
  std::deque<std::string> lines;  // guarded by mu
  bool ok = false;                // set by the worker before kMsgJobDone
  bool finished = false;          // UI side: the job is over
  std::thread worker;
  HWND dlg = nullptr;
  HFONT mono = nullptr;           // the log's columns line up in a fixed-pitch font
};

// Consolas at the dialog font's size, for the progress log.
HFONT make_mono_font(HWND dlg) {
  LOGFONTW lf{};
  const HFONT base = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
  if (base == nullptr || GetObjectW(base, sizeof(lf), &lf) == 0) return nullptr;
  wcscpy_s(lf.lfFaceName, L"Consolas");
  lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
  return CreateFontIndirectW(&lf);
}

void start_job(Progress& p) {
  G_ASSERT(p.job != nullptr && p.dlg != nullptr);
  p.worker = std::thread([&p] {
    const syncer::Report report = [&p](bool error, const std::string& line) {
      bool was_empty = false;
      {
        std::lock_guard<std::mutex> lock(p.mu);
        was_empty = p.lines.empty();
        p.lines.push_back(error ? "! " + line : line);
      }
      if (was_empty) PostMessageW(p.dlg, kMsgLines, 0, 0);  // one wake-up per batch
    };
    p.ok = (*p.job)(report, p.cancel);
    PostMessageW(p.dlg, kMsgJobDone, 0, 0);
  });
}

void drain_lines(Progress& p) {
  std::deque<std::string> batch;
  {
    std::lock_guard<std::mutex> lock(p.mu);
    batch.swap(p.lines);
  }
  HWND log = GetDlgItem(p.dlg, IDC_PROGRESS_LOG);
  if (GetWindowTextLengthW(log) > kMaxLogChars) SetWindowTextW(log, L"");
  std::wstring text;
  for (size_t i = 0; i < batch.size() && i < 100000; ++i) text += plot::widen(batch[i]) + L"\r\n";
  const int end = GetWindowTextLengthW(log);
  SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
  SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
  if (!batch.empty()) SetDlgItemTextW(p.dlg, IDC_PROGRESS_STATUS, plot::widen(batch.back()).c_str());
}

void on_job_done(Progress& p) {
  if (p.worker.joinable()) p.worker.join();
  drain_lines(p);
  p.finished = true;
  const wchar_t* status = p.cancel.load() ? L"Stopped. Anything already downloaded is kept."
                          : p.ok          ? L"Finished."
                                          : L"Finished with problems: see the lines marked ! above.";
  SetDlgItemTextW(p.dlg, IDC_PROGRESS_STATUS, status);
  SetDlgItemTextW(p.dlg, IDCANCEL, L"Close");
  EnableWindow(GetDlgItem(p.dlg, IDCANCEL), TRUE);
}

INT_PTR CALLBACK progress_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    auto* p = reinterpret_cast<Progress*>(lp);
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    p->dlg = dlg;
    SetWindowTextW(dlg, p->title.c_str());
    p->mono = make_mono_font(dlg);
    if (p->mono != nullptr) {
      SendDlgItemMessageW(dlg, IDC_PROGRESS_LOG, WM_SETFONT, reinterpret_cast<WPARAM>(p->mono), FALSE);
    }
    start_job(*p);
    return TRUE;
  }
  auto* p = reinterpret_cast<Progress*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (p == nullptr) return FALSE;
  G_ASSERT(p->dlg == dlg);  // the state belongs to this dialog
  switch (msg) {
    case kMsgLines: drain_lines(*p); return TRUE;
    case kMsgJobDone: on_job_done(*p); return TRUE;
    case WM_COMMAND:
      if (LOWORD(wp) != IDCANCEL) return FALSE;
      if (p->finished) {
        EndDialog(dlg, p->ok ? IDOK : IDCANCEL);
      } else {
        p->cancel = true;  // the job stops at its next check
        SetDlgItemTextW(dlg, IDCANCEL, L"Stopping...");
        EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
      }
      return TRUE;
    default: return FALSE;
  }
}

// ----------------------------------------------------------------- login

struct Login {
  std::filesystem::path base;
  std::wstring who;
  std::string email;
  std::string password;  // zeroed by syncer::login
  syncer::LoginResult result;
  std::thread worker;
  bool busy = false;
  HWND dlg = nullptr;
};

INT_PTR CALLBACK mfa_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    SendDlgItemMessageW(dlg, IDC_MFA_CODE, EM_LIMITTEXT, 12, 0);
    return TRUE;
  }
  if (msg != WM_COMMAND || (LOWORD(wp) != IDOK && LOWORD(wp) != IDCANCEL)) return FALSE;
  auto* out = reinterpret_cast<std::string*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (LOWORD(wp) == IDOK && out != nullptr) *out = narrow(field(dlg, IDC_MFA_CODE));
  EndDialog(dlg, LOWORD(wp));
  return TRUE;
}

void set_login_busy(Login& l, bool busy) {
  l.busy = busy;
  for (const int id : {IDC_LOGIN_EMAIL, IDC_LOGIN_PASSWORD, IDOK, IDCANCEL}) {
    EnableWindow(GetDlgItem(l.dlg, id), busy ? FALSE : TRUE);
  }
}

void start_login(Login& l) {
  l.email = narrow(field(l.dlg, IDC_LOGIN_EMAIL));
  std::wstring pw = field(l.dlg, IDC_LOGIN_PASSWORD);
  l.password = narrow(pw);
  SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
  SetDlgItemTextW(l.dlg, IDC_LOGIN_PASSWORD, L"");
  if (l.email.empty() || l.password.empty()) {
    SetDlgItemTextW(l.dlg, IDC_LOGIN_STATUS, L"Enter your Garmin Connect email and password.");
    return;
  }
  set_login_busy(l, true);
  SetDlgItemTextW(l.dlg, IDC_LOGIN_STATUS, L"Signing in to Garmin... this can take a few seconds.");
  l.worker = std::thread([&l] {
    // Garmin may ask for a security code mid-login: ask on the UI thread and wait.
    const gc::MfaPrompt mfa = [&l] {
      std::string code;
      SendMessageW(l.dlg, kMsgMfa, 0, reinterpret_cast<LPARAM>(&code));
      return code;
    };
    l.result = syncer::login(l.base, l.email, l.password, mfa);
    PostMessageW(l.dlg, kMsgLoginDone, 0, 0);
  });
}

void on_login_done(Login& l) {
  if (l.worker.joinable()) l.worker.join();
  set_login_busy(l, false);
  if (l.result.ok) {
    EndDialog(l.dlg, IDOK);
    return;
  }
  SetDlgItemTextW(l.dlg, IDC_LOGIN_STATUS, (L"Login failed: " + plot::widen(l.result.error)).c_str());
  SetFocus(GetDlgItem(l.dlg, IDC_LOGIN_PASSWORD));
}

INT_PTR CALLBACK login_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    auto* l = reinterpret_cast<Login*>(lp);
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    l->dlg = dlg;
    SetDlgItemTextW(dlg, IDC_LOGIN_WHO, (L"Garmin Connect login for " + l->who + L".").c_str());
    SendDlgItemMessageW(dlg, IDC_LOGIN_EMAIL, EM_LIMITTEXT, kMaxField, 0);
    SendDlgItemMessageW(dlg, IDC_LOGIN_PASSWORD, EM_LIMITTEXT, kMaxField, 0);
    return TRUE;
  }
  auto* l = reinterpret_cast<Login*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (l == nullptr) return FALSE;
  G_ASSERT(l->dlg == dlg);  // the state belongs to this dialog
  switch (msg) {
    case kMsgMfa: {
      auto* out = reinterpret_cast<std::string*>(lp);
      DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MFA), dlg, mfa_proc,
                      reinterpret_cast<LPARAM>(out));
      return TRUE;
    }
    case kMsgLoginDone: on_login_done(*l); return TRUE;
    case WM_COMMAND:
      if (l->busy) return TRUE;  // the network call cannot be interrupted; wait for it
      if (LOWORD(wp) == IDOK) start_login(*l);
      if (LOWORD(wp) == IDCANCEL) EndDialog(dlg, IDCANCEL);
      return TRUE;
    case WM_CLOSE:
      if (!l->busy) EndDialog(dlg, IDCANCEL);
      return TRUE;
    default: return FALSE;
  }
}

// --------------------------------------------------------- person, welcome

struct Person {
  std::filesystem::path exe_dir;
  std::string* name = nullptr;
};

bool accept_person(HWND dlg, const Person& p) {
  const std::string name = narrow(field(dlg, IDC_PERSON_NAME));
  std::error_code ec;
  const wchar_t* problem = nullptr;
  if (name.empty() || !gutil::valid_profile_name(name)) {
    problem = L"Use 1 to 32 letters, digits, _ or - (no spaces).";
  } else if (std::filesystem::exists(gutil::profile_dir(p.exe_dir, name), ec)) {
    problem = L"There is already a person with that name.";
  }
  if (problem != nullptr) {
    MessageBoxW(dlg, problem, L"Add a person", MB_ICONINFORMATION);
    return false;
  }
  *p.name = name;
  return true;
}

INT_PTR CALLBACK person_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    SendDlgItemMessageW(dlg, IDC_PERSON_NAME, EM_LIMITTEXT, static_cast<WPARAM>(gutil::kMaxProfileName), 0);
    return TRUE;
  }
  const auto* p = reinterpret_cast<const Person*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (p == nullptr || msg != WM_COMMAND) return FALSE;
  if (LOWORD(wp) == IDOK && accept_person(dlg, *p)) EndDialog(dlg, IDOK);
  if (LOWORD(wp) == IDCANCEL) EndDialog(dlg, IDCANCEL);
  return TRUE;
}

struct Welcome {
  std::wstring folder;
  std::wstring text;
  bool can_migrate = false;
};

INT_PTR CALLBACK welcome_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INITDIALOG) {
    const auto* w = reinterpret_cast<const Welcome*>(lp);
    SetDlgItemTextW(dlg, IDC_WELCOME_FOLDER, w->folder.c_str());  // SS_PATHELLIPSIS shortens long ones
    SetDlgItemTextW(dlg, IDC_WELCOME_TEXT, w->text.c_str());
    EnableWindow(GetDlgItem(dlg, IDC_WELCOME_MIGRATE), w->can_migrate ? TRUE : FALSE);
    return TRUE;
  }
  if (msg != WM_COMMAND) return FALSE;
  const int id = LOWORD(wp);
  if (id == IDC_WELCOME_LOGIN || id == IDC_WELCOME_MIGRATE || id == IDCANCEL) EndDialog(dlg, id);
  return TRUE;
}

}  // namespace

bool run_progress(HWND owner, const std::wstring& title, const Job& job) {
  G_REQUIRE_RET(job != nullptr, false);
  Progress p;
  p.job = &job;
  p.title = title;
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROGRESS), owner,
                                    progress_proc, reinterpret_cast<LPARAM>(&p));
  G_ASSERT(r != -1);                 // -1: the dialog template is missing
  if (p.worker.joinable()) p.worker.join();
  if (p.mono != nullptr) DeleteObject(p.mono);  // the dialog (and its edit) is gone by now
  return r == IDOK;
}

bool login_dialog(HWND owner, const std::filesystem::path& profile_base, const std::wstring& who) {
  G_REQUIRE_RET(!profile_base.empty(), false);
  Login l;
  l.base = profile_base;
  l.who = who;
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LOGIN), owner,
                                    login_proc, reinterpret_cast<LPARAM>(&l));
  G_ASSERT(r != -1);
  if (l.worker.joinable()) l.worker.join();
  return r == IDOK && l.result.ok;
}

bool sync_with_progress(HWND owner, const std::filesystem::path& profile_base,
                        const std::filesystem::path& data_dir, int first_days) {
  G_REQUIRE_RET(!profile_base.empty() && !data_dir.empty(), false);
  const Job job = [&](const syncer::Report& report, const std::atomic<bool>& cancel) {
    syncer::SyncOptions so;
    so.profile_base = profile_base;
    so.data_dir = data_dir;
    syncer::catch_up_range(data_dir, first_days, so.from, so.to);
    so.cancel = &cancel;
    const syncer::SyncResult r = syncer::run_sync(so, report);
    if (r.login_required || r.not_logged_in) {
      report(true, "Garmin needs you to log in again: use Data > Log in, then Sync now.");
    }
    return syncer::exit_code(r) == 0;
  };
  return run_progress(owner, L"Downloading from Garmin Connect", job);
}

bool migrate_with_progress(HWND owner, const std::filesystem::path& profile_base) {
  const Job job = [&](const syncer::Report& report, const std::atomic<bool>&) {
    return syncer::migrate_appdata(profile_base, report);
  };
  return run_progress(owner, L"Copying data from the previous version", job);
}

bool ask_person_name(HWND owner, const std::filesystem::path& exe_dir, std::string& name) {
  Person p;
  p.exe_dir = exe_dir;
  p.name = &name;
  return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PERSON), owner, person_proc,
                         reinterpret_cast<LPARAM>(&p)) == IDOK;
}

WelcomeChoice welcome_dialog(HWND owner, const std::filesystem::path& folder, bool can_migrate) {
  Welcome w;
  w.folder = folder.wstring();
  w.text = L"Log in to Garmin Connect to download the last " + std::to_wstring(kFirstSyncDays) +
           L" days, or copy your data from an earlier version of this program.";
  w.can_migrate = can_migrate;
  const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_WELCOME), owner,
                                    welcome_proc, reinterpret_cast<LPARAM>(&w));
  if (r == IDC_WELCOME_LOGIN) return WelcomeChoice::kLogin;
  if (r == IDC_WELCOME_MIGRATE) return WelcomeChoice::kMigrate;
  return WelcomeChoice::kExit;
}

}  // namespace gview
