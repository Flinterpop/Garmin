#include "gc/gc_client.h"

#include <windows.h>

#include <cstdlib>
#include <regex>

#include "gc/oauth1.h"
#include "util/assert.h"
#include "util/crypto_util.h"
#include "util/time_util.h"

namespace gc {

using nlohmann::json;

namespace {

constexpr wchar_t kUaSso[] = L"GCM-iOS-5.7.2.1";
constexpr wchar_t kUaOAuth[] = L"com.garmin.android.apps.connectmobile";
constexpr char kConsumerUrl[] = "https://thegarth.s3.amazonaws.com/oauth_consumer.json";
constexpr int kMaxActivitiesPerPage = 100;

std::string build_url(const std::string& base, const ParamList& query) {
  if (query.empty()) return base;
  return base + "?" + form_encode(query);
}

// First capture group of `re` in `text`, or empty.
std::string regex_first(const std::string& text, const char* pattern) {
  const std::regex re(pattern);
  std::smatch m;
  if (std::regex_search(text, m, re) && m.size() >= 2) return m[1].str();
  return std::string();
}

std::string env_or_empty(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::string();
  std::string v(buf);
  std::free(buf);
  return v;
}

std::string http_failure(const char* what, const HttpResponse& r) {
  std::string snippet = r.body.substr(0, 200);
  for (char& c : snippet) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  return std::string(what) + ": HTTP " + std::to_string(r.status) + " " + snippet;
}

}  // namespace

GarminClient::GarminClient(Tokens tokens)
    : http_(std::make_unique<HttpClient>(kUaSso)), tokens_(std::move(tokens)) {
  if (tokens_.oauth1.domain.empty()) tokens_.oauth1.domain = "garmin.com";
}

GarminClient::~GarminClient() = default;

// ------------------------------------------------------------------ login

bool GarminClient::fetch_consumer(OAuthConsumer& out, std::string& err) {
  G_ASSERT(http_ != nullptr);
  if (have_consumer_) {
    out = consumer_;
    return true;
  }
  // Allow overriding the public consumer pair without a network fetch.
  out.key = env_or_empty("GARMIN_OAUTH_CONSUMER_KEY");
  out.secret = env_or_empty("GARMIN_OAUTH_CONSUMER_SECRET");
  if (out.key.empty() || out.secret.empty()) {
    HttpResponse r;
    if (!http_->get(kConsumerUrl, {}, r, err)) return false;
    if (r.status != 200) {
      err = http_failure("oauth consumer fetch", r);
      return false;
    }
    const json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) {
      err = "oauth consumer response is not JSON";
      return false;
    }
    out.key = j.value("consumer_key", "");
    out.secret = j.value("consumer_secret", "");
  }
  if (out.key.empty() || out.secret.empty()) {
    err = "oauth consumer key/secret unavailable";
    return false;
  }
  consumer_ = out;
  have_consumer_ = true;
  return true;
}

