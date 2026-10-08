// A one-shot HTTP listener on 127.0.0.1 that catches the browser's redirect
// back from Strava's authorize page (?code=...&scope=... or ?error=...).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace strava {

struct Redirect {
  std::string code;
  std::string scope;
  std::string error;  // "access_denied" when the athlete clicked Cancel
};

// Parses the first line of an HTTP request ("GET /?code=..&scope=.. HTTP/1.1").
// False when it carries neither a code nor an error (a favicon request, say).
bool parse_redirect_request(const std::string& head, Redirect& out);

class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Binds 127.0.0.1:`port`. Open it before sending the browser to Strava.
  bool open(uint16_t port, std::string& err);

  // Waits up to `timeout_s` for the redirect, answering the browser with a
  // short page. False on timeout, cancel or a socket error.
  bool wait(int timeout_s, const std::atomic<bool>* cancel, Redirect& out, std::string& err);

 private:
  uintptr_t sock_ = ~uintptr_t{0};  // SOCKET; INVALID_SOCKET when closed
  bool wsa_ = false;
};

}  // namespace strava
