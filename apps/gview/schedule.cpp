#include "schedule.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#include "util/assert.h"
#include "util/file_util.h"

namespace gview {

namespace {

constexpr DWORD kSchtasksTimeoutMs = 15000;
constexpr size_t kMaxOutput = 64 * 1024;

std::wstring xml_escape(const std::wstring& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size() && i < 4096; ++i) {
    switch (s[i]) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      case L'"': out += L"&quot;"; break;
      default: out += s[i];
    }
  }
  return out;
}

std::wstring lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// Runs schtasks.exe hidden with `args`; returns its exit code (-1 if it could
// not run) and its stdout in `out`.
int run_schtasks(const std::wstring& args, std::string& out) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  G_REQUIRE_RET(CreatePipe(&rd, &wr, &sa, 0), -1);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"schtasks.exe " + args;
  const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!started) {
    CloseHandle(rd);
    return -1;
  }
  char buf[4096];
  DWORD n = 0;
  for (size_t i = 0; i < kMaxOutput / sizeof(buf) && ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0; ++i) {
    out.append(buf, n);
  }
  WaitForSingleObject(pi.hProcess, kSchtasksTimeoutMs);
  DWORD code = static_cast<DWORD>(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(rd);
  return static_cast<int>(code);
}

std::wstring widen_ascii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// schtasks output: UTF-16LE when it has zero bytes in it, else the ANSI code page.
std::wstring decode_output(const std::string& raw) {
  const size_t zeros = static_cast<size_t>(std::count(raw.begin(), raw.end(), '\0'));
  if (raw.size() >= 2 && zeros * 4 >= raw.size()) {
    std::wstring w(raw.size() / 2, L'\0');
    std::memcpy(w.data(), raw.data(), w.size() * sizeof(wchar_t));
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    return w;
  }
  const int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
  return w;
}

}  // namespace

std::wstring nightly_task_name(const std::string& profile) {
  G_ASSERT(gutil::valid_profile_name(profile));
  return profile.empty() ? L"GarminSync" : L"GarminSync-" + widen_ascii(profile);
}

std::wstring nightly_task_xml(const std::filesystem::path& exe_dir, const std::string& profile) {
  G_ASSERT(!exe_dir.empty() && gutil::valid_profile_name(profile));
  const std::wstring dir = exe_dir.wstring();
  const std::wstring log = (exe_dir / (profile.empty() ? L"sync.log" : L"sync-" + widen_ascii(profile) + L".log")).wstring();
  std::wstring args = L"sync --days 3 --log \"" + log + L"\"";
  if (!profile.empty()) args += L" --profile " + widen_ascii(profile);
  return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
         L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
         L"  <RegistrationInfo><Description>Garmin viewer: download new data every morning</Description></RegistrationInfo>\r\n"
         L"  <Triggers><CalendarTrigger><StartBoundary>2026-01-01T06:00:00</StartBoundary><Enabled>true</Enabled>"
         L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger></Triggers>\r\n"
         L"  <Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
         L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
         L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
         L"<StartWhenAvailable>true</StartWhenAvailable><RunOnlyIfNetworkAvailable>true</RunOnlyIfNetworkAvailable>"
         L"<ExecutionTimeLimit>PT45M</ExecutionTimeLimit><Enabled>true</Enabled></Settings>\r\n"
         L"  <Actions Context=\"Author\"><Exec><Command>" + xml_escape((exe_dir / L"gsync.exe").wstring()) +
         L"</Command><Arguments>" + xml_escape(args) + L"</Arguments><WorkingDirectory>" + xml_escape(dir) +
         L"</WorkingDirectory></Exec></Actions>\r\n"
         L"</Task>\r\n";
}

NightlyState nightly_state(const std::filesystem::path& exe_dir, const std::string& profile) {
  std::string out;
  const int code = run_schtasks(L"/Query /TN \"" + nightly_task_name(profile) + L"\" /XML", out);
  if (code != 0) return NightlyState::kOff;
  const std::wstring xml = lower(decode_output(out));  // paths compare case-insensitively
  const std::wstring mine = lower((exe_dir / L"gsync.exe").wstring());
  return xml.find(mine) != std::wstring::npos ? NightlyState::kOn : NightlyState::kOtherFolder;
}

bool enable_nightly(const std::filesystem::path& exe_dir, const std::string& profile, std::wstring& err) {
  std::error_code ec;
  if (!std::filesystem::exists(exe_dir / L"gsync.exe", ec)) {
    err = L"The morning download needs gsync.exe, which is missing from " + exe_dir.wstring() +
          L". Unzip the release into this folder again to restore it.";
    return false;
  }
  wchar_t tmp_dir[MAX_PATH] = {};
  wchar_t tmp[MAX_PATH] = {};
  G_REQUIRE_RET(GetTempPathW(MAX_PATH, tmp_dir) > 0 && GetTempFileNameW(tmp_dir, L"gvt", 0, tmp) != 0, false);
  const std::wstring xml = L"\xFEFF" + nightly_task_xml(exe_dir, profile);  // UTF-16 with BOM for schtasks
  const bool written = [&] {
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const DWORD bytes = static_cast<DWORD>(xml.size() * sizeof(wchar_t));
    const BOOL ok = WriteFile(f, xml.data(), bytes, &n, nullptr);
    CloseHandle(f);
    return ok && n == bytes;
  }();
  std::string out;
  const int code = written ? run_schtasks(L"/Create /F /TN \"" + nightly_task_name(profile) + L"\" /XML \"" +
                                              std::wstring(tmp) + L"\"", out)
                           : -1;
  DeleteFileW(tmp);
  if (code != 0) err = L"Windows Task Scheduler refused the task: " + decode_output(out);
  return code == 0;
}

bool disable_nightly(const std::string& profile, std::wstring& err) {
  std::string out;
  const int code = run_schtasks(L"/Delete /F /TN \"" + nightly_task_name(profile) + L"\"", out);
  if (code != 0) err = L"Could not remove the task: " + decode_output(out);
  return code == 0;
}

}  // namespace gview
