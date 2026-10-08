#include "strava/api.h"

#include <nlohmann/json.hpp>

#include "gc/http_client.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace strava {

using nlohmann::json;

namespace {

constexpr char kApi[] = "https://www.strava.com/api/v3";
constexpr int kPerPage = 200;
constexpr int kMaxPages = static_cast<int>(kMaxListed) / kPerPage;
constexpr size_t kMaxErrorBody = 300;
constexpr size_t kMaxFitBytes = 25u * 1024u * 1024u;  // Strava's upload limit

int64_t int_or_zero(const json& j, const char* key) {
  G_ASSERT(key != nullptr);
  if (!j.contains(key) || !j[key].is_number_integer()) return 0;
  return j[key].get<int64_t>();
}

}  // namespace

bool parse_activities(const std::string& body, std::vector<Activity>& out) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_array()) return false;
  for (size_t i = 0; i < j.size() && out.size() < kMaxListed; ++i) {
    const json& a = j[i];
    if (!a.is_object()) continue;
    Activity act;
    act.id = int_or_zero(a, "id");
    const std::string start = a.value("start_date", "");
    // "2024-01-04T11:17:49Z": parse_datetime reads it as UTC and ignores the Z.
    if (act.id == 0 || !gutil::parse_datetime(start.substr(0, 19), act.start_ts)) continue;
    act.sport_type = a.value("sport_type", "");
    act.name = a.value("name", "");
    out.push_back(act);
  }
  return true;
}

bool parse_upload(const std::string& body, UploadState& out) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object()) return false;
  const int64_t id = int_or_zero(j, "id");
  if (id != 0) out.upload_id = id;
  out.activity_id = int_or_zero(j, "activity_id");
  out.error = j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : std::string();
  out.duplicate_of = duplicate_activity_id(out.error);
  return out.upload_id != 0;
}

int64_t duplicate_activity_id(const std::string& error) {
  if (error.find("duplicate") == std::string::npos) return 0;
  const std::string marker = "/activities/";
  const size_t at = error.find(marker);
  if (at == std::string::npos) return 0;
  int64_t id = 0;
  size_t i = at + marker.size();
  for (int n = 0; n < 19 && i < error.size() && error[i] >= '0' && error[i] <= '9'; ++n, ++i) {
    id = id * 10 + (error[i] - '0');
  }
  return id;
}

const Activity* find_match(const std::vector<Activity>& acts, int64_t start_ts) {
  G_ASSERT(start_ts > 0);
  const Activity* best = nullptr;
  int64_t best_gap = kMatchToleranceS + 1;
  for (size_t i = 0; i < acts.size() && i < kMaxListed; ++i) {
    const int64_t gap = acts[i].start_ts > start_ts ? acts[i].start_ts - start_ts : start_ts - acts[i].start_ts;
    if (gap < best_gap) {
      best = &acts[i];
      best_gap = gap;
    }
  }
  return best;
}

std::string multipart_body(const std::string& boundary,
                           const std::vector<std::pair<std::string, std::string>>& fields,
                           const std::string& filename, const std::string& bytes) {
  G_ASSERT(!boundary.empty() && !filename.empty());
  G_ASSERT(fields.size() < 32);
  std::string out;
  out.reserve(bytes.size() + 1024);
  for (size_t i = 0; i < fields.size(); ++i) {
    out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fields[i].first + "\"\r\n\r\n" +
           fields[i].second + "\r\n";
  }
  out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename +
         "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
  out += bytes;
  out += "\r\n--" + boundary + "--\r\n";
  return out;
}

Client::Client(gc::HttpClient& http, std::string access_token) : http_(http), token_(std::move(access_token)) {
  G_ASSERT(!token_.empty());
}

Status Client::call(const std::string& method, const std::string& path, const std::string& content_type,
                    const std::string& body, std::string& resp, std::string& err) {
  G_ASSERT(!path.empty() && path[0] == '/');
  gc::HeaderMap hdr = {{"Authorization", "Bearer " + token_}};
  if (!content_type.empty()) hdr["Content-Type"] = content_type;
  gc::HttpResponse r;
  if (!http_.request(method, std::string(kApi) + path, hdr, body, r, err)) return Status::kFailed;
  resp = r.body;
  if (r.status == 429) return Status::kRateLimited;
  if (r.status == 401) {
    err = "Strava no longer accepts gview's login";
    return Status::kUnauthorized;
  }
  if (r.status < 200 || r.status >= 300) {
    err = "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, kMaxErrorBody);
    return Status::kFailed;
  }
  return Status::kOk;
}

Status Client::list(int64_t after, int64_t before, std::vector<Activity>& out, std::string& err) {
  G_REQUIRE_RET(after < before, Status::kFailed);
  out.clear();
  for (int page = 1; page <= kMaxPages; ++page) {
    const std::string q = gc::form_encode({{"after", std::to_string(after)},
                                           {"before", std::to_string(before)},
                                           {"page", std::to_string(page)},
                                           {"per_page", std::to_string(kPerPage)}});
    std::string body;
    const Status s = call("GET", "/athlete/activities?" + q, std::string(), std::string(), body, err);
    if (s != Status::kOk) return s;
    const size_t before_count = out.size();
    if (!parse_activities(body, out)) {
      err = "unexpected activity list from Strava";
      return Status::kFailed;
    }
    if (out.size() - before_count < static_cast<size_t>(kPerPage)) return Status::kOk;
  }
  err = "more activities in range than gview reads at once";
  return Status::kFailed;
}

Status Client::upload(const std::filesystem::path& fit, const std::string& external_id, UploadState& out,
                      std::string& err) {
  G_REQUIRE_RET(!external_id.empty(), Status::kFailed);
  std::vector<uint8_t> raw;
  if (!gutil::read_file(fit, raw) || raw.empty() || raw.size() > kMaxFitBytes) {
    err = "could not read " + fit.string();
    return Status::kFailed;
  }
  const std::string bytes(raw.begin(), raw.end());
  const std::string boundary = "----gview" + std::to_string(gutil::now_unix()) + external_id;
  const std::string body = multipart_body(boundary, {{"data_type", "fit"}, {"external_id", external_id}},
                                          fit.filename().string(), bytes);
  std::string resp;
  const Status s = call("POST", "/uploads", "multipart/form-data; boundary=" + boundary, body, resp, err);
  if (s != Status::kOk) return s;
  out = UploadState{};
  if (!parse_upload(resp, out)) {
    err = "unexpected upload answer from Strava";
    return Status::kFailed;
  }
  return Status::kOk;
}

Status Client::upload_status(int64_t upload_id, UploadState& out, std::string& err) {
  G_REQUIRE_RET(upload_id > 0, Status::kFailed);
  std::string resp;
  const Status s = call("GET", "/uploads/" + std::to_string(upload_id), std::string(), std::string(), resp, err);
  if (s != Status::kOk) return s;
  if (!parse_upload(resp, out)) {
    err = "unexpected upload status from Strava";
    return Status::kFailed;
  }
  return Status::kOk;
}

Status Client::update(int64_t activity_id, const std::string& sport_type, const std::string& name,
                      std::string& err) {
  G_REQUIRE_RET(activity_id > 0 && !sport_type.empty(), Status::kFailed);
  std::vector<std::pair<std::string, std::string>> form = {{"sport_type", sport_type}};
  if (!name.empty()) form.emplace_back("name", name);
  std::string resp;
  return call("PUT", "/activities/" + std::to_string(activity_id), "application/x-www-form-urlencoded",
              gc::form_encode(form), resp, err);
}

}  // namespace strava
