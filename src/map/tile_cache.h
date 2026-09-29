// Raster map tiles from the providers in tile_provider.h: memory cache ->
// disk cache (where the provider's terms allow it) -> download on a worker
// thread. The window is notified (PostMessage) when a tile arrives so it can
// repaint. Follows the OSM tile usage policy for every provider: identifying
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
#include <string>
#include <thread>
#include <vector>

namespace gc {
class HttpClient;
}

namespace map {

struct TileKey {
  int z = 0;
  int x = 0;
  int y = 0;
  int layer = 0;  // index into tile_providers()
  bool operator<(const TileKey& o) const {
    if (layer != o.layer) return layer < o.layer;
    if (z != o.z) return z < o.z;
    if (x != o.x) return x < o.x;
    return y < o.y;
  }
  bool operator==(const TileKey& o) const {
    return layer == o.layer && z == o.z && x == o.x && y == o.y;
  }
};

constexpr size_t kMaxMemoryTiles = 1024;   // encoded PNGs, ~20 KB each
constexpr size_t kMaxQueue = 128;
constexpr int64_t kFailureRetrySeconds = 120;  // longest back-off after repeated failures
constexpr uint64_t kMaxWorkerIterations = 1ull << 40;  // effectively unbounded, but bounded
constexpr size_t kWorkers = 2;  // the OSM tile policy allows at most two connections

// Seconds to wait before re-requesting a tile that failed `failures` times:
// 5, 20, 80, then 120. Short at first because render-on-demand servers
// (CyclOSM) answer 404 until the tile has been drawn.
int64_t retry_delay_s(int failures);

struct TileFailure {
  int64_t at = 0;
  int count = 0;
};

class TileCache {
 public:
  TileCache() = default;
  ~TileCache();
  TileCache(const TileCache&) = delete;
  TileCache& operator=(const TileCache&) = delete;

  // `keys` maps a provider's key_name to its API key (from gview.ini).
  bool start(const std::filesystem::path& disk_dir, HWND notify_hwnd, UINT notify_msg,
             std::map<std::string, std::string> keys);
  void stop();

  // True and `png` filled when the tile is available now; otherwise the
  // tile is queued for download (unless it failed recently) and false is
  // returned. Call from the UI thread.
  bool get(const TileKey& key, std::vector<uint8_t>& png);

  // Drops queued (not in-flight) requests; call before requesting the
  // tiles of a new view so we never fetch what scrolled away.
  void clear_queue();

  // Replaces the API keys (after the user edits them). Drops Google
  // sessions minted with the old key and forgets recent failures, so tiles
  // that failed for want of a key are fetched again at once.
  void set_keys(std::map<std::string, std::string> keys);

 private:
  void worker_main();
  bool fetch(gc::HttpClient& http, const TileKey& key, std::vector<uint8_t>& img);
  bool google_session(gc::HttpClient& http, const std::string& map_type, std::string& out);
  std::string key_for(const TileKey& k) const;
  std::filesystem::path path_for(const TileKey& k) const;
  bool read_disk(const TileKey& k, std::vector<uint8_t>& png) const;
  void remember(const TileKey& k, const std::vector<uint8_t>& png);  // needs mu_

  std::filesystem::path dir_;
  HWND hwnd_ = nullptr;
  UINT msg_ = 0;
  std::string key_named(const std::string& name) const;  // copy under cred_mu_

  mutable std::mutex cred_mu_;                          // guards keys_ and google_sessions_
  std::map<std::string, std::string> keys_;             // key_name -> API key
  std::map<std::string, std::string> google_sessions_;  // map_type -> token

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::thread> workers_;
  bool stop_ = false;
  std::map<TileKey, std::vector<uint8_t>> memory_;
  std::deque<TileKey> queue_;
  std::set<TileKey> in_flight_;
  std::map<TileKey, TileFailure> failed_;
};

}  // namespace map
