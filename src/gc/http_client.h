// Minimal HTTPS client over WinHTTP. One HttpClient = one WinHTTP session,
// which gives us an automatic cookie jar shared across hosts (needed for the
// Garmin SSO dance) and automatic redirect following.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gc {

struct HttpResponse {
  uint32_t status = 0;
  std::string body;
  std::string content_type;
  std::string final_url;  // after redirects
};

using HeaderMap = std::map<std::string, std::string>;

constexpr size_t kMaxResponseBytes = 128u * 1024u * 1024u;

class HttpClient {
 public:
  explicit HttpClient(const std::wstring& user_agent);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  bool ok() const { return session_ != nullptr; }

  // `url` must be absolute https://. Body is sent verbatim; set Content-Type
  // in `headers` when posting. Returns false only on transport failure; HTTP
  // error statuses are reported in `out.status`.
  bool request(const std::string& method, const std::string& url, const HeaderMap& headers,
               const std::string& body, HttpResponse& out, std::string& err);

  bool get(const std::string& url, const HeaderMap& headers, HttpResponse& out,
           std::string& err) {
    return request("GET", url, headers, std::string(), out, err);
  }

  // Overrides the session User-Agent for subsequent requests.
  void set_user_agent(const std::wstring& ua) { user_agent_ = ua; }

  const std::string& last_error() const { return last_error_; }

 private:
  void* session_ = nullptr;  // HINTERNET
  std::wstring user_agent_;
  std::string last_error_;
};

// URL helpers shared with the OAuth signer.
struct UrlParts {
  std::wstring host;
  uint16_t port = 443;
  std::wstring path_and_query;  // "/a/b?c=d"
  std::string scheme_host_path;  // "https://host/a/b" (no query) for OAuth base string
  std::string query;             // "c=d&e=f" without '?'
};
bool parse_https_url(const std::string& url, UrlParts& out);

// application/x-www-form-urlencoded encoding of key/value pairs.
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv);

}  // namespace gc
