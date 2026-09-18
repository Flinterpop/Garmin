#pragma once
#include <string>

namespace gutil {

// Reads one line from stdin with the prompt shown; echo is disabled when
// `secret` is true (passwords, MFA codes). Returns false on EOF.
bool read_line(const std::string& prompt, bool secret, std::string& out);

}  // namespace gutil
