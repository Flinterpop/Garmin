#include "gc/token_store.h"

#include <nlohmann/json.hpp>

#include "util/assert.h"
#include "util/crypto_util.h"
#include "util/file_util.h"

namespace gc {

using nlohmann::json;

bool Tokens::oauth2_valid(int64_t now, int64_t margin_s) const {
  return !oauth2.access_token.empty() && now + margin_s < oauth2.expires_at;
}

std::filesystem::path default_token_path() {
  const std::filesystem::path dir = gutil::app_data_dir();
  G_REQUIRE_RET(!dir.empty(), std::filesystem::path());
  return dir / L"tokens.bin";
}

bool load_tokens(const std::filesystem::path& p, Tokens& out, std::string& err) {
  std::vector<uint8_t> blob;
  if (!gutil::read_file(p, blob)) {
    err = "no saved login (" + p.string() + ")";
    return false;
  }
  std::string plain;
  if (!gutil::dpapi_unprotect(blob, plain)) {
    err = "could not decrypt token file (different Windows user?)";
    return false;
  }
  json j = json::parse(plain, nullptr, false);
  if (!j.is_object()) {
    err = "token file is corrupt";
    return false;
  }
  out = Tokens{};
  const json& o1 = j.value("oauth1", json::object());
  out.oauth1.token = o1.value("token", "");
  out.oauth1.token_secret = o1.value("token_secret", "");
  out.oauth1.mfa_token = o1.value("mfa_token", "");
  out.oauth1.domain = o1.value("domain", "garmin.com");
  const json& o2 = j.value("oauth2", json::object());
  out.oauth2.access_token = o2.value("access_token", "");
  out.oauth2.refresh_token = o2.value("refresh_token", "");
  out.oauth2.expires_at = o2.value("expires_at", int64_t{0});
  out.oauth2.refresh_expires_at = o2.value("refresh_expires_at", int64_t{0});
  out.display_name = j.value("display_name", "");
  out.full_name = j.value("full_name", "");
  return true;
}

bool save_tokens(const std::filesystem::path& p, const Tokens& t, std::string& err) {
  G_ASSERT(!p.empty());
  json j;
  j["oauth1"] = {{"token", t.oauth1.token},
                 {"token_secret", t.oauth1.token_secret},
                 {"mfa_token", t.oauth1.mfa_token},
                 {"domain", t.oauth1.domain}};
  j["oauth2"] = {{"access_token", t.oauth2.access_token},
                 {"refresh_token", t.oauth2.refresh_token},
                 {"expires_at", t.oauth2.expires_at},
                 {"refresh_expires_at", t.oauth2.refresh_expires_at}};
  j["display_name"] = t.display_name;
  j["full_name"] = t.full_name;
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

}  // namespace gc
