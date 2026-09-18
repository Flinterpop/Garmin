#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gutil {

constexpr size_t kMaxFileBytes = 256u * 1024u * 1024u;

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out);
bool read_text_file(const std::filesystem::path& p, std::string& out);
bool write_file(const std::filesystem::path& p, const void* data, size_t len);
bool write_text_file(const std::filesystem::path& p, const std::string& text);

// %LOCALAPPDATA%\GarminSync (created on demand). Empty path on failure.
std::filesystem::path app_data_dir();

}  // namespace gutil
