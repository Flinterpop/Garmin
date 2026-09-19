#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace cmd {

struct Options {
  std::string command;
  std::vector<std::string> args;
  std::filesystem::path data_dir;   // --data
  std::string from_date;            // --from YYYY-MM-DD
  std::string to_date;              // --to   YYYY-MM-DD
  int days = 7;                     // --days N (used when --from absent)
  int max_activities = 50;          // --activities N
  bool no_fit = false;              // --no-fit
  bool force = false;               // --force
  std::string out_file;             // --out (for `get`)
  std::string log_file;             // --log: append all output to this file
};

bool parse(int argc, char** argv, Options& o, std::string& err);
void usage();

int run_login(const Options& o);
int run_logout(const Options& o);
int run_whoami(const Options& o);
int run_sync(const Options& o);
int run_import(const Options& o);
int run_get(const Options& o);
int run_stats(const Options& o);

}  // namespace cmd
