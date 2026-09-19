#include "store/importer.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>

#include "fit/fit_decoder.h"
#include "fit/fit_profile.h"
#include "util/assert.h"
#include "util/time_util.h"

namespace store {

using nlohmann::json;

namespace {

constexpr size_t kMaxJsonArray = 200000;  // bound for any sample array we walk

// ---- tolerant JSON accessors -------------------------------------------

std::optional<int64_t> opt_int(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return std::nullopt;
  const json& v = j.at(key);
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
  return std::nullopt;
}

std::optional<double> opt_double(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return std::nullopt;
  const json& v = j.at(key);
  if (v.is_number()) return v.get<double>();
  return std::nullopt;
}

std::optional<std::string> opt_str(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return std::nullopt;
  const json& v = j.at(key);
  if (v.is_string()) return v.get<std::string>();
  return std::nullopt;
}

// Milliseconds-since-epoch -> seconds, if present.
std::optional<int64_t> opt_ms_to_s(const json& j, const char* key) {
  const auto ms = opt_int(j, key);
  if (!ms.has_value()) return std::nullopt;
  return *ms / 1000;
}

std::optional<int64_t> opt_datetime(const json& j, const char* key) {
  const auto s = opt_str(j, key);
  if (!s.has_value()) return std::nullopt;
  int64_t t = 0;
  if (!gutil::parse_datetime(*s, t)) return std::nullopt;
  return t;
}

// Scoped transaction: commits on success(), rolls back otherwise.
class Txn {
 public:
  explicit Txn(Db& db) : db_(db) {}
  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;
  ~Txn() {
    if (open_) db_.rollback();
  }
  bool begin(std::string& err) {
    open_ = db_.begin(err);
    return open_;
  }
  bool commit(std::string& err) {
    G_ASSERT(open_);
    open_ = false;
    return db_.commit(err);
  }

