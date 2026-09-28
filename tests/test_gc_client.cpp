#include <catch2/catch_test_macros.hpp>

#include "gc/gc_client.h"

using gc::classify_exchange;
using gc::ExchangeStep;

TEST_CASE("exchange 200 is done with or without an MFA token", "[gc]") {
  CHECK(classify_exchange(200, true) == ExchangeStep::kDone);
  CHECK(classify_exchange(200, false) == ExchangeStep::kDone);
}

TEST_CASE("exchange rejected with an MFA token retries without it", "[gc]") {
  // Garmin: 403 "The provided MFA token was invalid" once the token is stale.
  CHECK(classify_exchange(403, true) == ExchangeStep::kRetryWithoutMfa);
  CHECK(classify_exchange(401, true) == ExchangeStep::kRetryWithoutMfa);
}

TEST_CASE("exchange rejected without an MFA token needs a new login", "[gc]") {
  CHECK(classify_exchange(403, false) == ExchangeStep::kLoginRequired);
  CHECK(classify_exchange(401, false) == ExchangeStep::kLoginRequired);
  CHECK(classify_exchange(400, true) == ExchangeStep::kLoginRequired);
  CHECK(classify_exchange(400, false) == ExchangeStep::kLoginRequired);
}

TEST_CASE("exchange server and transport errors are transient", "[gc]") {
  CHECK(classify_exchange(500, true) == ExchangeStep::kTransient);
  CHECK(classify_exchange(504, false) == ExchangeStep::kTransient);
  CHECK(classify_exchange(0, false) == ExchangeStep::kTransient);
  CHECK(classify_exchange(302, false) == ExchangeStep::kTransient);
}

TEST_CASE("a fresh client without a login reports not logged in", "[gc]") {
  gc::GarminClient client{gc::Tokens{}};
  CHECK_FALSE(client.login_required());
  std::string err;
  CHECK_FALSE(client.ensure_access_token(err));
  CHECK(err.find("gsync login") != std::string::npos);
  CHECK_FALSE(client.login_required());  // never tried an exchange
}
