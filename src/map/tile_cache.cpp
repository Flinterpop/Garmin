#include "map/tile_cache.h"

#include <algorithm>

#include "gc/http_client.h"
#include "util/assert.h"
#include "util/file_util.h"
#include "util/time_util.h"

namespace map {

namespace {
constexpr wchar_t kUserAgent[] = L"GarminViewer/0.1 (personal desktop use; Win32; contact via app)";
constexpr char kTileUrl[] = "https://tile.openstreetmap.org/";
constexpr size_t kMaxTileBytes = 2u * 1024u * 1024u;
constexpr uint8_t kPngMagic[4] = {0x89, 'P', 'N', 'G'};

bool looks_like_png(const std::vector<uint8_t>& b) {
  return b.size() > 8 && std::equal(kPngMagic, kPngMagic + 4, b.begin());
}
}  // namespace

TileCache::~TileCache() { stop(); }

bool TileCache::start(const std::filesystem::path& disk_dir, HWND notify_hwnd, UINT notify_msg) {
  G_ASSERT(!worker_.joinable());
  G_ASSERT(notify_hwnd != nullptr);
  dir_ = disk_dir;
  hwnd_ = notify_hwnd;
  msg_ = notify_msg;
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  G_REQUIRE_RET(std::filesystem::is_directory(dir_, ec), false);
  stop_ = false;
  worker_ = std::thread([this] { worker_main(); });
  return true;
}

void TileCache::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::filesystem::path TileCache::path_for(const TileKey& k) const {
  return dir_ / std::to_wstring(k.z) / std::to_wstring(k.x) / (std::to_wstring(k.y) + L".png");
}

bool TileCache::read_disk(const TileKey& k, std::vector<uint8_t>& png) const {
  const std::filesystem::path p = path_for(k);
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return false;
  return gutil::read_file(p, png) && looks_like_png(png);
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
  const auto failed = failed_at_.find(key);
  if (failed != failed_at_.end() &&
      gutil::now_unix() - failed->second < kFailureRetrySeconds) {
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
  // Bounded by stop_: each iteration handles one tile or waits.
  for (;;) {
    TileKey key;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      key = queue_.front();
      queue_.pop_front();
      in_flight_.insert(key);
    }

    const std::string url = std::string(kTileUrl) + std::to_string(key.z) + "/" +
                            std::to_string(key.x) + "/" + std::to_string(key.y) + ".png";
    gc::HttpResponse resp;
    std::string err;
    const bool ok = http.get(url, {}, resp, err) && resp.status == 200 &&
                    resp.body.size() <= kMaxTileBytes;
    std::vector<uint8_t> png;
    if (ok) png.assign(resp.body.begin(), resp.body.end());
    const bool valid = ok && looks_like_png(png);
    if (valid) {
      const std::filesystem::path p = path_for(key);
      gutil::write_file(p, png.data(), png.size());
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      in_flight_.erase(key);
      if (valid) {
        remember(key, png);
        failed_at_.erase(key);
      } else {
        failed_at_[key] = gutil::now_unix();
      }
    }
    if (hwnd_ != nullptr) PostMessageW(hwnd_, msg_, 0, 0);
  }
}

}  // namespace map
