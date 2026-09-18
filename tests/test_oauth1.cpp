#include <catch2/catch_test_macros.hpp>

#include "gc/http_client.h"
#include "gc/oauth1.h"
#include "util/crypto_util.h"

namespace {

std::string hex(const std::vector<uint8_t>& v) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (const uint8_t b : v) {
    s.push_back(k[b >> 4]);
    s.push_back(k[b & 0xF]);
  }
  return s;
}

}  // namespace

TEST_CASE("hmac-sha1 known vector") {
  const auto d = gutil::hmac_sha1("key", "The quick brown fox jumps over the lazy dog");
  REQUIRE(hex(d) == "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9");
}

TEST_CASE("percent encoding follows RFC 3986 unreserved set") {
  CHECK(gutil::percent_encode("a-b.c_d~e") == "a-b.c_d~e");
  CHECK(gutil::percent_encode("r b") == "r%20b");
  CHECK(gutil::percent_encode("=") == "%3D");
  CHECK(gutil::percent_encode("https://sso.garmin.com/sso/embed") ==
        "https%3A%2F%2Fsso.garmin.com%2Fsso%2Fembed");
  CHECK(gutil::percent_decode("r%20b+c") == "r b c");
  CHECK(gutil::percent_decode("%zz") == "%zz");
}

TEST_CASE("base64") {
  const std::vector<uint8_t> v = {'M', 'a', 'n'};
  CHECK(gutil::base64_encode(v) == "TWFu");
  CHECK(gutil::base64_encode({'M'}) == "TQ==");
}

// RFC 5849 section 3.4.1.1 example.
TEST_CASE("oauth1 signature base string matches RFC 5849 example") {
  gc::ParamList params = {
      {"b5", "=%3D"}, {"a3", "a"}, {"c@", ""}, {"a2", "r b"},
      {"c2", ""}, {"a3", "2 q"},
      {"oauth_consumer_key", "9djdj82h48djs9d2"},
      {"oauth_token", "kkk9d7dh3k39sjv7"},
      {"oauth_signature_method", "HMAC-SHA1"},
      {"oauth_timestamp", "137131201"},
      {"oauth_nonce", "7d8f3e4a"},
  };
  const std::string expected =
      "POST&http%3A%2F%2Fexample.com%2Frequest&a2%3Dr%2520b%26a3%3D2%2520q%26a3%3Da%26b5%3D%253D%"
      "25253D%26c%2540%3D%26c2%3D%26oauth_consumer_key%3D9djdj82h48djs9d2%26oauth_nonce%3D7d8f3e4a%"
      "26oauth_signature_method%3DHMAC-SHA1%26oauth_timestamp%3D137131201%26oauth_token%"
      "3Dkkk9d7dh3k39sjv7";
  CHECK(gc::oauth1_base_string("POST", "http://example.com/request", params) == expected);
}

TEST_CASE("oauth1 authorization header carries a stable signature") {
  gc::OAuth1Credentials c{"ck", "cs", "tk", "ts"};
  const std::string h1 = gc::oauth1_authorization_header(c, "GET", "https://h/p", {{"q", "1"}},
                                                         {}, "nonce", "1700000000");
  const std::string h2 = gc::oauth1_authorization_header(c, "GET", "https://h/p", {{"q", "1"}},
                                                         {}, "nonce", "1700000000");
  CHECK(h1 == h2);
  CHECK(h1.rfind("OAuth ", 0) == 0);
  CHECK(h1.find("oauth_consumer_key=\"ck\"") != std::string::npos);
  CHECK(h1.find("oauth_token=\"tk\"") != std::string::npos);
  CHECK(h1.find("oauth_signature=\"") != std::string::npos);
  // Changing the query changes the signature.
  const std::string h3 = gc::oauth1_authorization_header(c, "GET", "https://h/p", {{"q", "2"}},
                                                         {}, "nonce", "1700000000");
  CHECK(h1 != h3);
}

TEST_CASE("form parsing and url parsing") {
  const gc::ParamList kv = gc::parse_form("oauth_token=abc&oauth_token_secret=x%20y&flag");
  REQUIRE(kv.size() == 3);
  CHECK(kv[0].first == "oauth_token");
  CHECK(kv[1].second == "x y");
  CHECK(kv[2].first == "flag");

  gc::UrlParts u;
  REQUIRE(gc::parse_https_url("https://connectapi.garmin.com/a/b?c=1&d=2", u));
  CHECK(u.host == L"connectapi.garmin.com");
  CHECK(u.port == 443);
  CHECK(u.path_and_query == L"/a/b?c=1&d=2");
  CHECK(u.scheme_host_path == "https://connectapi.garmin.com/a/b");
  CHECK(u.query == "c=1&d=2");
  CHECK_FALSE(gc::parse_https_url("http://x/", u));
  CHECK(gc::form_encode({{"a b", "c&d"}}) == "a%20b=c%26d");
}
