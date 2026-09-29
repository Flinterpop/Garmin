#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "store/db.h"
#include "sync/files.h"
#include "sync/watch.h"

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
  const fs::path d = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

void write(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

struct Lines {
  int ok = 0;
  int errors = 0;
  syncer::Report report() {
    return [this](bool error, const std::string&) { ++(error ? errors : ok); };
  }
};

}  // namespace

TEST_CASE("import_fit_path: folders are searched for .fit only; broken files count as failures", "[sync]") {
  const fs::path dir = fresh_dir("gview_test_import_fit");
  fs::create_directories(dir / "Activity");
  write(dir / "Activity" / "broken.FIT", "not a fit file at all");  // extension match ignores case
  write(dir / "notes.txt", "ignored");
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  syncer::ImportTotals t;
  Lines l;
  syncer::import_fit_path(db, dir, false, t, l.report());
  CHECK(t.files == 0);
  CHECK(t.failures == 1);  // broken.FIT tried and rejected; notes.txt never looked at
  CHECK(l.errors == 1);
  fs::remove_all(dir);
}

TEST_CASE("import_any: CSVs go to the blood-pressure importer", "[sync]") {
  const fs::path dir = fresh_dir("gview_test_import_any");
  write(dir / "readings_good.csv",
        "timestamp,model,device,user,systolic,diastolic,pulse,movement,irregular_heartbeat\r\n"
        "2026-08-22 17:37:59,HEM-7600T,evolv,1,148,87,49,0,0\r\n");
  write(dir / "readings_bad.csv", "date,sys,dia\n1,2,3\n");
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  syncer::ImportTotals t;
  Lines l;
  syncer::import_any(db, dir / "readings_good.csv", t, l.report());
  CHECK(t.rows == 1);
  CHECK(t.failures == 0);
  syncer::import_any(db, dir / "readings_bad.csv", t, l.report());
  CHECK(t.failures == 1);
  syncer::import_any(db, dir / "readings_good.csv", t, l.report());  // idempotent
  CHECK(t.rows == 1);
  fs::remove_all(dir);
}

TEST_CASE("device_folder_name keeps names folder-safe", "[sync]") {
  CHECK(syncer::device_folder_name(L"fenix 7") == L"fenix_7");
  CHECK(syncer::device_folder_name(L"Forerunner 265S") == L"Forerunner_265S");
  CHECK(syncer::device_folder_name(L"a/b\\c:d") == L"a_b_c_d");
  CHECK(syncer::device_folder_name(L"") == L"watch");
}
