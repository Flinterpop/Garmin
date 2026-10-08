// Strava login for one profile. Each person registers their own free Strava
// API application (strava.com/settings/api) and gives gview its client id and
// secret; the browser then authorizes it and Strava redirects to a one-shot
// listener on localhost. Everything is kept in <profile folder>\strava.bin,
// DPAPI-encrypted to this PC like tokens.bin.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace gc {
class HttpClient;
}

namespace strava {

constexpr uint16_t kRedirectPort = 8765;

struct Login {
  std::string client_id;
  std::string client_secret;
  std::string access_token;
  std::string refresh_token;
  int64_t expires_at = 0;  // Unix seconds
  std::string athlete;     // "First Last", for the menus and messages
};

std::filesystem::path login_path(const std::filesystem::path& profile_base);
bool connected(const std::filesystem::path& profile_base);
bool load_login(const std::filesystem::path& p, Login& out, std::string& err);
bool save_login(const std::filesystem::path& p, const Login& l, std::string& err);

// Strava client ids are numbers; secrets are 40 hex digits (accept 20..80).
bool valid_client_id(const std::string& s);
bool valid_client_secret(const std::string& s);

std::string redirect_uri();
// The page that asks the athlete to authorize reading and writing activities.
std::string authorize_url(const std::string& client_id);
// The authorization must include activity:write and activity:read_all.
bool scope_sufficient(const std::string& scope);

// Fills the token fields (and athlete, when present) from an /oauth/token answer.
bool parse_token_response(const std::string& body, Login& l, std::string& err);

// Trades the code from the redirect for tokens.
bool exchange_code(gc::HttpClient& http, Login& l, const std::string& code, std::string& err);
// Renews the access token when it expires within 5 minutes; `changed` tells
// the caller to save the login.
bool ensure_fresh(gc::HttpClient& http, Login& l, bool& changed, std::string& err);
// Revokes this app's access to the athlete's account (Disconnect).
bool deauthorize(gc::HttpClient& http, const Login& l, std::string& err);

}  // namespace strava
