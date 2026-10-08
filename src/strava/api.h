// The four Strava API calls gview needs: list the athlete's activities, upload
// a FIT file, poll the upload, and set an activity's sport type and title.
// The parsers are separate so they can be tested without the network.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gc {
class HttpClient;
}

namespace strava {

constexpr int64_t kMatchToleranceS = 120;  // Strava and the FIT file may differ by a few seconds
constexpr size_t kMaxListed = 10000;

struct Activity {
  int64_t id = 0;
  int64_t start_ts = 0;  // Unix seconds UTC
  std::string sport_type;
  std::string name;
};

struct UploadState {
  int64_t upload_id = 0;
  int64_t activity_id = 0;  // set once Strava has made the activity
  std::string error;        // set when Strava rejected the file
  int64_t duplicate_of = 0; // the existing activity, for "duplicate of ..." errors
};

enum class Status { kOk, kRateLimited, kUnauthorized, kFailed };

bool parse_activities(const std::string& body, std::vector<Activity>& out);
bool parse_upload(const std::string& body, UploadState& out);
// The activity id in Strava's "... duplicate of <a href='/activities/123'>" text; 0 if none.
int64_t duplicate_activity_id(const std::string& error);
// The activity starting within kMatchToleranceS of `start_ts`, or nullptr.
const Activity* find_match(const std::vector<Activity>& acts, int64_t start_ts);
// multipart/form-data body with `fields` and one file part named "file".
std::string multipart_body(const std::string& boundary,
                           const std::vector<std::pair<std::string, std::string>>& fields,
                           const std::string& filename, const std::string& bytes);

class Client {
 public:
  Client(gc::HttpClient& http, std::string access_token);

  // Activities that started in [after, before], oldest first, all pages.
  Status list(int64_t after, int64_t before, std::vector<Activity>& out, std::string& err);
  Status upload(const std::filesystem::path& fit, const std::string& external_id, UploadState& out,
                std::string& err);
  Status upload_status(int64_t upload_id, UploadState& out, std::string& err);
  // Empty `name` leaves the title alone.
  Status update(int64_t activity_id, const std::string& sport_type, const std::string& name,
                std::string& err);

 private:
  Status call(const std::string& method, const std::string& path, const std::string& content_type,
              const std::string& body, std::string& resp, std::string& err);

  gc::HttpClient& http_;
  std::string token_;
};

}  // namespace strava
