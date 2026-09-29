// Map settings from the sidecar gview.ini next to gview.exe:
//
//   [keys]                 API keys; never commit this file (it is gitignored)
//   google = ...           Google Map Tiles API
//   azure_maps = ...       Azure Maps (successor to Bing Maps)
//   thunderforest = ...    Thunderforest OpenCycleMap / Outdoors
//   [map]
//   base = osm             provider id, see tile_provider.cpp
//   overlays = wmt_hiking  comma-separated overlay ids, may be empty
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace map {

struct MapSettings {
  std::map<std::string, std::string> keys;  // key_name -> key
  size_t base = 0;                          // index into tile_providers()
  std::vector<size_t> overlays;             // indices of enabled overlays
};

constexpr size_t kMaxKeyLength = 512;
constexpr size_t kMaxOverlays = 8;

// Missing file or entries give defaults (OSM, no overlays, no keys).
MapSettings load_map_settings(const std::filesystem::path& ini);

// Writes only the [map] section; [keys] is left as the user wrote it.
bool save_map_layers(const std::filesystem::path& ini, const MapSettings& s);

// True when `provider` needs no key or its key is present.
bool provider_available(const MapSettings& s, size_t provider);

// Menu action on `provider`: an overlay toggles, an available base becomes
// the base. Returns false when nothing changed (e.g. a base without its key).
bool choose_layer(MapSettings& s, size_t provider);

// The next available base provider after the current one, wrapping around.
size_t next_base(const MapSettings& s);

// gview.ini beside the running executable.
std::filesystem::path sidecar_ini_path();

}  // namespace map
