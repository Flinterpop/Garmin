#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "gc/token_store.h"
#include "util/file_util.h"

namespace fs = std::filesystem;

TEST_CASE("profile names: letters, digits, _ and -, at most 32", "[profiles]") {
  CHECK(gutil::valid_profile_name(""));  // the default profile
  CHECK(gutil::valid_profile_name("ann"));
  CHECK(gutil::valid_profile_name("Bob_2-x"));
  CHECK(gutil::valid_profile_name(std::string(gutil::kMaxProfileName, 'a')));
  CHECK_FALSE(gutil::valid_profile_name(std::string(gutil::kMaxProfileName + 1, 'a')));
  CHECK_FALSE(gutil::valid_profile_name("../escape"));  // never a path outside profiles
  CHECK_FALSE(gutil::valid_profile_name("a b"));
  CHECK_FALSE(gutil::valid_profile_name("c:"));
  CHECK_FALSE(gutil::valid_profile_name("x.y"));
}

TEST_CASE("profile folders sit beside the exe; default is the exe folder", "[profiles]") {
  const fs::path base = fs::path("C:/portable/garmin");
  CHECK(gutil::profile_dir(base, "") == base);
  CHECK(gutil::profile_dir(base, "ann") == base / "profiles" / "ann");
  CHECK(gc::token_path(gutil::profile_dir(base, "ann")) == base / "profiles" / "ann" / "tokens.bin");
  CHECK(gc::token_path(base) == base / "tokens.bin");
}

TEST_CASE("exe_dir is the test binary's folder, not AppData", "[profiles]") {
  const fs::path dir = gutil::exe_dir();
  REQUIRE_FALSE(dir.empty());
  CHECK(fs::exists(dir / "garmin_tests.exe"));
  CHECK(dir.wstring().find(L"AppData") == std::wstring::npos);
}

TEST_CASE("list_profiles: default first, then valid folders sorted", "[profiles]") {
  const fs::path base = fs::temp_directory_path() / "gview_test_profiles";
  std::error_code ec;
  fs::remove_all(base, ec);
  CHECK(gutil::list_profiles(base) == std::vector<std::string>{""});  // no profiles folder
  fs::create_directories(base / "profiles" / "zed");
  fs::create_directories(base / "profiles" / "ann");
  fs::create_directories(base / "profiles" / "not valid");  // skipped
  { std::ofstream(base / "profiles" / "stray.txt") << "x"; }  // files are not profiles
  CHECK(gutil::list_profiles(base) == std::vector<std::string>{"", "ann", "zed"});
  fs::remove_all(base, ec);
}
