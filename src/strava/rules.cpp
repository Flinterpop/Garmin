#include "strava/rules.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace strava {

namespace {

constexpr size_t kMaxLines = 2000;
constexpr int kMaxFitSport = 255;

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  G_ASSERT(e >= b);
  return s.substr(b, e - b + 1);
}

bool parse_int(const std::string& s, int lo, int hi, int& out) {
  G_ASSERT(lo <= hi);
  if (s.empty() || s.size() > 6) return false;
  int v = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool valid_date(const std::string& ymd) {
  int64_t ts = 0;
  return ymd.size() == 10 && gutil::parse_date(ymd, ts);
}

// "[sport 73]" -> 73; -1 for any other section.
int section_sport(const std::string& header) {
  const std::string prefix = "sport ";
  if (header.rfind(prefix, 0) != 0) return -1;
  int sport = -1;
  return parse_int(header.substr(prefix.size()), 0, kMaxFitSport, sport) ? sport : -1;
}

void apply_key(SportRule& r, const std::string& key, const std::string& value) {
  int n = 0;
  if (key == "enabled") r.enabled = value == "1";
  if (key == "type" && valid_sport_type(value)) r.sport_type = value;
  if (key == "name" && (value.empty() || valid_title(value))) r.name = value;
  if (key == "min_minutes" && parse_int(value, 0, kMaxMinMinutes, n)) r.min_minutes = n;
}

}  // namespace

std::filesystem::path settings_path(const std::filesystem::path& profile_base) {
  G_REQUIRE_RET(!profile_base.empty(), std::filesystem::path());
  return profile_base / L"strava.ini";
}

const std::vector<std::string>& sport_types() {
  static const std::vector<std::string> kTypes = {
      "AlpineSki", "BackcountrySki", "Badminton", "Canoeing", "Crossfit", "EBikeRide", "Elliptical",
      "EMountainBikeRide", "Golf", "GravelRide", "Handcycle", "HighIntensityIntervalTraining", "Hike",
      "IceSkate", "InlineSkate", "Kayaking", "Kitesurf", "MountainBikeRide", "NordicSki", "Pickleball",
      "Pilates", "Racquetball", "Ride", "RockClimbing", "RollerSki", "Rowing", "Run", "Sail",
      "Skateboard", "Snowboard", "Snowshoe", "Soccer", "Squash", "StairStepper", "StandUpPaddling",
      "Surfing", "Swim", "TableTennis", "Tennis", "TrailRun", "Velomobile", "VirtualRide", "VirtualRow",
      "VirtualRun", "Walk", "WeightTraining", "Wheelchair", "Windsurf", "Workout", "Yoga"};
  return kTypes;
}

bool valid_sport_type(const std::string& t) {
  const std::vector<std::string>& all = sport_types();
  return std::find(all.begin(), all.end(), t) != all.end();
}

bool valid_title(const std::string& t) {
  if (t.empty() || t.size() > kMaxTitle) return false;
  for (size_t i = 0; i < t.size(); ++i) {
    if (static_cast<unsigned char>(t[i]) < 0x20 || t[i] == 0x7f) return false;
  }
  return true;
}

std::string default_sport_type(int fit_sport) {
  G_ASSERT(fit_sport >= -1);
  switch (fit_sport) {
    case 1: return "Run";
    case 2: return "Ride";
    case 5: return "Swim";
    case 10: return "WeightTraining";
    case 11: return "Walk";
    case 12: return "NordicSki";
    case 13: return "AlpineSki";
    case 14: return "Snowboard";
    case 15: return "Rowing";
    case 17: return "Hike";
    case 25: return "Golf";
    case 30: return "InlineSkate";
    case 31: return "RockClimbing";
    case 33: return "IceSkate";
    case 35: return "Snowshoe";
    case 41: return "Kayaking";
    case 62: return "HighIntensityIntervalTraining";
    case 73: return "IceSkate";  // Strava has no hockey type
    default: return "Workout";
  }
}

Settings parse_settings(const std::string& text, const std::string& default_since) {
  G_ASSERT(valid_date(default_since));
  Settings s;
  s.since = default_since;
  std::istringstream in(text);
  std::string line;
  SportRule* cur = nullptr;
  bool in_main = false;
  for (size_t n = 0; n < kMaxLines && std::getline(in, line); ++n) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      const std::string header = trim(line.substr(1, line.size() - 2));
      in_main = header == "strava";
      const int sport = section_sport(header);
      cur = sport >= 0 ? rule_for(s, sport) : nullptr;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (in_main && key == "since" && valid_date(value)) s.since = value;
    if (cur != nullptr) apply_key(*cur, key, value);
  }
  G_ASSERT(s.rules.size() <= kMaxRules);
  return s;
}

std::string format_settings(const Settings& s) {
  G_ASSERT(s.rules.size() <= kMaxRules);
  std::string out = "; gview: which activities go to Strava (Data > Strava > Settings)\n";
  out += "[strava]\nsince = " + s.since + "\n";
  for (size_t i = 0; i < s.rules.size() && i < kMaxRules; ++i) {
    const SportRule& r = s.rules[i];
    char mins[16] = {};
    std::snprintf(mins, sizeof(mins), "%d", r.min_minutes);
    out += "\n[sport " + std::to_string(r.sport) + "]\n";
    out += std::string("enabled = ") + (r.enabled ? "1" : "0") + "\n";
    out += "type = " + r.sport_type + "\n";
    out += "name = " + r.name + "\n";
    out += std::string("min_minutes = ") + mins + "\n";
  }
  return out;
}

Settings load_settings(const std::filesystem::path& ini) {
  G_ASSERT(!ini.empty());
  const std::string since =
      gutil::date_string_local(gutil::now_unix() - int64_t{kDefaultSinceDays} * 86400);
  std::string text;
  if (!gutil::read_text_file(ini, text)) text.clear();
  return parse_settings(text, since);
}

bool save_settings(const std::filesystem::path& ini, const Settings& s) {
  G_REQUIRE_RET(!ini.empty() && valid_date(s.since), false);
  return gutil::write_text_file(ini, format_settings(s));
}

const SportRule* find_rule(const Settings& s, int sport) {
  for (size_t i = 0; i < s.rules.size() && i < kMaxRules; ++i) {
    if (s.rules[i].sport == sport) return &s.rules[i];
  }
  return nullptr;
}

SportRule* find_rule(Settings& s, int sport) {
  for (size_t i = 0; i < s.rules.size() && i < kMaxRules; ++i) {
    if (s.rules[i].sport == sport) return &s.rules[i];
  }
  return nullptr;
}

SportRule* rule_for(Settings& s, int sport) {
  G_REQUIRE_RET(sport >= 0 && sport <= kMaxFitSport, nullptr);
  SportRule* found = find_rule(s, sport);
  if (found != nullptr) return found;
  if (s.rules.size() >= kMaxRules) return nullptr;
  SportRule r;
  r.sport = sport;
  r.sport_type = default_sport_type(sport);
  s.rules.push_back(r);
  return &s.rules.back();
}

bool any_enabled(const Settings& s) {
  for (size_t i = 0; i < s.rules.size() && i < kMaxRules; ++i) {
    if (s.rules[i].enabled) return true;
  }
  return false;
}

}  // namespace strava
