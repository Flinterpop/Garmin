// Persisted login state. The OAuth1 token lives about a year and is what we
// use to mint fresh OAuth2 bearer tokens, so both are kept. The file is
// DPAPI-encrypted to the current Windows user; the password is never stored.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace gc {

struct OAuth1Token {
  std::string token;
  std::string token_secret;
  std::string mfa_token;      // present when the account has MFA enabled
  std::string domain = "garmin.com";
};

struct OAuth2Token {
  std::string access_token;
  std::string refresh_token;
  int64_t expires_at = 0;          // Unix seconds
  int64_t refresh_expires_at = 0;
};

struct Tokens {
  OAuth1Token oauth1;
  OAuth2Token oauth2;
  std::string display_name;   // Garmin Connect displayName (a GUID-like id)
  std::string full_name;

  bool has_oauth1() const { return !oauth1.token.empty(); }
  bool oauth2_valid(int64_t now, int64_t margin_s = 60) const;
};

std::filesystem::path default_token_path();
bool load_tokens(const std::filesystem::path& p, Tokens& out, std::string& err);
bool save_tokens(const std::filesystem::path& p, const Tokens& t, std::string& err);

}  // namespace gc
