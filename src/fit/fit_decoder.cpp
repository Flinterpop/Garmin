#include "fit/fit_decoder.h"

#include <cstring>

#include "fit/fit_crc.h"
#include "fit/fit_profile.h"
#include "util/assert.h"

namespace fit {

namespace {

constexpr size_t kHeaderMin = 12;
constexpr size_t kHeaderWithCrc = 14;
constexpr size_t kCrcSize = 2;
constexpr size_t kDefFixed = 5;       // reserved, arch, global(2), num_fields
constexpr size_t kFieldDefSize = 3;   // num, size, base_type

uint64_t read_uint(const uint8_t* p, uint8_t size, bool big_endian) {
  G_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);
  uint64_t v = 0;
  if (big_endian) {
    for (uint8_t i = 0; i < size; ++i) v = (v << 8) | p[i];
  } else {
    for (uint8_t i = 0; i < size; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

// The "invalid" marker for each base type index (see FIT SDK fit.h).
uint64_t invalid_pattern(uint8_t base_type) {
  switch (base_type & kBaseTypeIndexMask) {
    case 0x00: return 0xFF;                   // enum
    case 0x01: return 0x7F;                   // sint8
    case 0x02: return 0xFF;                   // uint8
    case 0x03: return 0x7FFF;                 // sint16
    case 0x04: return 0xFFFF;                 // uint16
    case 0x05: return 0x7FFFFFFF;             // sint32
    case 0x06: return 0xFFFFFFFF;             // uint32
    case 0x07: return 0x00;                   // string
    case 0x08: return 0xFFFFFFFF;             // float32
    case 0x09: return 0xFFFFFFFFFFFFFFFFull;  // float64
    case 0x0A: return 0x00;                   // uint8z
    case 0x0B: return 0x0000;                 // uint16z
    case 0x0C: return 0x00000000;             // uint32z
    case 0x0D: return 0xFF;                   // byte
    case 0x0E: return 0x7FFFFFFFFFFFFFFFull;  // sint64
    case 0x0F: return 0xFFFFFFFFFFFFFFFFull;  // uint64
    case 0x10: return 0;                      // uint64z
    default: return 0;
  }
}

bool is_signed(uint8_t base_type) {
  const uint8_t idx = base_type & kBaseTypeIndexMask;
  return idx == 0x01 || idx == 0x03 || idx == 0x05 || idx == 0x0E;
}

bool is_float(uint8_t base_type) {
  const uint8_t idx = base_type & kBaseTypeIndexMask;
  return idx == 0x08 || idx == 0x09;
}

int64_t sign_extend(uint64_t v, uint8_t size) {
  switch (size) {
    case 1: return static_cast<int8_t>(v);
    case 2: return static_cast<int16_t>(v);
    case 4: return static_cast<int32_t>(v);
    default: return static_cast<int64_t>(v);
  }
}

}  // namespace

// ---------------------------------------------------------------- FieldValue

size_t FieldValue::count() const {
  const uint8_t elem = base_type_size(base_type);
  G_ASSERT(elem > 0);
  return size / elem;
}

bool FieldValue::element_valid(size_t idx) const {
  G_REQUIRE_RET(raw != nullptr && idx < count(), false);
  if ((base_type & kBaseTypeIndexMask) == (kString & kBaseTypeIndexMask)) {
    return raw[0] != 0;
  }
  const uint8_t elem = base_type_size(base_type);
  return read_uint(raw + idx * elem, elem, big_endian) != invalid_pattern(base_type);
}

int64_t FieldValue::as_int(size_t idx) const {
  G_ASSERT(raw != nullptr);
  G_ASSERT(idx < count());
  const uint8_t elem = base_type_size(base_type);
  if (is_float(base_type)) return static_cast<int64_t>(as_double(idx));
  const uint64_t v = read_uint(raw + idx * elem, elem, big_endian);
  return is_signed(base_type) ? sign_extend(v, elem) : static_cast<int64_t>(v);
}

double FieldValue::as_double(size_t idx) const {
  G_ASSERT(raw != nullptr);
  G_ASSERT(idx < count());
  const uint8_t elem = base_type_size(base_type);
  const uint64_t bits = read_uint(raw + idx * elem, elem, big_endian);
  if ((base_type & kBaseTypeIndexMask) == (kFloat32 & kBaseTypeIndexMask)) {
    const auto b32 = static_cast<uint32_t>(bits);
    float f = 0.0f;
    std::memcpy(&f, &b32, sizeof(f));
    return static_cast<double>(f);
  }
  if ((base_type & kBaseTypeIndexMask) == (kFloat64 & kBaseTypeIndexMask)) {
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }
  return static_cast<double>(as_int(idx));
}

std::string_view FieldValue::as_string() const {
  G_REQUIRE_RET(raw != nullptr && size > 0, std::string_view());
  size_t n = 0;
  while (n < size && raw[n] != 0) ++n;
  return std::string_view(reinterpret_cast<const char*>(raw), n);
}

// ------------------------------------------------------------------- Message

const FieldValue* Message::find(uint8_t field_num) const {
  G_ASSERT(num_fields <= kMaxTotalFields);
  for (uint16_t i = 0; i < num_fields; ++i) {
    const FieldValue& f = fields[i];
    if (!f.developer && f.num == field_num) return &f;
  }
  return nullptr;
}

bool Message::get_int(uint8_t field_num, int64_t& out) const {
  const FieldValue* f = find(field_num);
  G_REQUIRE_RET(f != nullptr && f->valid && f->element_valid(0), false);
  out = f->as_int(0);
  return true;
}

bool Message::get_scaled(uint8_t field_num, double& out) const {
  const FieldValue* f = find(field_num);
  G_REQUIRE_RET(f != nullptr && f->valid && f->element_valid(0), false);
  const FieldInfo* info = find_field(global_num, field_num);
  G_REQUIRE_RET(info != nullptr, false);
  G_ASSERT(info->scale != 0.0);
  out = f->as_double(0) / info->scale - info->offset;
  return true;
}

// ------------------------------------------------------------------- Decoder

Decoder::Decoder() { reset_file_state(); }

void Decoder::reset_file_state() {
  for (MessageDef& d : defs_) d.valid = false;
  for (DevField& d : dev_fields_) d.valid = false;
  last_timestamp_ = 0;
  have_last_timestamp_ = false;
}

bool Decoder::decode(const uint8_t* data, size_t len, const MessageCallback& cb,
                     std::string& err) {
  G_ASSERT(data != nullptr || len == 0);
  G_ASSERT(cb != nullptr);
  stats_ = DecodeStats{};
  err.clear();
  size_t pos = 0;
  // Each successful iteration consumes at least a header + CRC, so the loop
  // is bounded by len / (kHeaderMin + kCrcSize).
  const size_t max_files = len / (kHeaderMin + kCrcSize) + 1;
  for (size_t i = 0; i < max_files && pos < len; ++i) {
    // Tolerate zero padding between chained files.
    if (data[pos] == 0) {
      ++pos;
      continue;
    }
    if (!decode_file(data, len, pos, cb, err)) return false;
    ++stats_.files;
  }
  if (stats_.files == 0) {
    err = "fit: no FIT header found";
    return false;
  }
  if (!stats_.crc_ok) {
    err = "fit: CRC mismatch";
    return false;
  }
  return true;
}

bool Decoder::decode_file(const uint8_t* data, size_t len, size_t& pos,
                          const MessageCallback& cb, std::string& err) {
  const uint8_t* hdr = data + pos;
  const size_t avail = len - pos;
  if (avail < kHeaderMin) {
    err = "fit: truncated header";
    return false;
  }
  const uint8_t hdr_size = hdr[0];
  if (hdr_size != kHeaderMin && hdr_size != kHeaderWithCrc) {
    err = "fit: bad header size";
    return false;
  }
  if (std::memcmp(hdr + 8, ".FIT", 4) != 0) {
    err = "fit: missing .FIT signature";
    return false;
  }
  stats_.protocol_version = hdr[1];
  stats_.profile_version = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
  const uint32_t data_size = static_cast<uint32_t>(read_uint(hdr + 4, 4, false));
  if (hdr_size == kHeaderWithCrc) {
    const uint16_t hdr_crc = static_cast<uint16_t>(hdr[12] | (hdr[13] << 8));
    if (hdr_crc != 0 && hdr_crc != crc_compute(hdr, 12)) {
      err = "fit: header CRC mismatch";
      return false;
    }
  }
  const size_t file_len = static_cast<size_t>(hdr_size) + data_size;
  if (file_len + kCrcSize > avail) {
    err = "fit: data_size exceeds buffer";
    return false;
  }

  reset_file_state();
  size_t p = pos + hdr_size;
  const size_t data_end = pos + file_len;
  // Every record consumes at least its 1-byte header.
  for (size_t guard = 0; guard <= data_size && p < data_end; ++guard) {
    const uint8_t rh = data[p];
    ++p;
    if ((rh & kHdrCompressedBit) != 0) {
      const uint8_t local = static_cast<uint8_t>((rh & kHdrCompLocalMask) >> 5);
      const uint8_t offset = rh & kHdrCompOffsetMask;
      const MessageDef& def = defs_[local];
      if (!def.valid || p + def.data_size > data_end) {
        err = "fit: data message without definition or truncated";
        return false;
      }
      if (!build_message(def, local, data + p, true, offset)) {
        err = "fit: malformed data message";
        return false;
      }
      cb(msg_);
      p += def.data_size;
      continue;
    }
    const uint8_t local = rh & kHdrLocalMask;
    if ((rh & kHdrDefinitionBit) != 0) {
      size_t consumed = 0;
      if (!parse_definition(data + p, data_end - p, local, (rh & kHdrDeveloperBit) != 0,
                            consumed, err)) {
        return false;
      }
      p += consumed;
      ++stats_.definitions;
      continue;
    }
    const MessageDef& def = defs_[local];
    if (!def.valid || p + def.data_size > data_end) {
      err = "fit: data message without definition or truncated";
      return false;
    }
    if (!build_message(def, local, data + p, false, 0)) {
      err = "fit: malformed data message";
      return false;
    }
    if (def.global_num == kMesgFieldDescription) register_dev_field(msg_);
    cb(msg_);
    p += def.data_size;
  }
  if (p != data_end) {
    err = "fit: record overran data section";
    return false;
  }

  const uint16_t file_crc = static_cast<uint16_t>(data[data_end] | (data[data_end + 1] << 8));
  if (file_crc != crc_compute(data + pos, file_len)) stats_.crc_ok = false;
  pos = data_end + kCrcSize;
  return true;
}

bool Decoder::parse_definition(const uint8_t* p, size_t avail, uint8_t local, bool has_dev,
                               size_t& consumed, std::string& err) {
  G_ASSERT(local < kLocalMessageCount);
  if (avail < kDefFixed) {
    err = "fit: truncated definition";
    return false;
  }
  MessageDef def;
  def.big_endian = p[1] == 1;
  def.global_num = static_cast<uint16_t>(read_uint(p + 2, 2, def.big_endian));
  const uint8_t n_native = p[4];
  size_t off = kDefFixed;
  if (avail < off + static_cast<size_t>(n_native) * kFieldDefSize) {
    err = "fit: truncated definition fields";
    return false;
  }
  uint32_t total = 0;
  for (uint8_t i = 0; i < n_native; ++i) {
    FieldDef& f = def.fields[def.num_fields];
    f.num = p[off];
    f.size = p[off + 1];
    f.base_type = p[off + 2];
    f.developer = false;
    if (!base_type_valid(f.base_type)) {
      // Unknown base type: treat as opaque bytes so the size still tracks.
      f.base_type = kByte;
    }
    if (f.size % base_type_size(f.base_type) != 0) f.base_type = kByte;
    total += f.size;
    ++def.num_fields;
    off += kFieldDefSize;
  }
  if (has_dev) {
    if (avail < off + 1) {
      err = "fit: truncated developer field count";
      return false;
    }
    const uint8_t n_dev = p[off];
    ++off;
    if (avail < off + static_cast<size_t>(n_dev) * kFieldDefSize) {
      err = "fit: truncated developer fields";
      return false;
    }
    for (uint8_t i = 0; i < n_dev; ++i) {
      FieldDef& f = def.fields[def.num_fields];
      f.num = p[off];
      f.size = p[off + 1];
      f.dev_index = p[off + 2];
      f.developer = true;
      const DevField* info = find_dev_field(f.dev_index, f.num);
      f.base_type = (info != nullptr) ? info->base_type : static_cast<uint8_t>(kByte);
      if (!base_type_valid(f.base_type) || f.size % base_type_size(f.base_type) != 0) {
        f.base_type = kByte;
      }
      total += f.size;
      ++def.num_fields;
      off += kFieldDefSize;
    }
  }
  G_ASSERT(def.num_fields <= kMaxTotalFields);
  def.data_size = total;
  def.valid = true;
  defs_[local] = def;
  consumed = off;
  return true;
}

bool Decoder::build_message(const MessageDef& def, uint8_t local, const uint8_t* p,
                            bool compressed, uint8_t time_offset) {
  G_ASSERT(def.valid);
  msg_.global_num = def.global_num;
  msg_.local_num = local;
  msg_.num_fields = def.num_fields;
  msg_.has_timestamp = false;
  msg_.timestamp = 0;

  const FieldValue* ts_field = nullptr;
  const FieldValue* ts16_field = nullptr;
  size_t off = 0;
  for (uint16_t i = 0; i < def.num_fields; ++i) {
    const FieldDef& fd = def.fields[i];
    FieldValue& fv = msg_.fields[i];
    fv.num = fd.num;
    fv.base_type = fd.base_type;
    fv.size = fd.size;
    fv.dev_index = fd.dev_index;
    fv.developer = fd.developer;
    fv.big_endian = def.big_endian;
    fv.raw = p + off;
    fv.dev_name = nullptr;
    fv.dev_units = nullptr;
    if (fd.developer) {
      const DevField* info = find_dev_field(fd.dev_index, fd.num);
      if (info != nullptr) {
        fv.dev_name = info->name;
        fv.dev_units = info->units;
      }
    }
    fv.valid = false;
    const size_t n = fv.count();
    for (size_t k = 0; k < n && !fv.valid; ++k) fv.valid = fv.element_valid(k);

    if (!fd.developer && fd.num == kTimestampFieldNum && fd.size == 4 &&
        (fd.base_type & kBaseTypeIndexMask) == (kUint32 & kBaseTypeIndexMask)) {
      ts_field = &fv;
    }
    if (!fd.developer && def.global_num == kMesgMonitoring && fd.num == 26 && fd.size == 2) {
      ts16_field = &fv;
    }
    off += fd.size;
  }
  G_ASSERT(off == def.data_size);

  if (ts_field != nullptr && ts_field->valid) {
    last_timestamp_ = static_cast<uint32_t>(ts_field->as_int(0));
    have_last_timestamp_ = true;
    msg_.has_timestamp = true;
    msg_.timestamp = last_timestamp_;
  } else if (compressed && have_last_timestamp_) {
    const uint32_t delta = (time_offset - (last_timestamp_ & 0x1Fu)) & 0x1Fu;
    last_timestamp_ += delta;
    msg_.has_timestamp = true;
    msg_.timestamp = last_timestamp_;
  } else if (ts16_field != nullptr && ts16_field->valid && have_last_timestamp_) {
    const uint32_t ts16 = static_cast<uint32_t>(ts16_field->as_int(0));
    const uint32_t delta = (ts16 - (last_timestamp_ & 0xFFFFu)) & 0xFFFFu;
    last_timestamp_ += delta;
    msg_.has_timestamp = true;
    msg_.timestamp = last_timestamp_;
  }

  ++stats_.messages;
  if (find_mesg(def.global_num) == nullptr) ++stats_.unknown_messages;
  return true;
}

void Decoder::register_dev_field(const Message& m) {
  G_ASSERT(m.global_num == kMesgFieldDescription);
  int64_t dev_index = 0;
  int64_t field_num = 0;
  int64_t base_type = 0;
  if (!m.get_int(0, dev_index) || !m.get_int(1, field_num) || !m.get_int(2, base_type)) return;
  if (dev_index < 0 || dev_index > 255 || field_num < 0 || field_num > 255) return;

  // Reuse an existing slot for the same (index, num) or take the first free one.
  DevField* slot = nullptr;
  for (DevField& d : dev_fields_) {
    if (d.valid && d.dev_index == dev_index && d.field_num == field_num) {
      slot = &d;
      break;
    }
    if (!d.valid && slot == nullptr) slot = &d;
  }
  if (slot == nullptr) return;  // registry full: field decodes as bytes
  slot->valid = true;
  slot->dev_index = static_cast<uint8_t>(dev_index);
  slot->field_num = static_cast<uint8_t>(field_num);
  slot->base_type = static_cast<uint8_t>(base_type);
  slot->name[0] = '\0';
  slot->units[0] = '\0';
  const FieldValue* name = m.find(3);
  if (name != nullptr && name->valid) {
    const std::string_view s = name->as_string();
    const size_t n = std::min(s.size(), kDevNameLen - 1);
    std::memcpy(slot->name, s.data(), n);
    slot->name[n] = '\0';
  }
  const FieldValue* units = m.find(8);
  if (units != nullptr && units->valid) {
    const std::string_view s = units->as_string();
    const size_t n = std::min(s.size(), kDevUnitsLen - 1);
    std::memcpy(slot->units, s.data(), n);
    slot->units[n] = '\0';
  }
}

const Decoder::DevField* Decoder::find_dev_field(uint8_t dev_index, uint8_t field_num) const {
  for (const DevField& d : dev_fields_) {
    if (d.valid && d.dev_index == dev_index && d.field_num == field_num) return &d;
  }
  return nullptr;
}

}  // namespace fit
