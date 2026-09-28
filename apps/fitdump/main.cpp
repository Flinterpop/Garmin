// fitdump: inspect a FIT file (from the watch's USB storage or a Connect
// download). Prints a summary; --csv writes every field in long format
// (mesg,timestamp,field,value,units) which is easy to pivot or plot.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fit/fit_decoder.h"
#include "fit/fit_profile.h"
#include "fit/fit_types.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace {

constexpr size_t kMaxPrintedMessages = 1000000;

struct Options {
  std::string input;
  std::string csv_out;
  bool print_messages = false;
  bool include_unknown = false;
};

void usage() {
  std::fputs(
      "usage: fitdump <file.fit> [--csv out.csv] [--print] [--unknown]\n"
      "  --csv <path>   write all fields as mesg,timestamp,field,value,units\n"
      "  --print        print every message to stdout\n"
      "  --unknown      include messages/fields not in the built-in profile\n",
      stderr);
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--csv" && i + 1 < argc) {
      o.csv_out = argv[++i];
    } else if (a == "--print") {
      o.print_messages = true;
    } else if (a == "--unknown") {
      o.include_unknown = true;
    } else if (!a.empty() && a[0] == '-') {
      return false;
    } else if (o.input.empty()) {
      o.input = a;
    } else {
      return false;
    }
  }
  return !o.input.empty();
}

std::string field_label(const fit::Message& m, const fit::FieldValue& f) {
  if (f.developer) {
    if (f.dev_name != nullptr && f.dev_name[0] != '\0') return std::string("dev:") + f.dev_name;
    return "dev:" + std::to_string(f.dev_index) + "." + std::to_string(f.num);
  }
  const fit::FieldInfo* info = fit::find_field(m.global_num, f.num);
  if (info != nullptr) return info->name;
  return "field_" + std::to_string(f.num);
}

std::string format_value(const fit::Message& m, const fit::FieldValue& f, std::string& units) {
  G_ASSERT(f.raw != nullptr);
  G_ASSERT(f.valid);
  units.clear();
  const bool is_string = (f.base_type & fit::kBaseTypeIndexMask) ==
                         (fit::kString & fit::kBaseTypeIndexMask);
  if (is_string) return std::string(f.as_string());
  const fit::FieldInfo* info = f.developer ? nullptr : fit::find_field(m.global_num, f.num);
  if (f.developer && f.dev_units != nullptr) units = f.dev_units;
  if (info != nullptr) units = info->units;

  std::string out;
  const size_t n = f.count();
  char buf[64];
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) out.push_back('|');
    if (!f.element_valid(i)) {
      out += "-";
      continue;
    }
    if (info != nullptr && info->is_datetime) {
      out += gutil::iso8601_utc(gutil::fit_to_unix(static_cast<uint32_t>(f.as_int(i))));
    } else if (info != nullptr && (info->scale != 1.0 || info->offset != 0.0)) {
      std::snprintf(buf, sizeof(buf), "%.6g", f.as_double(i) / info->scale - info->offset);
      out += buf;
    } else if ((f.base_type & fit::kBaseTypeIndexMask) >= 0x08 &&
               (f.base_type & fit::kBaseTypeIndexMask) <= 0x09) {
      std::snprintf(buf, sizeof(buf), "%.6g", f.as_double(i));
      out += buf;
    } else {
      out += std::to_string(f.as_int(i));
    }
  }
  return out;
}

