// Login, first-run migration and "what to sync" decisions shared by gsync and gview.
#pragma once
#include <filesystem>
#include <string>

#include "gc/gc_client.h"
#include "sync/engine.h"

namespace syncer {

struct LoginResult {
  bool ok = false;
  std::string who;    // "Full Name" on success
  std::string error;  // why not, on failure
};

// Signs in (SSO, MFA through `mfa`), saves the login to <profile_base>\tokens.bin
// and zeroes `password`. Blocks on the network: call from a worker thread in a GUI.
LoginResult login(const std::filesystem::path& profile_base, const std::string& email,
                  std::string& password, const gc::MfaPrompt& mfa);

bool has_login(const std::filesystem::path& profile_base);

// v0.1.3 and earlier kept everything in %LOCALAPPDATA%\GarminSync.
bool legacy_install_present();

// Copies (never moves, never overwrites) a v0.1.3 AppData install into
// `profile_base` (login, data) and the exe folder (tiles).
bool migrate_appdata(const std::filesystem::path& profile_base, const Report& report);

// Dates for "sync now": from the last completed day in sync_log to today
// (completed days are skipped cheaply; the rest are fetched); the last
// `first_days` days when nothing has been synced yet. At most kMaxDays back.
void catch_up_range(const std::filesystem::path& data_dir, int first_days, std::string& from,
                    std::string& to);

}  // namespace syncer
