#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include "store/db.h"
#include "store/importer.h"

using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

int64_t count(store::Db& db, const char* sql) {
  store::Stmt s(db, sql);
  REQUIRE(s.ok());
  REQUIRE(s.row());
  return s.col_int(0);
}

}  // namespace

TEST_CASE("json importers store the documented Connect shapes") {
  store::Db db;
  std::string err;
  REQUIRE(db.open(":memory:", err));
  store::ImportCounts c;

  SECTION("daily summary") {
    const json j = {{"totalSteps", 9780}, {"restingHeartRate", 49}, {"averageStressLevel", 24},
                    {"bodyBatteryHighestValue", 77}, {"bodyBatteryLowestValue", 31},
                    {"sleepingSeconds", 27473}, {"totalDistanceMeters", 7123.5}};
    REQUIRE(store::import_daily_summary(db, "2026-09-17", j, c, err));
    CHECK(c.rows == 1);
    store::Stmt s(db, "SELECT steps, resting_hr, sleep_seconds FROM daily_summary WHERE date='2026-09-17'");
    REQUIRE((s.ok() && s.row()));
    CHECK(s.col_int(0) == 9780);
    CHECK(s.col_int(1) == 49);
    CHECK(s.col_int(2) == 27473);
    // Re-import replaces rather than duplicates.
    REQUIRE(store::import_daily_summary(db, "2026-09-17", j, c, err));
    CHECK(count(db, "SELECT COUNT(*) FROM daily_summary") == 1);
  }

  SECTION("heart rate pairs skip nulls") {
    const json j = {{"heartRateValues", json::array({json::array({1758100000000LL, 61}),
                                                      json::array({1758100120000LL, nullptr}),
                                                      json::array({1758100240000LL, 63})})}};
    REQUIRE(store::import_heart_rate(db, j, c, err));
    CHECK(c.rows == 2);
    CHECK(count(db, "SELECT COUNT(*) FROM hr_sample WHERE source='api'") == 2);
    CHECK(count(db, "SELECT MIN(ts) FROM hr_sample") == 1758100000LL);
  }

  SECTION("sleep summary and stage intervals") {
    json j;
    j["dailySleepDTO"] = {{"sleepStartTimestampGMT", 1758064200000LL},
                          {"sleepEndTimestampGMT", 1758087540000LL},
                          {"deepSleepSeconds", 4020},
                          {"lightSleepSeconds", 18840}, {"remSleepSeconds", 4140},
                          {"awakeSleepSeconds", 360},
                          {"sleepScores", {{"overall", {{"value", 87}}}}}};
    j["sleepLevels"] = json::array({{{"startGMT", "2026-09-16T22:30:00.0"},
                                     {"endGMT", "2026-09-16T23:10:00.0"}, {"activityLevel", 1.0}},
                                    {{"startGMT", "2026-09-16T23:10:00.0"},
                                     {"endGMT", "2026-09-17T00:00:00.0"}, {"activityLevel", 0.0}}});
    REQUIRE(store::import_sleep(db, "2026-09-17", j, c, err));
    CHECK(c.rows == 3);
    store::Stmt s(db, "SELECT score, deep_s FROM sleep WHERE date='2026-09-17'");
    REQUIRE((s.ok() && s.row()));
    CHECK(s.col_int(0) == 87);
    CHECK(s.col_int(1) == 4020);
    CHECK(count(db, "SELECT COUNT(*) FROM sleep_stage WHERE stage='deep'") == 1);
  }

  SECTION("stress with body battery, negative stress skipped") {
    const json j = {{"stressValuesArray", json::array({json::array({1758100000000LL, 25}),
                                                        json::array({1758100180000LL, -1})})},
                    {"bodyBatteryValuesArray", json::array({json::array({1758100000000LL, "MEASURED", 66, 1.0})})}};
    REQUIRE(store::import_stress(db, j, c, err));
    CHECK(count(db, "SELECT COUNT(*) FROM stress_sample") == 1);
    CHECK(count(db, "SELECT level FROM body_battery_sample") == 66);
  }

  SECTION("hrv summary and readings") {
    json j;
    j["hrvSummary"] = {{"weeklyAvg", 36}, {"lastNightAvg", 39}, {"lastNight5MinHigh", 61},
                       {"status", "BALANCED"}, {"baseline", {{"lowUpper", 30}, {"balancedLow", 33}, {"balancedUpper", 45}}}};
    j["hrvReadings"] = json::array({{{"readingTimeGMT", "2026-09-17T03:05:00.0"}, {"hrvValue", 41}}});
    REQUIRE(store::import_hrv(db, "2026-09-17", j, c, err));
    CHECK(count(db, "SELECT COUNT(*) FROM hrv_daily") == 1);
    CHECK(count(db, "SELECT COUNT(*) FROM hrv_sample") == 1);
  }

  SECTION("weight from dateWeightList, zero body composition becomes NULL") {
    const json j = {{"dateWeightList", json::array({{{"timestampGMT", 1789612434000LL}, {"weight", 88269.0},
                                                       {"bmi", 27.9}, {"bodyFat", 0.0}, {"boneMass", 0},
                                                       {"sourceType", "INDEX_SCALE"}}})}};
    REQUIRE(store::import_weight(db, j, c, err));
    store::Stmt s(db, "SELECT weight_kg, bmi, body_fat_pct IS NULL, bone_mass_kg IS NULL FROM weight");
    REQUIRE((s.ok() && s.row()));
    CHECK_THAT(s.col_double(0), WithinAbs(88.269, 1e-9));
    CHECK_THAT(s.col_double(1), WithinAbs(27.9, 1e-9));
    CHECK(s.col_int(2) == 1);
    CHECK(s.col_int(3) == 1);
  }

  SECTION("activity list upsert keeps the fit link") {
    const json list = json::array({{{"activityId", 22445976556LL}, {"activityName", "Morning Run"},
                                    {"activityType", {{"typeKey", "running"}}},
                                    {"startTimeGMT", "2026-04-07 22:29:08"}, {"distance", 700.0},
                                    {"duration", 1386.0}, {"averageHR", 95}, {"maxHR", 116}}});
    REQUIRE(store::import_activity_list(db, list, c, err));
    REQUIRE(db.exec("UPDATE activity SET fit_file_id = 42 WHERE id = 22445976556", err));
    REQUIRE(store::import_activity_list(db, list, c, err));
    CHECK(count(db, "SELECT fit_file_id FROM activity WHERE id = 22445976556") == 42);
    CHECK(count(db, "SELECT COUNT(*) FROM activity") == 1);
  }
}
