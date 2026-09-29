#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gutil {

constexpr size_t kMaxFileBytes = 256u * 1024u * 1024u;

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out);
bool read_text_file(const std::filesystem::path& p, std::string& out);
bool write_file(const std::filesystem::path& p, const void* data, size_t len);
bool write_text_file(const std::filesystem::path& p, const std::string& text);

// Folder of the running executable. Everything lives beside the exes, a
// portable install: logins, databases, tile cache, gview.ini, profiles.
// Nothing is kept in AppData or tied to the Windows account.
std::filesystem::path exe_dir();

// %LOCALAPPDATA%\GarminSync, where v0.1.3 and earlier kept everything. Only
// `gsync migrate-appdata` reads it (to copy it over); nothing writes there.
std::filesystem::path legacy_appdata_dir();

// Profiles: one Garmin login + database each. "" is the default profile
// (the exe folder itself, the pre-profile layout); a named one lives in
// <base>\profiles\<name>.
constexpr size_t kMaxProfileName = 32;
constexpr size_t kMaxProfiles = 64;

// "" or 1..kMaxProfileName characters of A-Z a-z 0-9 _ - (safe as a folder name).
bool valid_profile_name(const std::string& name);
std::filesystem::path profile_dir(const std::filesystem::path& base, const std::string& profile);
// "" first, then the named profiles under <base>\profiles, sorted.
std::vector<std::string> list_profiles(const std::filesystem::path& base);

}  // namespace gutil
