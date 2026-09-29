# Garmin

[![Release][release-badge]][release-latest] [![License: MIT][license-badge]](LICENSE)

[release-badge]: https://img.shields.io/badge/release-v0.1.5-blue
[release-latest]: https://github.com/Flinterpop/Garmin/releases/latest
[license-badge]: https://img.shields.io/badge/license-MIT-green

*Last updated: 29 Sep 2026*

<img src="docs/icon-256.png" alt="app icon" width="96" align="right" />

A Windows viewer for your own Garmin health data. It downloads everything Garmin Connect holds about your days, nights and activities (heart rate, sleep, stress and Body Battery, HRV, weight and body composition, every activity with its GPS track), keeps it in a local SQLite database, and shows it in ten views, from a single day to years of trends, hockey shifts, ski runs and a 3D map of any GPS activity. Everything runs on your PC: there is no account or cloud service of our own, and the only traffic goes to Garmin and to the map-tile servers.

<img width="2511" height="890" alt="image" src="https://github.com/user-attachments/assets/0f1d0201-dd61-40d7-854b-219ef89385ef" />

<img width="1380" height="890" alt="image" src="https://github.com/user-attachments/assets/f2ff7c5c-25ae-4d11-9169-01d8840a0df3" />

Written in C++20 on plain Win32, Direct2D and Direct3D 11, with no UI framework, no runtime to install and no Python.

## Getting started

1. Download `Garmin-vX.Y.Z-win64.zip` from [Releases][release-latest] and unzip it into a folder of your choice, for example `C:\GarminSync`. Windows 10 or 11, 64-bit.
2. Run **`gview.exe`**. The first time, it offers to **log in to Garmin Connect** (your email, password, and the security code Garmin sends if your account uses one) and downloads the last 30 days; or, if you used v0.1.3 or earlier, to **copy your data from the previous version**.
3. Tick **Data → Download new data every morning** so it stays current on its own.

No command line is needed for any of this. The folder also holds `gsync.exe`, which the morning download runs, and `fitdump.exe`, a FIT file inspector for the curious; you never have to start either.

## Views

Press the number key or pick from the **View** menu. The list on the left chooses the day, activity, game or period; the mouse wheel zooms, dragging pans, hovering shows every value at the cursor, `Home` fits.

| Key | View | Shows |
|---|---|---|
| `1` | Day | Heart rate, stress, Body Battery, sleep stages, respiration and HRV on one time axis |
| `2` | Trends | Resting HR, HRV, sleep, weight (lb), steps and blood pressure over 30 days to everything |
| `3` | Activity | HR, speed, altitude, cadence, power, temperature; laps as markers; the map beside the charts |
| `4` | Hockey | Shifts detected from the heart rate, HR zones, shift lengths, plus a season overview |
| `5` | Calendar | A month grid of steps, resting HR, Body Battery, sleep score and the day's activities |
| `6` | Map | A GPS activity on the chosen map layer, the track coloured by heart rate |
| `7` | Ski | Runs and lifts from the altitude profile: vertical, top speed and HR per run, plus a season |
| `8` | Compare | Two activities (Ctrl-click the second) overlaid |
| `9` | Sleep | One night: hypnogram, HR and HRV, respiration, SpO2, stress |
| `0` | 3D map | A GPS activity over real terrain, the map draped on it, the track as a heart-rate ribbon |

Notes:

- **Trends** add a trailing 30-day mean ± 1 σ band for resting HR, HRV and sleep hours and flag days beyond 2 σ; the blood-pressure panel appears once Omron CSV exports have been imported.
- **Activity**: with GPS, hovering the charts marks the moment on the map and hovering the track marks it on the charts.
- **3D map**: left-drag orbits, right-drag pans, wheel zooms, `Home` resets the camera, `E` cycles height exaggeration (×1 / 1.5 / 2 / 3). Needs Direct3D 11; without it the view falls back to the 2D map.
- **Other keys**: `Up` / `Down` step through the list, `+` / `-` zoom, `L` cycles the map layer, `F5` reloads, `F6` syncs.
- Weight is stored in kg and shown in pounds.

## The Data menu