bool GarminClient::sso_get_ticket(const std::string& email, const std::string& password,
                                  const MfaPrompt& mfa_prompt, std::string& ticket,
                                  std::string& err) {
  G_ASSERT(!email.empty());
  const std::string embed = sso_base() + "/embed";
  const ParamList embed_params = {
      {"id", "gauth-widget"}, {"embedWidgetVersion", "1"}, {"gauthHost", embed}};
  const ParamList signin_params = {{"id", "gauth-widget"},
                                   {"embedWidgetVersion", "1"},
                                   {"gauthHost", embed},
                                   {"service", embed},
                                   {"source", embed},
                                   {"redirectAfterAccountLoginUrl", embed},
                                   {"redirectAfterAccountCreationUrl", embed}};
  http_->set_user_agent(kUaSso);

  // 1. Prime cookies.
  HttpResponse r;
  const std::string embed_url = build_url(embed, embed_params);
  if (!http_->get(embed_url, {}, r, err)) return false;
  if (r.status != 200) {
    err = http_failure("sso embed", r);
    return false;
  }

  // 2. Fetch the sign-in form for its CSRF token.
  const std::string signin_url = build_url(sso_base() + "/signin", signin_params);
  HeaderMap hdr = {{"Referer", embed_url}};
  if (!http_->get(signin_url, hdr, r, err)) return false;
  if (r.status != 200) {
    err = http_failure("sso signin form", r);
    return false;
  }
  std::string csrf = regex_first(r.body, "name=\"_csrf\"\\s+value=\"([^\"]+)\"");
  if (csrf.empty()) {
    err = "sso: CSRF token not found (Garmin may have changed the login page)";
    return false;
  }

  // 3. Submit credentials.
  hdr = {{"Referer", signin_url}, {"Content-Type", "application/x-www-form-urlencoded"}};
  const std::string form = form_encode(
      {{"username", email}, {"password", password}, {"embed", "true"}, {"_csrf", csrf}});
  if (!http_->request("POST", signin_url, hdr, form, r, err)) return false;
  std::string title = regex_first(r.body, "<title>([^<]*)</title>");

  // 4. Optional MFA step.
  if (title.find("MFA") != std::string::npos) {
    if (!mfa_prompt) {
      err = "sso: MFA required but no prompt available";
      return false;
    }
    csrf = regex_first(r.body, "name=\"_csrf\"\\s+value=\"([^\"]+)\"");
    const std::string code = mfa_prompt();
    if (code.empty()) {
      err = "sso: empty MFA code";
      return false;
    }
    const std::string mfa_url =
        build_url(sso_base() + "/verifyMFA/loginEnterMfaCode", signin_params);
    hdr = {{"Referer", signin_url}, {"Content-Type", "application/x-www-form-urlencoded"}};
    const std::string mfa_form = form_encode({{"mfa-code", code},
                                              {"embed", "true"},
                                              {"_csrf", csrf},
                                              {"fromPage", "setupEnterMfaCode"}});
    if (!http_->request("POST", mfa_url, hdr, mfa_form, r, err)) return false;
    title = regex_first(r.body, "<title>([^<]*)</title>");
  }

  if (title != "Success") {
    err = "sso: login failed (page title: '" + title + "', HTTP " + std::to_string(r.status) +
          "). Check credentials; repeated failures can trigger a temporary lockout.";
    return false;
  }
  ticket = regex_first(r.body, "embed\\?ticket=([^\"]+)\"");
  if (ticket.empty()) {
    err = "sso: login succeeded but no ticket found";
    return false;
  }
  return true;
}

bool GarminClient::oauth1_preauthorized(const OAuthConsumer& c, const std::string& ticket,
                                        std::string& err) {
  G_ASSERT(!ticket.empty());
  const std::string base = api_base() + "/oauth-service/oauth/preauthorized";
  const ParamList query = {{"ticket", ticket},
                           {"login-url", sso_base() + "/embed"},
                           {"accepts-mfa-tokens", "true"}};
  OAuth1Credentials creds{c.key, c.secret, "", ""};
  HeaderMap hdr = {{"Authorization", oauth1_authorization_header(creds, "GET", base, query, {})}};
  http_->set_user_agent(kUaOAuth);
  HttpResponse r;
  if (!http_->get(build_url(base, query), hdr, r, err)) return false;
  if (r.status != 200) {
    err = http_failure("oauth1 preauthorized", r);
    return false;
  }
  const ParamList kv = parse_form(r.body);
  OAuth1Token t;
  t.domain = tokens_.oauth1.domain;
  for (const auto& [k, v] : kv) {
    if (k == "oauth_token") t.token = v;
    if (k == "oauth_token_secret") t.token_secret = v;
    if (k == "mfa_token") t.mfa_token = v;
  }
  if (t.token.empty() || t.token_secret.empty()) {
    err = "oauth1 preauthorized: no token in response";
    return false;
  }
  tokens_.oauth1 = t;
  dirty_ = true;
  return true;
}

ExchangeStep classify_exchange(uint32_t status, bool sent_mfa) {
  if (status == 200) return ExchangeStep::kDone;
  if ((status == 401 || status == 403) && sent_mfa) return ExchangeStep::kRetryWithoutMfa;
  if (status >= 400 && status <= 499) return ExchangeStep::kLoginRequired;
  return ExchangeStep::kTransient;
}

