#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <memory>
#include <vector>

#include "fit/fit_crc.h"
#include "fit/fit_decoder.h"
#include "fit/fit_profile.h"
#include "fit/fit_types.h"

using Catch::Matchers::WithinAbs;

namespace {

using FieldSpec = std::array<uint8_t, 3>;  // num, size, base_type (or dev index)

// Builds a syntactically valid FIT file byte by byte.
struct FitBuilder {
  std::vector<uint8_t> body;
  bool big_endian = false;

  void push16(uint16_t v) {
    if (big_endian) {
      body.push_back(static_cast<uint8_t>(v >> 8));
      body.push_back(static_cast<uint8_t>(v));
    } else {
      body.push_back(static_cast<uint8_t>(v));
      body.push_back(static_cast<uint8_t>(v >> 8));
    }
  }
  void push32(uint32_t v) {
    if (big_endian) {
      for (int i = 3; i >= 0; --i) body.push_back(static_cast<uint8_t>(v >> (8 * i)));
    } else {
      for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void push_str(const char* s, size_t field_size) {
    size_t i = 0;
    for (; s[i] != '\0' && i < field_size; ++i) body.push_back(static_cast<uint8_t>(s[i]));
    for (; i < field_size; ++i) body.push_back(0);
  }

  void def(uint8_t local, uint16_t global, const std::vector<FieldSpec>& fields,
           const std::vector<FieldSpec>& dev = {}) {
    body.push_back(static_cast<uint8_t>(0x40 | local | (dev.empty() ? 0 : 0x20)));
    body.push_back(0);
    body.push_back(big_endian ? 1 : 0);
    push16(global);
    body.push_back(static_cast<uint8_t>(fields.size()));
    for (const FieldSpec& f : fields) body.insert(body.end(), f.begin(), f.end());
    if (!dev.empty()) {
      body.push_back(static_cast<uint8_t>(dev.size()));
      for (const FieldSpec& f : dev) body.insert(body.end(), f.begin(), f.end());
    }
  }
  void data_header(uint8_t local) { body.push_back(local); }
  void compressed_header(uint8_t local, uint8_t offset) {
    body.push_back(static_cast<uint8_t>(0x80 | (local << 5) | (offset & 0x1F)));
  }

  std::vector<uint8_t> finish(bool corrupt_crc = false) const {
    std::vector<uint8_t> out;
    out.push_back(14);
    out.push_back(0x20);  // protocol 2.0
    out.push_back(static_cast<uint8_t>(2132 & 0xFF));
    out.push_back(static_cast<uint8_t>(2132 >> 8));
    const uint32_t size = static_cast<uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(size >> (8 * i)));
    out.insert(out.end(), {'.', 'F', 'I', 'T'});
    const uint16_t hcrc = fit::crc_compute(out.data(), 12);
    out.push_back(static_cast<uint8_t>(hcrc));
    out.push_back(static_cast<uint8_t>(hcrc >> 8));
    out.insert(out.end(), body.begin(), body.end());
    uint16_t crc = fit::crc_compute(out.data(), out.size());
    if (corrupt_crc) crc = static_cast<uint16_t>(crc ^ 0x5555);
    out.push_back(static_cast<uint8_t>(crc));
    out.push_back(static_cast<uint8_t>(crc >> 8));
    return out;
  }
};

struct Captured {
  uint16_t global = 0;
  bool has_ts = false;
  uint32_t ts = 0;
  std::vector<std::pair<uint8_t, int64_t>> ints;   // native valid fields
  std::string str8;                                  // field 8 string if any
  std::vector<std::pair<std::string, int64_t>> dev;  // developer fields
  std::vector<int64_t> array0;                       // field 0 elements (valid or -1)
  double scaled6 = -1.0;
  double scaled2 = -1.0;
};

std::vector<Captured> run(const std::vector<uint8_t>& file, bool& ok, std::string& err,
                          fit::DecodeStats* stats = nullptr) {
  std::vector<Captured> out;
  auto dec = std::make_unique<fit::Decoder>();
  ok = dec->decode(file.data(), file.size(),
                   [&](const fit::Message& m) {
                     Captured c;
                     c.global = m.global_num;
                     c.has_ts = m.has_timestamp;
                     c.ts = m.timestamp;
                     for (uint16_t i = 0; i < m.num_fields; ++i) {
                       const fit::FieldValue& f = m.fields[i];
                       if (f.developer) {
                         c.dev.emplace_back(f.dev_name != nullptr ? f.dev_name : "", f.as_int(0));
                         continue;
                       }
                       if (!f.valid) continue;
                       if ((f.base_type & 0x1F) == 0x07) {
                         if (f.num == 8) c.str8 = std::string(f.as_string());
                         continue;
                       }
                       if (f.num == 0 && f.count() > 1) {
                         for (size_t k = 0; k < f.count(); ++k) {
                           c.array0.push_back(f.element_valid(k) ? f.as_int(k) : -1);
                         }
                       }
                       c.ints.emplace_back(f.num, f.as_int(0));
                     }
                     m.get_scaled(6, c.scaled6);
                     m.get_scaled(2, c.scaled2);
                     out.push_back(c);
                   },
                   err);
  if (stats != nullptr) *stats = dec->stats();
  return out;
}

int64_t get(const Captured& c, uint8_t num) {
  for (const auto& [n, v] : c.ints) {
    if (n == num) return v;
  }
  return INT64_MIN;
}

}  // namespace

TEST_CASE("decodes file_id with string and scaled record fields") {
  FitBuilder b;
  b.def(0, fit::kMesgFileId,
        {{0, 1, fit::kEnum}, {1, 2, fit::kUint16}, {4, 4, fit::kUint32}, {8, 6, fit::kString}});
  b.data_header(0);
  b.body.push_back(fit::kFileActivity);
  b.push16(1);
  b.push32(1000000000);
  b.push_str("fenix", 6);

  b.def(1, fit::kMesgRecord,
        {{253, 4, fit::kUint32}, {3, 1, fit::kUint8}, {6, 2, fit::kUint16}, {2, 2, fit::kUint16}});
  b.data_header(1);
  b.push32(1000);
  b.body.push_back(120);
  b.push16(3500);          // 3.5 m/s
  b.push16((100 + 500) * 5);  // 100 m

  bool ok = false;
  std::string err;
  fit::DecodeStats st;
  const auto msgs = run(b.finish(), ok, err, &st);
  REQUIRE(ok);
  REQUIRE(st.crc_ok);
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0].global == fit::kMesgFileId);
  CHECK(get(msgs[0], 0) == fit::kFileActivity);
  CHECK(get(msgs[0], 4) == 1000000000);
  CHECK(msgs[0].str8 == "fenix");
  CHECK(msgs[1].has_ts);
  CHECK(msgs[1].ts == 1000);
  CHECK(get(msgs[1], 3) == 120);
  CHECK_THAT(msgs[1].scaled6, WithinAbs(3.5, 1e-9));
  CHECK_THAT(msgs[1].scaled2, WithinAbs(100.0, 1e-9));
}

