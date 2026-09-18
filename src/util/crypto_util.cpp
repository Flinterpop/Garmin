#include "util/crypto_util.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include "util/assert.h"

namespace gutil {

namespace {

// RAII for CNG handles so every early return releases them.
struct AlgHandle {
  BCRYPT_ALG_HANDLE h = nullptr;
  AlgHandle() = default;
  AlgHandle(const AlgHandle&) = delete;
  AlgHandle& operator=(const AlgHandle&) = delete;
  ~AlgHandle() {
    if (h != nullptr) BCryptCloseAlgorithmProvider(h, 0);
  }
};
struct HashHandle {
  BCRYPT_HASH_HANDLE h = nullptr;
  HashHandle() = default;
  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  ~HashHandle() {
    if (h != nullptr) BCryptDestroyHash(h);
  }
};

bool nt_ok(NTSTATUS s) { return s >= 0; }

PUCHAR as_puchar(const std::string& s) {
  return reinterpret_cast<PUCHAR>(const_cast<char*>(s.data()));
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::vector<uint8_t> hmac_sha1(const std::string& key, const std::string& data) {
  AlgHandle alg;
  G_REQUIRE_RET(nt_ok(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA1_ALGORITHM, nullptr,
                                                  BCRYPT_ALG_HANDLE_HMAC_FLAG)),
                std::vector<uint8_t>());
  HashHandle hash;
  G_REQUIRE_RET(nt_ok(BCryptCreateHash(alg.h, &hash.h, nullptr, 0, as_puchar(key),
                                       static_cast<ULONG>(key.size()), 0)),
                std::vector<uint8_t>());
  G_REQUIRE_RET(
      nt_ok(BCryptHashData(hash.h, as_puchar(data), static_cast<ULONG>(data.size()), 0)), std::vector<uint8_t>());
  std::vector<uint8_t> digest(20);
  G_REQUIRE_RET(
      nt_ok(BCryptFinishHash(hash.h, digest.data(), static_cast<ULONG>(digest.size()), 0)),
      std::vector<uint8_t>());
  return digest;
}

std::string random_hex(size_t num_bytes) {
  G_ASSERT(num_bytes > 0 && num_bytes <= 64);
  uint8_t buf[64] = {};
  G_REQUIRE_RET(nt_ok(BCryptGenRandom(nullptr, buf, static_cast<ULONG>(num_bytes),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG)),
                std::string());
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(num_bytes * 2);
  for (size_t i = 0; i < num_bytes; ++i) {
    out.push_back(kHex[buf[i] >> 4]);
    out.push_back(kHex[buf[i] & 0x0F]);
  }
  return out;
}

std::string base64_encode(const std::vector<uint8_t>& bytes) {
  G_REQUIRE_RET(!bytes.empty(), std::string());
  DWORD len = 0;
  const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  G_REQUIRE_RET(CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags,
                                     nullptr, &len),
                std::string());
  std::string out(len, '\0');
  G_REQUIRE_RET(CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags,
                                     out.data(), &len),
                std::string());
  // On the second call len excludes the terminating NUL.
  out.resize(len);
  return out;
}

bool dpapi_protect(const std::string& plain, std::vector<uint8_t>& out_blob) {
  DATA_BLOB in{};
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
  in.cbData = static_cast<DWORD>(plain.size());
  DATA_BLOB out{};
  G_REQUIRE_RET(CryptProtectData(&in, L"GarminSync tokens", nullptr, nullptr, nullptr,
                                 CRYPTPROTECT_UI_FORBIDDEN, &out),
                false);
  out_blob.assign(out.pbData, out.pbData + out.cbData);
  LocalFree(out.pbData);
  return true;
}

bool dpapi_unprotect(const std::vector<uint8_t>& blob, std::string& out_plain) {
  G_REQUIRE_RET(!blob.empty(), false);
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(blob.data());
  in.cbData = static_cast<DWORD>(blob.size());
  DATA_BLOB out{};
  G_REQUIRE_RET(CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &out),
                false);
  out_plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return true;
}

std::string percent_encode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                            c == '~';
    if (unreserved) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string percent_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '%' && i + 2 < n) {
      const int hi = hex_val(s[i + 1]);
      const int lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

}  // namespace gutil