| Item | What it does |
|---|---|
| **Sync now** (`F6`) | Downloads what is new since the last sync, with a progress window you can stop |
| **Download new data every morning** | A Windows scheduled task that syncs at 06:00, or as soon as the PC is on and online after that |
| **Log in to Garmin Connect…** | A fresh login, e.g. after Garmin stops accepting the saved one; fills the gap afterwards |
| **Profile** | Switch between people; **Add a person…**, **Log out…**, **Remove *name*…** |
| **Import from watch…** | Copies and imports the files of a watch plugged in by USB |
| **Import files…** | FIT files or Omron blood-pressure CSV exports from anywhere on the PC |
| **Copy data from the previous version…** | Brings in a v0.1.3-or-earlier install from AppData |
| **Map API keys…** | Keys for the map layers that need one (see [Map layers](#map-layers)) |

Notes:

- **More than one person.** **Profile → Add a person…** asks for a short name (letters, digits, `_`, `-`), their own Garmin login, and downloads their last 30 days. Each person has their own login and data; **Profile** switches between them, the choice is remembered, and the title bar shows who you are looking at. The morning download is set per person.
- **Log out** forgets the saved login but keeps the data. **Remove** deletes that person's login and downloaded data from this PC after a confirmation that says how much; their Garmin account is untouched. The main profile cannot be removed.
- **Import from watch** works with watches that connect like a phone (MTP, e.g. the fenix 7, no drive letter) as well as older ones that appear as a drive: unlock the watch if it asks. It copies `Activity`, `Monitor`, `SUMMARY`, `Sleep`, `Metrics` and `HRVStatus` from the watch's `GARMIN` folder into `data\fit\watch\<watch name>\` and imports every FIT file; files already imported are skipped. Syncing already fetches the same data from Garmin Connect, so this is for what Connect does not have.
- **Blood pressure**: CSVs named `readings_*.csv` in your Downloads folder (the export of [OmronBP](https://github.com/Flinterpop/OmronBP)) are imported on every sync; use **Import files…** for any saved elsewhere.
- **Copy data from the previous version** copies the login, database and map tiles; it never overwrites anything and never deletes the AppData original.

## Map layers

**View → Map layer** picks the base map for the Map, Activity and 3D views, plus optional route overlays; `L` cycles through the base maps you can use. The choice is remembered.

| Layer | Key needed | Notes |
|---|---|---|
| OpenStreetMap | no | default |
| CyclOSM | no | cycling: lanes, routes, surfaces |
| OpenTopoMap | no | contours and trails; zoom 17 max |
| Esri World Imagery, Esri World Topo | no | Esri's terms formally expect an ArcGIS account |
| Thunderforest OpenCycleMap, Outdoors | `thunderforest` | free hobby key from thunderforest.com |
| Google Maps, Satellite, Terrain | `google` | Map Tiles API key (Google Cloud, billing enabled); not stored on disk |
| Azure Maps road, imagery | `azure_maps` | Bing Maps' successor; Azure Maps key; not stored on disk |
| Hiking / cycling routes overlay | no | Waymarked Trails, drawn over any base map |

Enter keys with **Data → Map API keys…** (masked unless *Show keys* is ticked; an empty field removes a key). They take effect at once and are saved in `gview.ini` beside `gview.exe`, which you can also edit by hand; layers whose key is missing are greyed out.

```ini
[keys]
google = your-google-map-tiles-api-key
azure_maps = your-azure-maps-key
thunderforest = your-thunderforest-key

[map]
base = osm
overlays = wmt_hiking
```

**`gview.ini` holds your keys: keep it private.** It is in `.gitignore`, and the release zip never contains it.

## Where things are kept

Everything lives **in the folder that holds the programs**; nothing goes to AppData or depends on the Windows account. Copy the folder to move the whole install.

```text
<folder>\gview.exe, gsync.exe, fitdump.exe
<folder>\gview.ini                       map layers, API keys, last person chosen (keep private)
<folder>\tokens.bin                      main profile: Garmin login (encrypted to this PC)
<folder>\data\                           main profile: database and downloaded files
<folder>\profiles\<name>\tokens.bin      another person's login
<folder>\profiles\<name>\data\           and their database
<folder>\tiles\                          map tile cache, shared by everyone
<folder>\sync.log                        what the morning download did
```

Notes:

- **The login** in `tokens.bin` is encrypted with Windows DPAPI to this PC: any Windows user here can use the folder, a copy on another PC cannot and needs a fresh login. Your password is never stored; it goes only to Garmin.
- **Network traffic** goes to Garmin Connect and to the tile servers of the chosen map layer; the 3D map also fetches elevation from AWS Terrain Tiles (`s3.amazonaws.com/elevation-tiles-prod`, public, no key). Every tile server gets an identifying User-Agent, at most two connections and only the tiles on screen, as the OpenStreetMap tile policy asks; tiles are cached in `tiles\` except where the provider's terms forbid it (Google, Azure Maps).

## Command line

`gsync.exe` does everything the Data menu does, for scripting and for the morning task:

```text
gsync login                     # email, password, MFA code; login saved beside gsync.exe
gsync --profile ann login       # another person's own login and database
gsync profiles                  # who is set up
gsync whoami
gsync sync --days 30            # daily data, wellness FIT files and activities for the last 30 days
gsync sync --from 2026-01-01 --to 2026-03-31 --activities 200
gsync sync --days 3 --log sync.log                  # what the morning task runs
gsync import-watch              # copy and import from a USB-connected watch
gsync import <folder or .fit>   # FIT files you already have
gsync import-bp [<csv>...]      # Omron CSVs (default: readings_*.csv in Downloads)
gsync migrate-appdata           # copy a v0.1.3-or-earlier install from AppData
gsync logout
gsync stats
gsync get /usersummary-service/usersummary/daily/<displayName>?calendarDate=2026-09-17

fitdump some.fit                # summary
fitdump some.fit --print        # every message
fitdump some.fit --csv out.csv  # mesg,timestamp,field,value,units

gview --profile <name>          # open a given person; --data <dir> opens a database folder directly
```

Notes:

- Options: `--profile <name>` (before or after the command), `--data <dir>` (default `data\` in the profile folder), `--no-fit`, `--force`, `--out <file>`, `--log <file>`.
- Exit codes: 0 ok, 1 some fetches failed, 2 usage error, 3 login required.
- **Login lifetime.** The saved OAuth1 token lasts about a year; the bearer token it mints lasts about a day and is renewed automatically. If Garmin refuses the saved login outright, a sync stops after the first refusal, the log ends with `LOGIN REQUIRED` and the exit code is 3 (the task's *Last Run Result* shows `0x3`); log in again from the Data menu, which also downloads everything since the last good day.
- **The morning task by hand**, equivalent to the Data-menu tick box (for another person add `--profile <name>`, their own log name and a task name such as `GarminSync-<name>`):

```powershell
$dir = 'C:\GarminSync'   # the folder holding gsync.exe
$exe = Join-Path $dir 'gsync.exe'; $log = Join-Path $dir 'sync.log'
$action   = New-ScheduledTaskAction -Execute $exe -Argument "sync --days 3 --log `"$log`"" -WorkingDirectory $dir
$trigger  = New-ScheduledTaskTrigger -Daily -At 06:00
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RunOnlyIfNetworkAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 45) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'GarminSync' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force
```

## How it works

- **Garmin Connect.** `gsync` and `gview` sign in the way Garmin's mobile app does (SSO ticket → OAuth1 token → OAuth2 bearer, with MFA) and use the same unofficial app API as `garth` and `python-garminconnect`. Garmin can change it without notice; `gsync get <path>` is the debugging tool when that happens. The OAuth consumer key pair comes from the public location `garth` publishes, or from `GARMIN_OAUTH_CONSUMER_KEY` / `GARMIN_OAUTH_CONSUMER_SECRET`. The wellness-zip endpoint often answers Cloudflare 504 for older dates, so 5xx answers are retried with back-off and failed days are left unmarked for the next sync. On MFA accounts Garmin rejects the security token saved at login on later renewals, so renewal retries without it.
- **FIT decoder**, written from the protocol specification rather than wrapping Garmin's SDK: fixed-size state (16 local definitions), no heap use per message, compressed-timestamp and `timestamp_16` expansion, developer fields resolved through `field_description`, chained files, CRC verification. Messages outside the built-in profile subset still decode and are reported by number. Verified on Garmin-native and Zwift activity files and on 208 files copied off a fenix 7.
- **Plot engine**: Direct2D and DirectWrite, no third-party UI library. Series are decimated to the min/max per pixel column before drawing, so a day of 1 Hz data or a multi-hour activity redraws instantly while dragging. Tick generation and decimation are pure functions with unit tests.
- **3D map**: Direct3D 11. Heights come from AWS Terrain Tiles (Terrarium PNGs) sampled into a 129 × 129 grid over the activity; the chosen map layer is draped on it as one mipmapped texture with anisotropic filtering; the track is a ribbon lifted just above the ground. The geometry (height decoding, local frame, mesh, ribbon, orbit camera) is pure and unit-tested.
- **Hockey shifts** are found in the smoothed heart-rate trace, since HR climbs on the ice and falls on the bench: a shift is a rising leg (12 bpm hysteresis) lasting 20–330 s that climbs at least 20 bpm and peaks above a quarter of the way from the game's median HR to its 95th percentile, which leaves out the warm-up skate. Zones are 60/70/80/90 % of a robust HR max (the 95th percentile of per-game maxima).
- **Ski runs and lifts** come from the vertical speed of the barometric altitude: sustained descents of at least 25 m are runs, sustained ascents are lifts, and a pause at the bottom ends a run.
- **Sessions** recorded twice (the watch copy and the Connect download) are de-duplicated by start time, preferring the copy with records.

## Data layout

```text
<data>\garmin.db                       SQLite (WAL)
<data>\json\<kind>\<date>.json         raw API responses, kept for re-import / debugging
<data>\fit\activities\<id>_ACTIVITY.fit
<data>\fit\wellness\<date>\*.fit       daily monitoring / sleep / HRV files
<data>\fit\watch\<watch name>\         files imported from a watch over USB
```

All timestamps are Unix seconds (UTC). `source` distinguishes `api` (Garmin Connect JSON) from `fit` (decoded from FIT files) where both exist: the FIT data is the higher-resolution truth, the API data is Garmin's processed view.

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
| `blood_pressure` | reading | Omron cuff: systolic, diastolic, pulse, cuff user slot, device |
| `activity` | activity | Connect summary; `fit_file_id` links to the decoded FIT |
| `activity_session`, `activity_lap`, `activity_record`, `activity_hrv` | per FIT file | 1 Hz records (position, altitude, HR, cadence, speed, power, temperature), laps, R-R intervals |
| `fit_file` | file | provenance: path, type, device, time created |
| `sync_log` | kind × day | which days are already fetched (today is always refetched) |

## Building from source

Requirements: Visual Studio 2026 (MSVC), CMake 3.25+, vcpkg at `C:\vcpkg` with the `x64-windows-static` ports `nlohmann-json`, `zlib`, `sqlite3`, `catch2`.

```powershell
cmake --preset msvc-static
cmake --build --preset release      # build\apps\Release\{gview,gsync,fitdump}.exe
ctest --preset debug                # after cmake --build --preset debug
```

Everything compiles under `/W4 /WX` with the static CRT; there are no runtime DLL dependencies beyond Windows itself. The programs keep their data beside themselves, so a build folder starts empty: run `gview.exe` from there and it offers the same first-run setup.

The code follows NASA/JPL's Power of 10 as far as a desktop app sensibly can: every loop over data has a fixed upper bound, no recursion, assertions on preconditions (`G_ASSERT` stays on in Release), every return value checked, no function longer than a screen, single-level pointer indirection. `python tools/rot_scan.py` reports long functions, assertion-free functions, unbounded loops, TODO markers, dead declarations and dropped return values; it runs before each release and should list nothing beyond a few dispatch tables. The dependency set is four vcpkg ports plus the Windows SDK, so there is little to age.

```text
src/util     assertions, time (FIT epoch), CNG/DPAPI/base64/percent-encoding, zip reader (zlib), console, files and profile folders
src/fit      fit_types.h (protocol constants), fit_crc, fit_profile (message/field names + scaling), fit_decoder
src/gc       http_client (WinHTTP), oauth1 (RFC 5849 signing), token_store (DPAPI), gc_client (SSO login + endpoints)
src/store    db (SQLite wrapper + schema), importer (JSON and FIT -> rows)
src/sync     engine (the sync, cancellable, progress via callback), account (login, migration, catch-up range), files (FIT/CSV import), watch (MTP copy through the Shell)
src/plot     plot_types (Figure/Panel/Series model), ticks, decimate, plot_widget (Direct2D, zoom/pan/hover), calendar_widget
src/map      mercator, tile_provider (the layer table), map_settings (gview.ini), tile_cache (2 download workers), hr_color, map_widget
src/map3d    terrain (pure geometry), renderer (Direct3D 11), view3d (child window, tiles, camera), image_decode (WIC)
src/analysis hockey (shifts, zones, per-game stats), ski (runs and lifts, per-run and per-day stats)
apps/gview   main (window, views, menus), queries*.cpp (a Figure per view), sync_ui (login, MFA, progress, welcome, add-person dialogs), schedule (morning task), layer_menu, keys_dialog
apps/gsync   command line over src/sync
apps/fitdump FIT inspector
tests        Catch2, one file per module: FIT CRC and decoder, OAuth1 vectors, Connect client, zip, time, ticks, decimation, hockey, ski, Mercator, map tracks, tile providers, 3D terrain, profiles, sync, imports, blood pressure, JSON importers
```

## Next steps

1. Live chest-strap heart rate over Bluetooth LE (WinRT GATT heart-rate service) for treadmill and Wahoo KICKR sessions.
2. Export the current view to PNG; reload automatically when the database changes.
3. Download an arbitrary date range from `gview` (today: `gsync sync --from`), e.g. to backfill the years before 2024.
4. 3D map: hover readout linked to the charts, and the route overlays draped on the terrain.

Licensed under the [MIT License](LICENSE).
