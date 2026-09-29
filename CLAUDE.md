# Garmin

*Last updated: 29 Sep 2026*

Personal health-data tooling: C++20 / Win32, no UI framework. **This repository is not export-controlled** (it is the owner's own fitness data and open-source code under MIT), unlike the other repositories on this machine. Still keep work local; the only outbound traffic is to Garmin Connect and the map tile servers (OpenStreetMap by default; the providers in `src/map/tile_provider.cpp` when chosen in View → Map layer; AWS Terrain Tiles for the 3D map's heights).

**Portable install: everything lives beside the exes, nothing in AppData, nothing tied to a Windows account** (the owner's explicit rule). Logins (`tokens.bin`, DPAPI machine scope), databases (`data\`), the tile cache (`tiles\`), `gview.ini` and other people's profiles (`profiles\<name>\`) all sit in the exe folder; multi-user means `--profile <name>`, never Windows accounts. `gutil::exe_dir()` is the only base path; `legacy_appdata_dir()` exists solely for the copy-from-previous-version migration.

**Users never run `gsync.exe`** (the owner's rule). Every user-facing action (login incl. MFA, sync, adding a person, migration, the morning task) must be reachable from `gview`'s menus and dialogs; `gsync` is for the scheduled task and scripting. New sync/login logic goes in `src/sync` so both use it, never only in `apps/gsync`. Error text shown in `gview` must not tell the user to run a command.

**`gview.ini` (beside `gview.exe`) holds the user's map API keys.** Never commit it, never put it in a release zip, never paste its contents anywhere. `*.ini` is gitignored; check `git status` and the zip listing for it before every commit and release.

## Build and test

```powershell
cmake --preset msvc-static          # Visual Studio 18 2026, x64, vcpkg x64-windows-static
cmake --build --preset release      # build\apps\Release\{gsync,gview,fitdump}.exe
cmake --build --preset debug
ctest --preset debug                # Catch2, must stay at 100 %
```

`/W4 /WX` is on for our code and off for vcpkg headers (`/external:W0`). Static CRT, no runtime DLLs. A running `gsync.exe` (nightly task, backfill) blocks relinking it — check `Get-Process gsync` before a full build.

## Layout

- `src/util` assertions (`G_ASSERT`, `G_REQUIRE_RET`, `G_REQUIRE_VOID`), time, CNG/DPAPI, zip, files
- `src/fit` FIT decoder written from the spec; `src/gc` WinHTTP + OAuth1 Garmin Connect client; `src/store` SQLite schema + importers; `src/sync` the sync engine (progress via a `Report` callback, cancellable), login, migration and catch-up range, file import (`files.*`) and watch import over MTP through the Shell (`watch.*`, `IFileOperation`), shared by gsync and gview
- `src/plot` Direct2D plot engine (`PlotWidget`) and `CalendarWidget`; `src/map` Web Mercator, tile providers + `gview.ini` settings, tile cache (2 workers, per-provider folders), `MapWidget`; `src/map3d` 3D map (pure geometry in `terrain.*`, Direct3D 11 `renderer.*`, child-window `view3d.*`); `src/analysis` hockey shifts, ski runs
- `apps/gsync` CLI (thin wrapper over `src/sync`), `apps/gview` viewer (`queries*.cpp` build a `plot::Figure` per view; `sync_ui.*` login / MFA / progress / welcome / add-person dialogs, work on a worker thread; `schedule.*` the morning task via `schtasks /XML`), `apps/fitdump`
- `tests` one file per module; analysis modules are tested on synthetic traces

## Conventions

- Power of 10: bounded loops (every `for` over data has a `kMax*` cap), no recursion, assertions on preconditions, fixed-size decoder state, check every return value.
- New view = a `queries_<name>.cpp` returning a `Figure`, a `Mode` in `apps/gview/main.cpp`, a list function, a menu item and a digit key. Follow `queries_ski.cpp` as the template.
- Timestamps are Unix seconds UTC in the database; display converts to local. Weight is stored in kg and shown in lb.
- Session lists must de-duplicate by `start_ts` and prefer the copy with records (the watch's `SUMMARY` folder holds record-less twins).
- Version lives in three places and moves together: `project(Garmin VERSION ...)`, `apps/resource.h`, the README badge. Release = bump, build Release, `ctest`, `python tools/rot_scan.py`, zip the three exes + README + LICENSE, tag `vX.Y.Z`, `gh release create` with notes.
- Before a release run `python tools/rot_scan.py`; the only acceptable residue is dispatch-style functions (`on_command`, `on_key_down`, `sport_name`) in the assertion-free list. Anything else gets fixed, not waived.

## Verifying GUI changes

Launch `build\apps\Release\gview.exe`, drive it by posting messages (never `SendKeys` — it steals the user's focus), and capture with `PrintWindow` so overlapping windows are excluded:

```powershell
# WM_KEYDOWN '4' switches to Hockey; VK_DOWN moves the list; then PrintWindow the HWND.
[W]::PostMessage($h, 0x0100, [IntPtr]0x34, [IntPtr]::Zero)
```

The full helper is in the session notes; the pattern is: PostMessage keys, sleep 1–2 s, PrintWindow(hwnd, hdc, 2), Read the PNG.

## Traps

- The Bash tool turns `\f`, `\d`, `\t`, `\n` in the *command text* into escape characters, even inside quoted heredocs. Write source and patch scripts with the Write tool; use PowerShell for one-line edits that contain backslashes.
- `setvbuf(_IOLBF, 0)` is an invalid parameter on MSVC (fail-fast 0xC0000409); use `_IONBF`.
- `LBS_EXTENDEDSEL` list boxes ignore `LB_SETCURSEL`; use `LB_SETSEL` + `LB_GETSELITEMS`.
- Garmin's `download-service` answers Cloudflare 504 for older wellness zips; `gsync` retries with backoff and re-fetches unmarked days on the next run.
- A token-renewal bug only shows up a day after `gsync login`, once the bearer from login has expired; a sync the same evening proves nothing. The v0.1.1 re-exchange re-sent the login `mfa_token`, got 403 "MFA token was invalid", and the nightly task failed silently for 9 days. `tokens.bin` (beside `gsync.exe`) not rewritten since login is the tell. Check `Get-ScheduledTaskInfo GarminSync` (`LastTaskResult` 3 = login required) when reviewing the repo.
