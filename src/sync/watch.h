// Import straight from a connected Garmin watch. Recent watches (fenix 7 and
// similar) connect over MTP, like a phone, with no drive letter; the Windows
// Shell can still copy from them, so this walks "This PC" for any device (or
// drive) holding a GARMIN folder, copies its data folders beside the database
// and imports the FIT files.
#pragma once
#include <atomic>
#include <filesystem>

#include "sync/engine.h"
#include "sync/files.h"

namespace store {
class Db;
}

namespace syncer {

struct WatchImport {
  int watches = 0;       // devices with a GARMIN folder
  int folders = 0;       // data folders copied
  ImportTotals totals;   // FIT files imported from the copies
};

// Copies <device>\...\GARMIN\{Activity,Monitor,SUMMARY,Sleep,Metrics,HRVStatus}
// to <data_dir>\fit\watch\<device>\ and imports them into `db`. Initialises
// COM (apartment-threaded) on the calling thread for its own use.
WatchImport import_from_watches(store::Db& db, const std::filesystem::path& data_dir, const Report& report,
                                const std::atomic<bool>* cancel);

// Folder-safe version of a device name ("fenix 7" -> "fenix_7").
std::wstring device_folder_name(const std::wstring& display_name);

}  // namespace syncer
