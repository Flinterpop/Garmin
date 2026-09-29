// Importing files the user already has: FIT files (from the watch or
// anywhere) and Omron blood-pressure CSVs. Shared by gsync and gview.
#pragma once
#include <cstdint>
#include <filesystem>

#include "sync/engine.h"

namespace store {
class Db;
}

namespace syncer {

constexpr size_t kMaxImportFiles = 100000;

struct ImportTotals {
  int files = 0;       // FIT files imported (including ones already in the database)
  int64_t rows = 0;
  int failures = 0;
};

// A .fit file, or every .fit under a folder (recursively).
void import_fit_path(store::Db& db, const std::filesystem::path& p, bool force, ImportTotals& t,
                     const Report& report);

// One Omron readings CSV; returns false (and counts a failure) if it cannot be read or parsed.
bool import_bp_file(store::Db& db, const std::filesystem::path& p, ImportTotals& t, const Report& report);

// Dispatches on the extension: .fit / folders to import_fit_path, .csv to import_bp_file.
void import_any(store::Db& db, const std::filesystem::path& p, ImportTotals& t, const Report& report);

}  // namespace syncer
