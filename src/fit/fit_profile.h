// The subset of the FIT Global Profile used by this project: message names
// and per-field name / scale / offset / units so decoded values can be
// converted to engineering units. Unknown messages and fields still decode;
// they just come back with numeric identifiers instead of names.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fit {

struct FieldInfo {
  uint8_t num;
  const char* name;
  double scale;      // engineering value = raw / scale - offset
  double offset;
  const char* units;
  bool is_datetime;  // seconds since FIT epoch
};

struct MesgInfo {
  uint16_t num;
  const char* name;
  const FieldInfo* fields;
  size_t field_count;
};

// nullptr when the message / field is not in our profile subset.
const MesgInfo* find_mesg(uint16_t mesg_num);
const FieldInfo* find_field(uint16_t mesg_num, uint8_t field_num);

// Human-readable names for a few enums we display.
const char* file_type_name(uint8_t type);
const char* sport_name(uint8_t sport);

// Semicircles -> degrees.
double semicircles_to_degrees(int32_t semicircles);

}  // namespace fit
