// OAuth 1.0a request signing (HMAC-SHA1), RFC 5849. Garmin's
// oauth-service still uses this for the preauthorized/exchange endpoints.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace gc {

using ParamList = std::vector<std::pair<std::string, std::string>>;

struct OAuth1Credentials {
  std::string consumer_key;
  std::string consumer_secret;
  std::string token;         // empty for the first (preauthorized) call
  std::string token_secret;
};

// Builds the signature base string from decoded parameters (query + body +
// oauth_*). Parameters are percent-encoded, sorted and joined here.
std::string oauth1_base_string(const std::string& method, const std::string& base_url,
                               const ParamList& params);

// Returns the value for the "Authorization:" header. `nonce` and
// `timestamp` are injectable for tests; leave empty to generate.
std::string oauth1_authorization_header(const OAuth1Credentials& creds,
                                        const std::string& method,
                                        const std::string& base_url,
                                        const ParamList& query_params,
                                        const ParamList& body_params,
                                        const std::string& nonce = std::string(),
                                        const std::string& timestamp = std::string());

// Splits "a=1&b=2" into decoded pairs.
ParamList parse_form(const std::string& encoded);

}  // namespace gc
