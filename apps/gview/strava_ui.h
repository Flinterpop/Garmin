// gview's Data > Strava menu: connect a profile to Strava (the athlete's own
// API application, authorized in the browser), choose which sports go there
// and as what, send now, and disconnect. Network work runs in the progress
// window from sync_ui; the sending itself is strava::run_push, which also
// runs after every sync.
#pragma once
#include <windows.h>

#include <filesystem>
#include <string>

namespace gview {

// Asks for the API application's id and secret, then authorizes in the
// browser. True once the login is saved.
bool strava_connect(HWND owner, const std::filesystem::path& profile_base, const std::wstring& who);

// Per-sport rules. True when saved.
bool strava_settings(HWND owner, const std::filesystem::path& profile_base,
                     const std::filesystem::path& data_dir);

// Sends what the rules select now, with progress.
bool strava_send_now(HWND owner, const std::filesystem::path& profile_base,
                     const std::filesystem::path& data_dir);

// Revokes gview's access on Strava (best effort) and forgets the login. The
// settings and the record of what was sent stay. True when disconnected.
bool strava_disconnect(HWND owner, const std::filesystem::path& profile_base);

}  // namespace gview
