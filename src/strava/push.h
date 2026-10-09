// Sends a profile's activities to Strava by the rules in strava.ini: an
// activity Strava already has (Garmin's own Strava link, or an earlier
// upload) gets its sport type and title set; one it lacks is uploaded from
// the FIT file and then set. Each one is recorded in the strava_push table
// with the type and title applied, and left alone while the rule still says
// the same, so later edits on Strava stay; changing the rule re-applies it
// to the activities already sent (but never re-sends one deleted on Strava).
// Runs after every sync (gsync's morning task and gview's Sync now) and from
// gview's Data > Strava > Send now.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "strava/rules.h"

namespace store {
class Db;
}

namespace strava {

// Same signature as syncer::Report, so either can be passed.
using Report = std::function<void(bool error, const std::string& line)>;

constexpr int kMaxPerRun = 300;
constexpr int kMaxAttempts = 3;  // a file Strava keeps rejecting is given up on

struct PushOptions {
  std::filesystem::path profile_base;  // holds strava.bin and strava.ini
  std::filesystem::path data_dir;      // garmin.db
  const std::atomic<bool>* cancel = nullptr;
};

struct PushResult {
  int uploaded = 0;
  int updated = 0;    // already on Strava, sport type / title changed
  int unchanged = 0;  // already on Strava as wanted
  int failures = 0;
  int left = 0;       // still to do next time (rate limit, still processing, stopped)
  bool not_connected = false;
  bool nothing_chosen = false;  // connected, but no sport is enabled
  bool login_required = false;  // Strava refused the saved login
};

struct Candidate {
  int64_t start_ts = 0;
  int sport = -1;
  std::filesystem::path fit;
  bool recheck = false;  // sent before under a different type or title
};

// Activities the rules select that are not yet done, or were done under a
// type or title the rule has since changed; oldest first, one per start
// time (the copy with records), at most kMaxPerRun.
std::vector<Candidate> select_candidates(store::Db& db, const Settings& s);

PushResult run_push(const PushOptions& o, const Report& report);

// A one-line summary for the progress log.
std::string summary(const PushResult& r);

}  // namespace strava