bool GarminClient::exchange_once(const OAuthConsumer& c, bool send_mfa, HttpResponse& r,
                                 std::string& err) {
  G_ASSERT(tokens_.has_oauth1());
  G_ASSERT(!send_mfa || !tokens_.oauth1.mfa_token.empty());
  const std::string url = api_base() + "/oauth-service/oauth/exchange/user/2.0";
  ParamList body;
  if (send_mfa) body.emplace_back("mfa_token", tokens_.oauth1.mfa_token);
  OAuth1Credentials creds{c.key, c.secret, tokens_.oauth1.token, tokens_.oauth1.token_secret};
  HeaderMap hdr = {{"Authorization", oauth1_authorization_header(creds, "POST", url, {}, body)},
                   {"Content-Type", "application/x-www-form-urlencoded"}};
  http_->set_user_agent(kUaOAuth);
  return http_->request("POST", url, hdr, form_encode(body), r, err);
}

bool GarminClient::oauth2_exchange(const OAuthConsumer& c, std::string& err) {
  G_ASSERT(tokens_.has_oauth1());
  bool send_mfa = !tokens_.oauth1.mfa_token.empty();
  constexpr int kMaxExchangeAttempts = 2;  // with the MFA token, then without it
  for (int attempt = 0; attempt < kMaxExchangeAttempts; ++attempt) {
    HttpResponse r;
    if (!exchange_once(c, send_mfa, r, err)) return false;
    switch (classify_exchange(r.status, send_mfa)) {
      case ExchangeStep::kDone:
        if (!send_mfa && !tokens_.oauth1.mfa_token.empty()) {
          tokens_.oauth1.mfa_token.clear();  // stale: stop sending it
          dirty_ = true;
        }
        return store_oauth2(r, err);
      case ExchangeStep::kRetryWithoutMfa:
        send_mfa = false;
        continue;
      case ExchangeStep::kLoginRequired:
        login_required_ = true;
        login_error_ = http_failure("oauth2 exchange", r) +
                       " -- saved login is no longer accepted, run `gsync login`";
        err = login_error_;
        return false;
      case ExchangeStep::kTransient:
        err = http_failure("oauth2 exchange", r);
        return false;
    }
  }
  err = "oauth2 exchange: no attempt succeeded";
  return false;
}

bool GarminClient::store_oauth2(const HttpResponse& r, std::string& err) {
  G_ASSERT(r.status == 200);
  const json j = json::parse(r.body, nullptr, false);
  if (!j.is_object() || !j.contains("access_token")) {
    err = "oauth2 exchange: unexpected response";
    return false;
  }
  const int64_t now = gutil::now_unix();
  tokens_.oauth2.access_token = j.value("access_token", "");
  tokens_.oauth2.refresh_token = j.value("refresh_token", "");
  tokens_.oauth2.expires_at = now + j.value("expires_in", int64_t{0});
  tokens_.oauth2.refresh_expires_at = now + j.value("refresh_token_expires_in", int64_t{0});
  dirty_ = true;
  return true;
}

bool GarminClient::login(const std::string& email, const std::string& password,
                         const MfaPrompt& mfa_prompt, std::string& err) {
  if (email.empty() || password.empty()) {
    err = "email and password are required";
    return false;
  }
  OAuthConsumer c;
  if (!fetch_consumer(c, err)) return false;
  std::string ticket;
  if (!sso_get_ticket(email, password, mfa_prompt, ticket, err)) return false;
  if (!oauth1_preauthorized(c, ticket, err)) return false;
  if (!oauth2_exchange(c, err)) return false;
  return fetch_profile(err);
}

bool GarminClient::ensure_access_token(std::string& err) {
  if (tokens_.oauth2_valid(gutil::now_unix())) return true;
  if (login_required_) {
    err = login_error_;
    return false;
  }
  if (!tokens_.has_oauth1()) {
    err = "not logged in: run `gsync login`";
    return false;
  }
  OAuthConsumer c;
  if (!fetch_consumer(c, err)) return false;
  return oauth2_exchange(c, err);
}

// --------------------------------------------------------------- requests

bool GarminClient::authed_request(const std::string& path, HttpResponse& resp,
                                  std::string& err) {
  G_ASSERT(!path.empty() && path[0] == '/');
  if (!ensure_access_token(err)) return false;
  http_->set_user_agent(kUaSso);
  const std::string url = api_base() + path;
  // Garmin's download-service answers 5xx (Cloudflare 504) fairly often for
  // older dates; a couple of spaced retries clears most of them.
  constexpr int kMaxAttempts = 4;
  constexpr DWORD kBackoffMs[kMaxAttempts] = {0, 2000, 5000, 10000};
  bool refreshed = false;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (kBackoffMs[attempt] > 0) Sleep(kBackoffMs[attempt]);
    HeaderMap hdr = {{"Authorization", "Bearer " + tokens_.oauth2.access_token},
                     {"Accept", "application/json, */*"}};
    if (!http_->get(url, hdr, resp, err)) return false;
    if ((resp.status == 401 || resp.status == 403) && !refreshed) {
      // Bearer rejected: mint a fresh one once and retry immediately.
      refreshed = true;
      tokens_.oauth2.access_token.clear();
      if (!ensure_access_token(err)) return false;
      continue;
    }
    if (resp.status >= 500 && resp.status <= 599) continue;
    return true;
  }
  return true;  // last response (an error status) is reported by the caller
}

