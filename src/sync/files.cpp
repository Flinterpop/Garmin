#include "sync/files.h"

#include <cctype>
#include <string>
#include <vector>

#include "store/db.h"
#include "store/importer.h"
#include "util/assert.h"
#include "util/file_util.h"

namespace syncer {

namespace {

std::string lower_ext(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return ext;
}

void import_one_fit(store::Db& db, const std::filesystem::path& f, bool force, ImportTotals& t,
                    const Report& report) {
  std::vector<uint8_t> bytes;
  if (!gutil::read_file(f, bytes)) {
    report(true, "  cannot read " + f.string());
    ++t.failures;
    return;
  }
  store::ImportCounts c;
  std::string err;
  if (!store::import_fit(db, f.string(), bytes, 0, force, c, err)) {
    report(true, "  " + f.filename().string() + ": " + err);
    ++t.failures;
    return;
  }
  ++t.files;
  t.rows += c.rows;
  report(false, strf("  %-40s %lld msgs %lld rows%s", f.filename().string().c_str(),
                     static_cast<long long>(c.messages), static_cast<long long>(c.rows),
                     c.skipped ? " (already imported)" : ""));
}

}  // namespace

void import_fit_path(store::Db& db, const std::filesystem::path& p, bool force, ImportTotals& t,
                     const Report& report) {
  G_ASSERT(report != nullptr);
  std::vector<std::filesystem::path> targets;
  std::error_code ec;
  if (std::filesystem::is_directory(p, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(p, ec)) {
      if (targets.size() >= kMaxImportFiles) break;
      if (entry.is_regular_file() && lower_ext(entry.path()) == ".fit") targets.push_back(entry.path());
    }
  } else {
    targets.push_back(p);
  }
  for (size_t i = 0; i < targets.size() && i < kMaxImportFiles; ++i) import_one_fit(db, targets[i], force, t, report);
  G_ASSERT(t.files + t.failures >= 0);
}

bool import_bp_file(store::Db& db, const std::filesystem::path& p, ImportTotals& t, const Report& report) {
  std::string text;
  store::ImportCounts c;
  std::string err;
  if (!gutil::read_text_file(p, text) || !store::import_bp_csv(db, text, c, err)) {
    report(true, "  " + p.filename().string() + ": " + (err.empty() ? std::string("cannot read") : err));
    ++t.failures;
    return false;
  }
  t.rows += c.rows;
  report(false, strf("  %s  %lld new blood pressure readings", p.filename().string().c_str(),
                     static_cast<long long>(c.rows)));
  return true;
}

void import_any(store::Db& db, const std::filesystem::path& p, ImportTotals& t, const Report& report) {
  std::error_code ec;
  if (!std::filesystem::is_directory(p, ec) && lower_ext(p) == ".csv") {
    import_bp_file(db, p, t, report);
    return;
  }
  import_fit_path(db, p, false, t, report);
}

}  // namespace syncer
