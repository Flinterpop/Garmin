// Runtime assertions that stay enabled in Release builds (Power of 10 rule 5).
#pragma once
#include <cstdio>
#include <cstdlib>

#define G_ASSERT(cond)                                                        \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "ASSERT FAILED: %s (%s:%d)\n", #cond, __FILE__,    \
                   __LINE__);                                                 \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

// Precondition check that returns a value instead of aborting; use where the
// bad input comes from the outside world (files, network) rather than a bug.
#define G_REQUIRE_RET(cond, ret)                                              \
  do {                                                                        \
    if (!(cond)) return (ret);                                                \
  } while (0)

// Same for void functions.
#define G_REQUIRE_VOID(cond)                                                    do {                                                                            if (!(cond)) return;                                                        } while (0)
