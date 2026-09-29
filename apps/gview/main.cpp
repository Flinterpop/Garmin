// gview: Win32 + Direct2D viewer over garmin.db. Left: a list (days,
// trend ranges, or activities). Right: the plot. Wheel zooms, drag pans,
// hover shows values, Home fits, 1/2/3 switch views, F5 reloads.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "keys_dialog.h"
#include "layer_menu.h"
#include "map/map_settings.h"
#include "map/map_widget.h"
#include "map/tile_cache.h"
#include "plot/calendar_widget.h"
#include "plot/plot_widget.h"
#include "queries.h"
#include "resource.h"
#include "store/db.h"
#include "util/assert.h"
#include "util/file_util.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"GarminViewMain";
constexpr int kListWidthCss = 300;
constexpr int kListId = 1001;
constexpr int kIdmViewDay = 101;
constexpr int kIdmViewTrends = 102;
constexpr int kIdmViewActivity = 103;
constexpr int kIdmViewHockey = 104;
constexpr int kIdmViewCalendar = 105;
constexpr int kIdmViewMap = 106;
constexpr int kIdmViewSki = 107;
constexpr int kIdmViewCompare = 108;
constexpr int kIdmViewSleep = 109;
constexpr UINT kMsgTileReady = WM_APP + 1;
constexpr int kIdmFit = 201;
constexpr int kIdmZoomIn = 202;
constexpr int kIdmZoomOut = 203;
constexpr int kIdmReload = 301;
constexpr int kIdmExit = 302;
constexpr int kIdmMapKeys = 303;
constexpr double kWheelZoomPerNotch = 0.8;
constexpr double kKeyZoom = 0.7;
constexpr float kKeyPanFrac = 0.1f;
constexpr float kMapShare = 0.42f;      // Activity view: map's share of the plot width
constexpr int kMapMinWidthCss = 320;

enum class Mode { kDay, kTrends, kActivity, kHockey, kCalendar, kMap, kSki, kCompare, kSleep };

struct App {
  HWND hwnd = nullptr;
  HWND list = nullptr;
  HFONT list_font = nullptr;
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1HwndRenderTarget> rt;
  plot::PlotWidget widget;
  plot::CalendarWidget calendar;
  map::MapWidget mapw;
  map::TileCache tiles;
  map::MapSettings map_settings;  // from gview.ini beside the exe (keys + chosen layers)
  std::filesystem::path ini_path;
  ComPtr<IWICImagingFactory> wic;
  store::Db db;
  std::filesystem::path db_path;
  Mode mode = Mode::kDay;
  std::vector<gview::DayEntry> days;
  std::vector<gview::ActivityEntry> activities;
  std::vector<gview::TrendRange> ranges;
  std::vector<gview::GameEntry> games;
  std::vector<gview::MonthEntry> months;
  std::vector<gview::ActivityEntry> gps_activities;
  std::vector<gview::SkiEntry> ski_days;
  std::vector<gview::NightEntry> nights;
  double hockey_hr_max = 190.0;
  float scale = 1.0f;
  bool activity_map = false;  // Activity view: selected activity has GPS, map shown beside charts
  bool dragging = false;
  bool drag_map = false;      // the current drag pans the map, not the chart
  int drag_last_x = 0;
  int drag_last_y = 0;
  bool tracking_mouse = false;
};

App* g_app = nullptr;

float dpi_scale(HWND hwnd) { return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f; }

RECT plot_rect(const App& a) {
  RECT rc{};
  GetClientRect(a.hwnd, &rc);
  rc.left += static_cast<LONG>(kListWidthCss * a.scale);
  return rc;
}

// Activity view with a GPS track: charts left, map right.
bool split_view(const App& a) { return a.mode == Mode::kActivity && a.activity_map; }

