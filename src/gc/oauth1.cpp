#include "gc/oauth1.h"

#include <algorithm>

#include "util/assert.h"
#include "util/crypto_util.h"
#include "util/time_util.h"

namespace gc {

namespace {

constexpr size_t kMaxParams = 256;

}  // namespace

std::string oauth1_base_string(const std::string& method, const std::string& base_url,
                               const ParamList& params) {
  G_ASSERT(!method.empty());
  G_ASSERT(params.size() <= kMaxParams);
  // Encode first, then sort by encoded key then encoded value (RFC 5849 3.4.1.3.2).
  ParamList enc;
  enc.reserve(params.size());
  for (const auto& [k, v] : params) {
    enc.emplace_back(gutil::percent_encode(k), gutil::percent_encode(v));
  }
  std::sort(enc.begin(), enc.end());
  std::string joined;
  for (const auto& [k, v] : enc) {
    if (!joined.empty()) joined.push_back('&');
    joined += k + "=" + v;
  }
  return method + "&" + gutil::percent_encode(base_url) + "&" + gutil::percent_encode(joined);
}

std::string oauth1_authorization_header(const OAuth1Credentials& creds,
                                        const std::string& method,
                                        const std::string& base_url,
                                        const ParamList& query_params,
                                        const ParamList& body_params,
                                        const std::string& nonce_in,
                                        const std::string& timestamp_in) {
  G_ASSERT(!creds.consumer_key.empty());
  const std::string nonce = nonce_in.empty() ? gutil::random_hex(16) : nonce_in;
  const std::string timestamp =
      timestamp_in.empty() ? std::to_string(gutil::now_unix()) : timestamp_in;

  ParamList oauth = {
      {"oauth_consumer_key", creds.consumer_key},
      {"oauth_nonce", nonce},
      {"oauth_signature_method", "HMAC-SHA1"},
      {"oauth_timestamp", timestamp},
      {"oauth_version", "1.0"},
  };
  if (!creds.token.empty()) oauth.emplace_back("oauth_token", creds.token);

  ParamList all = query_params;
  all.insert(all.end(), body_params.begin(), body_params.end());
  all.insert(all.end(), oauth.begin(), oauth.end());

  const std::string base = oauth1_base_string(method, base_url, all);
  const std::string key =
      gutil::percent_encode(creds.consumer_secret) + "&" + gutil::percent_encode(creds.token_secret);
  const std::string signature = gutil::base64_encode(gutil::hmac_sha1(key, base));
  G_ASSERT(!signature.empty());
  oauth.emplace_back("oauth_signature", signature);

  std::string header = "OAuth ";
  bool first = true;
  for (const auto& [k, v] : oauth) {
    if (!first) header += ", ";
    first = false;
    header += gutil::percent_encode(k) + "=\"" + gutil::percent_encode(v) + "\"";
  }
  return header;
}

ParamList parse_form(const std::string& encoded) {
  ParamList out;
  size_t pos = 0;
  const size_t n = encoded.size();
  for (size_t guard = 0; guard <= n && pos <= n; ++guard) {
    size_t amp = encoded.find('&', pos);
    if (amp == std::string::npos) amp = n;
    const std::string pair = encoded.substr(pos, amp - pos);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        out.emplace_back(gutil::percent_decode(pair), std::string());
      } else {
        out.emplace_back(gutil::percent_decode(pair.substr(0, eq)),
                         gutil::percent_decode(pair.substr(eq + 1)));
      }
    }
    if (amp >= n || out.size() >= kMaxParams) break;
    pos = amp + 1;
  }
  return out;
}

}  // namespace gc
