#include "layer_menu.h"

#include <string>

#include "map/tile_provider.h"
#include "plot/plot_widget.h"  // widen()
#include "util/assert.h"

namespace gview {

namespace {

UINT_PTR id_of(size_t provider) { return static_cast<UINT_PTR>(kIdmLayerFirst) + provider; }

void append_item(HMENU m, const map::MapSettings& s, size_t i) {
  const map::TileProvider& p = map::tile_providers()[i];
  std::string label = p.name;
  const bool available = map::provider_available(s, i);
  if (!available) label += "   (add " + p.key_name + " key to gview.ini)";
  const UINT flags = MF_STRING | (available ? MF_ENABLED : MF_GRAYED);
  const BOOL ok = AppendMenuW(m, flags, id_of(i), plot::widen(label).c_str());
  G_ASSERT(ok);
}

}  // namespace

HMENU build_layer_menu(const map::MapSettings& s) {
  HMENU m = CreatePopupMenu();
  G_REQUIRE_RET(m != nullptr, nullptr);
  const std::vector<map::TileProvider>& t = map::tile_providers();
  for (size_t i = 0; i < t.size() && i < map::kMaxProviders; ++i) {
    if (!t[i].overlay && !t[i].hidden) append_item(m, s, i);
  }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  for (size_t i = 0; i < t.size() && i < map::kMaxProviders; ++i) {
    if (t[i].overlay && !t[i].hidden) append_item(m, s, i);
  }
  return m;
}

void check_layer_menu(HMENU menu, const map::MapSettings& s) {
  G_REQUIRE_VOID(menu != nullptr);
  const std::vector<map::TileProvider>& t = map::tile_providers();
  for (size_t i = 0; i < t.size() && i < map::kMaxProviders; ++i) {
    bool on = i == s.base;
    if (t[i].overlay) {
      on = false;
      for (size_t k = 0; k < s.overlays.size() && k < map::kMaxOverlays; ++k) on |= s.overlays[k] == i;
    }
    // Radio bullet for bases, tick for overlays; MFT_RADIOCHECK only changes the glyph.
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_FTYPE | MIIM_STATE;
    if (!GetMenuItemInfoW(menu, static_cast<UINT>(id_of(i)), FALSE, &mii)) continue;
    mii.fType = t[i].overlay ? (mii.fType & ~MFT_RADIOCHECK) : (mii.fType | MFT_RADIOCHECK);
    mii.fState = (mii.fState & ~MFS_CHECKED) | (on ? MFS_CHECKED : 0);
    const BOOL set = SetMenuItemInfoW(menu, static_cast<UINT>(id_of(i)), FALSE, &mii);
    G_ASSERT(set);
  }
}

bool layer_command(int id, size_t& provider) {
  if (id < kIdmLayerFirst) return false;
  const size_t i = static_cast<size_t>(id - kIdmLayerFirst);
  if (i >= map::tile_providers().size()) return false;
  provider = i;
  return true;
}

}  // namespace gview