LONG split_x(const App& a) {
  const RECT r = plot_rect(a);
  G_ASSERT(r.right >= r.left);
  const LONG map_w = static_cast<LONG>(static_cast<float>(r.right - r.left) * kMapShare);
  return r.right - std::max(map_w, static_cast<LONG>(kMapMinWidthCss * a.scale));
}

RECT chart_rect(const App& a) {
  RECT r = plot_rect(a);
  if (split_view(a)) r.right = std::max(r.left, split_x(a));
  return r;
}

RECT map_rect(const App& a) {
  RECT r = plot_rect(a);
  if (split_view(a)) r.left = std::max(r.left, split_x(a));
  return r;
}

bool in_rect(const RECT& r, int x, int y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

D2D1_RECT_F to_rectf(const RECT& r) {
  return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right), static_cast<float>(r.bottom));
}

// Gives each widget its share of the plot area; call whenever the split can change.
void apply_rects(App& a) {
  G_ASSERT(a.hwnd != nullptr);
  a.widget.set_rect(to_rectf(chart_rect(a)));
  a.calendar.set_rect(to_rectf(plot_rect(a)));
  a.mapw.set_rect(to_rectf(map_rect(a)));
}

void set_title(App& a, const std::string& sub) {
  std::wstring t = L"Garmin viewer";
  if (!sub.empty()) t += L"  -  " + plot::widen(sub);
  SetWindowTextW(a.hwnd, t.c_str());
}

bool create_render_target(App& a) {
  a.rt.Reset();
  RECT rc{};
  GetClientRect(a.hwnd, &rc);
  const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
                                       static_cast<UINT32>(rc.bottom - rc.top));
  // 96 DPI so that Direct2D units equal device pixels; we scale ourselves.
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
  props.dpiX = 96.0f;
  props.dpiY = 96.0f;
  const HRESULT hr = a.d2d->CreateHwndRenderTarget(
      props, D2D1::HwndRenderTargetProperties(a.hwnd, size), &a.rt);
  return SUCCEEDED(hr);
}

void layout(App& a) {
  RECT rc{};
  GetClientRect(a.hwnd, &rc);
  const int list_w = static_cast<int>(kListWidthCss * a.scale);
  MoveWindow(a.list, 0, 0, list_w, rc.bottom - rc.top, TRUE);
  apply_rects(a);
  if (a.rt) {
    a.rt->Resize(D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
                             static_cast<UINT32>(rc.bottom - rc.top)));
  }
}

