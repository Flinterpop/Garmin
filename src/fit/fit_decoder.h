// Streaming FIT decoder. Feeds every data message to a callback with the
// raw field bytes plus enough type information to read them; the profile
// (fit_profile.h) supplies names and scaling. Handles: 12/14-byte headers,
// CRC-16 verification, normal and compressed-timestamp record headers,
// definition messages with developer fields, monitoring timestamp_16
// expansion, and chained FIT files in one buffer.
//
// The decoder owns fixed-size state (16 local definitions x up to 510 fields)
// so instantiate it once, on the heap, and reuse it across files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fit/fit_types.h"

namespace fit {

constexpr size_t kMaxTotalFields = kMaxFieldsPerMessage * 2;  // native + developer
constexpr size_t kMaxDevFields = 64;
constexpr size_t kDevNameLen = 64;
constexpr size_t kDevUnitsLen = 16;

struct FieldValue {
  uint8_t num = 0;        // field definition number (dev: developer field num)
  uint8_t base_type = 0;  // fit::BaseType
  uint8_t size = 0;       // total bytes; count() = size / element size
  uint8_t dev_index = 0;
  bool developer = false;
  bool big_endian = false;
  bool valid = false;         // at least one element is not the invalid marker
  const uint8_t* raw = nullptr;
  const char* dev_name = nullptr;   // from field_description, if seen
  const char* dev_units = nullptr;

  size_t count() const;
  bool element_valid(size_t idx) const;
  // Raw (unscaled) integer / floating value of element idx.
  int64_t as_int(size_t idx = 0) const;
  double as_double(size_t idx = 0) const;
  std::string_view as_string() const;
};

struct Message {
  uint16_t global_num = 0;
  uint8_t local_num = 0;
  bool has_timestamp = false;
  uint32_t timestamp = 0;  // FIT seconds, expanded from compressed forms
  uint16_t num_fields = 0;
  FieldValue fields[kMaxTotalFields];

  // Native (non-developer) field lookup.
  const FieldValue* find(uint8_t field_num) const;
  bool get_int(uint8_t field_num, int64_t& out) const;
  // Applies profile scale/offset; false if absent, invalid, or not in profile.
  bool get_scaled(uint8_t field_num, double& out) const;
};

struct DecodeStats {
  size_t files = 0;
  size_t definitions = 0;
  size_t messages = 0;
  size_t unknown_messages = 0;
  uint8_t protocol_version = 0;
  uint16_t profile_version = 0;
  bool crc_ok = true;
};

using MessageCallback = std::function<void(const Message&)>;

class Decoder {
 public:
  Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // Decodes every FIT file in `data`. Returns false (with `err`) if the
  // stream is malformed; messages decoded before the fault were delivered.
  bool decode(const uint8_t* data, size_t len, const MessageCallback& cb, std::string& err);

  const DecodeStats& stats() const { return stats_; }

 private:
  struct FieldDef {
    uint8_t num = 0;
    uint8_t size = 0;
    uint8_t base_type = 0;
    uint8_t dev_index = 0;
    bool developer = false;
  };
  struct MessageDef {
    bool valid = false;
    bool big_endian = false;
    uint16_t global_num = 0;
    uint16_t num_fields = 0;
    uint32_t data_size = 0;
    FieldDef fields[kMaxTotalFields];
  };
  struct DevField {
    bool valid = false;
    uint8_t dev_index = 0;
    uint8_t field_num = 0;
    uint8_t base_type = 0;
    char name[kDevNameLen] = {};
    char units[kDevUnitsLen] = {};
  };

  bool decode_file(const uint8_t* data, size_t len, size_t& pos, const MessageCallback& cb,
                   std::string& err);
  bool parse_definition(const uint8_t* p, size_t avail, uint8_t local, bool has_dev,
                        size_t& consumed, std::string& err);
  bool build_message(const MessageDef& def, uint8_t local, const uint8_t* p, bool compressed,
                     uint8_t time_offset);
  void register_dev_field(const Message& m);
  const DevField* find_dev_field(uint8_t dev_index, uint8_t field_num) const;
  void reset_file_state();

  MessageDef defs_[kLocalMessageCount];
  DevField dev_fields_[kMaxDevFields];
  Message msg_;
  uint32_t last_timestamp_ = 0;
  bool have_last_timestamp_ = false;
  DecodeStats stats_;
};

}  // namespace fit
