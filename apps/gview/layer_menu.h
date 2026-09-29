// View > Map layer submenu: one radio item per base provider, one check item
// per overlay. Providers whose key is missing from gview.ini are greyed out.
#pragma once
#include <windows.h>

#include "map/map_settings.h"

namespace gview {

constexpr int kIdmLayerFirst = 400;  // + provider index

HMENU build_layer_menu(const map::MapSettings& s);

// Updates the radio/check marks to match `s`. `menu` is the window's menu bar.
void check_layer_menu(HMENU menu, const map::MapSettings& s);

// True and `provider` set when `id` is a Map layer command.
bool layer_command(int id, size_t& provider);

}  // namespace gview
