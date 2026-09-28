// Garmin Connect client. Reproduces the login handshake used by the mobile
// app (SSO ticket -> OAuth1 token -> OAuth2 bearer) and wraps the handful of
// connectapi endpoints we sync. This is the same unofficial API path the
// `garth` / `python-garminconnect` libraries use; Garmin can change it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gc/http_client.h"
#include "gc/token_store.h"

namespace gc {

using MfaPrompt = std::function<std::string()>;

// What to do with one oauth2 exchange response. Garmin rejects a stale MFA
// token with 403 "The provided MFA token was invalid", so a rejection while
// one was sent earns one retry without it. Any other 4xx means the stored
// login is no longer accepted and only `gsync login` can fix it.
enum class ExchangeStep { kDone, kRetryWithoutMfa, kLoginRequired, kTransient };
ExchangeStep classify_exchange(uint32_t status, bool sent_mfa);

class GarminClient {
 public:
  explicit GarminClient(Tokens tokens);
  ~GarminClient();
  GarminClient(const GarminClient&) = delete;
  GarminClient& operator=(const GarminClient&) = delete;

  // Full interactive login. Replaces any stored tokens on success.
  bool login(const std::string& email, const std::string& password, const MfaPrompt& mfa_prompt,
             std::string& err);

  // Makes sure a usable bearer token exists, minting a new one from the
  // OAuth1 token when expired. Called implicitly by the fetchers.
  bool ensure_access_token(std::string& err);

  // True once a token exchange was refused for good. Sticky for the life of
  // the client: every later request fails at once with the same message.
  bool login_required() const { return login_required_; }

  const Tokens& tokens() const { return tokens_; }
  bool tokens_dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // Generic authenticated GET against https://connectapi.<domain>/<path>.
  bool get_json(const std::string& path, nlohmann::json& out, std::string& err);
  bool get_bytes(const std::string& path, std::vector<uint8_t>& out, std::string& err);

  // Endpoint wrappers. Dates are "YYYY-MM-DD".
  bool fetch_profile(std::string& err);  // fills tokens().display_name
  bool daily_summary(const std::string& date, nlohmann::json& out, std::string& err);
  bool daily_heart_rate(const std::string& date, nlohmann::json& out, std::string& err);
  bool daily_sleep(const std::string& date, nlohmann::json& out, std::string& err);
  bool daily_stress(const std::string& date, nlohmann::json& out, std::string& err);
  bool daily_hrv(const std::string& date, nlohmann::json& out, std::string& err);
  bool weight_range(const std::string& start, const std::string& end, nlohmann::json& out,
                    std::string& err);
  bool activities(int start, int limit, nlohmann::json& out, std::string& err);
  // Both return a ZIP archive containing FIT file(s).
  bool download_activity_zip(int64_t activity_id, std::vector<uint8_t>& out, std::string& err);
  bool download_wellness_zip(const std::string& date, std::vector<uint8_t>& out,
                             std::string& err);

 private:
  struct OAuthConsumer {
    std::string key;
    std::string secret;
  };
  bool fetch_consumer(OAuthConsumer& out, std::string& err);
  bool sso_get_ticket(const std::string& email, const std::string& password,
                      const MfaPrompt& mfa_prompt, std::string& ticket, std::string& err);
  bool oauth1_preauthorized(const OAuthConsumer& c, const std::string& ticket, std::string& err);
  bool oauth2_exchange(const OAuthConsumer& c, std::string& err);
  bool exchange_once(const OAuthConsumer& c, bool send_mfa, HttpResponse& r, std::string& err);
  bool store_oauth2(const HttpResponse& r, std::string& err);
  bool authed_request(const std::string& path, HttpResponse& resp, std::string& err);
  std::string api_base() const { return "https://connectapi." + tokens_.oauth1.domain; }
  std::string sso_base() const { return "https://sso." + tokens_.oauth1.domain + "/sso"; }

  std::unique_ptr<HttpClient> http_;
  Tokens tokens_;
  bool dirty_ = false;
  bool login_required_ = false;
  std::string login_error_;
  bool have_consumer_ = false;
  OAuthConsumer consumer_;
};

}  // namespace gc
