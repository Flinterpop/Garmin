// gsync: pull health data from Garmin Connect into a local SQLite store,
// and import FIT files copied straight off a watch.
#include <cstdio>
#include <string>

#include "commands.h"

int main(int argc, char** argv) {
  cmd::Options o;
  std::string err;
  if (!cmd::parse(argc, argv, o, err)) {
    if (!err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
    cmd::usage();
    return 2;
  }
  if (o.command == "login") return cmd::run_login(o);
  if (o.command == "logout") return cmd::run_logout(o);
  if (o.command == "whoami") return cmd::run_whoami(o);
  if (o.command == "sync") return cmd::run_sync(o);
  if (o.command == "import") return cmd::run_import(o);
  if (o.command == "get") return cmd::run_get(o);
  if (o.command == "stats") return cmd::run_stats(o);
  cmd::usage();
  return 2;
}
