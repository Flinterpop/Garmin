#include "map/tile_provider.h"

#include <algorithm>

#include "util/assert.h"

namespace map {

namespace {

constexpr char kOsmCredit[] = "\xC2\xA9 OpenStreetMap contributors";
constexpr char kGoogleTiles[] =
    "https://tile.googleapis.com/v1/2dtiles/{z}/{x}/{y}?session={session}&key={key}";
constexpr char kAzureTiles[] =
    "https://atlas.microsoft.com/map/tile?api-version=2024-04-01&tileSize=256"
    "&zoom={z}&x={x}&y={y}&subscription-key={key}&tilesetId=";
constexpr char kEsriTiles[] = "https://server.arcgisonline.com/ArcGIS/rest/services/";

TileProvider make(const char* id, const char* name, std::string url, int max_zoom,
                  std::string attribution) {
  TileProvider p;
  p.id = id;
  p.name = name;
  p.url = std::move(url);
  p.max_zoom = max_zoom;
  p.attribution = std::move(attribution);
  return p;
}

TileProvider google(const char* id, const char* name, const char* map_type, int max_zoom) {
  TileProvider p = make(id, name, kGoogleTiles, max_zoom, "Map data \xC2\xA9 Google");
  p.key_name = "google";
  p.disk_cache = false;  // Map Tiles API terms: no storing tiles beyond the session
  p.session = SessionKind::kGoogle;
  p.map_type = map_type;
  return p;
}

TileProvider azure(const char* id, const char* name, const char* tileset) {
  TileProvider p = make(id, name, std::string(kAzureTiles) + tileset, 18,
                        "\xC2\xA9 Microsoft Azure Maps, \xC2\xA9 TomTom");
  p.key_name = "azure_maps";
  p.disk_cache = false;  // conservative: Azure Maps terms restrict caching
  return p;
}

TileProvider keyed(TileProvider p, const char* key_name) {
  p.key_name = key_name;
  return p;
}

TileProvider overlay(TileProvider p) {
  p.overlay = true;
  return p;
}

TileProvider with_subdomains(TileProvider p, const char* subdomains) {
  p.subdomains = subdomains;
  return p;
}

std::vector<TileProvider> build_table() {
  const std::string tf_credit =
      std::string("Maps \xC2\xA9 Thunderforest, data ") + kOsmCredit;
  std::vector<TileProvider> t;
  t.push_back(make("osm", "OpenStreetMap", "https://tile.openstreetmap.org/{z}/{x}/{y}.png", 18,
                   kOsmCredit));
  t.push_back(with_subdomains(
      make("cyclosm", "CyclOSM (cycling)",
           "https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png", 18,
           std::string(kOsmCredit) + ", style CyclOSM"),
      "abc"));
  t.push_back(with_subdomains(
      make("opentopomap", "OpenTopoMap (hiking / topo)",
           "https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png", 17,
           std::string(kOsmCredit) + ", SRTM, style \xC2\xA9 OpenTopoMap (CC-BY-SA)"),
      "abc"));
  t.push_back(make("esri_imagery", "Esri World Imagery",
                   std::string(kEsriTiles) + "World_Imagery/MapServer/tile/{z}/{y}/{x}", 18,
                   "Tiles \xC2\xA9 Esri, Maxar, Earthstar Geographics, GIS User Community"));
  t.push_back(make("esri_topo", "Esri World Topo",
                   std::string(kEsriTiles) + "World_Topo_Map/MapServer/tile/{z}/{y}/{x}", 18,
                   "Tiles \xC2\xA9 Esri and contributors"));
  t.push_back(keyed(make("tf_cycle", "Thunderforest OpenCycleMap",
                         "https://tile.thunderforest.com/cycle/{z}/{x}/{y}.png?apikey={key}", 18,
                         tf_credit),
                    "thunderforest"));
  t.push_back(keyed(make("tf_outdoors", "Thunderforest Outdoors",
                         "https://tile.thunderforest.com/outdoors/{z}/{x}/{y}.png?apikey={key}",
                         18, tf_credit),
                    "thunderforest"));
  t.push_back(google("google_road", "Google Maps", "roadmap", 18));
  t.push_back(google("google_satellite", "Google Satellite", "satellite", 18));
  t.push_back(google("google_terrain", "Google Terrain", "terrain", 15));
  t.push_back(azure("azure_road", "Azure Maps (Bing successor)", "microsoft.base.road"));
  t.push_back(azure("azure_imagery", "Azure Maps Imagery", "microsoft.imagery"));
  t.push_back(overlay(make("wmt_hiking", "Hiking routes (Waymarked Trails)",
                           "https://tile.waymarkedtrails.org/hiking/{z}/{x}/{y}.png", 18,
                           "Hiking routes \xC2\xA9 waymarkedtrails.org")));
  t.push_back(overlay(make("wmt_cycling", "Cycling routes (Waymarked Trails)",
                           "https://tile.waymarkedtrails.org/cycling/{z}/{x}/{y}.png", 18,
                           "Cycling routes \xC2\xA9 waymarkedtrails.org")));
  // Last, and hidden: height data for the 3D view, not a map layer.
  TileProvider terrain = make(kTerrainProviderId, "Terrain heights",
                              "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
                              kTerrainMaxZoom, "Terrain: Mapzen / AWS Terrain Tiles");
  terrain.hidden = true;
  t.push_back(terrain);
  G_ASSERT(t.size() <= kMaxProviders);
  G_ASSERT(t.front().id == "osm");
  return t;
}

// Replaces every `from` in `s` with `to`; templates hold each placeholder at most once or twice.
void replace_all(std::string& s, const std::string& from, const std::string& to) {
  G_ASSERT(!from.empty());
  constexpr int kMaxReplacements = 8;
  size_t pos = 0;
  for (int i = 0; i < kMaxReplacements; ++i) {
    pos = s.find(from, pos);
    if (pos == std::string::npos) return;
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

}  // namespace

const std::vector<TileProvider>& tile_providers() {
  static const std::vector<TileProvider> table = build_table();
  return table;
}

size_t provider_index(const std::string& id) {
  const std::vector<TileProvider>& t = tile_providers();
  for (size_t i = 0; i < t.size() && i < kMaxProviders; ++i) {
    if (t[i].id == id) return i;
  }
  return SIZE_MAX;
}

std::string tile_url(const TileProvider& p, int z, int x, int y, const std::string& key,
                     const std::string& session) {
  G_ASSERT(z >= 0 && x >= 0 && y >= 0);
  G_ASSERT(p.url.size() < kMaxUrlLength);
  std::string u = p.url;
  replace_all(u, "{z}", std::to_string(z));
  replace_all(u, "{x}", std::to_string(x));
  replace_all(u, "{y}", std::to_string(y));
  if (!p.subdomains.empty()) {
    const size_t i = static_cast<size_t>(x + y) % p.subdomains.size();
    replace_all(u, "{s}", std::string(1, p.subdomains[i]));
  }
  replace_all(u, "{key}", key);
  replace_all(u, "{session}", session);
  return u;
}

bool looks_like_image(const std::vector<uint8_t>& b) {
  if (b.size() < 8) return false;
  const bool png = b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
  const bool jpeg = b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
  return png || jpeg;
}

}  // namespace map
