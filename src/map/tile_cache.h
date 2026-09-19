// OpenStreetMap raster tiles: memory cache -> disk cache -> download on a
// worker thread. The window is notified (PostMessage) when a tile arrives
// so it can repaint. Follows the OSM tile usage policy: identifying
// User-Agent, on-disk caching, only tiles that are actually on screen.
#pragma once
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace map {

struct TileKey {
  int z = 0;
  int x = 0;
  int y = 0;
  bool operator<(const TileKey& o) const {
    if (z != o.z) return z < o.z;
    if (x != o.x) return x < o.x;
    return y < o.y;
  }
  bool operator==(const TileKey& o) const { return z == o.z && x == o.x && y == o.y; }
};

constexpr size_t kMaxMemoryTiles = 1024;   // encoded PNGs, ~20 KB each
constexpr size_t kMaxQueue = 128;
constexpr int64_t kFailureRetrySeconds = 120;
constexpr uint64_t kMaxWorkerIterations = 1ull << 40;  // effectively unbounded, but bounded

class TileCache {
 public:
  TileCache() = default;
  ~TileCache();
  TileCache(const TileCache&) = delete;
  TileCache& operator=(const TileCache&) = delete;

  bool start(const std::filesystem::path& disk_dir, HWND notify_hwnd, UINT notify_msg);
  void stop();

  // True and `png` filled when the tile is available now; otherwise the
  // tile is queued for download (unless it failed recently) and false is
  // returned. Call from the UI thread.
  bool get(const TileKey& key, std::vector<uint8_t>& png);

  // Drops queued (not in-flight) requests; call before requesting the
  // tiles of a new view so we never fetch what scrolled away.
  void clear_queue();

 private:
  void worker_main();
  std::filesystem::path path_for(const TileKey& k) const;
  bool read_disk(const TileKey& k, std::vector<uint8_t>& png) const;
  void remember(const TileKey& k, const std::vector<uint8_t>& png);  // needs mu_

  std::filesystem::path dir_;
  HWND hwnd_ = nullptr;
  UINT msg_ = 0;

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
  std::map<TileKey, std::vector<uint8_t>> memory_;
  std::deque<TileKey> queue_;
  std::set<TileKey> in_flight_;
  std::map<TileKey, int64_t> failed_at_;
};

}  // namespace map
