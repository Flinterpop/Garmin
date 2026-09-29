#include "map/tile_cache.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "gc/http_client.h"
#include "map/tile_provider.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace map {

namespace {
constexpr wchar_t kUserAgent[] = L"GarminViewer/0.1 (personal desktop use; Win32; contact via app)";
constexpr char kGoogleSessionUrl[] = "https://tile.googleapis.com/v1/createSession?key=";
constexpr size_t kMaxTileBytes = 2u * 1024u * 1024u;

// Google Map Tiles API session request body for one map type.
std::string google_session_body(const std::string& map_type) {
  nlohmann::json j = {{"mapType", map_type}, {"language", "en-US"}, {"region", "US"}};
  if (map_type == "terrain") j["layerTypes"] = {"layerRoadmap"};  // required for terrain
  return j.dump();
}
}  // namespace

int64_t retry_delay_s(int failures) {
  G_ASSERT(failures >= 0);
  int64_t d = 5;
  for (int i = 1; i < failures && i < 8; ++i) d *= 4;
  return std::min(d, kFailureRetrySeconds);
}

TileCache::~TileCache() { stop(); }

bool TileCache::start(const std::filesystem::path& disk_dir, HWND notify_hwnd, UINT notify_msg,
                      std::map<std::string, std::string> keys) {
  G_ASSERT(workers_.empty());
  G_ASSERT(notify_hwnd != nullptr);
  dir_ = disk_dir;
  hwnd_ = notify_hwnd;
  msg_ = notify_msg;
  keys_ = std::move(keys);
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  G_REQUIRE_RET(std::filesystem::is_directory(dir_, ec), false);
  stop_ = false;
  for (size_t i = 0; i < kWorkers; ++i) workers_.emplace_back([this] { worker_main(); });
  G_ASSERT(workers_.size() == kWorkers);
  return true;
}

void TileCache::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (size_t i = 0; i < workers_.size() && i < kWorkers; ++i) {
    if (workers_[i].joinable()) workers_[i].join();
  }
  workers_.clear();
}

std::filesystem::path TileCache::path_for(const TileKey& k) const {
  const std::vector<TileProvider>& t = tile_providers();
  G_ASSERT(k.layer >= 0 && static_cast<size_t>(k.layer) < t.size());
  const std::wstring zx = std::to_wstring(k.z) + L"\\" + std::to_wstring(k.x);
  // OSM keeps the pre-provider layout so the existing cache stays valid.
  if (k.layer == 0) return dir_ / zx / (std::to_wstring(k.y) + L".png");
  const std::string& id = t[static_cast<size_t>(k.layer)].id;
  return dir_ / std::wstring(id.begin(), id.end()) / zx / (std::to_wstring(k.y) + L".tile");
}

bool TileCache::read_disk(const TileKey& k, std::vector<uint8_t>& png) const {
  if (!tile_providers()[static_cast<size_t>(k.layer)].disk_cache) return false;
  const std::filesystem::path p = path_for(k);
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return false;
  return gutil::read_file(p, png) && looks_like_image(png);
}

std::string TileCache::key_named(const std::string& name) const {
  std::lock_guard<std::mutex> lock(cred_mu_);
  const auto it = keys_.find(name);
  return it == keys_.end() ? std::string() : it->second;
}

std::string TileCache::key_for(const TileKey& k) const {
  const std::string& name = tile_providers()[static_cast<size_t>(k.layer)].key_name;
  return name.empty() ? std::string() : key_named(name);
}

void TileCache::set_keys(std::map<std::string, std::string> keys) {
  {
    std::lock_guard<std::mutex> lock(cred_mu_);
    keys_ = std::move(keys);
    google_sessions_.clear();
  }
  std::lock_guard<std::mutex> lock(mu_);
  failed_.clear();
  G_ASSERT(failed_.empty());
}

// Worker thread. Google tiles need a session token per map type; it lasts
// about two weeks, far longer than a viewer session.
bool TileCache::google_session(gc::HttpClient& http, const std::string& map_type,
                               std::string& out) {
  G_ASSERT(!map_type.empty());
  {
    std::lock_guard<std::mutex> lock(cred_mu_);
    const auto it = google_sessions_.find(map_type);
    if (it != google_sessions_.end()) {
      out = it->second;
      return true;
    }
  }
  // Both workers may create a session at once; either token is valid.
  const std::string key = key_named("google");
  G_REQUIRE_RET(!key.empty(), false);
  gc::HttpResponse r;
  std::string err;
  const bool ok = http.request("POST", kGoogleSessionUrl + key,
                               {{"Content-Type", "application/json"}},
                               google_session_body(map_type), r, err);
  if (!ok || r.status != 200) return false;
  const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, false);
  if (!j.is_object() || !j.contains("session") || !j["session"].is_string()) return false;
  out = j["session"].get<std::string>();
  std::lock_guard<std::mutex> lock(cred_mu_);
  google_sessions_[map_type] = out;
  return true;
}

