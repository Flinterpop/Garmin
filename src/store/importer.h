// Turns Garmin Connect JSON responses and decoded FIT files into rows.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "store/db.h"

namespace store {

struct ImportCounts {
  int64_t rows = 0;      // rows written (upserts count as written)
  int64_t messages = 0;  // FIT messages seen
  bool skipped = false;  // FIT file was already imported
};

// JSON importers. Each runs inside its own transaction.
bool import_daily_summary(Db& db, const std::string& date, const nlohmann::json& j,
                          ImportCounts& c, std::string& err);
bool import_heart_rate(Db& db, const nlohmann::json& j, ImportCounts& c, std::string& err);
bool import_sleep(Db& db, const std::string& date, const nlohmann::json& j, ImportCounts& c,
                  std::string& err);
bool import_stress(Db& db, const nlohmann::json& j, ImportCounts& c, std::string& err);
bool import_body_battery(Db& db, const nlohmann::json& j, ImportCounts& c, std::string& err);
bool import_hrv(Db& db, const std::string& date, const nlohmann::json& j, ImportCounts& c,
                std::string& err);
bool import_weight(Db& db, const nlohmann::json& j, ImportCounts& c, std::string& err);
bool import_activity_list(Db& db, const nlohmann::json& j, ImportCounts& c, std::string& err);

// FIT importer. `path` is the on-disk location (unique key); `activity_id`
// links the file to a Connect activity when known (0 otherwise). Files
// already present in fit_file are skipped unless `force`.
bool import_fit(Db& db, const std::string& path, const std::vector<uint8_t>& bytes,
                int64_t activity_id, bool force, ImportCounts& c, std::string& err);

// Blood-pressure CSV as exported by the Omron connect app:
// timestamp,model,device,user,systolic,diastolic,pulse,movement,irregular_heartbeat
// Timestamps are local time. Re-importing the same file is a no-op.
bool import_bp_csv(Db& db, const std::string& csv_text, ImportCounts& c, std::string& err);

// sync_log bookkeeping so a re-run can skip fetched days.
bool sync_done(Db& db, const std::string& kind, const std::string& key);
bool mark_sync(Db& db, const std::string& kind, const std::string& key, bool ok,
               std::string& err);

}  // namespace store
