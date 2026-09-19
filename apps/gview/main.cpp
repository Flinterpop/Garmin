// gview: Win32 + Direct2D viewer over garmin.db. Left: a list (days,
// trend ranges, or activities). Right: the plot. Wheel zooms, drag pans,
// hover shows values, Home fits, 1/2/3 switch views, F5 reloads.
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "plot/plot_widget.h"
#include "queries.h"
#include "store/db.h"
#include "util/assert.h"
#include "util/file_util.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"GarminViewMain";
constexpr int kListWidthCss = 300;
constexpr int kListId = 1001;
constexpr int kIdmViewDay = 101;
constexpr int kIdmViewTrends = 102;
constexpr int kIdmViewActivity = 103;
constexpr int kIdmFit = 201;
constexpr int kIdmZoomIn = 202;
constexpr int kIdmZoomOut = 203;
constexpr int kIdmReload = 301;
constexpr int kIdmExit = 302;
constexpr double kWheelZoomPerNotch = 0.8;
constexpr double kKeyZoom = 0.7;
constexpr float kKeyPanFrac = 0.1f;

enum class Mode { kDay, kTrends, kActivity };

struct App {
  HWND hwnd = nullptr;
  HWND list = nullptr;
  HFONT list_font = nullptr;
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1HwndRenderTarget> rt;
  plot::PlotWidget widget;
  store::Db db;
  std::filesystem::path db_path;
  Mode mode = Mode::kDay;
  std::vector<gview::DayEntry> days;
  std::vector<gview::ActivityEntry> activities;
  std::vector<gview::TrendRange> ranges;
  float scale = 1.0f;
  bool dragging = false;
  bool moved_while_dragging = false;
  int drag_last_x = 0;
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
  const RECT pr = plot_rect(a);
  a.widget.set_rect(D2D1::RectF(static_cast<float>(pr.left), static_cast<float>(pr.top),
                                static_cast<float>(pr.right), static_cast<float>(pr.bottom)));
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
  }
  SendMessageW(a.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(a.list, nullptr, TRUE);
}

void show_selection(App& a) {
  const int sel = static_cast<int>(SendMessageW(a.list, LB_GETCURSEL, 0, 0));
  plot::Figure fig;
  std::string title;
  if (sel >= 0) {
    switch (a.mode) {
      case Mode::kDay:
        if (static_cast<size_t>(sel) < a.days.size()) {
          fig = gview::load_day(a.db, a.days[static_cast<size_t>(sel)].date);
        }
        break;
      case Mode::kTrends:
        if (static_cast<size_t>(sel) < a.ranges.size()) {
          fig = gview::load_trends(a.db, a.ranges[static_cast<size_t>(sel)].days);
        }
        break;
      case Mode::kActivity:
        if (static_cast<size_t>(sel) < a.activities.size()) {
          fig = gview::load_activity(a.db, a.activities[static_cast<size_t>(sel)]);
        }
        break;
    }
  }
  title = fig.title;
  a.widget.set_figure(std::move(fig));
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
  fill_list(a);
  SendMessageW(a.list, LB_SETCURSEL, 0, 0);
  show_selection(a);
}

void set_mode(App& a, Mode m) {
  a.mode = m;
  HMENU menu = GetMenu(a.hwnd);
  CheckMenuRadioItem(menu, kIdmViewDay, kIdmViewActivity,
                     m == Mode::kDay ? kIdmViewDay
                                     : (m == Mode::kTrends ? kIdmViewTrends : kIdmViewActivity),
                     MF_BYCOMMAND);
  fill_list(a);
  SendMessageW(a.list, LB_SETCURSEL, 0, 0);
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
  a.widget.render(a.rt.Get());
  const HRESULT hr = a.rt->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) a.rt.Reset();
  EndPaint(a.hwnd, &ps);
}

void move_list_selection(App& a, int delta) {
  const int count = static_cast<int>(SendMessageW(a.list, LB_GETCOUNT, 0, 0));
  int sel = static_cast<int>(SendMessageW(a.list, LB_GETCURSEL, 0, 0));
  if (count <= 0) return;
  sel = sel < 0 ? 0 : sel + delta;
  if (sel < 0) sel = 0;
  if (sel >= count) sel = count - 1;
  SendMessageW(a.list, LB_SETCURSEL, sel, 0);
  show_selection(a);
}

HMENU build_menu() {
  HMENU bar = CreateMenu();
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kIdmViewDay, L"&Day\t1");
  AppendMenuW(view, MF_STRING, kIdmViewTrends, L"&Trends\t2");
  AppendMenuW(view, MF_STRING, kIdmViewActivity, L"&Activity\t3");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kIdmFit, L"&Fit to data\tHome");
  AppendMenuW(view, MF_STRING, kIdmZoomIn, L"Zoom &in\t+");
  AppendMenuW(view, MF_STRING, kIdmZoomOut, L"Zoom &out\t-");
  HMENU data = CreatePopupMenu();
  AppendMenuW(data, MF_STRING, kIdmReload, L"&Reload database\tF5");
  AppendMenuW(data, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(data, MF_STRING, kIdmExit, L"E&xit");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(data), L"&Data");
  CheckMenuRadioItem(bar, kIdmViewDay, kIdmViewActivity, kIdmViewDay, MF_BYCOMMAND);
  return bar;
}

