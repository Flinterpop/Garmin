#include "util/console.h"

#include <windows.h>

#include <cstdio>
#include <iostream>

#include "util/assert.h"

namespace gutil {

namespace {

// Restores the console mode on scope exit so a Ctrl-C mid-password does not
// leave the terminal with echo off.
struct EchoGuard {
  HANDLE h = INVALID_HANDLE_VALUE;
  DWORD saved = 0;
  bool active = false;
  EchoGuard(const EchoGuard&) = delete;
  EchoGuard& operator=(const EchoGuard&) = delete;
  explicit EchoGuard(bool disable) {
    if (!disable) return;
    h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &saved)) return;
    active = SetConsoleMode(h, saved & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
  }
  ~EchoGuard() {
    if (active) SetConsoleMode(h, saved);
  }
};

}  // namespace

bool read_line(const std::string& prompt, bool secret, std::string& out) {
  G_ASSERT(!prompt.empty());
  std::fputs(prompt.c_str(), stdout);
  std::fflush(stdout);
  out.clear();
  {
    EchoGuard guard(secret);
    if (!std::getline(std::cin, out)) return false;
  }
  if (secret) std::fputc('\n', stdout);
  // Strip a trailing CR left by some consoles.
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return true;
}

}  // namespace gutil
