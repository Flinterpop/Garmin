# Garmin

*Last updated: 18 Sep 2026*

Local, C++/Win32 tooling for pulling health data off Garmin devices and out of Garmin Connect, and keeping it in a SQLite database shaped for plotting. No Python, no cloud service of our own: everything runs on this machine and talks only to Garmin.

Three executables:

- **`gsync`** — signs in to Garmin Connect the way the mobile app does, pulls daily summaries, heart rate, sleep, stress / Body Battery, HRV, body composition (the Index scale), the activity list, per-activity FIT files and the daily wellness (monitoring) FIT files, and imports it all into `garmin.db`. Also imports FIT files copied straight off a watch over USB.
- **`fitdump`** — inspects a single FIT file: summary, per-message counts, session line, and optional full dump to stdout or a long-format CSV.
- **`gview`** — Win32 + Direct2D viewer over `garmin.db`: a Day view (heart rate, stress, Body Battery, sleep stages, respiration, HRV on one time axis), a Trends view (resting HR, HRV, sleep hours and score, weight, steps over 30 days to everything) and an Activity view (HR, speed, altitude, cadence, power, temperature against elapsed time, laps as markers). Wheel zooms, drag pans, hover shows every value at the cursor.

## Status

| Piece | State |
|---|---|
| FIT decoder (`src/fit`) | Done; verified against Garmin-native and Zwift activity files, 10 unit tests |
| Garmin Connect login + endpoints (`src/gc`) | Done; verified against a live account (login with MFA, all daily endpoints, weight, activities, wellness FIT zips) |
| SQLite store + importers (`src/store`) | Done; verified on live JSON and on 208 files copied off a fenix 7 |
| Plot viewer (`src/plot`, `apps/gview`) | Done; own Direct2D plot engine, verified on the live database |

Notes:

- The Connect API is Garmin's unofficial app API (the same one `garth` / `python-garminconnect` use). Garmin can change it without notice; when that happens `gsync get <path>` is the debugging tool.
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
gsync get /usersummary-service/usersummary/daily/<displayName>?calendarDate=2026-09-17

fitdump some.fit                # summary
fitdump some.fit --print        # every message
fitdump some.fit --csv out.csv  # mesg,timestamp,field,value,units

gview                           # opens the default database; --data <dir> for another
```

`gview` keys: `1` / `2` / `3` switch Day / Trends / Activity, `Up` / `Down` step through the list, mouse wheel zooms around the cursor, drag pans, `Home` fits, `+` / `-` zoom, `F5` reloads after a sync.

Options: `--data <dir>` (default `%LOCALAPPDATA%\GarminSync\data`), `--no-fit`, `--force`, `--out <file>`.

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
| `activity` | activity | Connect summary; `fit_file_id` links to the decoded FIT |
| `activity_session`, `activity_lap`, `activity_record`, `activity_hrv` | per FIT file | 1 Hz records (position, altitude, HR, cadence, speed, power, temperature), laps, R-R intervals |
| `fit_file` | file | provenance: path, type, device, time created |
| `sync_log` | kind × day | which days are already fetched (today is always refetched) |

## Code map

```text
src/util    assertions, time (FIT epoch), CNG/DPAPI/base64/percent-encoding, zip reader (zlib), console, files
src/fit     fit_types.h (protocol constants), fit_crc, fit_profile (message/field names + scaling), fit_decoder
src/gc      http_client (WinHTTP), oauth1 (RFC 5849 signing), token_store (DPAPI), gc_client (SSO login + endpoints)
src/store   db (SQLite wrapper + schema), importer (JSON and FIT -> rows)
src/plot    plot_types (Figure/Panel/Series model), ticks (nice numbers, local-time and elapsed axes), decimate (min/max per pixel column), plot_widget (Direct2D rendering, zoom/pan/hover)
apps        fitdump, gsync, gview (Win32 window + queries that build Figures from garmin.db)
tests       Catch2: CRC, decoder (synthetic FIT files), OAuth1 (RFC test vectors), zip, time
```

The plot engine is plain Win32: Direct2D + DirectWrite from the Windows SDK, no third-party UI library. Series are decimated to the min/max per pixel column before drawing, so a day of 1 Hz data or a multi-hour activity redraws instantly while dragging. Tick generation and decimation are pure functions with unit tests.

The FIT decoder is written from the protocol specification rather than wrapping Garmin's SDK: fixed-size state (16 local definitions), no heap use per message, compressed-timestamp and `timestamp_16` expansion, developer fields resolved through `field_description`, chained files, CRC verification. Messages not in the built-in profile subset still decode; they are just reported by number.

## Next steps

1. Backfill: `gsync sync --from 2022-12-01 --activities 500`.
2. Viewer: a week/month "calendar" view, activity map from the GPS records, comparing two activities, printing/exporting a panel to PNG.
3. Read the watch over MTP from `gsync` directly (Windows Portable Devices API) instead of the PowerShell copy step.
4. Live chest-strap HR over BLE (WinRT `GattCharacteristic`), writing to `hr_sample` with `source='ble'`.
