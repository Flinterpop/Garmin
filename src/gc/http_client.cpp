#include "gc/http_client.h"

#include <windows.h>
#include <winhttp.h>

#include "util/assert.h"
#include "util/crypto_util.h"

namespace gc {

namespace {

constexpr size_t kReadChunk = 64 * 1024;
constexpr DWORD kTimeoutMs = 60000;

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  G_REQUIRE_RET(n > 0, std::wstring());
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  G_REQUIRE_RET(n > 0, std::string());
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

std::string win_error(const char* what, DWORD code) {
  return std::string(what) + " failed, error " + std::to_string(code);
}

struct RequestHandles {
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  RequestHandles() = default;
  RequestHandles(const RequestHandles&) = delete;
  RequestHandles& operator=(const RequestHandles&) = delete;
  ~RequestHandles() {
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connect != nullptr) WinHttpCloseHandle(connect);
  }
};

bool query_header_string(HINTERNET req, DWORD info, std::string& out) {
  DWORD len = 0;
  WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                      WINHTTP_NO_HEADER_INDEX);
  G_REQUIRE_RET(GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0, false);
  std::wstring buf(len / sizeof(wchar_t), L'\0');
  G_REQUIRE_RET(WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len,
                                    WINHTTP_NO_HEADER_INDEX),
                false);
  buf.resize(len / sizeof(wchar_t));
  out = narrow(buf);
  return true;
}

bool read_body(HINTERNET req, std::string& body, std::string& err) {
  body.clear();
  // Bounded by kMaxResponseBytes / 1 (each iteration reads >= 1 byte or ends).
  const size_t max_iters = kMaxResponseBytes / 1024 + 1;
  for (size_t i = 0; i < max_iters; ++i) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      err = win_error("WinHttpQueryDataAvailable", GetLastError());
      return false;
    }
    if (avail == 0) return true;
    const size_t want = std::min<size_t>(avail, kReadChunk);
    if (body.size() + want > kMaxResponseBytes) {
      err = "response exceeds size limit";
      return false;
    }
    const size_t old = body.size();
    body.resize(old + want);
    DWORD got = 0;
    if (!WinHttpReadData(req, body.data() + old, static_cast<DWORD>(want), &got)) {
      err = win_error("WinHttpReadData", GetLastError());
      return false;
    }
    body.resize(old + got);
    if (got == 0) return true;
  }
  err = "response read loop exceeded bound";
  return false;
}

}  // namespace

HttpClient::HttpClient(const std::wstring& user_agent) : user_agent_(user_agent) {
  session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_ == nullptr) {
    last_error_ = win_error("WinHttpOpen", GetLastError());
    return;
  }
  WinHttpSetTimeouts(session_, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
  WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                        sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
}

HttpClient::~HttpClient() {
  if (session_ != nullptr) WinHttpCloseHandle(session_);
}

bool HttpClient::request(const std::string& method, const std::string& url,
                         const HeaderMap& headers, const std::string& body, HttpResponse& out,
                         std::string& err) {
  G_ASSERT(!method.empty());
  G_REQUIRE_RET(ok(), false);
  out = HttpResponse{};
  UrlParts parts;
  if (!parse_https_url(url, parts)) {
    err = "bad URL: " + url;
    return false;
  }

  RequestHandles h;
  h.connect = WinHttpConnect(session_, parts.host.c_str(), parts.port, 0);
  if (h.connect == nullptr) {
    err = win_error("WinHttpConnect", GetLastError());
    return false;
  }
  h.request = WinHttpOpenRequest(h.connect, widen(method).c_str(), parts.path_and_query.c_str(),
                                 nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
  if (h.request == nullptr) {
    err = win_error("WinHttpOpenRequest", GetLastError());
    return false;
  }

  std::wstring hdr_block = L"User-Agent: " + user_agent_ + L"\r\n";
  for (const auto& [k, v] : headers) hdr_block += widen(k) + L": " + widen(v) + L"\r\n";

  const void* body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
  const DWORD body_len = static_cast<DWORD>(body.size());
  if (!WinHttpSendRequest(h.request, hdr_block.c_str(), static_cast<DWORD>(hdr_block.size()),
                          const_cast<void*>(body_ptr), body_len, body_len, 0)) {
    err = win_error("WinHttpSendRequest", GetLastError());
    return false;
  }
  if (!WinHttpReceiveResponse(h.request, nullptr)) {
    err = win_error("WinHttpReceiveResponse", GetLastError());
    return false;
  }

  DWORD status = 0;
  DWORD status_len = sizeof(status);
  if (!WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                           WINHTTP_NO_HEADER_INDEX)) {
    err = win_error("WinHttpQueryHeaders(status)", GetLastError());
    return false;
  }
  out.status = status;
  query_header_string(h.request, WINHTTP_QUERY_CONTENT_TYPE, out.content_type);

  DWORD url_len = 0;
  WinHttpQueryOption(h.request, WINHTTP_OPTION_URL, nullptr, &url_len);
  if (url_len > 0) {
    std::wstring wurl(url_len / sizeof(wchar_t), L'\0');
    if (WinHttpQueryOption(h.request, WINHTTP_OPTION_URL, wurl.data(), &url_len)) {
      wurl.resize(url_len / sizeof(wchar_t));
      while (!wurl.empty() && wurl.back() == L'\0') wurl.pop_back();
      out.final_url = narrow(wurl);
    }
  }
  return read_body(h.request, out.body, err);
}

bool parse_https_url(const std::string& url, UrlParts& out) {
  static const std::string kPrefix = "https://";
  G_REQUIRE_RET(url.compare(0, kPrefix.size(), kPrefix) == 0, false);
  const size_t host_start = kPrefix.size();
  const size_t path_start = url.find('/', host_start);
  std::string host_port = url.substr(host_start, path_start == std::string::npos
                                                    ? std::string::npos
                                                    : path_start - host_start);
  G_REQUIRE_RET(!host_port.empty(), false);
  out.port = 443;
  const size_t colon = host_port.find(':');
  if (colon != std::string::npos) {
    const int port = std::atoi(host_port.c_str() + colon + 1);
    G_REQUIRE_RET(port > 0 && port < 65536, false);
    out.port = static_cast<uint16_t>(port);
    host_port.resize(colon);
  }
  out.host = widen(host_port);
  const std::string path_query = path_start == std::string::npos ? "/" : url.substr(path_start);
  out.path_and_query = widen(path_query);
  const size_t q = path_query.find('?');
  const std::string path_only = path_query.substr(0, q);
  out.query = q == std::string::npos ? std::string() : path_query.substr(q + 1);
  out.scheme_host_path = kPrefix + host_port + path_only;
  return true;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
  std::string out;
  for (const auto& [k, v] : kv) {
    if (!out.empty()) out.push_back('&');
    out += gutil::percent_encode(k) + "=" + gutil::percent_encode(v);
  }
  return out;
}

}  // namespace gc