TEST_CASE("compressed timestamp headers extend the last timestamp") {
  FitBuilder b;
  b.def(1, fit::kMesgRecord, {{253, 4, fit::kUint32}, {3, 1, fit::kUint8}});
  b.data_header(1);
  b.push32(1000);  // 1000 & 0x1F == 8
  b.body.push_back(100);
  b.def(2, fit::kMesgRecord, {{3, 1, fit::kUint8}});
  b.compressed_header(2, 10);  // delta (10-8)&31 = 2
  b.body.push_back(101);
  b.compressed_header(2, 5);   // last=1002 (&31=10); delta (5-10)&31 = 27
  b.body.push_back(102);

  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 3);
  CHECK(msgs[1].ts == 1002);
  CHECK(msgs[2].ts == 1029);
  CHECK(get(msgs[2], 3) == 102);
}

TEST_CASE("invalid values are reported as invalid") {
  FitBuilder b;
  b.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}, {3, 1, fit::kUint8}, {6, 2, fit::kUint16}});
  b.data_header(0);
  b.push32(5);
  b.body.push_back(0xFF);
  b.push16(0xFFFF);
  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 1);
  CHECK(get(msgs[0], 3) == INT64_MIN);
  CHECK(msgs[0].scaled6 == -1.0);
}

TEST_CASE("monitoring timestamp_16 expands against the last full timestamp") {
  FitBuilder b;
  b.def(3, fit::kMesgMonitoring, {{253, 4, fit::kUint32}});
  b.data_header(3);
  b.push32(100000);
  b.def(4, fit::kMesgMonitoring, {{26, 2, fit::kUint16}, {27, 1, fit::kUint8}});
  b.data_header(4);
  b.push16(static_cast<uint16_t>((100000 + 60) & 0xFFFF));
  b.body.push_back(70);
  b.data_header(4);
  b.push16(static_cast<uint16_t>((100000 + 60 + 40000) & 0xFFFF));  // wraps 16 bits
  b.body.push_back(71);

  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 3);
  CHECK(msgs[1].ts == 100060);
  CHECK(get(msgs[1], 27) == 70);
  CHECK(msgs[2].ts == 140060);
}

