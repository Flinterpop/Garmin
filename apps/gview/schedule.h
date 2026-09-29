// Data > Download new data every morning: a Windows scheduled task that
// runs gsync.exe from this folder at 06:00, so nobody runs it by hand.
// One task per profile: "GarminSync", "GarminSync-<name>".
#pragma once
#include <filesystem>
#include <string>

namespace gview {

enum class NightlyState {
  kOff,           // no task with this profile's name
  kOn,            // task exists and runs this folder's gsync.exe
  kOtherFolder,   // task exists but runs a gsync.exe somewhere else
};

std::wstring nightly_task_name(const std::string& profile);

// `exe_dir` holds gsync.exe; the check reads the task's action.
NightlyState nightly_state(const std::filesystem::path& exe_dir, const std::string& profile);

// Creates (or replaces) the task for this folder and profile. False with `err` on failure.
bool enable_nightly(const std::filesystem::path& exe_dir, const std::string& profile, std::wstring& err);
bool disable_nightly(const std::string& profile, std::wstring& err);

// The task definition, exposed for tests: XML for schtasks /Create /XML.
std::wstring nightly_task_xml(const std::filesystem::path& exe_dir, const std::string& profile);

}  // namespace gview
