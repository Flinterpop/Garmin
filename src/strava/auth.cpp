#include "strava/auth.h"

#include <nlohmann/json.hpp>

#include "gc/http_client.h"
#include "util/assert.h"
#include "util/crypto_util.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace strava {

using nlohmann::json;

namespace {

constexpr char kTokenUrl[] = "https://www.strava.com/oauth/token";
constexpr char kDeauthUrl[] = "https://www.strava.com/oauth/deauthorize";
constexpr int64_t kRefreshMarginS = 300;
constexpr size_t kMaxErrorBody = 300;

bool all_of_class(const std::string& s, bool hex) {
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const bool digit = c >= '0' && c <= '9';
    const bool hexa = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!digit && !(hex && hexa)) return false;
  }
  return true;
}

bool post_token(gc::HttpClient& http, const std::vector<std::pair<std::string, std::string>>& form,
                Login& l, std::string& err) {
  G_ASSERT(!form.empty());
  gc::HttpResponse r;
  const gc::HeaderMap hdr = {{"Content-Type", "application/x-www-form-urlencoded"}};
  if (!http.request("POST", kTokenUrl, hdr, gc::form_encode(form), r, err)) return false;
  if (r.status != 200) {
    err = "Strava refused the login (HTTP " + std::to_string(r.status) + "): " +
          r.body.substr(0, kMaxErrorBody);
    return false;
  }
  return parse_token_response(r.body, l, err);
}

}  // namespace

std::filesystem::path login_path(const std::filesystem::path& profile_base) {
  G_REQUIRE_RET(!profile_base.empty(), std::filesystem::path());
  return profile_base / L"strava.bin";
}

bool connected(const std::filesystem::path& profile_base) {
  G_REQUIRE_RET(!profile_base.empty(), false);
  std::error_code ec;
  return std::filesystem::exists(login_path(profile_base), ec);
}

bool load_login(const std::filesystem::path& p, Login& out, std::string& err) {
  G_ASSERT(!p.empty());
  std::vector<uint8_t> blob;
  if (!gutil::read_file(p, blob)) {
    err = "not connected to Strava";
    return false;
  }
  std::string plain;
  if (!gutil::dpapi_unprotect(blob, plain)) {
    err = "could not decrypt the Strava login (saved on another PC?)";
    return false;
  }
  const json j = json::parse(plain, nullptr, false);
  if (!j.is_object()) {
    err = "the Strava login file is corrupt";
    return false;
  }
  out = Login{};
  out.client_id = j.value("client_id", "");
  out.client_secret = j.value("client_secret", "");
  out.access_token = j.value("access_token", "");
  out.refresh_token = j.value("refresh_token", "");
  out.expires_at = j.value("expires_at", int64_t{0});
  out.athlete = j.value("athlete", "");
  G_ASSERT(out.expires_at >= 0);
  return true;
}

bool save_login(const std::filesystem::path& p, const Login& l, std::string& err) {
  G_ASSERT(!p.empty());
  G_REQUIRE_RET(!l.refresh_token.empty(), false);
  const json j = {{"client_id", l.client_id},         {"client_secret", l.client_secret},
                  {"access_token", l.access_token},   {"refresh_token", l.refresh_token},
                  {"expires_at", l.expires_at},       {"athlete", l.athlete}};
  std::vector<uint8_t> blob;
  if (!gutil::dpapi_protect(j.dump(), blob)) {
    err = "DPAPI encryption failed";
    return false;
  }
  if (!gutil::write_file(p, blob.data(), blob.size())) {
    err = "could not write " + p.string();
    return false;
  }
  return true;
}

bool valid_client_id(const std::string& s) {
  return !s.empty() && s.size() <= 12 && all_of_class(s, false);
}

bool valid_client_secret(const std::string& s) {
  return s.size() >= 20 && s.size() <= 80 && all_of_class(s, true);
}

std::string redirect_uri() { return "http://localhost:" + std::to_string(kRedirectPort) + "/"; }

std::string authorize_url(const std::string& client_id) {
  G_REQUIRE_RET(valid_client_id(client_id), std::string());
  const std::string q = gc::form_encode({{"client_id", client_id},
                                         {"response_type", "code"},
                                         {"redirect_uri", redirect_uri()},
                                         {"approval_prompt", "auto"},
                                         {"scope", "activity:read_all,activity:write"}});
  return "https://www.strava.com/oauth/authorize?" + q;
}

bool scope_sufficient(const std::string& scope) {
  return scope.find("activity:write") != std::string::npos &&
         scope.find("activity:read_all") != std::string::npos;
}

bool parse_token_response(const std::string& body, Login& l, std::string& err) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object() || !j.contains("access_token") || !j["access_token"].is_string() ||
      !j.contains("refresh_token") || !j["refresh_token"].is_string()) {
    err = "unexpected answer from Strava's token service";
    return false;
  }
  l.access_token = j["access_token"].get<std::string>();
  l.refresh_token = j["refresh_token"].get<std::string>();
  l.expires_at = j.value("expires_at", int64_t{0});
  const json& a = j.value("athlete", json::object());
  if (a.is_object() && a.contains("firstname")) {
    l.athlete = a.value("firstname", "") + " " + a.value("lastname", "");
  }
  G_ASSERT(!l.access_token.empty() || l.refresh_token.empty());
  return !l.access_token.empty() && !l.refresh_token.empty();
}

bool exchange_code(gc::HttpClient& http, Login& l, const std::string& code, std::string& err) {
  G_REQUIRE_RET(valid_client_id(l.client_id) && !code.empty(), false);
  return post_token(http,
                    {{"client_id", l.client_id},
                     {"client_secret", l.client_secret},
                     {"code", code},
                     {"grant_type", "authorization_code"}},
                    l, err);
}

bool ensure_fresh(gc::HttpClient& http, Login& l, bool& changed, std::string& err) {
  G_REQUIRE_RET(!l.refresh_token.empty(), false);
  changed = false;
  if (!l.access_token.empty() && gutil::now_unix() + kRefreshMarginS < l.expires_at) return true;
  if (!post_token(http,
                  {{"client_id", l.client_id},
                   {"client_secret", l.client_secret},
                   {"refresh_token", l.refresh_token},
                   {"grant_type", "refresh_token"}},
                  l, err)) {
    return false;
  }
  changed = true;
  return true;
}

bool deauthorize(gc::HttpClient& http, const Login& l, std::string& err) {
  G_REQUIRE_RET(!l.access_token.empty(), false);
  gc::HttpResponse r;
  const gc::HeaderMap hdr = {{"Authorization", "Bearer " + l.access_token},
                             {"Content-Type", "application/x-www-form-urlencoded"}};
  if (!http.request("POST", kDeauthUrl, hdr, std::string(), r, err)) return false;
  if (r.status != 200) {
    err = "HTTP " + std::to_string(r.status);
    return false;
  }
  return true;
}

}  // namespace strava
