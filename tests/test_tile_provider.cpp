#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>

#include "map/map_settings.h"
#include "map/tile_cache.h"
#include "map/tile_provider.h"

using map::provider_index;
using map::tile_providers;

TEST_CASE("provider table is well formed", "[map]") {
  const auto& t = tile_providers();
  REQUIRE(!t.empty());
  CHECK(t.front().id == "osm");
  std::set<std::string> ids;
  bool overlays_started = false;
  for (const map::TileProvider& p : t) {
    CHECK(ids.insert(p.id).second);  // unique: ids are cache folders
    CHECK(!p.attribution.empty());
    CHECK(p.max_zoom >= 10);
    CHECK(p.url.rfind("https://", 0) == 0);
    CHECK((p.url.find("{key}") != std::string::npos) == !p.key_name.empty());
    if (p.hidden) continue;  // terrain data, not a layer: never in the menu
    if (p.overlay) overlays_started = true;
    CHECK((p.overlay || !overlays_started));  // overlays last: the menu radio range relies on it
  }
}

TEST_CASE("providers whose terms forbid storing tiles are memory-only", "[map]") {
  for (const char* id : {"google_road", "google_satellite", "google_terrain", "azure_road",
                         "azure_imagery"}) {
    const size_t i = provider_index(id);
    REQUIRE(i != SIZE_MAX);
    CHECK_FALSE(tile_providers()[i].disk_cache);
  }
  CHECK(tile_providers()[provider_index("osm")].disk_cache);
}

TEST_CASE("tile_url fills the templates", "[map]") {
  const auto& t = tile_providers();
  CHECK(map::tile_url(t[provider_index("osm")], 12, 1100, 1400, "", "") ==
        "https://tile.openstreetmap.org/12/1100/1400.png");
  // Esri puts y before x.
  CHECK(map::tile_url(t[provider_index("esri_imagery")], 5, 7, 9, "", "") ==
        "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/5/9/7");
  // Subdomain rotates with the tile: (x + y) % 3 -> a, b, c.
  const auto& topo = t[provider_index("opentopomap")];
  CHECK(map::tile_url(topo, 3, 0, 0, "", "") == "https://a.tile.opentopomap.org/3/0/0.png");
  CHECK(map::tile_url(topo, 3, 1, 0, "", "") == "https://b.tile.opentopomap.org/3/1/0.png");
  CHECK(map::tile_url(topo, 3, 1, 1, "", "") == "https://c.tile.opentopomap.org/3/1/1.png");
  CHECK(map::tile_url(t[provider_index("google_satellite")], 4, 2, 3, "KEY", "SESS") ==
        "https://tile.googleapis.com/v1/2dtiles/4/2/3?session=SESS&key=KEY");
  const std::string az = map::tile_url(t[provider_index("azure_road")], 4, 2, 3, "AZ", "");
  CHECK(az.find("zoom=4&x=2&y=3") != std::string::npos);
  CHECK(az.find("subscription-key=AZ") != std::string::npos);
  CHECK(az.find("tilesetId=microsoft.base.road") != std::string::npos);
}

TEST_CASE("failed tiles back off 5, 20, 80, then 120 s", "[map]") {
  CHECK(map::retry_delay_s(0) == 5);
  CHECK(map::retry_delay_s(1) == 5);  // first 404 from a render-on-demand server
  CHECK(map::retry_delay_s(2) == 20);
  CHECK(map::retry_delay_s(3) == 80);
  CHECK(map::retry_delay_s(4) == 120);
  CHECK(map::retry_delay_s(16) == 120);
}

