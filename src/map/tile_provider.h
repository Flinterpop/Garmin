// Raster tile providers for the map: URL templates, zoom limits, attribution
// and the terms each one comes with. Keys are never compiled in; they come
// from the sidecar gview.ini (see map_settings.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace map {

enum class SessionKind { kNone, kGoogle };

struct TileProvider {
  std::string id;           // stable: cache folder name and gview.ini value
  std::string name;         // menu text
  std::string url;          // placeholders: {z} {x} {y} {s} {key} {session}
  std::string subdomains;   // one char per subdomain for {s}, e.g. "abc"
  int max_zoom = 18;
  std::string attribution;
  std::string key_name;     // gview.ini [keys] entry; empty = no key needed
  bool overlay = false;     // transparent layer drawn over the base map
  bool hidden = false;      // data, not a map (terrain heights): never offered as a layer
  bool disk_cache = true;   // false where the terms forbid storing tiles
  SessionKind session = SessionKind::kNone;
  std::string map_type;     // Google createSession mapType
};

constexpr size_t kMaxProviders = 32;
constexpr size_t kMaxUrlLength = 1024;

// The built-in table; index 0 is OpenStreetMap, the default.
const std::vector<TileProvider>& tile_providers();

// Index of the provider with `id`, or SIZE_MAX.
size_t provider_index(const std::string& id);

// Terrain heights for the 3D view: AWS Terrain Tiles in Terrarium encoding.
constexpr char kTerrainProviderId[] = "terrain_terrarium";
constexpr int kTerrainMaxZoom = 15;

// Fills the URL template. {s} rotates over the subdomains by tile.
std::string tile_url(const TileProvider& p, int z, int x, int y, const std::string& key,
                     const std::string& session);

// PNG or JPEG signature; everything else (HTML error pages, JSON) is rejected.
bool looks_like_image(const std::vector<uint8_t>& bytes);

}  // namespace map
