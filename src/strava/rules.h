// Which sports go to Strava, as what, and under which title: one rule per FIT
// sport number, kept in <profile folder>\strava.ini (no secrets in it):
//
//   [strava]
//   since = 2026-01-01      only activities that started on or after this date
//   [sport 73]
//   enabled = 1
//   type = IceSkate         a Strava sport_type
//   name = Old guy hockey   the title the activity gets on Strava
//   min_minutes = 10        shorter ones (accidental starts) are skipped
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace strava {

constexpr size_t kMaxRules = 64;
constexpr size_t kMaxTitle = 100;
constexpr int kMaxMinMinutes = 600;
constexpr int kDefaultMinMinutes = 10;
constexpr int kDefaultSinceDays = 30;

struct SportRule {
  int sport = -1;  // FIT sport number (activity_session.sport)
  bool enabled = false;
  std::string sport_type;  // Strava sport_type, e.g. "IceSkate"
  std::string name;        // title on Strava; empty keeps Strava's own
  int min_minutes = kDefaultMinMinutes;
};

struct Settings {
  std::string since;  // YYYY-MM-DD
  std::vector<SportRule> rules;
};

std::filesystem::path settings_path(const std::filesystem::path& profile_base);

// Missing file or entries give defaults: since = 30 days ago, no rules.
Settings load_settings(const std::filesystem::path& ini);
bool save_settings(const std::filesystem::path& ini, const Settings& s);

Settings parse_settings(const std::string& text, const std::string& default_since);
std::string format_settings(const Settings& s);

// The sport types Strava accepts, in display order.
const std::vector<std::string>& sport_types();
bool valid_sport_type(const std::string& t);
// 1..kMaxTitle bytes, no control characters.
bool valid_title(const std::string& t);

// A sensible Strava type for a FIT sport, used when a rule is first created.
std::string default_sport_type(int fit_sport);

// The rule for `sport`, or nullptr.
const SportRule* find_rule(const Settings& s, int sport);
SportRule* find_rule(Settings& s, int sport);
// The rule for `sport`, created with defaults when missing (nullptr when full).
SportRule* rule_for(Settings& s, int sport);

bool any_enabled(const Settings& s);

}  // namespace strava
