// Data > Map API keys: a modal dialog that edits the [keys] section of the
// sidecar gview.ini. Keys are masked unless "Show keys" is ticked.
#pragma once
#include <windows.h>

#include <filesystem>

namespace gview {

// Shows the dialog. Returns true when the user saved new keys to `ini`.
bool edit_map_keys(HWND owner, const std::filesystem::path& ini);

}  // namespace gview
