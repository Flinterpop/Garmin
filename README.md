# Garmin

[![Release][release-badge]][release-latest] [![License: MIT][license-badge]](LICENSE)

[release-badge]: https://img.shields.io/badge/release-v0.1.3-blue
[release-latest]: https://github.com/Flinterpop/Garmin/releases/latest
[license-badge]: https://img.shields.io/badge/license-MIT-green

*Last updated: 18 Sep 2026*

<img src="docs/icon-256.png" alt="app icon" width="96" align="right" />

Local, C++/Win32 tooling for pulling health data off Garmin devices and out of Garmin Connect, and keeping it in a SQLite database shaped for plotting. No Python, no cloud service of our own: everything runs on this machine and talks only to Garmin.

Three executables:

- **`gsync`** — signs in to Garmin Connect the way the mobile app does, pulls daily summaries, heart rate, sleep, stress / Body Battery, HRV, body composition (the Index scale), the activity list, per-activity FIT files and the daily wellness (monitoring) FIT files, and imports it all into `garmin.db`. Also imports FIT files copied straight off a watch over USB.
- **`fitdump`** — inspects a single FIT file: summary, per-message counts, session line, and optional full dump to stdout or a long-format CSV.
- **`gview`** — Win32 + Direct2D viewer over `garmin.db` with ten views: **Day** (heart rate, stress, Body Battery, sleep stages, respiration, HRV on one time axis), **Trends** (resting HR, HRV, sleep hours and score, weight in lb, steps over 30 days to everything), **Activity** (HR, speed, altitude, cadence, power, temperature against elapsed time, laps as markers; activities with GPS get the map beside the charts, and hovering either one marks the same moment on the other), **Hockey** (per-game shift detection from the HR trace with on-ice bands, HR zones, shift lengths and peak HR, plus a season overview across every game), **Calendar** (month grid with steps, resting HR, Body Battery range, sleep score and the day's activities), **Map** (GPS activities on OpenStreetMap tiles, track coloured by heart rate, start/finish/lap markers, scale bar, hover readout), **Ski** (runs and lifts detected from the altitude profile: per-run vertical, top speed and HR, plus a season overview), **Compare** (Ctrl-click two activities to overlay HR, speed, altitude and cadence), **Sleep** (one night: hypnogram, HR/HRV, respiration/SpO2/stress) and **3D map** (a GPS activity over real terrain: the selected map layer draped on elevation data, the track as a heart-rate-coloured ribbon, orbit camera, adjustable height exaggeration; Direct3D 11). Wheel zooms, drag pans, hover shows every value at the cursor. Trends carry a trailing 30-day mean ± 1 σ band for resting HR, HRV and sleep hours with days beyond 2 σ flagged, and a blood-pressure panel when Omron CSV exports are present.

## Status

| Piece | State |
|---|---|
| FIT decoder (`src/fit`) | Done; verified against Garmin-native and Zwift activity files, 10 unit tests |
| Garmin Connect login + endpoints (`src/gc`) | Done; verified against a live account (login with MFA, all daily endpoints, weight, activities, wellness FIT zips) |
| SQLite store + importers (`src/store`) | Done; verified on live JSON and on 208 files copied off a fenix 7 |
| Plot viewer (`src/plot`, `apps/gview`) | Done; own Direct2D plot engine, five views, verified on the live database |
| Hockey analysis (`src/analysis`) | Done; shift detection tuned on real games |
| Map (`src/map`) | Done; OSM tiles + HR-coloured tracks, verified on runs and ski days |
| Ski analysis, Compare, Sleep, baselines, blood pressure | Done |
| Nightly sync task, app icon, MIT license, public releases | Done |

Notes:

- The Connect API is Garmin's unofficial app API (the same one `garth` / `python-garminconnect` use). Garmin can change it without notice; when that happens `gsync get <path>` is the debugging tool. The wellness-zip endpoint answers Cloudflare 504 for older dates fairly often; `gsync` retries 5xx with backoff and leaves failed days unmarked so the next sync picks them up.
- Weight is stored in kg and displayed in pounds.
- The map fetches tiles from the selected map layer's server (OpenStreetMap by default; see [Map layers](#map-layers)), and the 3D map also fetches elevation tiles from AWS Terrain Tiles (`s3.amazonaws.com/elevation-tiles-prod`, public, no key). That is the only traffic that does not go to Garmin. Every provider gets an identifying User-Agent, at most two connections and only the tiles currently on screen, as the OSM tile usage policy asks; tiles are cached on disk in `%LOCALAPPDATA%\GarminSync\tiles` except where the provider's terms forbid it (Google, Azure Maps).
- The OAuth consumer key pair is fetched from the public location `garth` publishes, or can be supplied via `GARMIN_OAUTH_CONSUMER_KEY` / `GARMIN_OAUTH_CONSUMER_SECRET`.

## Build

Requirements: Visual Studio 2026 (MSVC), CMake 3.25+, vcpkg at `C:\vcpkg` with the `x64-windows-static` ports `nlohmann-json`, `zlib`, `sqlite3`, `catch2`.

```powershell
cmake --preset msvc-static
cmake --build --preset release
ctest --preset debug          # after cmake --build --preset debug
```

Executables land in `build\apps\Release\`. Everything compiles under `/W4 /WX` with static CRT; there are no runtime DLL dependencies.

## Usage

```text
gsync login                     # prompts for email, password, MFA code; tokens saved DPAPI-encrypted
gsync whoami
gsync sync --days 30            # daily data + wellness FIT + activities for the last 30 days
gsync sync --from 2026-01-01 --to 2026-03-31 --activities 200
gsync import <staging>\Activity <staging>\Monitor       # files copied off the watch (see below)
gsync stats
gsync import-bp                 # Omron blood-pressure CSVs from Downloads (also runs during sync)
gsync sync --days 3 --log %LOCALAPPDATA%\GarminSync\sync.log   # what the nightly task runs
gsync get /usersummary-service/usersummary/daily/<displayName>?calendarDate=2026-09-17

fitdump some.fit                # summary
fitdump some.fit --print        # every message
fitdump some.fit --csv out.csv  # mesg,timestamp,field,value,units

gview                           # opens the default database; --data <dir> for another
```

`gview` keys: `1` – `9` switch Day / Trends / Activity / Hockey / Calendar / Map / Ski / Compare / Sleep and `0` the 3D map (left-drag orbits, right-drag pans, wheel zooms, `Home` resets the camera, `E` cycles height exaggeration), `Up` / `Down` step through the list, mouse wheel zooms around the cursor, drag pans, `Home` fits, `+` / `-` zoom, `L` cycles the map layer, `F5` reloads after a sync.

### Map layers

**View → Map layer** picks the base map for the Map view and the Activity view's map, plus optional route overlays; `L` cycles through the base maps you can use. The choice is remembered in `gview.ini` next to `gview.exe`.

| Layer | Key needed | Notes |
|---|---|---|
| OpenStreetMap | no | default |
| CyclOSM | no | cycling: lanes, routes, surfaces |
| OpenTopoMap | no | contours and trails; zoom 17 max |
| Esri World Imagery, Esri World Topo | no | Esri's terms formally expect an ArcGIS account |
| Thunderforest OpenCycleMap, Outdoors | `thunderforest` | free hobby key from thunderforest.com |
| Google Maps, Satellite, Terrain | `google` | Map Tiles API key (Google Cloud, billing enabled); tiles are not stored on disk |
| Azure Maps road, imagery | `azure_maps` | Bing Maps' successor; Azure Maps key; tiles are not stored on disk |
| Hiking / cycling routes overlay | no | Waymarked Trails, drawn over any base map |

Enter keys with **Data → Map API keys…** (masked unless *Show keys* is ticked; an empty field removes that key). New keys take effect at once. They are saved in a sidecar `gview.ini` beside `gview.exe`, which you can also edit by hand. Layers whose key is missing are greyed out in the menu.

```ini
[keys]
google = your-google-map-tiles-api-key
azure_maps = your-azure-maps-key
thunderforest = your-thunderforest-key

[map]
base = osm
overlays = wmt_hiking
```

**`gview.ini` holds secrets: never commit it or ship it.** `*.ini` is in `.gitignore`, and the release zip contains only the three executables, the README and the LICENSE.

Hockey shifts are detected from the smoothed HR trace: each rising leg (with 12 bpm hysteresis) whose peak clears the game's median HR is a shift, since HR climbs on the ice and falls on the bench. Zones are 60/70/80/90 % of a robust HR max (95th percentile of per-game maxima). Both live in `src/analysis/hockey.cpp` and are unit-tested on a synthetic game.

Options: `--data <dir>` (default `%LOCALAPPDATA%\GarminSync\data`), `--no-fit`, `--force`, `--out <file>`.

### Keeping it current

A Windows Task Scheduler job named `GarminSync` runs `gsync sync --days 3 --log ...\sync.log` daily at 06:00 when you are logged on, catching up if the machine was off and skipping when offline. Register it with:

```powershell
$exe = 'C:\source_games\Garmin\build\apps\Release\gsync.exe'; $log = "$env:LOCALAPPDATA\GarminSync\sync.log"
$action   = New-ScheduledTaskAction -Execute $exe -Argument "sync --days 3 --log `"$log`"" -WorkingDirectory "$env:LOCALAPPDATA\GarminSync"
$trigger  = New-ScheduledTaskTrigger -Daily -At 06:00
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RunOnlyIfNetworkAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 45) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'GarminSync' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force
```

The OAuth1 token lasts about a year. The bearer token it mints lasts about a day and is renewed automatically; on MFA accounts Garmin rejects the `mfa_token` saved at login on later renewals (403 "The provided MFA token was invalid"), so `gsync` retries the exchange without it and drops it (before v0.1.2 this broke every nightly run from the second day on). If Garmin refuses the saved login outright, `gsync sync` stops after the first refusal, the log ends with `LOGIN REQUIRED`, and the exit code is 3 (the task's *Last Run Result* shows `0x3`). Run `gsync login`, then `gsync sync --from <last good day>` to fill the gap, since the nightly job only looks back 3 days.

Exit codes: 0 ok, 1 some fetches failed, 2 usage error, 3 login required.

### Getting files off the watch

Recent watches (fenix 7 and similar) connect over **MTP**, not as a drive letter, so `gsync import` cannot read them directly. Copy the folders out through the Windows Shell first — this PowerShell snippet pulls `Activity`, `Monitor` and `SUMMARY` into the data directory, after which `gsync import` on that folder does the rest:

```powershell
$sh = New-Object -ComObject Shell.Application
$dev = $sh.NameSpace(17).Items() | Where-Object Name -eq 'fenix 7'
$g = ($dev.GetFolder.Items() | Select-Object -First 1).GetFolder.Items() | Where-Object Name -eq 'GARMIN'
$base = Join-Path $env:LOCALAPPDATA 'GarminSync\data\fit\watch\fenix7'
foreach ($name in 'Activity','Monitor','SUMMARY') {
  $src = $g.GetFolder.Items() | Where-Object Name -eq $name
  $dst = Join-Path $base $name; New-Item -ItemType Directory -Force $dst | Out-Null
  $sh.NameSpace($dst).CopyHere($src.GetFolder.Items(), 16 + 4 + 1024)   # async; wait for the file count
}
gsync import $base
```

`Sleep`, `Metrics` and `HRVStatus` on the watch are usually empty because the watch purges them once Garmin Connect has them; `gsync sync` fetches the same files from Connect as the daily wellness zips.

Tokens live in `%LOCALAPPDATA%\GarminSync\tokens.bin`, encrypted with DPAPI to the current Windows user. The password is never written anywhere. The OAuth1 token is good for about a year; bearer tokens are re-minted from it automatically.

## Data layout

```text
<data>\garmin.db                       SQLite (WAL)
<data>\json\<kind>\<date>.json         raw API responses, kept for re-import / debugging
<data>\fit\activities\<id>_ACTIVITY.fit
<data>\fit\wellness\<date>\*.fit       daily monitoring / sleep / HRV files
```

### Tables

All timestamps are Unix seconds (UTC). `source` distinguishes `api` (Garmin Connect JSON) from `fit` (decoded from FIT files) where both exist — the FIT data is the higher-resolution truth, the API data is Garmin's processed view.

| Table | Grain | Notes |
|---|---|---|
| `daily_summary` | day | steps, resting/min/max HR, stress, Body Battery hi/lo, calories, sleep seconds |
| `hr_sample` | ~2 min (api) / 1 min–1 s (fit) | heart rate |
| `stress_sample` | 3 min | stress level 0–100 |
| `body_battery_sample` | 3 min | Body Battery 0–100 |
| `sleep`, `sleep_stage`, `sleep_level_sample` | night / interval / sample | summary, API stage intervals, FIT sleep levels |
| `respiration_sample`, `spo2_sample` | sample | from wellness FIT |
| `hrv_daily`, `hrv_sample` | night / 5 min | RMSSD summary and readings |
| `weight` | measurement | Index scale: weight, BMI, body fat/water %, bone/muscle mass, visceral fat, metabolic age |
| `blood_pressure` | reading | Omron cuff readings from [OmronBP](https://github.com/Flinterpop/OmronBP) CSV exports (`readings_*.csv` in Downloads): systolic, diastolic, pulse, cuff user slot, device |
| `activity` | activity | Connect summary; `fit_file_id` links to the decoded FIT |
| `activity_session`, `activity_lap`, `activity_record`, `activity_hrv` | per FIT file | 1 Hz records (position, altitude, HR, cadence, speed, power, temperature), laps, R-R intervals |
| `fit_file` | file | provenance: path, type, device, time created |
| `sync_log` | kind × day | which days are already fetched (today is always refetched) |

## Code health

The code follows NASA/JPL's Power of 10 as far as a desktop app sensibly can: every loop over data has a fixed upper bound, no recursion, assertions on preconditions (`G_ASSERT` stays on in Release), every return value checked, no function longer than a screen, single-level pointer indirection, `/W4 /WX`. `python tools/rot_scan.py` reports long functions, assertion-free functions, unbounded loops, TODO markers, dead declarations and dropped return values; it is run before each release and its output should stay empty apart from a handful of dispatch tables. The dependency set is four vcpkg ports (nlohmann-json, zlib, sqlite3, Catch2) plus the Windows SDK, so there is little to age.

## Code map

```text
src/util    assertions, time (FIT epoch), CNG/DPAPI/base64/percent-encoding, zip reader (zlib), console, files
src/fit     fit_types.h (protocol constants), fit_crc, fit_profile (message/field names + scaling), fit_decoder
src/gc      http_client (WinHTTP), oauth1 (RFC 5849 signing), token_store (DPAPI), gc_client (SSO login + endpoints)
src/store   db (SQLite wrapper + schema), importer (JSON and FIT -> rows)
src/plot    plot_types (Figure/Panel/Series model), ticks (nice numbers, local-time and elapsed axes), decimate (min/max per pixel column), plot_widget (Direct2D rendering, zoom/pan/hover), calendar_widget (month grid)
src/analysis hockey (shift detection, HR zones, per-game stats), ski (run/lift detection, per-run and per-day stats)
src/map     mercator (Web Mercator + tile maths), tile_cache (OSM download thread + disk cache), map_widget (Direct2D map)
apps        fitdump, gsync, gview (Win32 window; queries*.cpp build Figures / MonthData from garmin.db)
tests       Catch2: CRC, decoder (synthetic FIT files), OAuth1 (RFC test vectors), zip, time
```

Licensed under the [MIT License](LICENSE).

The plot engine is plain Win32: Direct2D + DirectWrite from the Windows SDK, no third-party UI library. Series are decimated to the min/max per pixel column before drawing, so a day of 1 Hz data or a multi-hour activity redraws instantly while dragging. Tick generation and decimation are pure functions with unit tests.

The FIT decoder is written from the protocol specification rather than wrapping Garmin's SDK: fixed-size state (16 local definitions), no heap use per message, compressed-timestamp and `timestamp_16` expansion, developer fields resolved through `field_description`, chained files, CRC verification. Messages not in the built-in profile subset still decode; they are just reported by number.

## Next steps

1. Live chest-strap HR over BLE (WinRT GATT heart-rate service) for treadmill and Wahoo KICKR sessions — next up.
2. Export the current view to PNG; auto-reload when the database changes.
3. Read the watch over MTP from `gsync` directly (Windows Portable Devices API) instead of the PowerShell copy step.
4. Backfill the years before 2024 (`gsync sync --from 2022-12-01`).