bool in_plot(const App& a, int x, int y) {
  const RECT r = plot_rect(a);
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* a = g_app;
  switch (msg) {
    case WM_CREATE: {
      G_ASSERT(a != nullptr);
      a->hwnd = hwnd;
      a->scale = dpi_scale(hwnd);
      a->list = CreateWindowExW(0, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_NOINTEGRALHEIGHT | WS_BORDER,
                                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), nullptr,
                                nullptr);
      apply_list_font(*a);
      a->widget.set_dpi_scale(a->scale);
      layout(*a);
      reload(*a);
      return 0;
    }
    case WM_SIZE:
      if (a != nullptr && a->list != nullptr) layout(*a);
      return 0;
    case WM_DPICHANGED: {
      a->scale = static_cast<float>(HIWORD(wp)) / 96.0f;
      a->widget.set_dpi_scale(a->scale);
      apply_list_font(*a);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      layout(*a);
      return 0;
    }
    case WM_PAINT:
      paint(*a);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_COMMAND: {
      const int id = LOWORD(wp);
      if (id == kListId && HIWORD(wp) == LBN_SELCHANGE) {
        show_selection(*a);
        return 0;
      }
      switch (id) {
        case kIdmViewDay: set_mode(*a, Mode::kDay); break;
        case kIdmViewTrends: set_mode(*a, Mode::kTrends); break;
        case kIdmViewActivity: set_mode(*a, Mode::kActivity); break;
        case kIdmFit: a->widget.fit_x(); InvalidateRect(hwnd, nullptr, FALSE); break;
        case kIdmZoomIn:
        case kIdmZoomOut: {
          const RECT r = plot_rect(*a);
          const float mid = static_cast<float>(r.left + r.right) / 2.0f;
          a->widget.zoom_at(mid, id == kIdmZoomIn ? kKeyZoom : 1.0 / kKeyZoom);
          InvalidateRect(hwnd, nullptr, FALSE);
          break;
        }
        case kIdmReload: reload(*a); break;
        case kIdmExit: DestroyWindow(hwnd); break;
        default: break;
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      if (!in_plot(*a, pt.x, pt.y)) break;
      const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
      double factor = 1.0;
      for (int i = 0; i < 10 && i < (notches < 0 ? -notches : notches); ++i) {
        factor *= notches > 0 ? kWheelZoomPerNotch : 1.0 / kWheelZoomPerNotch;
      }
      a->widget.zoom_at(static_cast<float>(pt.x), factor);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const int x = GET_X_LPARAM(lp);
      const int y = GET_Y_LPARAM(lp);
      if (!in_plot(*a, x, y)) break;
      a->dragging = true;
      a->moved_while_dragging = false;
      a->drag_last_x = x;
      SetCapture(hwnd);
      SetFocus(hwnd);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lp);
      const int y = GET_Y_LPARAM(lp);
      if (!a->tracking_mouse) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        a->tracking_mouse = true;
      }
      if (a->dragging) {
        const int dx = x - a->drag_last_x;
        if (dx != 0) {
          a->widget.pan_pixels(static_cast<float>(dx));
          a->drag_last_x = x;
          a->moved_while_dragging = true;
        }
      }
      a->widget.set_hover(static_cast<float>(x), static_cast<float>(y), in_plot(*a, x, y));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP:
      if (a->dragging) {
        a->dragging = false;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSELEAVE:
      a->tracking_mouse = false;
      a->widget.set_hover(0.0f, 0.0f, false);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_KEYDOWN: {
      const RECT r = plot_rect(*a);
      const float w = static_cast<float>(r.right - r.left);
      switch (wp) {
        case VK_HOME: a->widget.fit_x(); break;
        case VK_LEFT: a->widget.pan_pixels(w * kKeyPanFrac); break;
        case VK_RIGHT: a->widget.pan_pixels(-w * kKeyPanFrac); break;
        case VK_UP:
        case VK_PRIOR: move_list_selection(*a, -1); return 0;
        case VK_DOWN:
        case VK_NEXT: move_list_selection(*a, 1); return 0;
        case VK_ADD:
        case VK_OEM_PLUS:
          a->widget.zoom_at(static_cast<float>(r.left + r.right) / 2.0f, kKeyZoom);
          break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
          a->widget.zoom_at(static_cast<float>(r.left + r.right) / 2.0f, 1.0 / kKeyZoom);
          break;
        case VK_F5: reload(*a); return 0;
        case '1': set_mode(*a, Mode::kDay); return 0;
        case '2': set_mode(*a, Mode::kTrends); return 0;
        case '3': set_mode(*a, Mode::kActivity); return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_DESTROY:
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
  auto app = std::make_unique<App>();
  g_app = app.get();
  app->db_path = resolve_db_path();
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

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClassName;
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  G_REQUIRE_RET(RegisterClassExW(&wc) != 0, 3);

  const HWND hwnd = CreateWindowExW(0, kClassName, L"Garmin viewer", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr,
                                    build_menu(), hinst, nullptr);
  G_REQUIRE_RET(hwnd != nullptr, 3);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (app->list_font != nullptr) DeleteObject(app->list_font);
  g_app = nullptr;
  return static_cast<int>(msg.wParam);
}