// Worker thread: downloads one tile. False on any failure (the caller backs off).
bool TileCache::fetch(gc::HttpClient& http, const TileKey& key, std::vector<uint8_t>& img) {
  const TileProvider& p = tile_providers()[static_cast<size_t>(key.layer)];
  const std::string api_key = key_for(key);
  if (!p.key_name.empty() && api_key.empty()) return false;
  std::string session;
  if (p.session == SessionKind::kGoogle && !google_session(http, p.map_type, session)) return false;
  gc::HttpResponse resp;
  std::string err;
  const std::string url = tile_url(p, key.z, key.x, key.y, api_key, session);
  if (!http.get(url, {}, resp, err)) return false;
  // A rejected Google session is re-created on the next attempt.
  if (p.session == SessionKind::kGoogle && (resp.status == 401 || resp.status == 403)) {
    std::lock_guard<std::mutex> lock(cred_mu_);
    google_sessions_.erase(p.map_type);
  }
  if (resp.status != 200 || resp.body.size() > kMaxTileBytes) return false;
  img.assign(resp.body.begin(), resp.body.end());
  return looks_like_image(img);
}

void TileCache::remember(const TileKey& k, const std::vector<uint8_t>& png) {
  if (memory_.size() >= kMaxMemoryTiles) {
    // Simple bounded eviction: drop the first half of the map.
    auto it = memory_.begin();
    for (size_t i = 0; i < kMaxMemoryTiles / 2 && it != memory_.end(); ++i) it = memory_.erase(it);
  }
  memory_[k] = png;
}

bool TileCache::get(const TileKey& key, std::vector<uint8_t>& png) {
  G_ASSERT(key.z >= 0 && key.z <= 30);
  G_REQUIRE_RET(key.layer >= 0 && static_cast<size_t>(key.layer) < tile_providers().size(), false);
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = memory_.find(key);
  if (it != memory_.end()) {
    png = it->second;
    return true;
  }
  if (read_disk(key, png)) {
    remember(key, png);
    return true;
  }
  const auto failed = failed_.find(key);
  if (failed != failed_.end() &&
      gutil::now_unix() - failed->second.at < retry_delay_s(failed->second.count)) {
    return false;
  }
  if (in_flight_.count(key) == 0 &&
      std::find(queue_.begin(), queue_.end(), key) == queue_.end() && queue_.size() < kMaxQueue) {
    queue_.push_back(key);
    cv_.notify_one();
  }
  return false;
}

void TileCache::clear_queue() {
  std::lock_guard<std::mutex> lock(mu_);
  queue_.clear();
}

void TileCache::worker_main() {
  gc::HttpClient http(kUserAgent);
  G_ASSERT(http.ok());
  // Each iteration handles one tile or waits; stop_ ends the loop, the
  // iteration cap keeps it formally bounded.
  for (uint64_t iteration = 0; iteration < kMaxWorkerIterations; ++iteration) {
    TileKey key;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      key = queue_.front();
      queue_.pop_front();
      in_flight_.insert(key);
    }

    std::vector<uint8_t> png;
    const bool valid = fetch(http, key, png);
    if (valid && tile_providers()[static_cast<size_t>(key.layer)].disk_cache) {
      const std::filesystem::path p = path_for(key);
      if (!gutil::write_file(p, png.data(), png.size())) {
        // Not fatal: the tile is still served from memory this session.
      }
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      in_flight_.erase(key);
      if (valid) {
        remember(key, png);
        failed_.erase(key);
      } else {
        if (failed_.size() >= kMaxMemoryTiles) failed_.clear();  // bounded; worst case a retry
        TileFailure& f = failed_[key];
        f.at = gutil::now_unix();
        f.count = std::min(f.count + 1, 16);
      }
    }
    if (hwnd_ != nullptr) PostMessageW(hwnd_, msg_, 0, 0);
  }
}

}  // namespace map