TEST_CASE("looks_like_image accepts PNG and JPEG only", "[map]") {
  CHECK(map::looks_like_image({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0}));
  CHECK(map::looks_like_image({0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I'}));
  CHECK_FALSE(map::looks_like_image({'<', 'h', 't', 'm', 'l', '>', ' ', ' ', ' '}));
  CHECK_FALSE(map::looks_like_image({0x89, 'P', 'N'}));
}

namespace {

std::filesystem::path write_ini(const char* name, const char* text) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
  std::ofstream(p) << text;
  return p;
}

}  // namespace

TEST_CASE("map settings: keys gate providers, base and overlays load", "[map]") {
  const auto ini = write_ini("gview_test_settings.ini",
                             "[keys]\ngoogle =  gk \n[map]\nbase = google_satellite\n"
                             "overlays = wmt_hiking, bogus ,osm\n");
  const map::MapSettings s = map::load_map_settings(ini);
  CHECK(s.keys.at("google") == "gk");  // trimmed
  CHECK(s.keys.count("azure_maps") == 0);
  CHECK(s.base == provider_index("google_satellite"));
  REQUIRE(s.overlays.size() == 1);  // unknown ids and non-overlays are dropped
  CHECK(s.overlays[0] == provider_index("wmt_hiking"));
  CHECK(map::provider_available(s, provider_index("google_road")));
  CHECK_FALSE(map::provider_available(s, provider_index("azure_road")));
  std::filesystem::remove(ini);
}

TEST_CASE("map settings: a keyed base without its key falls back to OSM", "[map]") {
  const auto ini = write_ini("gview_test_nokey.ini", "[map]\nbase = azure_imagery\n");
  CHECK(map::load_map_settings(ini).base == 0);
  std::filesystem::remove(ini);
  CHECK(map::load_map_settings(std::filesystem::path()).base == 0);  // no file at all
}

TEST_CASE("api keys: names, validation, save and remove", "[map]") {
  const auto names = map::key_names();
  CHECK(names == std::vector<std::string>{"thunderforest", "google", "azure_maps"});

  CHECK(map::valid_api_key("AIzaSyTestKey_0123-abc.~"));
  CHECK_FALSE(map::valid_api_key("short"));
  CHECK_FALSE(map::valid_api_key("has space in it"));
  CHECK_FALSE(map::valid_api_key("amp&injected=1"));  // would break the URL query
  CHECK_FALSE(map::valid_api_key("semi;colon-key"));  // would break the ini line
  CHECK_FALSE(map::valid_api_key(std::string(map::kMaxKeyLength + 1, 'a')));

  const auto ini = write_ini("gview_test_keys.ini",
                             "[keys]\nthunderforest = oldtfkey1\n[map]\nbase = tf_cycle\n");
  REQUIRE(map::save_map_keys(ini, {{"google", "googlekey1"}, {"azure_maps", "azurekey1"}}));
  map::MapSettings s = map::load_map_settings(ini);
  CHECK(s.keys.at("google") == "googlekey1");
  CHECK(s.keys.at("azure_maps") == "azurekey1");
  CHECK(s.keys.count("thunderforest") == 0);  // missing from the save = removed
  CHECK(s.base == 0);                          // tf_cycle lost its key: back to OSM
  CHECK_FALSE(map::save_map_keys(ini, {{"bogus", "whateverkey"}}));
  CHECK_FALSE(map::save_map_keys(ini, {{"google", "bad key"}}));
  CHECK(map::load_map_settings(ini).keys.at("google") == "googlekey1");  // rejected saves change nothing
  std::filesystem::remove(ini);
}

TEST_CASE("map settings: choose_layer, next_base and save round-trip", "[map]") {
  map::MapSettings s;
  CHECK_FALSE(map::choose_layer(s, provider_index("google_road")));  // no key
  CHECK(map::choose_layer(s, provider_index("esri_imagery")));
  CHECK(s.base == provider_index("esri_imagery"));
  CHECK(map::choose_layer(s, provider_index("wmt_cycling")));  // overlay on
  CHECK(s.overlays.size() == 1);
  CHECK(map::choose_layer(s, provider_index("wmt_cycling")));  // and off again
  CHECK(s.overlays.empty());
  // next_base skips keyed providers without keys and the overlays, and wraps.
  s.base = provider_index("esri_topo");
  CHECK(map::next_base(s) == 0);
  s.keys["thunderforest"] = "tf";
  CHECK(map::next_base(s) == provider_index("tf_cycle"));

  const auto ini = write_ini("gview_test_save.ini", "[keys]\nthunderforest = tf\n");
  s.base = provider_index("tf_outdoors");
  s.overlays = {provider_index("wmt_hiking")};
  REQUIRE(map::save_map_layers(ini, s));
  const map::MapSettings back = map::load_map_settings(ini);
  CHECK(back.base == s.base);
  CHECK(back.overlays == s.overlays);
  CHECK(back.keys.at("thunderforest") == "tf");  // [keys] untouched by the save
  std::filesystem::remove(ini);
}