void apply_list_font(App& a) {
  if (a.list_font != nullptr) DeleteObject(a.list_font);
  a.list_font = CreateFontW(-static_cast<int>(13 * a.scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  SendMessageW(a.list, WM_SETFONT, reinterpret_cast<WPARAM>(a.list_font), TRUE);
}

void fill_list(App& a) {
  G_ASSERT(a.list != nullptr);
  SendMessageW(a.list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(a.list, LB_RESETCONTENT, 0, 0);
  auto add = [&](const std::string& s) {
    const std::wstring w = plot::widen(s);
    SendMessageW(a.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  };
  switch (a.mode) {
    case Mode::kDay:
      for (const auto& d : a.days) add(d.label);
      break;
    case Mode::kTrends:
      for (const auto& r : a.ranges) add(r.label);
      break;
    case Mode::kActivity:
      for (const auto& e : a.activities) add(e.label);
      break;
    case Mode::kHockey:
      for (const auto& g : a.games) add(g.label);
      break;
    case Mode::kCalendar:
      for (const auto& m : a.months) add(m.label);
      break;
    case Mode::kMap:
      for (const auto& e : a.gps_activities) add(e.label);
      break;
    case Mode::kSki:
      for (const auto& e : a.ski_days) add(e.label);
      break;
    case Mode::kCompare:
      for (const auto& e : a.activities) add(e.label);
      break;
    case Mode::kSleep:
      for (const auto& e : a.nights) add(e.label);
      break;
  }
  SendMessageW(a.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(a.list, nullptr, TRUE);
}

// Extended-selection list: plain click selects one, Ctrl-click adds.
// Returns the selected indices in list order.
std::vector<int> selected_indices(const App& a) {
  std::vector<int> out;
  const int n = static_cast<int>(SendMessageW(a.list, LB_GETSELCOUNT, 0, 0));
  if (n <= 0) return out;
  out.resize(static_cast<size_t>(std::min(n, 64)));
  const int got = static_cast<int>(
      SendMessageW(a.list, LB_GETSELITEMS, static_cast<WPARAM>(out.size()),
                   reinterpret_cast<LPARAM>(out.data())));
  out.resize(static_cast<size_t>(std::max(got, 0)));
  return out;
}

void select_only(App& a, int index) {
  SendMessageW(a.list, LB_SETSEL, FALSE, static_cast<LPARAM>(-1));
  if (index >= 0) {
    SendMessageW(a.list, LB_SETSEL, TRUE, static_cast<LPARAM>(index));
    SendMessageW(a.list, LB_SETCARETINDEX, static_cast<WPARAM>(index), FALSE);
  }
}

// Builds the figure (or calendar / map content) for the current list
// selection. Returns false when the mode paints something other than a Figure.
bool figure_for_selection(App& a, const std::vector<int>& picked, plot::Figure& fig,
                          std::string& title) {
  const int sel = picked.empty() ? -1 : picked.front();
  const size_t idx = static_cast<size_t>(sel);
  if (sel < 0) return a.mode != Mode::kCalendar && a.mode != Mode::kMap;
  switch (a.mode) {
    case Mode::kDay:
      if (idx < a.days.size()) fig = gview::load_day(a.db, a.days[idx].date);
      return true;
    case Mode::kTrends:
      if (idx < a.ranges.size()) fig = gview::load_trends(a.db, a.ranges[idx].days);
      return true;
    case Mode::kActivity:
      if (idx < a.activities.size()) {
        fig = gview::load_activity(a.db, a.activities[idx]);
        map::Track t = gview::load_track(a.db, a.activities[idx]);
        a.activity_map = !t.points.empty();
        apply_rects(a);  // set_track fits to the map's rect, so size it first
        if (a.activity_map) a.mapw.set_track(std::move(t));
      }
      return true;
    case Mode::kHockey:
      if (idx < a.games.size()) {
        const gview::GameEntry& g = a.games[idx];
        fig = g.season ? gview::load_season(a.db, a.hockey_hr_max)
                       : gview::load_game(a.db, g, a.hockey_hr_max);
      }
      return true;
    case Mode::kSki:
      if (idx < a.ski_days.size()) {
        const gview::SkiEntry& e = a.ski_days[idx];
        fig = e.season ? gview::load_ski_season(a.db) : gview::load_ski_day(a.db, e);
      }
      return true;
    case Mode::kCompare:
      if (picked.size() >= 2 && static_cast<size_t>(picked[1]) < a.activities.size()) {
        fig = gview::load_compare(a.db, a.activities[idx],
                                  a.activities[static_cast<size_t>(picked[1])]);
      } else if (idx < a.activities.size()) {
        fig = gview::load_activity(a.db, a.activities[idx]);
        fig.title = "Compare: Ctrl-click a second activity   (" + fig.title + ")";
      }
      return true;
    case Mode::kSleep:
      if (idx < a.nights.size()) fig = gview::load_night(a.db, a.nights[idx]);
      return true;
    case Mode::kCalendar:
      if (idx < a.months.size()) {
        plot::MonthData md = gview::load_month(a.db, a.months[idx].year, a.months[idx].month);
        title = "Calendar  " + md.title;
        a.calendar.set_month(std::move(md));
      }
      return false;
    case Mode::kMap:
      if (idx < a.gps_activities.size()) {
        map::Track t = gview::load_track(a.db, a.gps_activities[idx]);
        title = "Map  " + t.title;
        a.mapw.set_track(std::move(t));
      }
      return false;
  }
  return true;
}

void show_selection(App& a) {
  G_ASSERT(a.list != nullptr);
  plot::Figure fig;
  std::string title;
  a.activity_map = false;
  a.mapw.set_cursor_time(0.0, false);
  apply_rects(a);
  if (figure_for_selection(a, selected_indices(a), fig, title)) {
    title = fig.title;
    a.widget.set_figure(std::move(fig));
  }
  set_title(a, title);
  InvalidateRect(a.hwnd, nullptr, FALSE);
}

void reload(App& a) {
  a.db.close();
  std::string err;
  if (!a.db.open(a.db_path, err)) {
    MessageBoxW(a.hwnd, plot::widen(err).c_str(), L"Garmin viewer", MB_ICONERROR);
    return;
  }
  a.days = gview::list_days(a.db);
  a.activities = gview::list_activities(a.db);
  a.ranges = gview::trend_ranges();
  a.games = gview::list_games(a.db);
  a.months = gview::list_months(a.db);
  a.gps_activities = gview::list_gps_activities(a.db);
  a.ski_days = gview::list_ski_days(a.db);
  a.nights = gview::list_nights(a.db);
  a.hockey_hr_max = gview::hockey_hr_max(a.db);
  fill_list(a);
  select_only(a, 0);
  show_selection(a);
}

void set_mode(App& a, Mode m) {
  a.mode = m;
  HMENU menu = GetMenu(a.hwnd);
  const int id = kIdmViewDay + static_cast<int>(m);
  CheckMenuRadioItem(menu, kIdmViewDay, kIdmViewSleep, id, MF_BYCOMMAND);
  fill_list(a);
  select_only(a, 0);
  show_selection(a);
}

void paint(App& a) {
  PAINTSTRUCT ps{};
  BeginPaint(a.hwnd, &ps);
  if (!a.rt && !create_render_target(a)) {
    EndPaint(a.hwnd, &ps);
    return;
  }
  a.rt->BeginDraw();
  a.rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
  if (a.mode == Mode::kCalendar) {
    a.calendar.render(a.rt.Get());
  } else if (a.mode == Mode::kMap) {
    a.mapw.render(a.rt.Get());
  } else {
    a.widget.render(a.rt.Get());
    if (split_view(a)) a.mapw.render(a.rt.Get());
  }
  const HRESULT hr = a.rt->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    a.rt.Reset();
    a.mapw.drop_bitmaps();
  }
  EndPaint(a.hwnd, &ps);
}

void move_list_selection(App& a, int delta) {
  const int count = static_cast<int>(SendMessageW(a.list, LB_GETCOUNT, 0, 0));
  const std::vector<int> picked = selected_indices(a);
  int sel = picked.empty() ? -1 : picked.front();
  if (count <= 0) return;
  sel = sel < 0 ? 0 : sel + delta;
  if (sel < 0) sel = 0;
  if (sel >= count) sel = count - 1;
  select_only(a, sel);
  show_selection(a);
}

HMENU build_menu(const App& a) {
  HMENU bar = CreateMenu();
  HMENU view = CreatePopupMenu();
  G_REQUIRE_RET(bar != nullptr && view != nullptr, nullptr);
  AppendMenuW(view, MF_STRING, kIdmViewDay, L"&Day\t1");
  AppendMenuW(view, MF_STRING, kIdmViewTrends, L"&Trends\t2");
  AppendMenuW(view, MF_STRING, kIdmViewActivity, L"&Activity\t3");
  AppendMenuW(view, MF_STRING, kIdmViewHockey, L"&Hockey\t4");
  AppendMenuW(view, MF_STRING, kIdmViewCalendar, L"&Calendar\t5");
  AppendMenuW(view, MF_STRING, kIdmViewMap, L"&Map\t6");
  AppendMenuW(view, MF_STRING, kIdmViewSki, L"&Ski\t7");
  AppendMenuW(view, MF_STRING, kIdmViewCompare, L"C&ompare\t8");
  AppendMenuW(view, MF_STRING, kIdmViewSleep, L"S&leep\t9");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kIdmFit, L"&Fit to data\tHome");
  AppendMenuW(view, MF_STRING, kIdmZoomIn, L"Zoom &in\t+");
  AppendMenuW(view, MF_STRING, kIdmZoomOut, L"Zoom &out\t-");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(gview::build_layer_menu(a.map_settings)),
              L"Map &layer  (L cycles)");
  HMENU data = CreatePopupMenu();
  AppendMenuW(data, MF_STRING, kIdmReload, L"&Reload database\tF5");
  AppendMenuW(data, MF_STRING, kIdmMapKeys, L"Map API &keys...");
  AppendMenuW(data, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(data, MF_STRING, kIdmExit, L"E&xit");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(data), L"&Data");
  CheckMenuRadioItem(bar, kIdmViewDay, kIdmViewSleep, kIdmViewDay, MF_BYCOMMAND);
  return bar;
}

// The list keeps Up/Down/PageUp/PageDown; every other key is the plot's.
LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  if (msg == WM_KEYDOWN || msg == WM_CHAR) {
    const bool nav = wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT;
    if (!nav && msg == WM_KEYDOWN) {
      SendMessageW(GetParent(hwnd), msg, wp, lp);
      return 0;
    }
    if (!nav && msg == WM_CHAR) return 0;  // no type-ahead search
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, list_proc, 0);
  return DefSubclassProc(hwnd, msg, wp, lp);
}

bool in_plot(const App& a, int x, int y) {
  const RECT r = plot_rect(a);
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---- message handlers, one per message, so wnd_proc stays a dispatch table

// Activity view with a map: hovering either side marks the same moment on
// the other (chart time -> map marker, track point -> chart cursor).
void set_split_hover(App& a, int x, int y, bool inside) {
  G_ASSERT(split_view(a));
  const bool on_chart = inside && in_rect(chart_rect(a), x, y);
  const bool on_map = inside && in_rect(map_rect(a), x, y);
  const float fx = static_cast<float>(x);
  const float fy = static_cast<float>(y);
  a.widget.set_hover(fx, fy, on_chart);
  a.mapw.set_hover(fx, fy, on_map);
  a.mapw.set_cursor_time(on_chart ? a.widget.px_to_x(fx) : 0.0, on_chart);
  double t = 0.0;
  if (on_map && a.mapw.hovered_time(t)) {
    const RECT c = chart_rect(a);
    a.widget.set_hover(a.widget.x_to_px(t), static_cast<float>(c.top + c.bottom) / 2.0f, true);
  }
}

void set_all_hover(App& a, int x, int y, bool inside) {
  if (split_view(a)) {
    set_split_hover(a, x, y, inside);
    return;
  }
  const float fx = static_cast<float>(x);
  const float fy = static_cast<float>(y);
  a.widget.set_hover(fx, fy, inside);
  a.calendar.set_hover(fx, fy, inside);
  a.mapw.set_hover(fx, fy, inside);
  a.mapw.set_cursor_time(0.0, false);
}

void apply_scale(App& a) {
  G_ASSERT(a.scale > 0.1f);
  a.widget.set_dpi_scale(a.scale);
  a.calendar.set_dpi_scale(a.scale);
  a.mapw.set_dpi_scale(a.scale);
  apply_list_font(a);
}

void on_create(App& a, HWND hwnd) {
  G_ASSERT(a.hwnd == nullptr);
  a.hwnd = hwnd;
  a.scale = dpi_scale(hwnd);
  a.list = CreateWindowExW(0, L"LISTBOX", nullptr,
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                               LBS_EXTENDEDSEL | WS_BORDER,
                           0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                           nullptr, nullptr);
  G_ASSERT(a.list != nullptr);
  const BOOL subclassed = SetWindowSubclass(a.list, list_proc, 0, 0);
  G_ASSERT(subclassed);
  apply_scale(a);
  a.mapw.set_layers(a.map_settings.base, a.map_settings.overlays);
  if (!a.tiles.start(gutil::app_data_dir() / L"tiles", hwnd, kMsgTileReady, a.map_settings.keys)) {
    MessageBoxW(hwnd, L"Could not create the map tile cache directory; the Map view will be empty.",
                L"Garmin viewer", MB_ICONWARNING);
  }
  layout(a);
  reload(a);
}

void on_dpi_changed(App& a, HWND hwnd, WPARAM wp, LPARAM lp) {
  a.scale = static_cast<float>(HIWORD(wp)) / 96.0f;
  apply_scale(a);
  const RECT* r = reinterpret_cast<const RECT*>(lp);
  G_ASSERT(r != nullptr);
  SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
               SWP_NOZORDER | SWP_NOACTIVATE);
  layout(a);
}

// Pushes the chosen map layers to the widget and the menu, and remembers them in gview.ini.
void apply_layers(App& a) {
  G_ASSERT(a.hwnd != nullptr);
  a.mapw.set_layers(a.map_settings.base, a.map_settings.overlays);
  gview::check_layer_menu(GetMenu(a.hwnd), a.map_settings);
  if (!map::save_map_layers(a.ini_path, a.map_settings)) {
    // Not fatal (e.g. the exe folder is read-only): the choice lasts this session.
  }
  InvalidateRect(a.hwnd, nullptr, FALSE);
}

// Data > Map API keys: after a save, new keys apply at once: the tile cache
// gets them and the menu is rebuilt so newly usable layers are enabled.
void on_map_keys(App& a) {
  G_ASSERT(a.hwnd != nullptr);
  if (!gview::edit_map_keys(a.hwnd, a.ini_path)) return;
  a.map_settings.keys = map::load_map_settings(a.ini_path).keys;
  if (!map::provider_available(a.map_settings, a.map_settings.base)) a.map_settings.base = 0;
  a.tiles.set_keys(a.map_settings.keys);
  HMENU old = GetMenu(a.hwnd);
  HMENU fresh = build_menu(a);
  G_REQUIRE_VOID(fresh != nullptr);
  const BOOL set = SetMenu(a.hwnd, fresh);
  G_ASSERT(set);
  if (old != nullptr) DestroyMenu(old);
  const int id = kIdmViewDay + static_cast<int>(a.mode);
  CheckMenuRadioItem(fresh, kIdmViewDay, kIdmViewSleep, id, MF_BYCOMMAND);
  apply_layers(a);
}

// Returns the mode for a View menu id / digit key, or false if it is not one.
bool mode_for_id(int id, Mode& out) {
  if (id < kIdmViewDay || id > kIdmViewSleep) return false;
  out = static_cast<Mode>(id - kIdmViewDay);
  return true;
}

void on_command(App& a, HWND hwnd, WPARAM wp) {
  const int id = LOWORD(wp);
  if (id == kListId) {
    if (HIWORD(wp) == LBN_SELCHANGE) show_selection(a);
    return;
  }
  Mode m = Mode::kDay;
  if (mode_for_id(id, m)) {
    set_mode(a, m);
    return;
  }
  size_t provider = 0;
  if (gview::layer_command(id, provider)) {
    if (map::choose_layer(a.map_settings, provider)) apply_layers(a);
    return;
  }
  switch (id) {
    case kIdmFit:
      a.widget.fit_x();
      break;
    case kIdmZoomIn:
    case kIdmZoomOut: {
      const RECT r = chart_rect(a);
      const float mid = static_cast<float>(r.left + r.right) / 2.0f;
      a.widget.zoom_at(mid, id == kIdmZoomIn ? kKeyZoom : 1.0 / kKeyZoom);
      break;
    }
    case kIdmReload:
      reload(a);
      return;
    case kIdmMapKeys:
      on_map_keys(a);
      return;
    case kIdmExit:
      DestroyWindow(hwnd);
      return;
    default:
      return;
  }
  InvalidateRect(hwnd, nullptr, FALSE);
}

// Returns true when handled.
bool on_mouse_wheel(App& a, HWND hwnd, WPARAM wp, LPARAM lp) {
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(hwnd, &pt);
  if (!in_plot(a, pt.x, pt.y) || a.mode == Mode::kCalendar) return false;
  const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
  if (a.mode == Mode::kMap || (split_view(a) && in_rect(map_rect(a), pt.x, pt.y))) {
    a.mapw.zoom_step(static_cast<float>(pt.x), static_cast<float>(pt.y), notches > 0 ? 1 : -1);
  } else {
    double factor = 1.0;
    const int steps = std::min(10, notches < 0 ? -notches : notches);
    for (int i = 0; i < steps; ++i) {
      factor *= notches > 0 ? kWheelZoomPerNotch : 1.0 / kWheelZoomPerNotch;
    }
    a.widget.zoom_at(static_cast<float>(pt.x), factor);
  }
  InvalidateRect(hwnd, nullptr, FALSE);
  return true;
}

bool on_button_down(App& a, HWND hwnd, LPARAM lp) {
  const int x = GET_X_LPARAM(lp);
  const int y = GET_Y_LPARAM(lp);
  if (!in_plot(a, x, y) || a.mode == Mode::kCalendar) return false;
  a.dragging = true;
  a.drag_map = a.mode == Mode::kMap || (split_view(a) && in_rect(map_rect(a), x, y));
  a.drag_last_x = x;
  a.drag_last_y = y;
  SetCapture(hwnd);
  SetFocus(hwnd);
  return true;
}

void on_mouse_move(App& a, HWND hwnd, LPARAM lp) {
  const int x = GET_X_LPARAM(lp);
  const int y = GET_Y_LPARAM(lp);
  if (!a.tracking_mouse) {
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
    a.tracking_mouse = TrackMouseEvent(&tme) != 0;
  }
  if (a.dragging) {
    const int dx = x - a.drag_last_x;
    const int dy = y - a.drag_last_y;
    if (a.drag_map) {
      a.mapw.pan_pixels(static_cast<float>(dx), static_cast<float>(dy));
    } else if (dx != 0) {
      a.widget.pan_pixels(static_cast<float>(dx));
    }
    a.drag_last_x = x;
    a.drag_last_y = y;
  }
  set_all_hover(a, x, y, in_plot(a, x, y));
  InvalidateRect(hwnd, nullptr, FALSE);
}

// Returns true when handled (the caller repaints); false defers to DefWindowProc.
bool on_key_down(App& a, WPARAM key) {
  if (key >= '1' && key <= '9') {
    set_mode(a, static_cast<Mode>(key - '1'));
    return true;
  }
  const RECT r = chart_rect(a);
  const float w = static_cast<float>(r.right - r.left);
  const float mid = static_cast<float>(r.left + r.right) / 2.0f;
  switch (key) {
    case VK_HOME:
      if (a.mode == Mode::kMap || split_view(a)) a.mapw.fit();
      if (a.mode != Mode::kMap) a.widget.fit_x();
      return true;
    case VK_LEFT: a.widget.pan_pixels(w * kKeyPanFrac); return true;
    case VK_RIGHT: a.widget.pan_pixels(-w * kKeyPanFrac); return true;
    case VK_UP:
    case VK_PRIOR: move_list_selection(a, -1); return true;
    case VK_DOWN:
    case VK_NEXT: move_list_selection(a, 1); return true;
    case VK_ADD:
    case VK_OEM_PLUS: a.widget.zoom_at(mid, kKeyZoom); return true;
    case VK_SUBTRACT:
    case VK_OEM_MINUS: a.widget.zoom_at(mid, 1.0 / kKeyZoom); return true;
    case VK_F5: reload(a); return true;
    case 'L':
      a.map_settings.base = map::next_base(a.map_settings);
      apply_layers(a);
      return true;
    default: return false;
  }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* a = g_app;
  G_ASSERT(a != nullptr);
  switch (msg) {
    case WM_CREATE: on_create(*a, hwnd); return 0;
    case WM_SIZE:
      if (a->list != nullptr) layout(*a);
      return 0;
    case WM_DPICHANGED: on_dpi_changed(*a, hwnd, wp, lp); return 0;
    case WM_PAINT: paint(*a); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND: on_command(*a, hwnd, wp); return 0;
    case WM_MOUSEWHEEL:
      if (on_mouse_wheel(*a, hwnd, wp, lp)) return 0;
      break;
    case WM_LBUTTONDOWN:
      if (on_button_down(*a, hwnd, lp)) return 0;
      break;
    case WM_MOUSEMOVE: on_mouse_move(*a, hwnd, lp); return 0;
    case WM_LBUTTONUP:
      if (a->dragging) {
        a->dragging = false;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSELEAVE:
      a->tracking_mouse = false;
      set_all_hover(*a, 0, 0, false);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_KEYDOWN:
      if (on_key_down(*a, wp)) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    case kMsgTileReady:
      if (a->mode == Mode::kMap || split_view(*a)) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_DESTROY:
      a->tiles.stop();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

std::filesystem::path resolve_db_path() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::filesystem::path data_dir;
  if (argv != nullptr) {
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::wstring(argv[i]) == L"--data") data_dir = argv[i + 1];
    }
    LocalFree(argv);
  }
  if (data_dir.empty()) {
    const std::filesystem::path base = gutil::app_data_dir();
    data_dir = base.empty() ? std::filesystem::path(L"data") : base / L"data";
  }
  return data_dir / L"garmin.db";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
  auto app = std::make_unique<App>();
  g_app = app.get();
  app->db_path = resolve_db_path();
  app->ini_path = map::sidecar_ini_path();
  app->map_settings = map::load_map_settings(app->ini_path);
  if (!std::filesystem::exists(app->db_path)) {
    MessageBoxW(nullptr, (L"Database not found:\n" + app->db_path.wstring() +
                          L"\n\nRun `gsync sync` first, or pass --data <dir>.")
                             .c_str(),
                L"Garmin viewer", MB_ICONERROR);
    return 1;
  }

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, app->d2d.GetAddressOf()))) {
    return 2;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(app->dwrite.GetAddressOf())))) {
    return 2;
  }
  if (!app->widget.init(app->d2d.Get(), app->dwrite.Get())) return 2;
  if (!app->calendar.init(app->d2d.Get(), app->dwrite.Get())) return 2;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&app->wic)))) {
    return 2;
  }
  if (!app->mapw.init(app->d2d.Get(), app->dwrite.Get(), app->wic.Get(), &app->tiles)) return 2;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClassName;
  wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = wc.hIcon;
  G_REQUIRE_RET(RegisterClassExW(&wc) != 0, 3);

  // WS_CLIPCHILDREN: paint() clears and redraws the whole Direct2D target on
  // every hover move; without it that repaint covers the list box each time.
  const HWND hwnd = CreateWindowExW(0, kClassName, L"Garmin viewer",
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr,
                                    build_menu(*app), hinst, nullptr);
  G_REQUIRE_RET(hwnd != nullptr, 3);
  gview::check_layer_menu(GetMenu(hwnd), app->map_settings);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (app->list_font != nullptr) DeleteObject(app->list_font);
  g_app = nullptr;
  app.reset();
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
