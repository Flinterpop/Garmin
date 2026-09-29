// gview's own login, sync and first-run dialogs, so nobody has to run
// gsync.exe: the network work runs on a worker thread, progress shows in a
// modal window with Stop, and Garmin's MFA prompt is a dialog.
#pragma once
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "sync/engine.h"

namespace gview {

// Work for the progress window: runs on a worker thread, reports lines,
// polls `cancel`, returns true on success.
using Job = std::function<bool(const syncer::Report& report, const std::atomic<bool>& cancel)>;

// Modal progress window running `job`. Returns the job's result (false if stopped).
bool run_progress(HWND owner, const std::wstring& title, const Job& job);

// Modal Garmin login for the profile at `profile_base`; `who` names it in the
// dialog ("your account" / "Ann"). True once the login is saved.
bool login_dialog(HWND owner, const std::filesystem::path& profile_base, const std::wstring& who);

// Downloads recent data for a profile with progress: from the last synced day
// to today, or the last `first_days` days for a new profile.
bool sync_with_progress(HWND owner, const std::filesystem::path& profile_base,
                        const std::filesystem::path& data_dir, int first_days);

// Copies a v0.1.3 AppData install into `profile_base`, with progress.
bool migrate_with_progress(HWND owner, const std::filesystem::path& profile_base);

// Asks for a new person's name; false if cancelled. Rejects names in use.
bool ask_person_name(HWND owner, const std::filesystem::path& exe_dir, std::string& name);

enum class WelcomeChoice { kLogin, kMigrate, kExit };

// First start with no data: log in, copy from the previous version, or exit.
WelcomeChoice welcome_dialog(HWND owner, const std::filesystem::path& folder, bool can_migrate);

}  // namespace gview
