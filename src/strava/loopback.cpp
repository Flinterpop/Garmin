#include "strava/loopback.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "util/assert.h"

namespace strava {

namespace {

constexpr int kPollMs = 250;
constexpr int kMaxTimeoutS = 900;
constexpr int kMaxRequests = 16;      // the browser may ask for /favicon.ico first
constexpr int kMaxHeadBytes = 8192;
constexpr int kRecvTimeoutMs = 5000;

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  G_ASSERT(out.size() <= s.size());
  return out;
}

// Value of `key` in "a=1&b=2", decoded; empty when absent.
std::string query_value(const std::string& query, const std::string& key) {
  G_ASSERT(!key.empty());
  size_t pos = 0;
  for (int n = 0; n < 64 && pos <= query.size(); ++n) {
    size_t amp = query.find('&', pos);
    if (amp == std::string::npos) amp = query.size();
    const std::string pair = query.substr(pos, amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return url_decode(pair.substr(eq + 1));
    pos = amp + 1;
  }
  return std::string();
}

void answer(SOCKET s, bool ok) {
  const std::string page = ok ? "<html><body><h2>gview is connected to Strava.</h2>"
                                "<p>You can close this tab and go back to gview.</p></body></html>"
                              : "<html><body><h2>Strava did not authorize gview.</h2>"
                                "<p>Close this tab; gview says what to do next.</p></body></html>";
  const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                           std::to_string(page.size()) + "\r\nConnection: close\r\n\r\n" + page;
  const int sent = send(s, resp.data(), static_cast<int>(resp.size()), 0);
  (void)sent;  // best effort: the browser page is a courtesy
}

// Reads up to the end of the request head (or kMaxHeadBytes).
std::string read_head(SOCKET s) {
  const DWORD tmo = kRecvTimeoutMs;
  const int set = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
  G_REQUIRE_RET(set == 0, std::string());
  std::string head;
  char buf[1024];
  for (int n = 0; n < kMaxHeadBytes / 1024 + 1; ++n) {
    const int got = recv(s, buf, sizeof(buf), 0);
    if (got <= 0) break;
    head.append(buf, static_cast<size_t>(got));
    if (head.find("\r\n\r\n") != std::string::npos || head.size() >= kMaxHeadBytes) break;
  }
  return head;
}

}  // namespace

bool parse_redirect_request(const std::string& head, Redirect& out) {
  const size_t eol = head.find("\r\n");
  const std::string line = head.substr(0, eol);
  if (line.rfind("GET ", 0) != 0) return false;
  const size_t sp = line.find(' ', 4);
  const std::string target = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
  const size_t q = target.find('?');
  if (q == std::string::npos) return false;
  const std::string query = target.substr(q + 1);
  out.code = query_value(query, "code");
  out.scope = query_value(query, "scope");
  out.error = query_value(query, "error");
  return !out.code.empty() || !out.error.empty();
}

Listener::~Listener() {
  if (sock_ != static_cast<uintptr_t>(INVALID_SOCKET)) closesocket(static_cast<SOCKET>(sock_));
  if (wsa_) WSACleanup();
}

bool Listener::open(uint16_t port, std::string& err) {
  G_ASSERT(sock_ == static_cast<uintptr_t>(INVALID_SOCKET));
  G_REQUIRE_RET(port > 1024, false);
  WSADATA wd{};
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
    err = "Winsock did not start";
    return false;
  }
  wsa_ = true;
  const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    err = "could not create a socket";
    return false;
  }
  sock_ = static_cast<uintptr_t>(s);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0) {
    err = "port " + std::to_string(port) + " on this PC is in use by another program";
    return false;
  }
  return true;
}

bool Listener::wait(int timeout_s, const std::atomic<bool>* cancel, Redirect& out, std::string& err) {
  G_REQUIRE_RET(sock_ != static_cast<uintptr_t>(INVALID_SOCKET), false);
  G_REQUIRE_RET(timeout_s > 0 && timeout_s <= kMaxTimeoutS, false);
  const SOCKET ls = static_cast<SOCKET>(sock_);
  const int polls = timeout_s * 1000 / kPollMs;
  int requests = 0;
  for (int i = 0; i < polls && requests < kMaxRequests; ++i) {
    if (cancel != nullptr && cancel->load()) {
      err = "stopped";
      return false;
    }
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(ls, &rd);
    timeval tv{0, kPollMs * 1000};
    const int ready = select(0, &rd, nullptr, nullptr, &tv);
    if (ready < 0) {
      err = "socket error while waiting for the browser";
      return false;
    }
    if (ready == 0) continue;
    const SOCKET c = accept(ls, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    ++requests;
    const bool got = parse_redirect_request(read_head(c), out);
    if (got) answer(c, !out.code.empty());
    closesocket(c);
    if (got) return true;
  }
  err = "no answer from the browser within " + std::to_string(timeout_s / 60) + " minutes";
  return false;
}

}  // namespace strava
