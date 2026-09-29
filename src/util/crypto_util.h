#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gutil {

// HMAC-SHA1 via CNG. Returns the 20-byte digest; empty on failure.
std::vector<uint8_t> hmac_sha1(const std::string& key, const std::string& data);

// Cryptographically random bytes as lowercase hex (num_bytes <= 64).
std::string random_hex(size_t num_bytes);

std::string base64_encode(const std::vector<uint8_t>& bytes);

// DPAPI protection bound to this PC (machine scope): any Windows user here
// can decrypt, another PC cannot. Unprotect also reads older user-scope blobs.
bool dpapi_protect(const std::string& plain, std::vector<uint8_t>& out_blob);
bool dpapi_unprotect(const std::vector<uint8_t>& blob, std::string& out_plain);

// RFC 3986 percent-encoding with the unreserved set A-Z a-z 0-9 - . _ ~
std::string percent_encode(const std::string& s);
std::string percent_decode(const std::string& s);

}  // namespace gutil
