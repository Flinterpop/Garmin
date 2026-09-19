#include <catch2/catch_test_macros.hpp>

#include "store/db.h"
#include "store/importer.h"

TEST_CASE("blood pressure csv import is idempotent and validates rows") {
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  const std::string csv =
      "timestamp,model,device,user,systolic,diastolic,pulse,movement,irregular_heartbeat\r\n"
      "2026-07-31 05:57:26,HEM-7342T,ten,2,129,80,50,0,0\r\n"
      "2026-08-22 17:37:59,HEM-7600T,evolv,1,148,87,49,0,0\r\n"
      "2026-08-22 17:37:59,HEM-7600T,evolv,1,149,84,49,0,0\r\n"
      "2026-08-22 17:37:59,HEM-7600T,evolv,1,999,84,49,0,0\r\n"  // out of range: skipped
      "garbage line\r\n";
  store::ImportCounts c;
  REQUIRE(store::import_bp_csv(db, csv, c, err));
  CHECK(c.rows == 3);

  store::ImportCounts again;
  REQUIRE(store::import_bp_csv(db, csv, again, err));
  CHECK(again.rows == 0);

  store::Stmt q(db, "SELECT COUNT(*), MIN(systolic), MAX(systolic), SUM(cuff_user = 2) FROM blood_pressure");
  REQUIRE(q.ok());
  REQUIRE(q.row());
  CHECK(q.col_int(0) == 3);
  CHECK(q.col_int(1) == 129);
  CHECK(q.col_int(2) == 149);
  CHECK(q.col_int(3) == 1);

  store::ImportCounts bad;
  CHECK_FALSE(store::import_bp_csv(db, "date,sys,dia\n1,2,3\n", bad, err));
}