bool GarminClient::get_json(const std::string& path, json& out, std::string& err) {
  HttpResponse r;
  if (!authed_request(path, r, err)) return false;
  if (r.status == 204 || r.body.empty()) {
    out = json();
    return true;
  }
  if (r.status != 200) {
    err = http_failure(path.c_str(), r);
    return false;
  }
  out = json::parse(r.body, nullptr, false);
  if (out.is_discarded()) {
    err = path + ": response is not JSON";
    return false;
  }
  return true;
}

bool GarminClient::get_bytes(const std::string& path, std::vector<uint8_t>& out,
                             std::string& err) {
  HttpResponse r;
  if (!authed_request(path, r, err)) return false;
  if (r.status != 200) {
    err = http_failure(path.c_str(), r);
    return false;
  }
  out.assign(r.body.begin(), r.body.end());
  return true;
}

// --------------------------------------------------------------- endpoints

bool GarminClient::fetch_profile(std::string& err) {
  json j;
  if (!get_json("/userprofile-service/socialProfile", j, err)) return false;
  const std::string name = j.value("displayName", "");
  if (name.empty()) {
    err = "profile: displayName missing";
    return false;
  }
  tokens_.display_name = name;
  tokens_.full_name = j.value("fullName", "");
  dirty_ = true;
  return true;
}

bool GarminClient::daily_summary(const std::string& date, json& out, std::string& err) {
  G_REQUIRE_RET(!tokens_.display_name.empty() || fetch_profile(err), false);
  return get_json("/usersummary-service/usersummary/daily/" + tokens_.display_name +
                      "?calendarDate=" + date,
                  out, err);
}

bool GarminClient::daily_heart_rate(const std::string& date, json& out, std::string& err) {
  G_REQUIRE_RET(!tokens_.display_name.empty() || fetch_profile(err), false);
  return get_json("/wellness-service/wellness/dailyHeartRate/" + tokens_.display_name +
                      "?date=" + date,
                  out, err);
}

bool GarminClient::daily_sleep(const std::string& date, json& out, std::string& err) {
  G_REQUIRE_RET(!tokens_.display_name.empty() || fetch_profile(err), false);
  return get_json("/wellness-service/wellness/dailySleepData/" + tokens_.display_name +
                      "?date=" + date + "&nonSleepBufferMinutes=60",
                  out, err);
}

bool GarminClient::daily_stress(const std::string& date, json& out, std::string& err) {
  return get_json("/wellness-service/wellness/dailyStress/" + date, out, err);
}

bool GarminClient::daily_hrv(const std::string& date, json& out, std::string& err) {
  return get_json("/hrv-service/hrv/" + date, out, err);
}

bool GarminClient::weight_range(const std::string& start, const std::string& end, json& out,
                                std::string& err) {
  return get_json("/weight-service/weight/dateRange?startDate=" + start + "&endDate=" + end,
                  out, err);
}

bool GarminClient::activities(int start, int limit, json& out, std::string& err) {
  G_ASSERT(start >= 0);
  G_ASSERT(limit > 0 && limit <= kMaxActivitiesPerPage);
  return get_json("/activitylist-service/activities/search/activities?start=" +
                      std::to_string(start) + "&limit=" + std::to_string(limit),
                  out, err);
}

bool GarminClient::download_activity_zip(int64_t activity_id, std::vector<uint8_t>& out,
                                         std::string& err) {
  G_ASSERT(activity_id > 0);
  return get_bytes("/download-service/files/activity/" + std::to_string(activity_id), out, err);
}

bool GarminClient::download_wellness_zip(const std::string& date, std::vector<uint8_t>& out,
                                         std::string& err) {
  return get_bytes("/download-service/files/wellness/" + date, out, err);
}

}  // namespace gc