std::string csv_escape(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

struct Summary {
  std::map<uint16_t, size_t> counts;
  std::string file_type;
  std::string product;
  std::string created;
  std::vector<std::string> sessions;
  size_t records = 0;
  uint32_t first_ts = 0;
  uint32_t last_ts = 0;
};

void note_session(const fit::Message& m, Summary& s) {
  int64_t sport = 0;
  double dist = 0.0;
  double secs = 0.0;
  int64_t avg_hr = 0;
  int64_t kcal = 0;
  std::string line = "  session: ";
  if (m.get_int(5, sport)) line += fit::sport_name(static_cast<uint8_t>(sport));
  char buf[128];
  if (m.get_scaled(8, secs)) {
    std::snprintf(buf, sizeof(buf), "  %.0f min", secs / 60.0);
    line += buf;
  }
  if (m.get_scaled(9, dist)) {
    std::snprintf(buf, sizeof(buf), "  %.2f km", dist / 1000.0);
    line += buf;
  }
  if (m.get_int(16, avg_hr)) line += "  avg HR " + std::to_string(avg_hr);
  if (m.get_int(11, kcal)) line += "  " + std::to_string(kcal) + " kcal";
  s.sessions.push_back(line);
}

// Folds one message into the summary counters.
void summarize(const fit::Message& m, Summary& sum) {
  ++sum.counts[m.global_num];
  if (m.has_timestamp) {
    if (sum.first_ts == 0) sum.first_ts = m.timestamp;
    sum.last_ts = m.timestamp;
  }
  if (m.global_num == fit::kMesgFileId) {
    int64_t type = 0;
    if (m.get_int(0, type)) sum.file_type = fit::file_type_name(static_cast<uint8_t>(type));
    int64_t created = 0;
    if (m.get_int(4, created)) {
      sum.created = gutil::iso8601_utc(gutil::fit_to_unix(static_cast<uint32_t>(created)));
    }
    const fit::FieldValue* name = m.find(8);
    if (name != nullptr && name->valid) sum.product = std::string(name->as_string());
  }
  if (m.global_num == fit::kMesgDeviceInfo && sum.product.empty()) {
    const fit::FieldValue* name = m.find(27);
    if (name != nullptr && name->valid) sum.product = std::string(name->as_string());
  }
  if (m.global_num == fit::kMesgSession) note_session(m, sum);
  if (m.global_num == fit::kMesgRecord) ++sum.records;
}

// Writes one message to stdout and/or the CSV, field by field.
void emit(const fit::Message& m, const std::string& mesg_name, const Options& opt, FILE* csv) {
  G_ASSERT(opt.print_messages || csv != nullptr);
  const std::string ts =
      m.has_timestamp ? gutil::iso8601_utc(gutil::fit_to_unix(m.timestamp)) : std::string();
  if (opt.print_messages) std::printf("%s [%s]", mesg_name.c_str(), ts.c_str());
  for (uint16_t i = 0; i < m.num_fields; ++i) {
    const fit::FieldValue& f = m.fields[i];
    if (!f.valid) continue;
    const bool known = f.developer || fit::find_field(m.global_num, f.num) != nullptr;
    if (!known && !opt.include_unknown) continue;
    std::string units;
    const std::string label = field_label(m, f);
    const std::string value = format_value(m, f, units);
    if (opt.print_messages) {
      std::printf(" %s=%s%s%s", label.c_str(), value.c_str(), units.empty() ? "" : " ",
                  units.c_str());
    }
    if (csv != nullptr) {
      std::fprintf(csv, "%s,%s,%s,%s,%s\n", mesg_name.c_str(), ts.c_str(), label.c_str(),
                   csv_escape(value).c_str(), units.c_str());
    }
  }
  if (opt.print_messages) std::printf("\n");
}

void print_summary(const Options& opt, size_t bytes, const fit::DecodeStats& st,
                   const Summary& sum) {
  std::printf("file:      %s (%zu bytes)\n", opt.input.c_str(), bytes);
  std::printf("protocol:  %u.%u  profile: %u.%02u  files: %zu  crc: %s\n",
              st.protocol_version >> 4, st.protocol_version & 0x0F, st.profile_version / 100,
              st.profile_version % 100, st.files, st.crc_ok ? "ok" : "MISMATCH");
  std::printf("type:      %s\n", sum.file_type.empty() ? "?" : sum.file_type.c_str());
  if (!sum.product.empty()) std::printf("device:    %s\n", sum.product.c_str());
  if (!sum.created.empty()) std::printf("created:   %s\n", sum.created.c_str());
  if (sum.first_ts != 0) {
    std::printf("span:      %s .. %s\n",
                gutil::iso8601_utc(gutil::fit_to_unix(sum.first_ts)).c_str(),
                gutil::iso8601_utc(gutil::fit_to_unix(sum.last_ts)).c_str());
  }
  std::printf("messages:  %zu (%zu definitions, %zu of unknown type)\n", st.messages,
              st.definitions, st.unknown_messages);
  for (const auto& [num, count] : sum.counts) {
    const fit::MesgInfo* mi = fit::find_mesg(num);
    const std::string name = mi != nullptr ? mi->name : "mesg_" + std::to_string(num);
    std::printf("  %-24s %8zu\n", name.c_str(), count);
  }
  for (const std::string& s : sum.sessions) std::printf("%s\n", s.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    usage();
    return 2;
  }
  G_ASSERT(!opt.input.empty());
  std::vector<uint8_t> bytes;
  if (!gutil::read_file(opt.input, bytes)) {
    std::fprintf(stderr, "cannot read %s\n", opt.input.c_str());
    return 1;
  }
  FILE* csv = nullptr;
  if (!opt.csv_out.empty()) {
    csv = std::fopen(opt.csv_out.c_str(), "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", opt.csv_out.c_str());
      return 1;
    }
    std::fputs("mesg,timestamp,field,value,units\n", csv);
  }

  Summary sum;
  size_t printed = 0;
  auto decoder = std::make_unique<fit::Decoder>();
  const auto on_message = [&](const fit::Message& m) {
    summarize(m, sum);
    const fit::MesgInfo* mi = fit::find_mesg(m.global_num);
    const bool wanted = (mi != nullptr) || opt.include_unknown;
    if (!wanted || (csv == nullptr && !opt.print_messages) || printed >= kMaxPrintedMessages) return;
    ++printed;
    emit(m, mi != nullptr ? mi->name : "mesg_" + std::to_string(m.global_num), opt, csv);
  };
  std::string err;
  const bool ok = decoder->decode(bytes.data(), bytes.size(), on_message, err);
  G_ASSERT(printed <= kMaxPrintedMessages);
  if (csv != nullptr && std::fclose(csv) != 0) {
    std::fprintf(stderr, "warning: error closing %s\n", opt.csv_out.c_str());
  }
  print_summary(opt, bytes.size(), decoder->stats(), sum);
  if (!ok) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  return 0;
}
