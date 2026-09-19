#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "plot/ticks.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("nice_step picks 1-2-5 multiples") {
  CHECK_THAT(plot::nice_step(100.0, 5), WithinAbs(20.0, 1e-12));
  CHECK_THAT(plot::nice_step(7.0, 5), WithinAbs(2.0, 1e-12));
  CHECK_THAT(plot::nice_step(0.35, 5), WithinAbs(0.1, 1e-12));
  CHECK_THAT(plot::nice_step(1000.0, 4), WithinAbs(500.0, 1e-12));
  CHECK_THAT(plot::nice_step(0.0, 5), WithinAbs(1.0, 1e-12));
}

TEST_CASE("nice_range expands outward and handles degenerate input") {
  double lo = 47.0;
  double hi = 173.0;
  plot::nice_range(lo, hi, 5);
  CHECK(lo <= 47.0);
  CHECK(hi >= 173.0);
  CHECK_THAT(lo, WithinAbs(40.0, 1e-9));
  CHECK_THAT(hi, WithinAbs(180.0, 1e-9));
  double a = 88.0;
  double b = 88.0;
  plot::nice_range(a, b, 5);
  CHECK(a < 88.0);
  CHECK(b > 88.0);
}

TEST_CASE("linear_ticks covers the range with labels") {
  std::vector<plot::Tick> t;
  plot::linear_ticks(0.0, 100.0, 5, t);
  REQUIRE(t.size() == 6);
  CHECK(t.front().label == "0");
  CHECK(t.back().label == "100");
  plot::linear_ticks(0.0, 1.0, 5, t);
  REQUIRE(!t.empty());
  CHECK(t[1].label == "0.2");
  plot::linear_ticks(5.0, 5.0, 5, t);
  CHECK(t.empty());
}

TEST_CASE("time_ticks chooses a step that fits the pixel width") {
  std::vector<plot::Tick> t;
  const double day0 = 1758168000.0;  // an arbitrary instant in Sep 2026
  // One day over 1000 px -> 2-3 hour steps -> 8..13 ticks.
  plot::time_ticks(day0, day0 + 86400.0, 1000.0, t);
  CHECK(t.size() >= 8);
  CHECK(t.size() <= 13);
  for (size_t i = 1; i < t.size(); ++i) CHECK(t[i].value > t[i - 1].value);
  // Every tick sits within the range.
  for (const plot::Tick& k : t) {
    CHECK(k.value >= day0);
    CHECK(k.value <= day0 + 86400.0);
  }
  // Ninety days over 1000 px -> weekly steps.
  plot::time_ticks(day0, day0 + 90.0 * 86400.0, 1000.0, t);
  CHECK(t.size() >= 10);
  CHECK(t.size() <= 14);
  // Two years -> quarterly or half-yearly, all labels non-empty.
  plot::time_ticks(day0, day0 + 730.0 * 86400.0, 1000.0, t);
  CHECK(t.size() >= 4);
  CHECK(t.size() <= 10);
  for (const plot::Tick& k : t) CHECK(!k.label.empty());
  // Tiny spans still produce something sensible.
  plot::time_ticks(day0, day0 + 300.0, 1000.0, t);
  CHECK(t.size() >= 3);
  CHECK(t.size() <= 7);
}

TEST_CASE("elapsed_ticks formats mm:ss and h:mm:ss") {
  std::vector<plot::Tick> t;
  plot::elapsed_ticks(0.0, 2700.0, 900.0, t);  // 45 min over 900 px -> 5 min steps
  REQUIRE(t.size() == 10);
  CHECK(t[0].label == "0:00");
  CHECK(t[1].label == "0:05");
  plot::elapsed_ticks(0.0, 4.0 * 3600.0, 900.0, t);
  REQUIRE(!t.empty());
  CHECK(t.back().label == "4:00");
  plot::elapsed_ticks(0.0, 40.0, 900.0, t);  // sub-minute steps keep seconds
  REQUIRE(t.size() >= 5);
  CHECK(t[1].label == "00:05");
  CHECK(plot::format_elapsed(59.0) == "00:59");
  CHECK(plot::format_elapsed(3661.0) == "1:01:01");
}

TEST_CASE("format_value picks sensible precision") {
  CHECK(plot::format_value(120.0, "bpm") == "120 bpm");
  CHECK(plot::format_value(88.269, "kg") == "88.3 kg");
  CHECK(plot::format_value(3.456, "") == "3.46");
  CHECK(plot::format_value(7.0, "h") == "7 h");
}
