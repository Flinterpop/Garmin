// gsync: pull health data from Garmin Connect into a local SQLite store,
// and import FIT files copied straight off a watch.
#include <cstdio>
#include <string>

#include "commands.h"
#include "util/time_util.h"

namespace {

// Redirects stdout and stderr to an append-mode log file with a timestamped
// header, so a Task Scheduler run leaves a record.
bool open_log(const std::string& path, int argc, char** argv) {
  if (std::freopen(path.c_str(), "a", stdout) == nullptr) return false;
  if (std::freopen(path.c_str(), "a", stderr) == nullptr) return false;
  // Unbuffered so an interrupted run still leaves its last lines in the log.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::printf("\n==== %s  gsync", gutil::iso8601_utc(gutil::now_unix()).c_str());
  for (int i = 1; i < argc; ++i) std::printf(" %s", argv[i]);
  std::printf("\n");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  cmd::Options o;
  std::string err;
  if (!cmd::parse(argc, argv, o, err)) {
    if (!err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
    cmd::usage();
    return 2;
  }
  if (!o.log_file.empty() && !open_log(o.log_file, argc, argv)) {
    std::fprintf(stderr, "error: cannot open log file %s\n", o.log_file.c_str());
    return 2;
  }
  if (o.command == "login") return cmd::run_login(o);
  if (o.command == "logout") return cmd::run_logout(o);
  if (o.command == "whoami") return cmd::run_whoami(o);
  if (o.command == "sync") return cmd::run_sync(o);
  if (o.command == "import") return cmd::run_import(o);
  if (o.command == "get") return cmd::run_get(o);
  if (o.command == "stats") return cmd::run_stats(o);
  if (o.command == "import-bp") return cmd::run_import_bp(o);
  if (o.command == "profiles") return cmd::run_profiles(o);
  if (o.command == "migrate-appdata") return cmd::run_migrate_appdata(o);
  cmd::usage();
  return 2;
}