TEST_CASE("developer fields are named via field_description") {
  FitBuilder b;
  b.def(5, fit::kMesgFieldDescription,
        {{0, 1, fit::kUint8}, {1, 1, fit::kUint8}, {2, 1, fit::kUint8}, {3, 8, fit::kString},
         {8, 4, fit::kString}});
  b.data_header(5);
  b.body.push_back(0);             // developer_data_index
  b.body.push_back(0);             // field_definition_number
  b.body.push_back(fit::kUint16);  // base type
  b.push_str("hr2", 8);
  b.push_str("bpm", 4);
  b.def(6, fit::kMesgRecord, {{3, 1, fit::kUint8}}, {{0, 2, 0}});
  b.data_header(6);
  b.body.push_back(90);
  b.push16(150);

  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 2);
  REQUIRE(msgs[1].dev.size() == 1);
  CHECK(msgs[1].dev[0].first == "hr2");
  CHECK(msgs[1].dev[0].second == 150);
  CHECK(get(msgs[1], 3) == 90);
}

TEST_CASE("big-endian definitions decode") {
  FitBuilder b;
  b.big_endian = true;
  b.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}, {6, 2, fit::kUint16}});
  b.data_header(0);
  b.push32(0x01020304);
  b.push16(2500);
  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 1);
  CHECK(msgs[0].ts == 0x01020304);
  CHECK_THAT(msgs[0].scaled6, WithinAbs(2.5, 1e-9));
}

TEST_CASE("array fields expose per-element validity") {
  FitBuilder b;
  b.def(0, fit::kMesgHrv, {{0, 10, fit::kUint16}});
  b.data_header(0);
  const uint16_t values[] = {800, 810, 0xFFFF, 790, 0xFFFF};
  for (const uint16_t v : values) b.push16(v);
  bool ok = false;
  std::string err;
  const auto msgs = run(b.finish(), ok, err);
  REQUIRE(ok);
  REQUIRE(msgs.size() == 1);
  REQUIRE(msgs[0].array0.size() == 5);
  CHECK(msgs[0].array0[0] == 800);
  CHECK(msgs[0].array0[2] == -1);
  CHECK(msgs[0].array0[3] == 790);
}

TEST_CASE("crc mismatch is reported after delivering messages") {
  FitBuilder b;
  b.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}});
  b.data_header(0);
  b.push32(1);
  bool ok = true;
  std::string err;
  const auto msgs = run(b.finish(true), ok, err);
  CHECK_FALSE(ok);
  CHECK(err.find("CRC") != std::string::npos);
  CHECK(msgs.size() == 1);
}

TEST_CASE("truncated and garbage input fail cleanly") {
  FitBuilder b;
  b.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}});
  b.data_header(0);
  b.push32(1);
  std::vector<uint8_t> file = b.finish();
  file.resize(file.size() - 5);
  bool ok = true;
  std::string err;
  run(file, ok, err);
  CHECK_FALSE(ok);

  const std::vector<uint8_t> junk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  run(junk, ok, err);
  CHECK_FALSE(ok);

  // Data message referencing an undefined local type.
  FitBuilder c;
  c.data_header(7);
  run(c.finish(), ok, err);
  CHECK_FALSE(ok);
}

TEST_CASE("chained FIT files in one buffer are all decoded") {
  FitBuilder a;
  a.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}});
  a.data_header(0);
  a.push32(10);
  FitBuilder b;
  b.def(0, fit::kMesgRecord, {{253, 4, fit::kUint32}, {3, 1, fit::kUint8}});
  b.data_header(0);
  b.push32(20);
  b.body.push_back(55);
  std::vector<uint8_t> file = a.finish();
  const std::vector<uint8_t> second = b.finish();
  file.insert(file.end(), second.begin(), second.end());

  bool ok = false;
  std::string err;
  fit::DecodeStats st;
  const auto msgs = run(file, ok, err, &st);
  REQUIRE(ok);
  CHECK(st.files == 2);
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[1].ts == 20);
  CHECK(get(msgs[1], 3) == 55);
}

TEST_CASE("profile lookups") {
  const fit::FieldInfo* f = fit::find_field(fit::kMesgRecord, 3);
  REQUIRE(f != nullptr);
  CHECK(std::string(f->name) == "heart_rate");
  CHECK(fit::find_field(fit::kMesgRecord, 250) == nullptr);
  CHECK(fit::find_mesg(9999) == nullptr);
  CHECK_THAT(fit::semicircles_to_degrees(1073741824), WithinAbs(90.0, 1e-9));
  CHECK(std::string(fit::sport_name(1)) == "running");
}