 private:
  Db& db_;
  bool open_ = false;
};

bool stmt_ready(const Stmt& s, Db& db, std::string& err) {
  if (s.ok()) return true;
  err = "sqlite prepare failed: " + db.last_error();
  return false;
}

// Walks an array of [ts_ms, value, ...] pairs, calling fn(ts_s, value_json).
template <typename Fn>
void for_each_pair(const json& arr, Fn fn) {
  if (!arr.is_array()) return;
  const size_t n = std::min(arr.size(), kMaxJsonArray);
  for (size_t i = 0; i < n; ++i) {
    const json& e = arr[i];
    if (!e.is_array() || e.size() < 2 || !e[0].is_number()) continue;
    fn(e[0].get<int64_t>() / 1000, e);
  }
}

// Body battery entries are [ts, ...] with the level being the first numeric
// element after the timestamp (the layout differs between endpoints).
// Runs inside the caller's transaction.
bool body_battery_values(Db& db, const json& j, ImportCounts& c, std::string& err) {
  if (!j.is_object() || !j.contains("bodyBatteryValuesArray")) return true;
  Stmt s(db, "INSERT OR REPLACE INTO body_battery_sample(ts, level) VALUES(?, ?)");
  if (!stmt_ready(s, db, err)) return false;
  bool ok = true;
  for_each_pair(j.at("bodyBatteryValuesArray"), [&](int64_t ts, const json& e) {
    if (!ok) return;
    std::optional<int64_t> level;
    for (size_t k = 1; k < e.size() && k < 4 && !level; ++k) {
      if (e[k].is_number()) level = e[k].get<int64_t>();
    }
    if (!level || *level < 0) return;
    ok = s.bind(1, ts).bind(2, *level).run(err);
    if (ok) ++c.rows;
  });
  return ok;
}

}  // namespace

// ------------------------------------------------------------ JSON imports

bool import_daily_summary(Db& db, const std::string& date, const json& j, ImportCounts& c,
                          std::string& err) {
  G_ASSERT(date.size() == 10);
  if (!j.is_object()) return true;  // nothing for that day
  Stmt s(db,
         "INSERT OR REPLACE INTO daily_summary(date, steps, distance_m, floors_up, resting_hr,"
         " min_hr, max_hr, avg_stress, body_battery_high, body_battery_low, total_kcal,"
         " active_kcal, sleep_seconds, json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;
  s.bind(1, date)
      .bind(2, opt_int(j, "totalSteps"))
      .bind(3, opt_double(j, "totalDistanceMeters"))
      .bind(4, opt_double(j, "floorsAscended"))
      .bind(5, opt_int(j, "restingHeartRate"))
      .bind(6, opt_int(j, "minHeartRate"))
      .bind(7, opt_int(j, "maxHeartRate"))
      .bind(8, opt_int(j, "averageStressLevel"))
      .bind(9, opt_int(j, "bodyBatteryHighestValue"))
      .bind(10, opt_int(j, "bodyBatteryLowestValue"))
      .bind(11, opt_int(j, "totalKilocalories"))
      .bind(12, opt_int(j, "activeKilocalories"))
      .bind(13, opt_int(j, "sleepingSeconds"))
      .bind(14, j.dump());
  if (!s.run(err)) return false;
  ++c.rows;
  return true;
}

bool import_heart_rate(Db& db, const json& j, ImportCounts& c, std::string& err) {
  if (!j.is_object() || !j.contains("heartRateValues")) return true;
  Txn txn(db);
  if (!txn.begin(err)) return false;
  Stmt s(db, "INSERT OR REPLACE INTO hr_sample(ts, source, bpm) VALUES(?, 'api', ?)");
  if (!stmt_ready(s, db, err)) return false;
  bool ok = true;
  for_each_pair(j.at("heartRateValues"), [&](int64_t ts, const json& e) {
    if (!ok || !e[1].is_number()) return;
    const int64_t bpm = e[1].get<int64_t>();
    if (bpm <= 0) return;
    ok = s.bind(1, ts).bind(2, bpm).run(err);
    if (ok) ++c.rows;
  });
  return ok && txn.commit(err);
}

bool import_sleep(Db& db, const std::string& date, const json& j, ImportCounts& c,
                  std::string& err) {
  if (!j.is_object() || !j.contains("dailySleepDTO")) return true;
  const json& d = j.at("dailySleepDTO");
  if (!d.is_object() || !opt_int(d, "sleepStartTimestampGMT").has_value()) return true;
  Txn txn(db);
  if (!txn.begin(err)) return false;

  std::optional<int64_t> score;
  if (d.contains("sleepScores") && d.at("sleepScores").is_object() &&
      d.at("sleepScores").contains("overall")) {
    score = opt_int(d.at("sleepScores").at("overall"), "value");
  }
  Stmt s(db,
         "INSERT OR REPLACE INTO sleep(date, start_ts, end_ts, deep_s, light_s, rem_s, awake_s,"
         " score, avg_spo2, avg_resp, avg_hrv, json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;
  s.bind(1, date)
      .bind(2, opt_ms_to_s(d, "sleepStartTimestampGMT"))
      .bind(3, opt_ms_to_s(d, "sleepEndTimestampGMT"))
      .bind(4, opt_int(d, "deepSleepSeconds"))
      .bind(5, opt_int(d, "lightSleepSeconds"))
      .bind(6, opt_int(d, "remSleepSeconds"))
      .bind(7, opt_int(d, "awakeSleepSeconds"))
      .bind(8, score)
      .bind(9, opt_double(d, "averageSpO2Value"))
      .bind(10, opt_double(d, "averageRespirationValue"))
      .bind(11, opt_double(d, "avgOvernightHrv"))
      .bind(12, j.dump());
  if (!s.run(err)) return false;
  ++c.rows;

  // Stage intervals: activityLevel 0=deep 1=light 2=rem 3=awake.
  if (j.contains("sleepLevels") && j.at("sleepLevels").is_array()) {
    Stmt st(db,
            "INSERT OR REPLACE INTO sleep_stage(start_ts, end_ts, stage, source)"
            " VALUES(?,?,?,'api')");
    if (!stmt_ready(st, db, err)) return false;
    static const char* kStages[] = {"deep", "light", "rem", "awake"};
    const json& levels = j.at("sleepLevels");
    const size_t n = std::min(levels.size(), kMaxJsonArray);
    for (size_t i = 0; i < n; ++i) {
      const json& e = levels[i];
      const auto start = opt_datetime(e, "startGMT");
      const auto end = opt_datetime(e, "endGMT");
      const auto lvl = opt_double(e, "activityLevel");
      if (!start || !end || !lvl) continue;
      const int idx = static_cast<int>(*lvl);
      if (idx < 0 || idx > 3) continue;
      if (!st.bind(1, *start).bind(2, *end).bind(3, std::string(kStages[idx])).run(err)) {
        return false;
      }
      ++c.rows;
    }
  }
  return txn.commit(err);
}

bool import_stress(Db& db, const json& j, ImportCounts& c, std::string& err) {
  if (!j.is_object()) return true;
  Txn txn(db);
  if (!txn.begin(err)) return false;
  bool ok = true;
  if (j.contains("stressValuesArray")) {
    Stmt s(db, "INSERT OR REPLACE INTO stress_sample(ts, source, level) VALUES(?, 'api', ?)");
    if (!stmt_ready(s, db, err)) return false;
    for_each_pair(j.at("stressValuesArray"), [&](int64_t ts, const json& e) {
      if (!ok || !e[1].is_number()) return;
      const int64_t level = e[1].get<int64_t>();
      if (level < 0) return;  // -1/-2 = not measured / activity
      ok = s.bind(1, ts).bind(2, level).run(err);
      if (ok) ++c.rows;
    });
  }
  if (ok && j.contains("bodyBatteryValuesArray")) {
    ok = body_battery_values(db, j, c, err);
  }
  return ok && txn.commit(err);
}

bool import_body_battery(Db& db, const json& j, ImportCounts& c, std::string& err) {
  // Accepts the dailyStress object or the bodyBattery/reports/daily array.
  Txn txn(db);
  if (!txn.begin(err)) return false;
  if (j.is_object()) {
    if (!body_battery_values(db, j, c, err)) return false;
  } else if (j.is_array()) {
    const size_t n = std::min(j.size(), kMaxJsonArray);
    for (size_t i = 0; i < n; ++i) {
      if (!body_battery_values(db, j[i], c, err)) return false;
    }
  }
  return txn.commit(err);
}

bool import_hrv(Db& db, const std::string& date, const json& j, ImportCounts& c,
                std::string& err) {
  if (!j.is_object() || !j.contains("hrvSummary")) return true;
  const json& sum = j.at("hrvSummary");
  Txn txn(db);
  if (!txn.begin(err)) return false;
  const json base = sum.is_object() ? sum.value("baseline", json::object()) : json::object();
  Stmt s(db,
         "INSERT OR REPLACE INTO hrv_daily(date, weekly_avg, last_night_avg,"
         " last_night_5min_high, status, baseline_low_upper, baseline_balanced_lower,"
         " baseline_balanced_upper, json) VALUES(?,?,?,?,?,?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;
  s.bind(1, date)
      .bind(2, opt_double(sum, "weeklyAvg"))
      .bind(3, opt_double(sum, "lastNightAvg"))
      .bind(4, opt_double(sum, "lastNight5MinHigh"))
      .bind(5, opt_str(sum, "status"))
      .bind(6, opt_double(base, "lowUpper"))
      .bind(7, opt_double(base, "balancedLow"))
      .bind(8, opt_double(base, "balancedUpper"))
      .bind(9, j.dump());
  if (!s.run(err)) return false;
  ++c.rows;

  if (j.contains("hrvReadings") && j.at("hrvReadings").is_array()) {
    Stmt r(db,
           "INSERT OR REPLACE INTO hrv_sample(ts, source, rmssd_ms) VALUES(?, 'api', ?)");
    if (!stmt_ready(r, db, err)) return false;
    const json& readings = j.at("hrvReadings");
    const size_t n = std::min(readings.size(), kMaxJsonArray);
    for (size_t i = 0; i < n; ++i) {
      const auto ts = opt_datetime(readings[i], "readingTimeGMT");
      const auto v = opt_double(readings[i], "hrvValue");
      if (!ts || !v) continue;
      if (!r.bind(1, *ts).bind(2, *v).run(err)) return false;
      ++c.rows;
    }
  }
  return txn.commit(err);
}

namespace {

// One weight-service measurement object -> a `weight` row. Body-composition
// fields come back as 0 when the impedance measurement did not take, which
// is "unknown", not a value, so zeros are stored as NULL.
bool weight_row(Stmt& s, const json& e, ImportCounts& c, std::string& err) {
  auto ts = opt_ms_to_s(e, "timestampGMT");
  if (!ts) ts = opt_ms_to_s(e, "date");
  const auto grams = opt_double(e, "weight");
  if (!ts || !grams || *grams <= 0.0) return true;
  auto nz = [](const std::optional<double>& v) -> std::optional<double> {
    if (!v || *v <= 0.0) return std::nullopt;
    return v;
  };
  auto to_kg = [&nz](const std::optional<double>& g) -> std::optional<double> {
    const auto v = nz(g);
    if (!v) return std::nullopt;
    return *v / 1000.0;
  };
  s.bind(1, *ts)
      .bind(2, *grams / 1000.0)
      .bind(3, nz(opt_double(e, "bmi")))
      .bind(4, nz(opt_double(e, "bodyFat")))
      .bind(5, nz(opt_double(e, "bodyWater")))
      .bind(6, to_kg(opt_double(e, "boneMass")))
      .bind(7, to_kg(opt_double(e, "muscleMass")))
      .bind(8, nz(opt_double(e, "visceralFat")))
      .bind(9, opt_int(e, "metabolicAge"))
      .bind(10, opt_str(e, "sourceType"))
      .bind(11, e.dump());
  if (!s.run(err)) return false;
  ++c.rows;
  return true;
}

}  // namespace

bool import_weight(Db& db, const json& j, ImportCounts& c, std::string& err) {
  // weight/dateRange returns {"dateWeightList": [measurement, ...]}; older
  // shapes nest them as dailyWeightSummaries[].allWeightMetrics[]. Accept both.
  if (!j.is_object()) return true;
  Txn txn(db);
  if (!txn.begin(err)) return false;
  Stmt s(db,
         "INSERT OR REPLACE INTO weight(ts, weight_kg, bmi, body_fat_pct, body_water_pct,"
         " bone_mass_kg, muscle_mass_kg, visceral_fat, metabolic_age, source, json)"
         " VALUES(?,?,?,?,?,?,?,?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;
  if (j.contains("dateWeightList") && j.at("dateWeightList").is_array()) {
    const json& list = j.at("dateWeightList");
    const size_t n = std::min(list.size(), kMaxJsonArray);
    for (size_t i = 0; i < n; ++i) {
      if (!weight_row(s, list[i], c, err)) return false;
    }
  }
  if (!j.contains("dailyWeightSummaries")) return txn.commit(err);
  const json& days = j.at("dailyWeightSummaries");
  if (!days.is_array()) return txn.commit(err);
  const size_t nd = std::min(days.size(), kMaxJsonArray);
  for (size_t d = 0; d < nd; ++d) {
    const json& day = days[d];
    if (!day.is_object() || !day.contains("allWeightMetrics")) continue;
    const json& metrics = day.at("allWeightMetrics");
    if (!metrics.is_array()) continue;
    const size_t nm = std::min(metrics.size(), kMaxJsonArray);
    for (size_t m = 0; m < nm; ++m) {
      if (!weight_row(s, metrics[m], c, err)) return false;
    }
  }
  return txn.commit(err);
}

bool import_activity_list(Db& db, const json& j, ImportCounts& c, std::string& err) {
  if (!j.is_array()) return true;
  Txn txn(db);
  if (!txn.begin(err)) return false;
  // fit_file_id is preserved across re-imports of the summary.
  Stmt s(db,
         "INSERT INTO activity(id, name, type, start_ts, duration_s, distance_m, avg_hr, max_hr,"
         " calories, elevation_gain_m, avg_speed_mps, training_effect, anaerobic_effect, json)"
         " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
         " ON CONFLICT(id) DO UPDATE SET name=excluded.name, type=excluded.type,"
         " start_ts=excluded.start_ts, duration_s=excluded.duration_s,"
         " distance_m=excluded.distance_m, avg_hr=excluded.avg_hr, max_hr=excluded.max_hr,"
         " calories=excluded.calories, elevation_gain_m=excluded.elevation_gain_m,"
         " avg_speed_mps=excluded.avg_speed_mps, training_effect=excluded.training_effect,"
         " anaerobic_effect=excluded.anaerobic_effect, json=excluded.json");
  if (!stmt_ready(s, db, err)) return false;
  const size_t n = std::min(j.size(), kMaxJsonArray);
  for (size_t i = 0; i < n; ++i) {
    const json& a = j[i];
    const auto id = opt_int(a, "activityId");
    if (!id) continue;
    std::optional<std::string> type;
    if (a.contains("activityType") && a.at("activityType").is_object()) {
      type = opt_str(a.at("activityType"), "typeKey");
    }
    s.bind(1, *id)
        .bind(2, opt_str(a, "activityName"))
        .bind(3, type)
        .bind(4, opt_datetime(a, "startTimeGMT"))
        .bind(5, opt_double(a, "duration"))
        .bind(6, opt_double(a, "distance"))
        .bind(7, opt_int(a, "averageHR"))
        .bind(8, opt_int(a, "maxHR"))
        .bind(9, opt_int(a, "calories"))
        .bind(10, opt_double(a, "elevationGain"))
        .bind(11, opt_double(a, "averageSpeed"))
        .bind(12, opt_double(a, "aerobicTrainingEffect"))
        .bind(13, opt_double(a, "anaerobicTrainingEffect"))
        .bind(14, a.dump());
    if (!s.run(err)) return false;
    ++c.rows;
  }
  return txn.commit(err);
}

// ------------------------------------------------------------- FIT import

namespace {

// Holds the prepared statements for one FIT import and routes messages.
class FitSink {
 public:
  FitSink(Db& db, int64_t file_id)
      : db_(db),
        file_id_(file_id),
        rec_(db,
             "INSERT OR REPLACE INTO activity_record(fit_file_id, ts, lat, lon, alt_m, hr,"
             " cadence, dist_m, speed_mps, power_w, temp_c) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
        sess_(db,
              "INSERT OR REPLACE INTO activity_session(fit_file_id, start_ts, sport, sub_sport,"
              " elapsed_s, timer_s, distance_m, calories, avg_hr, max_hr, avg_speed_mps,"
              " max_speed_mps, ascent_m, descent_m, avg_cadence, avg_power, training_effect,"
              " anaerobic_effect) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
        lap_(db,
             "INSERT OR REPLACE INTO activity_lap(fit_file_id, start_ts, elapsed_s, timer_s,"
             " distance_m, avg_hr, max_hr, avg_speed_mps, calories) VALUES(?,?,?,?,?,?,?,?,?)"),
        hrv_(db, "INSERT OR REPLACE INTO activity_hrv(fit_file_id, seq, rr_ms) VALUES(?,?,?)"),
        hr_(db, "INSERT OR REPLACE INTO hr_sample(ts, source, bpm) VALUES(?, 'fit', ?)"),
        stress_(db,
                "INSERT OR REPLACE INTO stress_sample(ts, source, level) VALUES(?, 'fit', ?)"),
        sleep_(db, "INSERT OR REPLACE INTO sleep_level_sample(ts, level) VALUES(?, ?)"),
        resp_(db, "INSERT OR REPLACE INTO respiration_sample(ts, brpm) VALUES(?, ?)"),
        spo2_(db, "INSERT OR REPLACE INTO spo2_sample(ts, pct, confidence) VALUES(?, ?, ?)"),
        hrvv_(db, "INSERT OR REPLACE INTO hrv_sample(ts, source, rmssd_ms) VALUES(?, 'fit', ?)"),
        weight_(db,
                "INSERT OR REPLACE INTO weight(ts, weight_kg, bmi, body_fat_pct, body_water_pct,"
                " bone_mass_kg, muscle_mass_kg, visceral_fat, metabolic_age, source, json)"
                " VALUES(?,?,?,?,?,?,?,?,?,'fit',NULL)") {}

  bool ready(std::string& err) const {
    const Stmt* all[] = {&rec_, &sess_, &lap_, &hrv_, &hr_, &stress_, &sleep_, &resp_, &spo2_,
                         &hrvv_, &weight_};
    for (const Stmt* s : all) {
      if (!stmt_ready(*s, db_, err)) return false;
    }
    return true;
  }

  void on_message(const fit::Message& m) {
    if (!ok_) return;
    switch (m.global_num) {
      case fit::kMesgFileId: on_file_id(m); break;
      case fit::kMesgRecord: on_record(m); break;
      case fit::kMesgSession: on_session(m); break;
      case fit::kMesgLap: on_lap(m); break;
      case fit::kMesgHrv: on_hrv(m); break;
      case fit::kMesgMonitoring: on_monitoring(m); break;
      case fit::kMesgStressLevel: on_stress(m); break;
      case fit::kMesgSleepLevel: on_sleep_level(m); break;
      case fit::kMesgRespirationRate: on_respiration(m); break;
      case fit::kMesgSpo2Data: on_spo2(m); break;
      case fit::kMesgHrvValue: on_hrv_value(m); break;
      case fit::kMesgWeightScale: on_weight(m); break;
      default: break;
    }
  }

  bool ok() const { return ok_; }
  const std::string& error() const { return err_; }
  int64_t rows() const { return rows_; }

  // file_id summary for the fit_file row.
  std::optional<int64_t> file_type;
  std::optional<int64_t> time_created;
  std::optional<int64_t> serial_number;
  std::optional<std::string> product_name;

 private:
  static std::optional<double> scaled(const fit::Message& m, uint8_t f) {
    double v = 0.0;
    if (m.get_scaled(f, v)) return v;
    return std::nullopt;
  }
  static std::optional<int64_t> integer(const fit::Message& m, uint8_t f) {
    int64_t v = 0;
    if (m.get_int(f, v)) return v;
    return std::nullopt;
  }
  static std::optional<double> degrees(const fit::Message& m, uint8_t f) {
    int64_t v = 0;
    if (!m.get_int(f, v)) return std::nullopt;
    return fit::semicircles_to_degrees(static_cast<int32_t>(v));
  }
  static std::optional<int64_t> unix_ts(const fit::Message& m, uint8_t f) {
    int64_t v = 0;
    if (!m.get_int(f, v) || v <= 0) return std::nullopt;
    return gutil::fit_to_unix(static_cast<uint32_t>(v));
  }
  // Prefers the "enhanced" 32-bit field when present.
  static std::optional<double> either(const fit::Message& m, uint8_t enhanced, uint8_t base) {
    auto v = scaled(m, enhanced);
    return v ? v : scaled(m, base);
  }

  void commit(Stmt& s) {
    if (!s.run(err_)) {
      ok_ = false;
      return;
    }
    ++rows_;
  }

  void on_file_id(const fit::Message& m) {
    file_type = integer(m, 0);
    serial_number = integer(m, 3);
    time_created = unix_ts(m, 4);
    const fit::FieldValue* name = m.find(8);
    if (name != nullptr && name->valid) product_name = std::string(name->as_string());
  }

  void on_record(const fit::Message& m) {
    if (!m.has_timestamp) return;
    rec_.bind(1, file_id_)
        .bind(2, gutil::fit_to_unix(m.timestamp))
        .bind(3, degrees(m, 0))
        .bind(4, degrees(m, 1))
        .bind(5, either(m, 78, 2))
        .bind(6, integer(m, 3))
        .bind(7, integer(m, 4))
        .bind(8, scaled(m, 5))
        .bind(9, either(m, 73, 6))
        .bind(10, integer(m, 7))
        .bind(11, scaled(m, 13));
    commit(rec_);
  }

  void on_session(const fit::Message& m) {
    const auto start = unix_ts(m, 2);
    if (!start) return;
    sess_.bind(1, file_id_)
        .bind(2, *start)
        .bind(3, integer(m, 5))
        .bind(4, integer(m, 6))
        .bind(5, scaled(m, 7))
        .bind(6, scaled(m, 8))
        .bind(7, scaled(m, 9))
        .bind(8, integer(m, 11))
        .bind(9, integer(m, 16))
        .bind(10, integer(m, 17))
        .bind(11, either(m, 124, 14))
        .bind(12, either(m, 125, 15))
        .bind(13, scaled(m, 22))
        .bind(14, scaled(m, 23))
        .bind(15, integer(m, 18))
        .bind(16, integer(m, 20))
        .bind(17, scaled(m, 24))
        .bind(18, scaled(m, 137));
    commit(sess_);
  }

  void on_lap(const fit::Message& m) {
    const auto start = unix_ts(m, 2);
    if (!start) return;
    lap_.bind(1, file_id_)
        .bind(2, *start)
        .bind(3, scaled(m, 7))
        .bind(4, scaled(m, 8))
        .bind(5, scaled(m, 9))
        .bind(6, integer(m, 15))
        .bind(7, integer(m, 16))
        .bind(8, either(m, 110, 13))
        .bind(9, integer(m, 11));
    commit(lap_);
  }

  void on_hrv(const fit::Message& m) {
    const fit::FieldValue* t = m.find(0);
    if (t == nullptr || !t->valid) return;
    const size_t n = t->count();
    for (size_t i = 0; i < n && ok_; ++i) {
      if (!t->element_valid(i)) continue;
      // Raw units are 1/1000 s, i.e. milliseconds.
      hrv_.bind(1, file_id_).bind(2, hrv_seq_++).bind(3, static_cast<double>(t->as_int(i)));
      commit(hrv_);
    }
  }

  void on_monitoring(const fit::Message& m) {
    if (!m.has_timestamp) return;
    const auto hr = integer(m, 27);
    if (!hr || *hr <= 0) return;
    hr_.bind(1, gutil::fit_to_unix(m.timestamp)).bind(2, *hr);
    commit(hr_);
  }

  void on_stress(const fit::Message& m) {
    const auto level = integer(m, 0);
    const auto ts = unix_ts(m, 1);
    if (!level || !ts || *level < 0) return;
    stress_.bind(1, *ts).bind(2, *level);
    commit(stress_);
  }

  void on_sleep_level(const fit::Message& m) {
    const auto level = integer(m, 0);
    if (!m.has_timestamp || !level) return;
    sleep_.bind(1, gutil::fit_to_unix(m.timestamp)).bind(2, *level);
    commit(sleep_);
  }

  void on_respiration(const fit::Message& m) {
    const auto v = scaled(m, 0);
    if (!m.has_timestamp || !v || *v <= 0.0) return;
    resp_.bind(1, gutil::fit_to_unix(m.timestamp)).bind(2, *v);
    commit(resp_);
  }

  void on_spo2(const fit::Message& m) {
    const auto pct = integer(m, 0);
    if (!m.has_timestamp || !pct || *pct <= 0) return;
    spo2_.bind(1, gutil::fit_to_unix(m.timestamp)).bind(2, *pct).bind(3, integer(m, 1));
    commit(spo2_);
  }

  void on_hrv_value(const fit::Message& m) {
    const auto v = scaled(m, 0);
    if (!m.has_timestamp || !v || *v <= 0.0) return;
    hrvv_.bind(1, gutil::fit_to_unix(m.timestamp)).bind(2, *v);
    commit(hrvv_);
  }

  void on_weight(const fit::Message& m) {
    const auto kg = scaled(m, 0);
    if (!m.has_timestamp || !kg || *kg <= 0.0) return;
    weight_.bind(1, gutil::fit_to_unix(m.timestamp))
        .bind(2, *kg)
        .bind(3, scaled(m, 13))
        .bind(4, scaled(m, 1))
        .bind(5, scaled(m, 2))
        .bind(6, scaled(m, 4))
        .bind(7, scaled(m, 5))
        .bind(8, scaled(m, 3))
        .bind(9, integer(m, 10));
    commit(weight_);
  }

  Db& db_;
  int64_t file_id_;
  Stmt rec_, sess_, lap_, hrv_, hr_, stress_, sleep_, resp_, spo2_, hrvv_, weight_;
  int64_t hrv_seq_ = 0;
  int64_t rows_ = 0;
  bool ok_ = true;
  std::string err_;
};

}  // namespace

bool import_fit(Db& db, const std::string& path, const std::vector<uint8_t>& bytes,
                int64_t activity_id, bool force, ImportCounts& c, std::string& err) {
  G_ASSERT(!path.empty());
  Txn txn(db);
  if (!txn.begin(err)) return false;

  // Claim the fit_file row; if it already exists and we are not forcing, skip.
  int64_t file_id = 0;
  {
    Stmt find(db, "SELECT id FROM fit_file WHERE path = ?");
    if (!stmt_ready(find, db, err)) return false;
    find.bind(1, path);
    if (find.row()) {
      file_id = find.col_int(0);
      if (!force) {
        c.skipped = true;
        return txn.commit(err);
      }
    }
  }
  if (file_id == 0) {
    Stmt ins(db, "INSERT INTO fit_file(path, imported_at, messages) VALUES(?, ?, 0)");
    if (!stmt_ready(ins, db, err)) return false;
    if (!ins.bind(1, path).bind(2, gutil::now_unix()).run(err)) return false;
    file_id = db.last_insert_rowid();
  } else {
    // Re-import: drop the previous derived rows for this file.
    const char* kClear[] = {"DELETE FROM activity_record WHERE fit_file_id = ?",
                            "DELETE FROM activity_session WHERE fit_file_id = ?",
                            "DELETE FROM activity_lap WHERE fit_file_id = ?",
                            "DELETE FROM activity_hrv WHERE fit_file_id = ?"};
    for (const char* sql : kClear) {
      Stmt del(db, sql);
      if (!stmt_ready(del, db, err)) return false;
      if (!del.bind(1, file_id).run(err)) return false;
    }
  }

  FitSink sink(db, file_id);
  if (!sink.ready(err)) return false;
  auto decoder = std::make_unique<fit::Decoder>();
  std::string decode_err;
  const bool decoded = decoder->decode(bytes.data(), bytes.size(),
                                       [&sink](const fit::Message& m) { sink.on_message(m); },
                                       decode_err);
  if (!sink.ok()) {
    err = sink.error();
    return false;
  }
  if (!decoded) {
    err = path + ": " + decode_err;
    return false;
  }
  c.messages += static_cast<int64_t>(decoder->stats().messages);
  c.rows += sink.rows();

  Stmt upd(db,
           "UPDATE fit_file SET file_type=?, time_created=?, serial_number=?, product_name=?,"
           " imported_at=?, messages=? WHERE id=?");
  if (!stmt_ready(upd, db, err)) return false;
  upd.bind(1, sink.file_type)
      .bind(2, sink.time_created)
      .bind(3, sink.serial_number)
      .bind(4, sink.product_name)
      .bind(5, gutil::now_unix())
      .bind(6, static_cast<int64_t>(decoder->stats().messages))
      .bind(7, file_id);
  if (!upd.run(err)) return false;

  if (activity_id > 0) {
    Stmt link(db, "UPDATE activity SET fit_file_id=? WHERE id=?");
    if (!stmt_ready(link, db, err)) return false;
    if (!link.bind(1, file_id).bind(2, activity_id).run(err)) return false;
  }
  return txn.commit(err);
}

// ---------------------------------------------------------- blood pressure

namespace {

// Splits one CSV line on commas (the Omron export has no quoting).
std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : line) {
    if (ch == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (ch != '\r') {
      cur.push_back(ch);
    }
  }
  out.push_back(cur);
  return out;
}

// "YYYY-MM-DD HH:MM:SS" in local time -> Unix seconds.
bool parse_local_datetime(const std::string& s, int64_t& out) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  G_REQUIRE_RET(s.size() >= 19, false);
  G_REQUIRE_RET(std::sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) == 6,
                false);
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;
  const time_t t = mktime(&tm);
  G_REQUIRE_RET(t != static_cast<time_t>(-1), false);
  out = static_cast<int64_t>(t);
  return true;
}

int64_t to_int(const std::string& s) { return std::atoll(s.c_str()); }

}  // namespace

bool import_bp_csv(Db& db, const std::string& csv_text, ImportCounts& c, std::string& err) {
  constexpr size_t kMaxLines = 200000;
  Txn txn(db);
  if (!txn.begin(err)) return false;
  Stmt s(db,
         "INSERT OR IGNORE INTO blood_pressure(ts, cuff_user, systolic, diastolic, pulse, model,"
         " device, movement, irregular) VALUES(?,?,?,?,?,?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;

  std::vector<std::string> header;
  size_t pos = 0;
  for (size_t line_no = 0; line_no < kMaxLines && pos < csv_text.size(); ++line_no) {
    size_t end = csv_text.find('\n', pos);
    if (end == std::string::npos) end = csv_text.size();
    const std::string line = csv_text.substr(pos, end - pos);
    pos = end + 1;
    if (line.empty() || line == "\r") continue;
    const std::vector<std::string> f = split_csv(line);
    if (header.empty()) {
      header = f;
      if (header.size() < 7 || header[0] != "timestamp" || header[4] != "systolic") {
        err = "blood pressure csv: unexpected header";
        return false;
      }
      continue;
    }
    if (f.size() < 7) continue;
    int64_t ts = 0;
    if (!parse_local_datetime(f[0], ts)) continue;
    const int64_t sys = to_int(f[4]);
    const int64_t dia = to_int(f[5]);
    if (sys < 50 || sys > 300 || dia < 30 || dia > 200) continue;
    s.bind(1, ts)
        .bind(2, to_int(f[3]))
        .bind(3, sys)
        .bind(4, dia)
        .bind(5, f[6].empty() ? std::optional<int64_t>() : std::optional<int64_t>(to_int(f[6])))
        .bind(6, f[1])
        .bind(7, f[2])
        .bind(8, f.size() > 7 ? std::optional<int64_t>(to_int(f[7])) : std::optional<int64_t>())
        .bind(9, f.size() > 8 ? std::optional<int64_t>(to_int(f[8])) : std::optional<int64_t>());
    if (!s.run(err)) return false;
    c.rows += db.changes();
  }
  return txn.commit(err);
}

// --------------------------------------------------------------- sync_log

bool sync_done(Db& db, const std::string& kind, const std::string& key) {
  Stmt s(db, "SELECT ok FROM sync_log WHERE kind=? AND key=?");
  G_REQUIRE_RET(s.ok(), false);
  s.bind(1, kind).bind(2, key);
  return s.row() && s.col_int(0) != 0;
}

bool mark_sync(Db& db, const std::string& kind, const std::string& key, bool ok,
               std::string& err) {
  Stmt s(db, "INSERT OR REPLACE INTO sync_log(kind, key, fetched_at, ok) VALUES(?,?,?,?)");
  if (!stmt_ready(s, db, err)) return false;
  return s.bind(1, kind).bind(2, key).bind(3, gutil::now_unix()).bind(4, ok ? 1 : 0).run(err);
}

}  // namespace store
