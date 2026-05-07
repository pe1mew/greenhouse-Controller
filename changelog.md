# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.16.25] — 2026-05-07

*Add per-key validation bounds to all integer config parameters received via Q4.*

### Added
- `firmware/src/data_manager/data_manager.cpp` — new `cfg_clamp()` function and `CFG_MIN_*` / `CFG_MAX_*` constants enforce valid ranges on every integer config value before it is written to NVS or applied to the in-RAM shadow.  Out-of-range values are silently clamped to the nearest bound; a `LOGW` line is emitted so the operator can see the correction.  The clamp runs inside `apply_config_update()`, before the NVS write, so neither storage nor the running config can hold an illegal value.

  Bounds defined for: `t_max/min_day/ngt`, `rh_max/min_day/ngt`, `hyst_t` (min 2), `hyst_rh` (min 2), `avg_win_t/rh` (min 1, max 30), `v_max` (min 1), `dir_excl_low/high` (0–359), `travel_m1/m2/m3` (5–300 s), `dwell_open_m1/m2/m3` (0–600 s), `dwell_close_m1/m2/m3` (0–300 s), `poll_interval_s` (30–300 s), `session_timeout_min` / `ap_timeout_min` (1–1440 min).

  Key oscillation guards: `hyst_t ≥ 2` prevents the step-width from collapsing to zero in the climate control algorithm; `poll_interval_s ≥ 30` prevents excessive relay actuation frequency.

---

## [1.16.24] — 2026-05-07

*Fix RGB LED night dimming: replace setBrightness() loop calls with direct colour-component scaling.*

### Fixed
- `firmware/src/main.cpp` — `setBrightness()` is documented for one-time initialisation only; calling it every 500 ms T1 tick re-scales the internal NeoPixel pixel buffer on every day↔night brightness change, degrading stored values through lossy integer arithmetic and producing inconsistent output. Fixed by setting `setBrightness(255)` once at startup and scaling the R/G/B components manually before each `setPixelColor()` call (`channel = (raw × dim) >> 8`). With the internal brightness fixed at 255, NeoPixel stores values unmodified and the LED output matches the computed scale exactly. Night schedule and brightness levels are unchanged: 22:00–06:00 local time at brightness 20; daytime at brightness 200.

---

## [1.16.23] — 2026-05-07

*Promote optimised anti-oscillation parameters to firmware defaults.*

### Changed
- `firmware/src/data_manager/data_manager.cpp` — updated compile-time defaults to match `simulation/settings_optimised.json`:
  - `DEF_HYST_T` 3 → 5 (wider temperature dead band)
  - `DEF_AVG_WIN_T` 3 → 6 (longer averaging window to smooth thermal spikes)
  - `DEF_DWELL_OPEN_S` 120 → 300 (5 min minimum open time; now effective following the v1.16.22 CMD_CLOSE fix)

  These defaults apply on a fresh flash or after an NVS reset.  Existing devices retain their NVS-stored values; update via the web GUI if needed.

---

## [1.16.22] — 2026-05-07

*Fix window oscillation: use CMD_CLOSE (dwell-respecting) instead of CMD_CLOSE_ALL for normal step→0 transitions in climate control.*

### Fixed
- `firmware/src/climate_control/climate_control.cpp` — `apply_step_delta()` previously issued `CMD_CLOSE_ALL` whenever `new_step == 0`.  `CMD_CLOSE_ALL` zeroes the per-channel dwell deadline in T2 (relay controller), bypassing `dwell_open_s` entirely and causing rapid oscillation when indoor temperature rebounds after ventilation.  Changed to issue per-channel `CMD_CLOSE` for all climate-control step transitions, including full close (step → 0).  `CMD_CLOSE` respects `dwell_open_s`, so windows remain open for at least the configured minimum time before closing.  `CMD_CLOSE_ALL` is now reserved exclusively for safety events (wind override in T3, motor alarm / calibration in T2).

### Recommended settings change (apply via GUI)
Together with the firmware fix the following NVS parameter changes eliminate oscillation in simulation across all test scenarios:

| Parameter | Old default | New recommended | Reason |
|---|---|---|---|
| `hyst_t` | 3 | 5 | Wider dead band; close-guard requires T_avg to drop 5 °C below t_max before closing |
| `avg_win_t` | 3 min | 6 min | Longer averaging window prevents a single cooled-air reading from instantly clearing the close guard |
| `dwell_open_m1/2/3` | 120 s | 300 s | Now effective (CMD_CLOSE respects dwell); 5 min minimum open time prevents short re-close cycles |

Simulation results with these three changes + firmware fix (S1 Daytime Solar Gain scenario):

| Metric | v1.16.19 defaults | Optimised | Improvement |
|---|---|---|---|
| M1 open/close cycles (24 h) | 24 | 1 | −96 % |
| Peak indoor temperature | 49.4 °C | 35.7 °C | −13.7 °C |
| Time T within t_max + 2 °C | 66.5 % | 70.9 % | +4.4 pp |
| Total actuations | 58 | 6 | −90 % |

---

## [1.16.21] — 2026-05-07

*Fix timezone not applied when changed via web GUI — clock remained in old zone until reboot.*

### Fixed
- `firmware/src/web_server/web_server.cpp` — `POST /api/config` with `key = "tz_str"` now calls `setenv("TZ", str_value, 1)` + `tzset()` immediately after writing to NVS.  Previously the new POSIX TZ string was persisted to flash but the C-library timezone was only updated on the next reboot (from `data_manager.cpp::nvs_load_system()`), so `localtime_r` kept formatting timestamps in the old zone.

---

## [1.16.20] — 2026-05-07

*Fix session idle timeout: background polls were silently preventing the timeout from ever firing.*

### Fixed
- `firmware/src/web_server/web_server.cpp` — Added `session_find_peek()`: a non-sliding variant of `session_find()` that checks session validity without resetting the expiry deadline. `/api/whoami` now uses `session_find_peek` so the browser's 60 s probe call no longer keeps the session alive.
- `firmware/data/app.js` — Added client-side idle timer (`g_last_activity` / `g_session_timeout_ms`). Real user gestures (click, keydown, touchstart) update `g_last_activity`. The periodic session-check interval now calls `doLogout()` when the user has been idle for `session_timeout_min` minutes, and skips the `/api/whoami` probe. `g_session_timeout_ms` is updated from `cfg.session_timeout_min` each time the config is loaded.
- `firmware/data/app.js` — The background `loadConfig()` poll (every 60 s) is now gated: it only fires while the user is active (`Date.now() − g_last_activity < g_session_timeout_ms`). This stops config polling from silently extending the server-side session when nobody is at the keyboard.

---

## [1.16.19] — 2026-05-07

*Hide T min day / T min night sliders in web GUI — heating control not yet implemented.*

### Changed
- `firmware/data/index.html` — **T min day** and **T min night** slider rows commented out with `<!-- HEATING CONTROL NOT IMPLEMENTED — preserved for future use -->`. The NVS keys (`t_min_day`, `t_min_ngt`) remain in firmware and continue to be stored; the sliders are only hidden from the UI.
- `firmware/data/app.js` — Corresponding `setVal()` calls and `linkAllSliders` entries commented out with the same note.

---

## [1.16.18] — 2026-05-07

*Fix dwell_open unit: macro renamed and value corrected from 2 (seconds) to 120 (seconds = 2 min).*

### Fixed
- `firmware/src/data_manager/data_manager.cpp` — **`DEF_DWELL_OPEN_MIN` unit bug**: T2 stores and reads the dwell NVS value in **seconds** (multiplies by 1 000 ms on load). The macro was named `DEF_DWELL_OPEN_MIN` with value `2`, which T2 interpreted as 2 seconds — not the intended 2 minutes. Renamed to `DEF_DWELL_OPEN_S` and corrected to `120` (120 s = 2 min). `DEF_DWELL_CLOSE_MIN` renamed to `DEF_DWELL_CLOSE_S` (value remains 0) for consistency.

---

## [1.16.17] — 2026-05-07

*Factory defaults updated to general-crop greenhouse values.*

### Changed
- `firmware/src/data_manager/data_manager.cpp` — **Climate defaults**: `t_max_day` 26→28 °C, `t_max_ngt` 22→20 °C, `t_min_day` 15→16 °C, `t_min_ngt` 12→14 °C; `rh_max_day` 80→75 %, `rh_max_ngt` 85→80 %, `rh_min_day` 40→50 %, `rh_min_ngt` 50→55 %; `hyst_t` 2→3 °C; `avg_win_t` 1→3 min, `avg_win_rh` 1→5 min.
- `firmware/src/data_manager/data_manager.cpp` — **Wind default**: `v_max` 7→6 m/s (Beaufort 4 onset with margin).
- `firmware/src/data_manager/data_manager.cpp` — **Motor dwell default**: `dwell_open_m1/m2/m3` 0→2 min; prevents immediate re-close after window opens.
- `firmware/src/data_manager/data_manager.cpp` — **Poll interval default**: `poll_interval` 30→60 s; reduces relay wear without meaningful loss of responsiveness.

> **Note:** factory defaults only apply on first boot (empty NVS) or after an NVS erase. Existing installations retain their current NVS values and must be updated manually via the web GUI or a full NVS erase.

---

## [1.16.16] — 2026-05-07

*24-hour periodic NTP resync added.*

### Added
- `firmware/src/network_manager/network_manager.cpp` — **Periodic NTP resync**: while in `NET_RUNNING` state the firmware re-runs `configTime()` / NTP wait once every 24 hours (`NTP_RESYNC_INTERVAL_S = 86400`). On success, T4 is notified (TN4) and writes the updated time to the DS1307 RTC. Geo/TZ lookup is intentionally skipped on periodic resyncs (location is stable); only the initial WiFi-connect sync fetches geo data.

---

## [1.16.15] — 2026-05-07

*OTA flow hardened: manual two-step upload, STORE-only ZIP enforcement, no auto-upload, live status polling fixed.*

### Fixed
- `firmware/data/app.js` — **OTA status panel not updating**: `loadOtaStatus()` is now called each time the System tab is opened, so the panel always reflects current device state immediately.
- `firmware/data/app.js` — **"not yet accepted" never clearing**: after an OTA reboot the panel now keeps polling every 5 s until `accepted` flips to `true` (~35 s), then stops. Previously polling stopped as soon as state returned to `idle`.
- `firmware/data/app.js` — **`fw_done` progress label**: status text while waiting for assets changed from "Firmware ready — uploading assets…" to "Firmware ready — please upload the web assets ZIP" to match the new manual flow.

### Changed
- `firmware/data/app.js` — **Removed auto-upload of assets**: after firmware upload succeeds, the assets ZIP is no longer uploaded automatically even if it is already selected. Both uploads are now fully manual (firmware first, then assets as a separate step).
- `bin/build_release.ps1` — confirmed as the canonical release builder; always used for all version packages. Web-assets ZIP uses STORE (method=0) enforced by the script's raw ZIP writer and verified before output.

---

## [1.16.14] — 2026-05-07

*Auto-upload of web assets removed; STORE-only ZIP required by on-device extractor.*

### Changed
- `firmware/data/app.js` — **Removed auto-upload**: `uploadOtaFirmware()` no longer calls `uploadOtaAssets()` automatically when an assets file is pre-selected. Status message updated to prompt the user to upload assets manually.

### Fixed
- `bin/build_release.ps1` — **STORE-only ZIP**: `Compress-Archive` (DEFLATE, method=8) replaced by a raw ZIP writer using PowerShell's `MemoryStream`; all entries use method=0 (STORE). The on-device extractor rejects method=8 with "compressed ZIP entry (method 8) is not supported".

---

## [1.16.13] — 2026-05-07

*OTA WDT crash eliminated; fallback reboot timer removed; stale-asset concern resolved.*

### Fixed
- `firmware/src/ota_manager/ota_manager.cpp` — **Task Watchdog crash**: `esp_partition_erase_range()` on the 1 MB inactive LittleFS partition took ~12 s, triggering the ESP32-S3 TWDT (~5 s default) and rebooting the device before web assets could be written. Removed entirely from T13. The partition is now simply unmounted then remounted; `littlefs_write()` truncates files in-place so no pre-erase is needed.
- `firmware/src/ota_manager/ota_manager.cpp` — **Premature reboot (fallback timer)**: the 120 s `fallback_reboot_cb` FreeRTOS timer in `ota_firmware_end()` fired if assets were not uploaded within 2 minutes. Removed the timer variable, callback, create/start call, and the cancel call in `ota_assets_begin()`.
- `firmware/src/ota_manager/ota_manager.cpp` — **False 100 % progress after firmware upload**: `s_progress = 100` in `ota_firmware_end()` changed to `s_progress = 0` so the bar correctly resets before the assets phase.
- `firmware/src/ota_manager/ota_manager.cpp` — **C++ goto jump-over-initialization**: hoisted `lfs_status_t lfs_st;` declaration above the format guard to satisfy the C++ rule that forbids jumping over a variable initialisation.
- `firmware/data/app.js` — **`rebooting` state not polled**: `'rebooting'` added to `OTA_ACTIVE_STATES` so the 2 s poll continues through the reboot transition.
- `firmware/data/app.js` — **Connection-loss during reboot**: `.catch()` handler added to `loadOtaStatus()` to display "Rebooting — reload the page once the device comes back online" when the fetch fails during device restart.

### Added
- `drivers/littleFS/src/littlefs_storage.cpp` — `littlefs_format()`: lightweight format using `fs.begin(true)` + `fs.format()` + `fs.end()`. Kept in the driver API for future use; not called from T13 in this release.
- `drivers/littleFS/src/littlefs_storage.h` — `littlefs_format()` declaration and Doxygen documentation added to the public API.

---

## [1.16.7] — 2026-05-07

*SD card logging overhauled: timestamp-based file names, ISO 8601 CSV timestamps, proactive free-space guard, local-time filenames, and automatic remount on card insertion.*

### Added
- `firmware/src/event_logger/event_logger.cpp` — **SD automount**: T9 main loop now wakes every 60 s when SD is absent and calls `event_logger_sd_remount()`, so a card inserted after boot is picked up automatically within one minute. When SD is active the task blocks indefinitely as before (no polling overhead).
- `firmware/src/event_logger/event_logger.cpp` — **Proactive free-space guard** (`check_free_space()`): called after every rotation. If free space drops below 2 MB and the file count is above the 3-file retention floor, the oldest file is deleted to reclaim space. If already at the floor, SD logging is suspended and a `LOG_SYSTEM` event with `value_a = −2` is emitted.
- `firmware/src/event_logger/event_logger.cpp` — **Write-failure reclaim**: on `STORAGE_ERR_FULL` / `STORAGE_ERR_IO`, a single oldest-file deletion is attempted and the write retried before falling back to NVS-only mode.

### Changed
- `firmware/src/event_logger/event_logger.cpp` — **Timestamp-based SD file naming**: files are now named `YYYYMMDDHHMMSS.csv` (local time of creation) instead of the previous sequential-index scheme (`ghc_NNNN.csv`). Lexicographic sort equals chronological order. Old `ghc_*` files are silently ignored via `is_ts_filename()` filter.
- `firmware/src/event_logger/event_logger.cpp` — **SD filename uses local time**: `make_ts_filename()` calls `localtime_r()` so filenames are human-readable without timezone conversion when browsing the card directly.
- `firmware/src/event_logger/event_logger.cpp` — **ISO 8601 CSV timestamps**: `build_csv_line()` now formats the timestamp as `YYYY-MM-DDTHH:MM:SS` (UTC) via `gmtime_r()` + `strftime()` instead of a raw Unix epoch integer.
- `firmware/src/web_server/web_server.cpp` — **ISO 8601 NVS export**: `/api/log/download?src=nvs` CSV timestamps updated to ISO 8601 format, matching SD output.
- `firmware/src/web_server/web_server.cpp` — **Sorted SD file list**: `/api/log/files` now returns SD filenames sorted lexicographically (oldest → newest) via an in-place bubble sort before building the JSON response.
- `firmware/src/event_logger/event_logger.h` — file naming and CSV format documentation updated.
- `design/technicalSoftwareDesignSpecification.md` — §5.3 updated: timestamp file naming (local time), ISO 8601 CSV format, rotation procedure, free-space guard, startup scan behaviour. Version 0.2 → 0.3.
- `design/functionalRequirementsSpecification.md` — FR-S03, FR-CF07 poll interval range updated to 15–120 s (default 30 s); FR-LG06 worst-case budget recalculated for 15 s minimum poll (400 entries). Version 0.3 → 0.4.

---

## [test/3.4] — 2026-05-07

*Automated test suite `3_4_Conflict_Resolution.py` completed and passing: all 7 §3.4 test cases verified on hardware (UT-CC-020, UT-CC-021, UT-CC-022a/b, UT-CC-030, UT-CC-031a/b). All four branches of `vent_resolve_conflict()` exercised — Rule 2 (both open → max), Rule 3 (both close → equal), Rule 4 CR_TEMP_FIRST, CR_RH_FIRST, and CR_DEVIATION. One script defect identified and fixed in run 1; firmware was correct throughout.*

### Added
- `test/3_4_Conflict_Resolution.py` — new automated test script for §3.4 Conflict Resolution. Covers all five test-plan cases (with UT-CC-022 and UT-CC-031 each split into two sub-cases). All lessons learned from `3_3_Setpoints_and_Hysteresis.py` are applied: `force_windows_closed()` before every opening/closing test, `push_and_verify_sensor()` for all sensor commits, `windows_all_closing()` accepting `MOVING_CLOSE` for M3 tolerance, `WAIT_FOR_MOTOR_S = 45 s` uniformly, 401 re-auth inline in `write_config()`, guarded `finally` blocks in teardown, and 2-poll-cycle confirmation for negative assertions (CC-021, CC-030).
- `test/3_4_Conflict_Resolution.md` — documentation for the §3.4 test script: purpose, prerequisites, how to run, NVS test parameters, expected duration (~15 min), algorithm description with the four-rule table, mirror-test table (CC-020 ↔ CC-030; CC-021 ↔ CC-031a), and log file format example.

### Fixed
- `test/3_4_Conflict_Resolution.py` — **CC-031b assertion**: `wins[2] == "CLOSED"` changed to `wins[2] in ("CLOSED", "MOVING_CLOSE")` for M3. Step=2 correctly commands M3 to close; at `TEST_TRAVEL_S=5` the relay is only energised for 10 s while the FSM transition lag can leave M3 in `MOVING_CLOSE` at the polling window. Identical root cause to the CC-024 / CC-025–027 fix in `3_3_Setpoints_and_Hysteresis.py`.

### Changed
- `test/softwareTestResult.md` — §3.4 results updated (7/7 passed, run 2 2026-05-07 13:05–13:24); coverage table revised (CC: 24 PASS, 7 NOT EXECUTED; total: 129 PASS; UT rate 55%; overall pass rate 69%).

---

## [test/3.3] — 2026-05-07

*Automated test suite `3_3_Setpoints_and_Hysteresis.py` completed and passing: all 12 §3.3 test cases verified on hardware (UT-CC-014–019, UT-CC-024–029). Six script defects identified and fixed across five test runs; firmware behaviour was correct throughout.*

### Added
- `test/3_3_Setpoints_and_Hysteresis.py` — **`windows_all_closing()`** helper: accepts `CLOSED` or `MOVING_CLOSE` per window. Used by `force_windows_closed()` and the UT-CC-024 assertion to handle M3's physical travel time exceeding the 10 s relay pulse (`travel_m3` production default 171 s; test value 5 s → relay energised for only 10 s).

### Fixed
- `test/3_3_Setpoints_and_Hysteresis.py` — **`TEST_AVG_WIN`** corrected `1` → `0`. Value `1` means 1 minute, producing a 2-sample sliding window (`window_size = clamp(1×60/30, 1, 360) = 2`) instead of the intended single-sample immediate response. `0` → `clamp(0, 1, 360) = 1` sample.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`write_config()` 401 re-auth**: on `HTTP 401 Unauthorized`, the function now calls `do_login()` to restore the session and retries the write immediately (no sleep). Previously 401 was retried with a 3 s sleep (ineffective). The ~22-minute gap between `setup()` writes and `run_cc028`'s first write caused session expiry, which silently failed UT-CC-028 and skipped UT-CC-029 in earlier runs.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc028` finally block**: `set_daytime()` and `write_config()` calls wrapped in `try/except`. Previously an uncaught exception propagated out of `finally`, skipping UT-CC-029 entirely and suppressing the test summary print.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`setup()`** missing `rh_ctrl_en=1` write added. Without this, RH control was off at runtime and UT-CC-018/024 could not open windows via RH demand.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc018` / `run_cc024`** — `write_config(session, "climate", "cr_priority", 1)` (CR_RH_FIRST) added to each test's setup writes. With `t_max_day=40` and `T=10°C`, `vent_step_required_t()` returns step=0 (a genuine close vote); `vent_resolve_conflict()` rule 4 with CR_TEMP_FIRST (default) returned step_t=0, vetoing the RH open demand. Setting CR_RH_FIRST lets RH win the conflict. `teardown()` restores `cr_priority` via the `orig` config loop.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc019` setup**: `force_windows_closed()` added before the T=26°C open push; bare `push_sensors()` replaced with `push_and_verify_sensor()`. Previously the prior test (CC-018) left all windows open with stale T=10°C on the emulator; after CC-019 wrote `rh_ctrl_en=0` and `t_max_day=25`, the next firmware poll read T=10°C < close threshold 19°C and issued CLOSE_ALL before T=26°C was recognised.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`force_windows_closed()`**: success criterion changed from `windows_all_closed()` (requires all `CLOSED`) to `windows_all_closing()` (accepts `CLOSED` or `MOVING_CLOSE`). M3's physical travel outlasts the relay pulse at TEST_TRAVEL_S=5, so the helper no longer logs spurious "not fully closed" warnings or returns False when M3 is legitimately completing its close stroke.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`teardown()`** and **`write_config()`**: error handling added. `teardown()` uses an inner `_safe_write()` that catches exceptions and logs warnings, so a single 503 response no longer aborts the remaining restore writes. `write_config()` retries up to 3 times on transient HTTP/network errors (5xx, connection errors) with a 3 s wait; application-level rejections (`ok=false`) are never retried.
- `test/3_3_Setpoints_and_Hysteresis.py` — **Motor wait race**: all bare `time.sleep(TEST_TRAVEL_S + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S)` calls (15 s) replaced with `time.sleep(WAIT_FOR_MOTOR_S)` (45 s = `TEST_POLL_S + TEST_TRAVEL_S + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S`). The poll can fire anywhere within the 35 s sensor-confirmation window; the 15 s bare sleep was a race condition.

### Changed
- `test/3_3_Setpoints_and_Hysteresis.md` — `avg_win_t`/`avg_win_rh` table values updated `1` → `0`; explanation updated to reflect 0-minute → 1-sample immediate response.
- `test/softwareTestResult.md` — §3.3 results updated through run 5 (12/12 passed); UT-CC-018, UT-CC-019, UT-CC-024, UT-CC-028, UT-CC-029 evidence updated; coverage table revised (CC: 19 PASS, 0 FAIL; total: 124 PASS, 0 FAIL; UT rate 46%; pass rate 98%).

---

## [1.16.6] — 2026-05-06

*Sensor history table now shows newest readings at the top; sensor history stale/frozen bug fixed (always showed the 60 oldest entries); `dm_ring_count()` added.*

### Fixed
- `firmware/src/data_manager/data_manager.cpp` — **`dm_ring_count()`** added: thread-safe getter (MX3, 500 ms timeout) that returns the current number of valid entries in the ring buffer. Previously callers had no way to query this without taking MX3 themselves.
- `firmware/src/data_manager/data_manager.h` — **`dm_ring_count()` declaration** added to the public API header.
- `firmware/src/web_server/web_server.cpp` — **`/api/history` newest-n fix**: handler called `dm_ring_read(0, rows, n, &got)` which always returned the `n` **oldest** entries (logical offset 0 = oldest). After DM_RING_DEPTH (360) entries accumulate, these never change, so the history table appeared frozen. Fixed by calling `dm_ring_count()` and computing `offset = max(0, avail − n)` to fetch the `n` **newest** entries.
- `firmware/data/app.js` — **`loadHistory()` `.catch` added**: promise chain now has a `.catch(function(err){ console.warn(...) })` handler so network failures surface in the browser console instead of producing an unhandled rejection.

### Changed
- `firmware/data/app.js` — **Sensor history newest-at-top**: `data.rows.forEach(...)` changed to `data.rows.slice().reverse().forEach(...)`. The server returns rows oldest-first; reversing before rendering places the most recent reading at the top of the table and the oldest at the bottom.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.5` → `1.16.6`.

---

## [1.16.5] — 2026-05-06

*Motor alarm aborts window calibration immediately; web GUI Settings moved above Sensor history; tooltips added to Sensor history heading and Refresh button.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — **`calib_close_all()` alarm abort**: motor alarm was silently ignored for the full calibration duration (up to the full travel time of M3 ≈ 176 s). Added two checks: (1) an **entry guard** before any relay is energised — if the alarm pin is already LOW, calibration is skipped and `handle_alarm_onset()` is called immediately; (2) a **per-chunk pin check** inside the poll loop (every `CALIB_CHUNK_MS` = 400 ms) — if the pin goes LOW mid-calibration, `EG1_BIT_CALIBRATING` is cleared, `handle_alarm_onset()` is called (de-energises all relays, sets `EG1_BIT_MOTOR_ALARM`), and calibration returns. Maximum alarm response latency during calibration is now **400 ms**.
- `firmware/src/relay_controller/relay_controller.cpp` — **forward declaration** of `handle_alarm_onset` added before `calib_close_all` to resolve the out-of-order definition required by the above fix.

### Changed
- `firmware/data/index.html` — **Settings section reordered**: `<section id="section-settings">` moved to appear before the Sensor history section. Settings are now visible at the top of the page when logged in, without scrolling past the history table.
- `firmware/data/index.html` — **Sensor history tooltips**: `data-tip` added to the section heading (`"Logged sensor readings — one row per poll cycle…"`) and to the Refresh button (`"Fetch the latest sensor history… also refreshes automatically every 2 minutes"`), consistent with the existing CSS tooltip system used throughout the Status section.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.4` → `1.16.5`.

---

## [1.16.4] — 2026-05-06

*Motor alarm onset detection fix: re-assertion during the 60 s guard period is now detected within 5 s instead of after the full 60 s. Boot-time alarm-at-power-on is now detected and handled.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — **`handle_alarm_clearance()` guard loop**: moved the GPIO42 pin re-check from a single test **after** the full 60 s guard to a test **inside every 5 s chunk iteration**. When a re-assertion is detected mid-guard, `s_alarm_edge` is consumed and `handle_alarm_onset()` is called immediately, so the alarm appears on LCD, web GUI, and RED LED within ≤5 s rather than up to 60 s.  Root cause: T2 blocks in `vTaskDelay` inside the guard loop and cannot execute the main-loop debounce code while blocked; the only pin re-check was at guard expiry.
- `firmware/src/relay_controller/relay_controller.cpp` — **boot-time alarm check**: `attachInterrupt` uses CHANGE mode and does not fire for a pin that is already in the asserted (LOW) state at power-on.  Added an explicit `gpio_read(PIN_OPTO_INPUT)` immediately after `attachInterrupt`; if already LOW, `handle_alarm_onset()` is called and `calib_close_all()` is skipped (energising CLOSE relays onto a latched alarm relay is unsafe).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.3` → `1.16.4` in both `lolin_s3` and `test_t2_relay` environments.

---

## [1.16.3] — 2026-05-06

*LCD I2C bus reliability fix (AiP31068L silent-drop), LCD display polish, "Window Cal." mode on LCD and web GUI, and web GUI public-access redesign (Status + Sensor history + SD card without login; Login modal replaces full-page overlay).*

### Added
- `drivers/LCD1602_I2C/src/lcd1602.h/.cpp` — **`lcd_display_on()`**: sends CMD_DISP_ON (0x0C, ~37 µs busy) as an idempotent sacrificial preamble write before every `lcd_flush()`. Absorbs the AiP31068L silent first-transaction drop that occurs after ~2.5 s of I2C bus inactivity, without the 1.52 ms cursor-positioning side-effect of CMD_HOME that caused a 4-character row-0 shift artifact.
- `firmware/src/types/app_types.h` — **`EG1_BIT_CALIBRATING` (bit 6)**: new EG1 system-state flag; set/cleared by T2 around `calib_close_all()`. Highest display priority after MOTOR_ALARM and WIND_OVERRIDE. Allows all display consumers to detect the calibration window without polling relay state.
- `firmware/src/relay_controller/relay_controller.cpp` — `calib_close_all()` now **sets `EG1_BIT_CALIBRATING`** at entry (`CLOSE_ALL calibration start` log line) and **clears it** on completion (`CLOSE_ALL calibration complete` log line). Called at boot and after the 60 s motor-alarm guard.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **`lcd_flush()` preamble**: replaced `lcd_home()` call with `lcd_display_on()`; eliminates the 4-character cursor-shift artifact that appeared after 2.5 s of screen-message display (e.g. after returning from a timed message screen).
- `firmware/src/ui_display/ui_display.cpp` — **`show_group_summary()` removed**: the 2.5 s intermediate summary screen (`Day T28..27°C / RH   80.. 80%`) shown when pressing `*` in a browse state was removed entirely. `*` now navigates directly to `UI_MENU_CLIMATE` (the group selector). Eliminates the associated timing complexity and AiP31068L first-transaction glitch window.
- `firmware/src/ui_display/ui_display.cpp` — **Session label format**: `"SESS:%-4s %s"` with `"ADMN"` / `"FRMR"` / `"NONE"` replaced by `"Sess: %-6s%s"` with `"Admin"` / `"Farmer"` / `"NONE"` — a space is now always present after the colon, and the role names use readable mixed-case.
- `firmware/src/ui_display/ui_display.cpp` — **Mode display**: single `snprintf` with a `mode_str` variable replaced by per-case `snprintf` calls that check EG1 bits directly: `MOTOR_ALARM` → `"Mode: ALARM     "`, `WIND_OVERRIDE` → `"Mode: WIND      "`, `EG1_BIT_CALIBRATING` → `"Mode:Window Cal."`, default → `"Mode: AUTO      "`.
- `firmware/src/web_server/web_server.cpp` — **`/api/status`**: auth check removed — endpoint is now public (no session required). Mode derivation updated: added `else if (eg1 & EG1_BIT_CALIBRATING) mode_str = "WINDOW_CAL"` before the `AUTOMATIC` fallback.
- `firmware/src/web_server/web_server.cpp` — **`/api/history`**: auth check removed — endpoint is now public.
- `firmware/src/web_server/web_server.cpp` — **`/api/sd/status`**: auth check removed — endpoint is now public.
- `firmware/data/app.js` — **`setRole(role)`**: rewritten to show/hide `#btn-login`, `#btn-logout`, `#role-badge`, and `#section-settings` based on auth state instead of hiding/showing the full-page overlay. Logged-in: settings visible, Login hidden, role badge + Logout shown. Logged-out: settings hidden, Login shown.
- `firmware/data/app.js` — **`showLogin()`** (session expiry handler): no longer re-displays a login overlay; now simply calls `setRole(null)` to drop back to the unauthenticated public view.
- `firmware/data/app.js` — **`doLogout()`**: calls `setRole(null)` directly instead of `showLogin()`.
- `firmware/data/app.js` — **`doLogin()`**: calls `hideLoginModal()` on success before `setRole()`.
- `firmware/data/app.js` — **`showLoginModal()` / `hideLoginModal()` / `modalBackdropClick()`**: new modal management functions replacing the full-page overlay show/hide. `modalBackdropClick` closes the modal only when the semi-transparent backdrop is clicked, not the login box.
- `firmware/data/app.js` — **`loadHistory()`**: 401 guard removed; SD card status and history now load on page load regardless of auth. Periodic 30 s SD and 120 s history refresh intervals no longer guarded by `if (g_role === null) return`.
- `firmware/data/app.js` — **`WINDOW_CAL`** added to `modeNames` map → `'Window Cal.'`.
- `firmware/data/app.js` — **Page-load init**: `wsConnect()`, `loadHistory()`, and `loadSdStatus()` now called immediately on page load; `/api/whoami` check follows to restore an existing session if present.
- `firmware/data/style.css` — CSS rule renamed `#login-overlay` → `#login-modal`.
- `firmware/data/index.html` — **Full restructure for public-access pattern**: full-page blocking overlay replaced by a dismissible modal (`<div id="login-modal" style="display:none" onclick="modalBackdropClick(event)">`). Header gains `#btn-login` (visible by default) and `#btn-logout` + `#role-badge` (hidden by default). Status and Sensor history sections always visible. Settings section wrapped in `<section id="section-settings" style="display:none">` — revealed only after login.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.2` → `1.16.3` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.2% (1157 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.16.2] — 2026-05-06

*Day/Night setpoint browse interface on LCD: farmer can now browse and edit the 4 day setpoints (T max/min, RH max/min) and 4 night setpoints directly from the keypad. Two new FSM states `UI_BROWSE_DAY` and `UI_BROWSE_NIGHT`. Climate menu converted from a flat 11-param paginated list to a Day/Night group selector.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — **`UI_BROWSE_DAY` / `UI_BROWSE_NIGHT` FSM states**: browse one setpoint at a time; row 0 shows the parameter label; row 1 shows value, position counter (1/4…4/4), and key hints. Navigation: `A`=previous, `B`=next, `#`=edit, `*`=group min/max summary (2.5 s) then back to the group selector.
- `firmware/src/ui_display/ui_display.cpp` — **`render_menu_climate()`**: replaces old flat climate param menu with a two-line group selector (`1=Day  2=Ngt  *`).
- `firmware/src/ui_display/ui_display.cpp` — **`render_browse_setpoints(bool is_day)`**: renders the current browse slot using the parameter's `edit_lbl` on row 0 and `"<val> N/4 A B #* "` on row 1.
- `firmware/src/ui_display/ui_display.cpp` — **`show_group_summary(bool is_day)`**: displays T min..max °C and RH min..max % on the LCD for 2.5 s when `*` is pressed in a browse state.
- `firmware/src/ui_display/ui_display.cpp` — **`DAY_PARAM_IDX[4]` / `NIGHT_PARAM_IDX[4]`**: map browse position (0–3) to `CLIMATE_PARAMS` indices (`{0,2,4,6}` / `{1,3,5,7}`).
- `firmware/src/ui_display/ui_display.cpp` — **`handle_menu_climate()`** and **`handle_browse_setpoints()`**: key handlers for the two new states.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **`begin_edit()` signature extended**: third parameter `ui_state_t return_to` replaces the hardcoded `is_wind ? UI_MENU_WIND : UI_MENU_CLIMATE` logic, allowing browse states to be preserved through the PIN→edit chain. All callers updated.
- `firmware/src/ui_display/ui_display.cpp` — **`handle_pin()` pending-edit resume**: passes `s_return_menu` (set by the initial `begin_edit()` before PIN entry) so editing returns to the correct browse state after successful authentication.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.1` → `1.16.2` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.1% (approx.)
- RAM: 19.4% (approx.)

---

## [1.16.1] — 2026-05-06

*IO0 BOOT button factory-reset sequence with animated LCD progress bar. LCD rendering muted during button hold. `lcd_create_char()` added to the LCD1602 I2C driver.*

### Added
- `drivers/LCD1602_I2C/src/lcd1602.h/.cpp` — **`lcd_create_char(slot, pattern[8])`**: programs one of the 8 HD44780 CGRAM custom-character slots. Sets the CGRAM address (`0x40 | slot<<3`), writes the 8 pixel rows (5 LSBs used per row), then issues CMD_HOME to return the cursor to DDRAM so subsequent text writes target the visible display area.
- `firmware/src/ui_display/ui_display.cpp` — **IO0 factory-reset sequence**: holding the LOLIN S3 BOOT button (GPIO0, active-low) displays an animated growing bar on LCD row 1; row 0 shows a contextual stage label. Four stages of 5 s each (200 ticks at 100 ms/tick):
  - **0–5 s** — no label. Release restores normal display with no action.
  - **5–10 s** — `Reset PIN?`. Release resets farmer and admin PINs to defaults (`1234` / `12345678`) by erasing NVS namespace `access` and calling `pin_auth_init()`; system continues operating.
  - **10–15 s** — `Reset settings?`. Release erases all NVS namespaces (climate, wind, motor, access, wifi, mqtt, system), resets PINs to defaults, closes any open session; system continues with factory defaults.
  - **15–20 s** — `Restarting?`. Release performs the same full NVS erase then calls `ESP.restart()`. Holding for the full 20 s auto-executes the restart stage without requiring release.
  - Bar fills left-to-right using `\xFF` (HD44780 ROM A00 full-block glyph) for filled cells and CGRAM slot 1 (5×8 outline-square pattern `{0x1F,0x11,0x11,0x11,0x11,0x11,0x1F,0x00}`) for unfilled cells. Slot 0 is avoided because it is the C null terminator.
  - CGRAM slot 1 is programmed under MX1 immediately after `lcd_init()` at T8 startup via the new `lcd_create_char()` API.

### Fixed
- `firmware/src/ui_display/ui_display.cpp` — **LCD updates muted while IO0 is held**: the main loop previously continued executing Q5 network-status renders, status-page rotation, and key dispatch every 100 ms tick while the reset bar was displayed, causing brief status-page flashes to appear behind the bar. Fixed by `continue`-ing the loop after `render_reset_bar()` whenever the button is still held (and the 20 s limit not yet reached), skipping steps 3–6 entirely until release.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.0` → `1.16.1` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.1% (1157 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.16.0] — 2026-05-06

*LCD display improvements: T/RH page reformatted, WiFi page `#`-shortcut to AP enable, boot splash version alignment. Web GUI poll-interval label clarified. Sensor timestamp bug fixed (duplicate log rows). Integration test suite development started.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — **WiFi status page `#` shortcut**: pressing `#` on the network status page (page 3) goes directly to the System menu when an admin session is active; without a session it enters `UI_PIN_ENTRY` (admin PIN); on success lands on `UI_MENU_SYSTEM` where `1` toggles the AP. `s_pending_ap` flag added (mirrors `s_pending_settime` pattern). Row 1 now shows `#=AP` hint in all non-connected states.
- `test/` — **integration test suite** development started: `test/lib/serial_monitor.py`, `test/lib/device_api.py`, `test/lib/emulator_api.py`, `test/conftest.py`, and per-TC test files. The suite targets the device at `192.168.20.150` and the Modbus sensor emulator at `192.168.20.226`; serial assertions use COM8 at 115 200 baud.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **T/RH status page (page 0) reformatted**:
  - Valid sensors: row 0 `Temp: 43 °C    `, row 1 `  RH: 65 %     ` — temperature and humidity on separate rows with aligned `°C` / `%` units. Hex escape `\xDF` followed by `C` split into `"\xDF" "C"` string literals to prevent the compiler from parsing `\xDFC` as a single (out-of-range) hex escape.
  - Invalid sensors: row 0 `Temp: --- °C    `, row 1 `  RH: ---  %    ` — consistent dash style; "Sensors not ready" text removed.
- `firmware/src/ui_display/ui_display.cpp` — **boot splash row 1**: format changed from `"v%-5.5s Init..."` to `"v%-9.9sInit.."` — version field expanded to 9 characters, left-justified; `Init..` sits flush at the right edge of the 16-char display.
- `firmware/data/index.html` — poll-interval label changed from `"Poll interval (s)"` to `"Sensor poll interval (s)"`; tooltip extended to note that the new value takes effect after reboot.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.15.1` → `1.16.0` in both `lolin_s3` and `test_t2_relay` environments.

### Fixed
- **Duplicate sensor log rows** (critical): `sensor_poll.cpp` set `reading.timestamp = dm_get_unix_time()`, which returns a cached value that T4 refreshes only every ~60 s from the RTC. With `poll_interval_s` set to 30 s, two consecutive polls received the same stale timestamp, producing duplicate rows in the sensor history table. Fixed by replacing with `reading.timestamp = (uint32_t)time(NULL)` — the POSIX system clock, always current.

### Integration test — bugs found during development
The following bugs in the firmware or REST API were discovered while building the integration test infrastructure. Both are fixed in this release:
- **Duplicate sensor log entries** — root cause documented above under Fixed.
- **`GET /api/config` vs `POST /api/config` key naming mismatch**: motor travel times are written with keys `travel_m1` / `travel_m2` / `travel_m3` (POST) but read back as a single `travel_s` array (GET). The `wait_for_config` helper in `conftest.py` was updated to exclude travel keys and use array indexing on teardown restore.

### Build metrics
- Flash: 55.0% (1154 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.15.1] — 2026-05-06

*Post-Phase-10 correctness fixes: two-phase atomic OTA commit; STORE-only ZIP writer; OTA idle-status bank/accepted display; release build tooling.*

### Added
- `firmware/src/ota_manager/ota_manager.h` — `OTA_STATE_FW_DONE` (= 7): new intermediate state entered after `esp_ota_end()` succeeds but before the boot partition is switched; the device waits up to 120 s for asset upload. New status accessors: `ota_get_active_bank()` (returns `'A'`/`'B'`/`'?'` from the running partition subtype) and `ota_is_accepted()` (returns true when NVS `ota_fail_cnt` == 0).
- `firmware/src/ota_manager/ota_manager.cpp` — `s_fallback_timer`: FreeRTOS one-shot timer (120 000 ms) started by `ota_firmware_end()`; fires `fallback_reboot_cb()` which switches the boot partition and reboots without touching LittleFS if no assets arrive; cancelled by `ota_assets_begin()`. Implementations of `ota_get_active_bank()` and `ota_is_accepted()`.
- `firmware/src/web_server/web_server.cpp` — `GET /api/ota/status` response extended with `bank` and `accepted` fields; `STATE_NAMES` extended with `"fw_done"` at index 7 (bound check raised to `< 8`); firmware endpoint response changed to `{ok:true, rebooting:false, awaiting_assets:true}`.
- `firmware/data/app.js` — OTA idle label shows `Idle — Bank A, accepted` / `not yet accepted`; `uploadOtaFirmware()` auto-chains `uploadOtaAssets()` if an assets file is already selected; `OTA_ACTIVE_STATES` includes `'fw_done'` so status polling continues through the intermediate state.
- `build_release.ps1` — new project-root PowerShell 5.1 script: reads `FIRMWARE_VERSION` from `platformio.ini`; builds firmware binary; validates LittleFS build; produces a STORE-only (method=0) ZIP via a self-contained binary writer (no .NET `ZipFile`); outputs versioned files under `bin/<version>/`. Run: `powershell -ExecutionPolicy Bypass -File .\build_release.ps1`.
- `bin/README.md` — comprehensive guide: prerequisites, version bump, script invocation, OTA via web GUI (Path A), USB initial flash / recovery (Path B), rollback behaviour, partition layout table.

### Changed
- `firmware/src/ota_manager/ota_manager.cpp` — `ota_firmware_end()` no longer calls `esp_ota_set_boot_partition()` or schedules an immediate reboot; it verifies the image (`esp_ota_end()`), enters `OTA_STATE_FW_DONE`, and starts the 120 s fallback timer. The boot partition switch is deferred to `task_ota_manager()` after successful asset extraction, making both the firmware and paired LittleFS partition switch atomically. `ota_assets_begin()` accepts `OTA_STATE_FW_DONE` in addition to `OTA_STATE_IDLE` / `OTA_STATE_ERROR`, and cancels the fallback timer on entry. `task_ota_manager()` uses `s_ota_part` (saved by `ota_firmware_end()`) when a same-session firmware upload preceded assets, otherwise falls back to `esp_ota_get_next_update_partition(NULL)`.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.15.0` → `1.15.1` in both `lolin_s3` and `test_t2_relay` environments.
- `webUiMock/mock_server.py` — OTA state list extended with `fw_done`; firmware endpoint returns `{ok:true, rebooting:false, awaiting_assets:true}`; assets endpoint accepts `fw_done` initial state; `ota` dict carries `bank` and `accepted` fields (bank flips after asset install; `accepted` is `False` for 5 s then `True`); `cfg["fw_ver"]` updated to `"1.15.1"`.

### Fixed
- **OTA premature reboot** (critical): the device previously rebooted immediately after firmware upload, before web assets could be transferred, leaving the inactive LittleFS partition empty and the web UI inaccessible. Fixed by the two-phase commit: boot partition switch now happens only after asset extraction succeeds in T13 (or after the 120 s fallback timer if no assets arrive).
- **ZIP DEFLATE entries rejected by extractor**: `build_release.ps1` originally used `System.IO.Compression.ZipFile` (PS 5.1 / .NET Framework), which silently writes method=8 (DEFLATE) even at `CompressionLevel.NoCompression` — a known .NET Framework defect. Fixed by replacing with a self-contained binary ZIP writer emitting raw Local File Header, Central Directory, and EOCD records with method=0. CRC-32 uses decimal `[long]3988292384` for the polynomial to avoid PS 5.1 signed-int32 overflow on constants above `0x7FFFFFFF`.
- **`STATE_NAMES` bounds overrun**: the `< 7` upper-bound check in `web_server.cpp` excluded the new index-7 entry; corrected to `< 8`.

### Build metrics
- No binary size change from v1.15.0.

---

## [1.15.0] — 2026-05-06

*Phase 10: dual-bank OTA (firmware + web assets) with 3-fail rollback; version bump 1.14.0 → 1.15.0.*

### Added
- `firmware/src/ota_manager/ota_manager.h` — full OTA Manager public API: rollback management (`ota_check_rollback`, `ota_mark_healthy`), streaming firmware OTA (`ota_firmware_begin/write/end`), PSRAM-buffered web-asset OTA (`ota_assets_begin/accumulate/end`), status accessors (`ota_get_state`, `ota_get_progress`, `ota_get_error`), T13 task entry point; `OTA_HEALTHY_MS` constant (30 000 ms)
- `firmware/src/ota_manager/ota_manager.cpp` — full implementation:
  - **3-fail rollback**: NVS `system/ota_fail_cnt` incremented on every boot; on count ≥ 3 the counter is cleared and `esp_ota_mark_app_invalid_rollback_and_reboot()` is called, reverting to the previous firmware bank
  - **Firmware OTA** (T11 inline): `esp_ota_begin/write/end` on the inactive `app` partition; reboots via one-shot FreeRTOS timer after 1 s; `EG1_BIT_OTA_IN_PROGRESS` held throughout
  - **Web-asset OTA** (T13 spawned on-demand): ZIP uploaded into PSRAM; T13 extracts it onto the inactive LittleFS partition and writes `manifest.json`; then calls `esp_ota_set_boot_partition` (paired bank switch) and reboots; ZIP must use STORE compression (`zip -0`); DEFLATE entries rejected with a clear error message
  - ZIP LOCAL FILE HEADER parser: reads signature, compression method, sizes, and name; strips directory prefix to extract basename; validates every entry before writing to LittleFS
- `firmware/src/web_server/web_server.cpp` — three new OTA REST endpoints:
  - `GET /api/ota/status` — any logged-in role; returns `{ok, state, progress, error}`
  - `POST /api/ota/firmware` — admin only; streaming body callback (`index == 0` → begin, per-chunk → write, `index+len >= total` → end + 200 `{ok,rebooting:true}`); error state suppresses double-response
  - `POST /api/ota/assets` — admin only; same streaming pattern; final response 202 `{ok, message:"extracting — poll GET /api/ota/status"}`
- `firmware/data/index.html` — **OTA section** in System tab (admin only): firmware `.bin` file input + Upload button; web assets `.zip` file input + Upload button; OTA status span; progress bar (hidden when idle)
- `firmware/data/app.js` — `uploadOtaFirmware()`, `uploadOtaAssets()`, `loadOtaStatus()`: POST binary/zip body to OTA endpoints; `loadOtaStatus()` auto-polls every 2 s while state is not `idle`/`error`; progress bar updated from `progress` field; `setRole('admin')` now calls `loadOtaStatus()` on login
- `firmware/data/style.css` — `.ota-progress-bar` and inner `div` styles (8 px height, green fill, 0.4 s width transition)
- `webUiMock/mock_server.py` — **OTA simulation endpoints**: `GET /api/ota/status`, `POST /api/ota/firmware`, `POST /api/ota/assets`; background thread simulates chunked upload progress (0–100%) with per-state transitions (`fw_begin → fw_write → fw_end → idle`, similar for assets); thread-safe via `ota_lock`

### Changed
- `firmware/src/main.cpp` — `setup()` calls `ota_check_rollback()` immediately after NVS init; T1 task calls `ota_mark_healthy()` once after `OTA_HEALTHY_MS` (30 s, 60 × 500 ms ticks) of stable uptime
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.14.0` → `1.15.0` in both `lolin_s3` and `test_t2_relay` environments
- `webUiMock/mock_server.py` — `cfg["fw_ver"]` updated to `"1.15.0"`

### Build metrics
- Flash: ~56% (est.)
- RAM:   ~19% (est.)

### Notes
- Web-asset ZIP must be created with `zip -0 assets.zip data/*` (STORE only — no compression). DEFLATE entries are rejected at extraction time with a diagnostic error.
- On a successful OTA update, the inactive bank becomes the new boot target; both the firmware partition and the paired LittleFS partition are switched atomically.

---

## [1.14.0] — 2026-05-06

*Web GUI polish: hover tooltips on all fields; session expiry handling; history buffer fix; SD card management (status card + mount/unmount) with full T9 logging integration; LCD truncation fix.*

### Added
- `firmware/data/index.html` — **65 `data-tip` hover tooltips** on every status card field and every settings label; **8 `ⓘ` tip-icon spans** on card `<h3>` headings (zero JS, CSS `[data-tip]::after` pseudo-element, 260 px dark bubble, 150 ms fade)
- `firmware/data/index.html` — **SD card status card** in the Status section (visible to both Farmer and Admin): shows Mounted/Not mounted, total size (MB), free space (MB); refreshed on login and every 30 s
- `firmware/data/index.html` + `firmware/data/app.js` — **SD card controls** in System tab (Admin only): Mount button and Unmount (danger) button; feedback span; button auto-disabled to match current mount state
- `firmware/data/app.js` — `loadSdStatus()` / `postSdMount()` / `postSdUnmount()`: fetch `/api/sd/status|mount|unmount`, update status card fields and button disabled-state; called on admin/farmer login and on 30 s interval
- `firmware/data/app.js` — `showLogin()`: restores login overlay, clears role, and resets WS initialisation flag; called on logout, 401 response, and session-check failure
- `firmware/data/app.js` — **60 s `whoami` polling**: detects server-side session expiry while the UI is idle; calls `showLogin()` on failure
- `firmware/data/app.js` — **2 min auto-refresh** of sensor history table
- `firmware/data/app.js` — 401 detection in `post()`, `loadConfig()`, and `loadHistory()`: all three call `showLogin()` on an unexpected 401 response
- `firmware/data/app.js` — firmware version set from `/api/config` on login (`cfg.fw_ver`) so the footer is populated immediately, before the first WebSocket push
- `firmware/src/web_server/web_server.cpp` — `GET /api/sd/status` → `{"mounted":…,"free_mb":…,"size_mb":…}` (Farmer + Admin)
- `firmware/src/web_server/web_server.cpp` — `POST /api/sd/mount` → calls `event_logger_sd_remount()`; Admin only
- `firmware/src/web_server/web_server.cpp` — `POST /api/sd/unmount` → calls `event_logger_sd_unmount()`; Admin only
- `drivers/sdCard/src/sd_storage.h/.cpp` — `storage_sd_total_bytes()`: returns FAT32 volume total capacity via `SD.totalBytes()`
- `drivers/sdCard/src/sd_storage.h/.cpp` — `storage_sd_unmount()`: clears `g_mounted` and calls `SD.end()` to release the SPI bus
- `firmware/src/event_logger/event_logger.h/.cpp` — `event_logger_sd_remount()`: calls `storage_init()`, writes CSV header if file is new, sets T9's `s_sd_ok` flag — T9 begins logging to SD immediately; safe to call from any task when `s_sd_ok` is false
- `firmware/src/event_logger/event_logger.h/.cpp` — `event_logger_sd_unmount()`: clears T9's `s_sd_ok` flag first (prevents in-flight SD write), then calls `storage_sd_unmount()`

### Fixed
- `firmware/src/ui_display/ui_display.cpp` — status page 4 row 1: format string `"Src:%-3s     #=Set"` (17 chars) truncated to `"#=Se"` on the 16-char LCD; corrected to `"Src:%-3s    #=Set"` (16 chars)
- `firmware/src/web_server/web_server.cpp` — sensor history buffer raised from 4096 → 6144 bytes; pre-write overflow guard replaced `pos >= 3800` heuristic with proper `pos + written >= HIST_BUF - 4` check; history table no longer truncates after the first hour
- `firmware/src/web_server/web_server.cpp` — firmware version now read from NVS `system/fw_version` in `build_config_json()` and included in `/api/config` response; footer no longer shows "—" until the first WebSocket push

### Changed
- `firmware/data/style.css` — tooltip CSS block added (`[data-tip]` relative positioning, `::after` pseudo-element, `.tip-icon` helper class)
- `firmware/data/style.css` — `.row` and `.slider-row` `margin-bottom` increased from `0.5 rem` to `1 rem` for better touch ergonomics on smartphones
- `firmware/data/app.js` — `doLogout()` simplified to `post('/api/logout', {}).then(() => showLogin())`
- `firmware/src/web_server/web_server.cpp` — `/api/sd/status` accessible to SESSION_FARMER and SESSION_ADMIN (was SESSION_ADMIN only)

### Added (tooling)
- `webUiMock/mock_server.py` — **Flask web UI mock server**: serves `firmware/data/` static files and emulates all REST and WebSocket endpoints (`/api/whoami`, `/api/login`, `/api/logout`, `/api/status`, `/api/config` GET/POST, `/api/wifi`, `/api/pin`, `/api/history`, `/api/sd/status|mount|unmount`, `/ws`); sine-wave sensor simulation; in-memory NVS config state with correct `(ns, key)` → field mapping for all motor/climate/wind/system keys; SD card state toggled by mount/unmount; session and access-control rules match firmware exactly (farmer vs. admin restrictions)
- `webUiMock/requirements.txt` — Python dependencies: `flask>=2.3.0`, `flask-sock>=0.7.0`
- `webUiMock/README.md` — setup and usage instructions, full endpoint table, access-control notes, differences-from-firmware table

### Build metrics
- Flash: 54.4% (1141 kB / 2 MB)
- RAM:   19.3% (63 kB / 320 kB)

### Notes
- `pio run -t uploadfs` always targets lfs1 (0x520000). When running firmware from app0/Bank A, flash web assets to lfs0 (0x420000) directly with esptool (command in `platformio.ini` comments).
- Start the mock server with `cd webUiMock && pip install -r requirements.txt && python mock_server.py`; open `http://localhost:5000` (farmer PIN: `1234`, admin PIN: `12345678`).

---

## [1.13.0] — 2026-05-05

*Geolocation + automatic timezone; local-time clock display fix; LCD time status page; LCD manual date/time set (admin).*

### Added
- `firmware/src/network_manager/network_manager.cpp` — **automatic geolocation and timezone** (`do_geo_sync()`):
  - After every successful NTP sync, performs HTTP GET `http://ip-api.com/json?fields=status,lat,lon,timezone` (5 s timeout)
  - Parses JSON response (strstr/atof, no cJSON dependency) for latitude, longitude, and IANA timezone name
  - Lookup table of ~100 IANA timezone names → POSIX TZ strings (Europe, Americas, Asia, Australia, Pacific)
  - Posts `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` to Q4 → T4 updates shadow + sunrise/sunset immediately
  - Writes POSIX TZ string to NVS `system/tz_str`; calls `setenv("TZ", …, 1)` + `tzset()` immediately without reboot
  - Falls back gracefully on HTTP error or unknown IANA name (logs warning, leaves TZ unchanged)
- `firmware/src/types/app_types.h` — `net_status_t` extended with `bool ntp_synced` field
- `firmware/src/data_manager/data_manager.h/.cpp` — **`dm_set_manual_time(time_t unix_ts)`** public API:
  - Updates POSIX system clock via `settimeofday()`
  - Writes UTC time to DS1307 RTC under MX1 via `rtc_set_time()`
  - Updates `current_unix_ts` in MX4 configuration shadow
- `firmware/src/ui_display/ui_display.cpp` — **LCD time status page** (page 4 of 5):
  - Row 0: `YYYY-MM-DD HH:MM` (local time via `localtime_r`)
  - Row 1: `Src:NTP  #=SetTm` or `Src:RTC  #=SetTm` — source from `net_status_t.ntp_synced`
- `firmware/src/ui_display/ui_display.cpp` — **LCD manual date/time set** (admin only, two-screen flow):
  - `#` on time status page (page 4) → admin PIN required → `UI_SET_DATE`
  - **Date screen** (`UI_SET_DATE`): row 0 shows current date; row 1 entry `DD/MM/YY #OK *Bk`; 6 digits DDMMYY; `#` advances to time screen; `*` backtracks/cancels to status
  - **Time screen** (`UI_SET_TIME`): row 0 shows current time; row 1 entry `HH:MM #OK *Bk`; 4 digits HHMM; `#` converts entered local time via `mktime()` to UTC epoch, calls `dm_set_manual_time()`, writes to DS1307, returns to status; `*` backtracks to date screen (date digits restored)
  - Validation: day 01–31, month 01–12, hour 00–23, minute 00–59; error messages on bad input
  - New FSM states: `UI_SET_DATE`, `UI_SET_TIME`; `s_pending_settime` flag for deferred PIN flow

### Changed
- `firmware/src/web_server/web_server.cpp` — **time display fix**: `gmtime_r` → `localtime_r`; format `"%Y-%m-%dT%H:%M:%SZ"` → `"%Y-%m-%dT%H:%M:%S"` — clock in web UI now shows local time with DST applied instead of UTC
- `firmware/src/network_manager/network_manager.cpp` — `post_q5()` now sets `st.ntp_synced = s_ntp_synced`
- `firmware/src/ui_display/ui_display.cpp` — `STATUS_PAGES` constant 4 → 5

### Build metrics
- Flash: 54.3% (1 138 kB / 2 MB)
- RAM: 19.3% (63 kB / 320 kB)

---

## [1.12.0] — 2026-05-05

*NVS partition fix (critical); AP lifecycle hardening; LCD display improvements; web GUI tab restructure and RH-dependent grayout.*

### Fixed
- **Critical — `firmware/partitions.csv`**: The Arduino ESP32 toolchain unconditionally flashes `boot_app0.bin` to the hardcoded address 0xe000. The old partition table placed NVS at 0x9000–0x1DFFF (84 KB), so 0xe000 landed inside NVS page 5 and corrupted the entire namespace on every firmware flash. Fixed by redesigning the partition layout:

  | Name    | Type | Sub-type | Offset   | Size    |
  |---------|------|----------|----------|---------|
  | otadata | data | ota      | 0xe000   | 0x2000  |
  | nvs     | data | nvs      | 0x10000  | 0x10000 |
  | app0    | app  | ota_0    | 0x20000  | 0x200000|
  | app1    | app  | ota_1    | 0x220000 | 0x200000|
  | lfs0    | data | spiffs   | 0x420000 | 0x100000|
  | lfs1    | data | spiffs   | 0x520000 | 0x100000|

  `board_upload.offset_address = 0x20000` in `platformio.ini` updated accordingly.
- `firmware/src/ui_display/ui_display.cpp` — `UI_MENU_SYSTEM` / `UI_MENU_MOTORS` tab panes no longer use `admin-only-block` CSS class (was forcing `display:block` for both when admin, overriding tab show/hide logic); both are now plain `tab-pane`

### Added
- `firmware/src/network_manager/network_manager.cpp`:
  - **AP auto-stop on client connect**: when `WL_CONNECTED` is reached, if AP is active the NVS `wifi/ap_enable` flag is cleared and `stop_ap()` is called immediately
  - **AP non-persistent on reboot**: at T10 startup `nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0)` unconditionally forces AP disabled; admin must explicitly enable it each boot via LCD or web GUI
- `firmware/src/ui_display/ui_display.cpp`:
  - **Boot splash**: shows `"Greenhouse Ctrl "` / `"v0.1.0 Init... "` on LCD for 2 s using `FIRMWARE_VERSION` macro
  - **Network status page — AP SSID**: when AP is active, row 1 shows computed SSID `"Greenhouse-XXYY"` (last 2 MAC bytes) instead of blank
  - **Wind direction cardinal**: row 1 of wind page now shows `" Dir:%3d ° (%-2s) "` — degree value, degree symbol (`\xDF`), and 8-point cardinal name (N/NE/E/SE/S/SW/W/NW); helper `deg_to_cardinal()` added
- `firmware/data/index.html`, `app.js`, `style.css` — **web GUI restructure**:
  - `<h1>` badge: WS online/offline badge moved inside `<h1>` title element
  - RH-dependent rows (6 rows) carry `.rh-dep` class; `applyRhCtrl()` in JS toggles `.rh-disabled` on all `.rh-dep` rows when humidity control is disabled; CSS: `.rh-dep.rh-disabled { opacity: 0.35; pointer-events: none }`
  - **System tab** (admin): session & timing sliders, WiFi AP settings, WiFi client settings, NTP timezone, location (lat/lon)
  - **Access tab** (admin): Farmer PIN change, Admin PIN change
  - Standalone "System" and "Access control" sections removed (content consolidated into tabs)
  - Motors tab and System tab content correctly separated (removed duplicate content)

### Changed
- `firmware/src/ui_display/ui_display.cpp`:
  - Wind status page: valid sensors: `" Dir:%3d \xDF (%-2s) "` (space before degree symbol); invalid sensors: `" Dir: --- \xDF     "` (consistent column alignment with valid case)

---

## [1.11.0] — 2026-05-05

*Phase 9 — Web Server (T11) implemented: ESPAsyncWebServer with LittleFS file serving, cookie-session auth (farmer/admin roles), REST API, WebSocket status push, config read/write, PIN management, WiFi provisioning.*

### Added
- `firmware/data/index.html` — single-page web application:
  - Login overlay with role select (farmer/admin) and PIN entry
  - Header: connection badge (WS Online/Offline), role badge, Logout button
  - Status section: 8 cards — Temperature (raw + average), Humidity (raw + average), Wind (speed/direction/average), Windows M1/M2/M3, Mode + sunrise/sunset, Alarms (EG1 bits decoded as badges), Clock + NTP status, WiFi (RSSI + IP)
  - Settings section with 6 tabs: Climate (farmer+), Wind (farmer for enable, admin for wind speed/direction limits), Motors (admin only), System (admin only), Network (admin only), Access / PIN change (admin only)
  - Sensor history table (last 60 readings)
- `firmware/data/style.css` — dark-theme CSS:
  - CSS custom properties: `--bg #1a1a2e`, `--card #16213e`, `--accent #0f3460`
  - Login overlay, status card grid, badge styles, tab system, form rows
  - Role-gated visibility: `.admin-only { display: none }` / `body.is-admin .admin-only { display: flex }`; farmer equivalent for `.farmer-hidden`
- `firmware/data/app.js` — web application logic:
  - Auth: `setRole()`, `doLogin()` (POST /api/login), `doLogout()` (POST /api/logout), session check on load via `fetch('/api/whoami')`
  - WebSocket: `wsConnect()` with 3 s auto-reconnect; `handleStatus()` updates all DOM elements from WS push
  - Config: `loadConfig()` (GET /api/config); `postCfg()`, `postCfgSelect()`, `postCfgStr()`, `postLocation()`, `postWifi()`, `postApPsk()`, `postPinChange()`
  - History: `loadHistory()` (GET /api/history?n=60) → table rows
  - Tab system: `showTab()` toggles `.active` class on pane + button
- `firmware/src/web_server/web_server.cpp` — full T11 implementation:
  - 4-slot cookie session map (`s_sessions[MAX_SESSIONS]`), FreeRTOS mutex, 16-byte hex token from `esp_fill_random()`
  - LittleFS file serving via `serve_lfs()` — PSRAM-allocated 32 KB read buffer, MIME type from extension
  - `build_status_json()` / `build_config_json()` — PSRAM 1 KB JSON builders
  - `AsyncWebSocket s_ws("/ws")` with 2 s push loop in T11 task
  - `t2_get_window_states()` cross-task window state read via spinlock
  - Farmer-key whitelist for partial access: climate setpoints + `rh_ctrl_en` + `wind_prot_en`
  - Body parser: `json_get_str()` / `json_get_int()` — strstr-based, no cJSON dependency
  - Routes: 11 endpoints (see firmwareImplementationResults.md for full table)

### Modified
- `firmware/src/relay_controller/relay_controller.h` — added `t2_get_window_states(window_state_t out[3])` declaration
- `firmware/src/relay_controller/relay_controller.cpp` — added `portMUX_TYPE s_state_mux` spinlock and `t2_get_window_states()` implementation
- `firmware/platformio.ini` — added `board_build.filesystem = littlefs` for `pio run -t uploadfs`

### Fixed
- `pin_auth_set_pin` → `pin_auth_set` (correct API name discovered on first build)

### Build metrics
- Flash: 46.4% (973 kB / 2 MB)
- RAM: 19.0% (62 kB / 320 kB)
- Build time: 46 s

---

## [1.10.1] — 2026-05-05

*Post-Phase 8 corrections: AP enable/disable added to T8 system menu; root menu now shows all four items; AP password defaulted to `0123456789`; TSDS AP password description corrected.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — `handle_menu_system()` and updated `render_menu_system()`:
  - Root menu row 1 changed from `"3:Access  *:Back"` to `"3:Access 4:Sys *"` so item 4 (System) is visible
  - System menu shows `"1=WiFi AP   *:Bk"` (or `"1=AP(on)    *:Bk"` when AP is already active)
  - Pressing `1` with admin session toggles AP on/off: posts `Q4 {ns="wifi", key="ap_enable", value=0/1}` → T4 persists → T10 acts on next 5 s poll
  - Without admin session: shows `"Admin login req. / 3=Access menu"` for 2 s, returns to system menu

### Changed
- `firmware/src/network_manager/network_manager.cpp`:
  - Added `#define AP_PSK_DEFAULT "0123456789"` — AP is never started open/passwordless
  - `nvs_cfg_get_str_or_default` for `ap_psk` seeds NVS with `AP_PSK_DEFAULT` on first boot (was empty string)
  - `start_ap()` uses NVS password when set, falls back to `AP_PSK_DEFAULT` if empty
- `design/technicalSoftwareDesignSpecification.md`:
  - §WiFi AP mode: corrected "password hashed" to plaintext with rationale (WPA2 requires raw key); documented default `0123456789`; noted it is configurable by admin via web interface
  - NVS schema `wifi` row: `ap_psk` annotated as plaintext with default; contrast with `psk_hash` (client, hashed) made explicit

### Verified on hardware
- AP `Greenhouse-XXYY` visible in WiFi scan after enabling via LCD system menu ✅
- AP requires password `0123456789` ✅
- LCD system menu shows `"1=AP(on)    *:Bk"` when AP is active ✅
- Admin session required; non-admin press shows prompt ✅

---

## [1.10.0] — 2026-05-05

*Phase 8 — Network Manager (T10) implemented: WiFi station FSM with exponential backoff, soft-AP management, NTP synchronisation with DS1307 update via TN4, and Q5 network status to T8.*

### Added
- `firmware/src/network_manager/network_manager.cpp` — full T10 task body (replaces Phase 0 stub):
  - 5-state client FSM: `NET_IDLE` → `NET_CONNECTING` → `NET_CONNECTED` → `NET_RUNNING` → `NET_BACKOFF`
  - `NET_IDLE`: polls NVS `wifi/ssid` every 5 s; advances to `NET_CONNECTING` when SSID appears (supports post-boot provisioning via web server)
  - `NET_CONNECTING`: 30 s hard timeout; `WiFi.setAutoReconnect(false)` — T10 owns reconnection
  - `NET_CONNECTED`: posts Q5 with IP; runs `run_ntp_sync()` inline (blocks up to 30 s for plausible `time(NULL) > 1 700 000 000`); advances to `NET_RUNNING`
  - `NET_RUNNING`: monitors `WiFi.status()` every 5 s for connection drop
  - `NET_BACKOFF`: exponential wait 2 → 4 → 8 → 16 → 32 → 60 s (capped); re-reads NVS credentials before retry
  - AP management: `poll_ap()` reads NVS `wifi/ap_enable` every loop tick; starts/stops `WiFi.softAP()` on change; enforces `cfg.ap_timeout_min` auto-shutdown (writes `ap_enable=0` back to NVS on expiry)
  - AP SSID: `"Greenhouse-XXYY"` from last two MAC bytes
  - NTP sync: `configTime(0, 0, "pool.ntp.org")` → on success: `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` → T4 calls `rtc_set_time()` under MX1
  - Q5: `xQueueOverwrite(Q5, &status)` on every state change; `net_status_t {client_connected, ap_active, ip_str[16]}`
  - LOG_SYSTEM events: STA connect/disconnect (value_a=1), NTP success/timeout (value_a=2), AP start/stop (value_a=3)

### Changed
- `firmware/firmwareImplementationPlan.md` — Phase 8 marked ✅ done; `network_manager` added to Critical Files Summary
- `firmware/firmwareImplementationResults.md` — Phase 8 section added

### Verified on hardware (runtime capture)
- T10-01: Build clean — 0 errors, 0 warnings ✅
- T10-02: Upload and boot without crash ✅
- T10-03: T1 heartbeat steady at t=15–40 s; no Guru Meditation ✅
- T10-04: NET_IDLE (no SSID configured) — no periodic log output (expected behaviour) ✅
- T10-05: Q5 initial post received by T8 — LCD network page shows "No WiFi" ✅

---

## [1.9.0] — 2026-05-05

*Phase 7 — UI Layer (T7 + T8) implemented: 4×4 keypad scan with key-repeat and full LCD menu FSM with PIN authentication, config editing, and session management.*

### Added
- `firmware/src/keypad_scan/keypad_scan.cpp` — full T7 task body (replaces Phase 0 stub):
  - 20 ms scan period via `vTaskDelay(pdMS_TO_TICKS(20))` + `keypad_scan()` from LIB-5
  - Key-repeat: same key held ≥ 500 ms → repeat events every 100 ms (`repeated=true`)
  - Posts `key_event_t` to Q2 non-blocking; first-press overflow → `ESP_LOGW`, repeat overflow → `ESP_LOGD`
- `firmware/src/ui_display/ui_display.cpp` — full T8 task body (replaces Phase 0 stub):
  - LCD init via `lcd_init()` under MX1 at task entry; 2 s boot splash
  - 100 ms main loop: Q2 key receive, Q5 network status poll, session timeout tick, FSM dispatch, status page rotation, dirty-flag render
  - 8-state FSM: `UI_STATUS` → `UI_MENU_ROOT` → `UI_MENU_CLIMATE` / `UI_MENU_WIND` / `UI_MENU_ACCESS` / `UI_MENU_SYSTEM` → `UI_PIN_ENTRY` / `UI_EDIT_VALUE`
  - Status display: 4 pages × 5 s auto-rotate (T/RH, wind, mode/session, network)
  - Parameter table: 11 climate params + 2 wind params; 2 params per sub-menu page; `#` cycles pages; current NVS value shown alongside label
  - PIN entry: masked display, `pin_auth_verify()`, lockout seconds shown on lock, pending-edit preserved through PIN flow
  - Config edit: digit builder, `B`=sign toggle for negative temps, `#` confirm → `xQueueSend(Q4)` + `log_post(LOG_SETPOINT)`
  - Session: `session_open()` / `session_close()` log `LOG_SESSION`; timeout from `cfg.session_timeout_min` (default 5 min); idle counter reset on non-repeat keys only
  - `show_msg()` helper: fills rows, flushes LCD under MX1, blocks for delay, sets dirty for re-render
  - FR-UI07 satisfied: ≤ 4 keypresses from status screen to any first-level setting when authenticated

### Changed
- `firmware/src/keypad_scan/keypad_scan.h` — phase reference updated to Phase 7
- `firmware/src/ui_display/ui_display.h` — phase reference updated to Phase 7
- `firmware/src/main.cpp` — added `#include "auth/pin_auth.h"` and `pin_auth_init()` call after `nvs_cfg_init()`; logs `PIN auth OK` on success
- `firmware/firmwareImplementationPlan.md` — Phase 7 marked ✅ done; `keypad_scan` and `ui_display` added to Critical Files Summary
- `firmware/firmwareImplementationResults.md` — Phase 7 section added

### Verified on hardware (boot capture, 340 s runtime)
- T8-02: LCD init OK — no `ESP_LOGE` from `T8_UI` in 340 s capture ✅
- T8-03: MX1 not deadlocked — T4 periodic RTC read completes at t=297 s ✅
- All tasks stable: T1 heartbeat continuous, T5/T6 nominal at t=309/310 s, no watchdog resets ✅
- T7-01/T8-01 (task alive), T7-02/03, T8-04–09: deferred to integration testing (USB-CDC pre-connect window / physical keypresses required)

---

## [1.8.0] — 2026-05-05

*Phase 6 — Climate Control (T6) implemented: autonomous graduated ventilation driven by live T/RH sensor data, EG1 inhibit gate, conflict resolution, and incremental Q1 command posting.*

### Added
- `firmware/src/climate_control/climate_control.cpp` — full T6 task body (replaces Phase 0 stub):
  - Blocks on TN2 (`ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`) — wakes only when T4 has new Q6 data
  - EG1 gate: skips evaluation and resets `current_step_t/rh = 0` if `WIND_OVERRIDE`, `MOTOR_ALARM`, or `SENSOR_FAULT_T` is set; logs inhibit transitions; resumes from step 0 on clearance
  - Snapshots `cfg_shadow_t` under MX4 (`dm_cfg_snapshot()`) and `sensor_reading_t` under MX2 (`dm_meas_snapshot()`)
  - Selects day vs. night setpoints (`t_max`, `rh_max`, `rh_min`) from `cfg.is_daytime`
  - Calls `vent_step_required_t()` and `vent_step_required_rh()` (already implemented, Gap G); resolves with `vent_resolve_conflict()`
  - `apply_step_delta()`: posts CLOSE commands before OPEN commands for changed channels; single `CMD_CLOSE_ALL ch=0` when resolved step == 0
  - `post_log_mode()`: posts `LOG_MODE_CHANGE` to Q3 on every step change with `value_a=resolved_step`, `value_b=(step_t<<8)|step_rh`
  - `post_q1()`: non-blocking `xQueueSend(Q1, ..., 0)` with LOGW on queue-full (never blocks T6)
  - Defensive hysteresis clamp: `hyst_t/rh` clamped to ≥ 1 to prevent division by zero in `step_from_deviation()`

### Changed
- `firmware/src/climate_control/climate_control.h` — Doxygen updated: per-wake sequence documented (8 steps), inhibit behaviour and step-reset logic described
- `firmware/firmwareImplementationPlan.md` — Phase 6 marked ✅ done
- `firmware/firmwareImplementationResults.md` — Phase 6 section added

### Verified on hardware (VERIFY_T6 harness, 471 s capture)
- Clean build: RAM 10.8% (35 364 B), Flash 20.0% (419 869 B); zero warnings
- **T6-04/05** — VERIFY_T6 Phase A: Q4 `t_max_ngt=5` injected at t=15 s; T5 iter 1 at t=68.8 s fired TN2; T6 evaluated `T_avg=12 t_max=5 hyst=2 → step_t=3 | step_rh=−1 (NEUTRAL) | resolved=3`; CMD_OPEN ch=1, CMD_OPEN ch=2, CMD_OPEN ch=3 posted to Q1
- **T6-07** — VERIFY_T6 Phase B: Q4 `t_max_ngt=22` injected at t=106 s; T5 iter 2 at t=129 s fired TN2; T6 evaluated `deviation=−10 < −hyst=−2` → close-hysteresis guard cleared → step 0 → CMD_CLOSE_ALL
- **T2 Q1 acceptance** — T2 drained queued commands at t=176 s (post-calib); CMD_OPEN ch1/2/3 then CMD_CLOSE_ALL accepted; SRC_T6 correctly identified in T2 log
- **Stable idle** — T6 held step=0 with no Q1 posts for iters 3–7 (t=189–430 s); no WDT resets or crashes
- T6-02/03/06/08/09/10 deferred to integration testing

---

## [1.7.0] — 2026-05-05

*Phase 5 — Event Logger (T9) implemented and hardware-verified: Q3 drain loop, NVS ring buffer, SD CSV append with rotation, drop-counter surfacing, and SD failure fallback all confirmed on device. Duplicate LOG_SENSOR fixed.*

### Added
- `firmware/src/event_logger/event_logger.cpp` — full T9 implementation (replaces Phase 0 stub):
  - SD init via `storage_sd_init()` with NVS-only fallback when card absent or mount fails
  - NVS file-index recovery: `nvs_cfg_get_i32("log", "file_idx")` at boot; resumes on same file across reboots without writing a duplicate CSV header
  - Drain-pass loop: `xQueueReceive(Q3, portMAX_DELAY)` blocks for first event, then `xQueueReceive(Q3, 0)` drains all remaining in a tight loop; after each pass calls `log_take_dropped_count()` and, if > 0, posts a synthetic `LOG_SYSTEM` event via `xQueueSend(Q3, ..., 0)` **directly** (not `log_post()`) to avoid re-entrant eviction
  - Every event written to NVS ring buffer unconditionally via `nvs_log_append()`; SD write additionally if `s_sd_ok`
  - SD log rotation at 512 KB: increments `s_file_idx`, persists to NVS, writes CSV header to new file; deletes `s_file_idx − 10` when file count exceeds 10
  - SD write failure: clears `s_sd_ok`, emits `LOG_SYSTEM value_a=−1` via `log_post()` so failure is visible in NVS log; NVS-only operation continues without firmware restart
  - CSV format: `timestamp,type,initiator,ch,param,value_a,value_b\n`; `evt_type_str()` / `initiator_str()` string maps; `build_csv_line()` via `snprintf`
- `firmware/src/event_logger/event_logger.h` — Doxygen rewrite: full T9 behaviour description, drain-pass structure, SD rotation parameters, CSV column table

### Changed
- `firmware/src/main.cpp` — T9 stack increased 4 096 → 6 144 bytes (SD + FAT32 + snprintf stack headroom)
- `firmware/src/sensor_poll/sensor_poll.cpp` — **removed** `log_post(LOG_SENSOR)` from Step 7 of the poll loop; T4 (`data_manager.cpp`) is the sole canonical poster per FR-LG09; posting from both T5 and T4 produced two `SENSOR` CSV rows per poll cycle (Finding 1)
- `firmware/firmwareImplementationPlan.md` — Phase 5 marked ✅ done
- `firmware/firmwareImplementationResults.md` — Phase 5 section added: implementation design, build output, hardware verification checklist, findings

### Verified on hardware (bkhkhe0s8 serial capture + SD card inspection)
- **T9-01** — Task alive: `[T9] task alive` confirmed (pre-USB-CDC; SD CSV present proves T9 ran)
- **T9-03** — SD mount and CSV creation: `/ghc_0001.csv` created with correct header on first boot
- **T9-04** — LOG_SENSOR rows in CSV: `SENSOR,SYS,0,0,11,81` and similar rows at each 60 s poll cycle
- **T9-05** — LOG_RELAY rows in CSV: CH1–CH3 `MOVING_CLOSE` + `CLOSED` calibration sequence fully present
- **T9-07** — Drop-counter surfacing: VERIFY_T9 harness flooded 40 events into Q3 (depth 32); serial showed `[T9] Q3 overflow: 7 event(s) dropped` at t=91 s; `SYSTEM,SYS,0,0,7,0` row in CSV
- **T9-08** — SD failure fallback: SD card contact failure at t=431 s (iter 7) triggered `sdWait(): Wait Failed` / `fopen() failed`; T9 logged `SD write failed (3) — falling back to NVS-only`; subsequent poll iters (8–10) continued without crash — NVS-only fallback confirmed
- **T9-09** — File index recovery: second boot appended to `/ghc_0001.csv` without writing a second header; NVS `file_idx=1` recovered correctly
- T9-02, T9-06, T9-10 deferred to integration testing

### Findings
- **Finding 1 (fixed)** — Duplicate LOG_SENSOR: both T5 (`sensor_poll.cpp`) and T4 (`data_manager.cpp`) were calling `log_post(LOG_SENSOR)` per poll cycle, producing two `SENSOR` rows per interval. Fixed by removing the T5 call; T4 is now the canonical source.
- **Finding 2 (deferred)** — timestamp=0 race: T2 (PRIO_HIGH) wins scheduler at boot and logs the first two calibration events before T4 has populated `dm_get_unix_time()`. Cosmetic; will resolve automatically with NTP sync in Phase 8.

---

## [1.6.0] — 2026-05-05

*Phase 4 — Safety Monitor (T3) implemented: wind speed threshold check, direction exclusion zone (with wrap-through-0°), SENSOR_FAULT_W safe-fail, EG1.WIND_OVERRIDE management, CMD_CLOSE_ALL / CMD_RESUME to Q1, and LOG_ALARM events (W1/W2/W3) to Q3.*

### Added
- `firmware/src/safety_monitor/safety_monitor.cpp` — full T3 implementation (replaces Phase 0 stub):
  - Wakes on TN1 (`ulTaskNotifyTake`, pdTRUE) from T4 after each new wind measurement
  - Reads `sensor_reading_t` via `dm_meas_snapshot()` (MX2) and config via `dm_cfg_snapshot()` (MX4)
  - `wind_prot_en = false` → clears WIND_OVERRIDE if previously set, posts CMD_RESUME, skips evaluation
  - `EG1_BIT_SENSOR_FAULT_W` → safe-fail: sets WIND_OVERRIDE without consulting measurements; `value_a = −1` log marker (FR-W04)
  - Speed check: `wind_speed_avg_ms10 >= v_max × 10` (int32 arithmetic; `v_max ≤ 0` disables check)
  - Direction check: `dir_in_exclusion_zone()` handles non-wrapping and wrap-through-0° arcs; zero-width zone disabled
  - Onset (safe → unsafe): `xEventGroupSetBits(EG1, EG1_BIT_WIND_OVERRIDE)` → `xQueueSend(Q1, CMD_CLOSE_ALL, SRC_T3, 0)` → log W1 and/or W2
  - Both speed and direction unsafe simultaneously → two separate LOG_ALARM records (W1 + W2), one CMD_CLOSE_ALL
  - Clearance (unsafe → safe): `xEventGroupClearBits` → `xQueueSend(Q1, CMD_RESUME, SRC_T3, 0)` → log W3 with current speed + direction
  - MOTOR_ALARM interaction: T3 evaluates and posts normally; T2 discards Q1 commands while MOTOR_ALARM is set; WIND_OVERRIDE bit maintained correctly
  - `#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE` before all includes (same fix as Phase 3 T5 Issue 1)
- `firmware/src/safety_monitor/safety_monitor.h` — full Phase 4 Doxygen documentation: behaviour summary, log event table, design references

### Verified (build)
- Clean build: RAM 10.7% (35 120 B), Flash 17.4% (364 525 B); zero warnings
- Flash delta from Phase 3: +1 436 B

---

## [1.5.0] — 2026-05-03

*Phase 3 — Sensor Polling (T5) implemented: Modbus RTU master for FG6485A (T/RH) and S200 (wind), sliding averages for all four channels, edge-triggered fault detection, Q6 overwrite, and LOG_SENSOR posting. Bug fixed: `ESP_LOGI` compile-time suppression caused by `LOG_LOCAL_LEVEL` being overridden by transitive Arduino HAL includes.*

### Added
- `firmware/src/sensor_poll/sensor_poll.cpp` — full T5 implementation:
  - 8 s boot grace delay (ensures visibility after USB-CDC re-enumeration)
  - Poll loop: `dm_get_poll_interval_s()` → `vTaskDelay()` → `dm_cfg_snapshot()` → window recalculation → FG6485A read → S200 read → build `sensor_reading_t` → `xQueueOverwrite(Q6)` → `log_post(LOG_SENSOR)`
  - Arithmetic circular-sum sliding average for T, RH, wind speed (`avg_ctx_t`); unit-vector (sin/cos) circular-sum sliding average for wind direction (`dir_avg_ctx_t`) — handles 0°/360° wrap via `atan2()`
  - Window size = `avg_win_x_min × 60 / poll_s`, clamped [1, 360]; context reset (re-warm) on window-size change
  - One immediate retry per sensor per poll cycle; fault onset after 2nd consecutive failure; fault cleared on first success — both edge-triggered with `xEventGroupSetBits/ClearBits(EG1)` and `log_post(LOG_ALARM)`
  - ~7.2 KB BSS for four averaging buffers (360-sample depth × 4 channels)
- `firmware/src/sensor_poll/sensor_poll.h` — full Phase 3 Doxygen documentation

### Fixed
- `firmware/src/sensor_poll/sensor_poll.cpp` — `ESP_LOGI` calls were silently compiled away due to `LOG_LOCAL_LEVEL` being overridden below `ESP_LOG_INFO` by a transitive Arduino HAL include reached through the driver headers. Fix: `#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE` placed before `#include <esp_log.h>` as the first two lines of the translation unit. TAG changed from `"sensor_poll"` to `"T5_SEN"`.

### Changed
- `firmware/src/main.cpp` — T1 heartbeat reverted to clean form after Phase 3 debugging: removed `eTaskGetState(task_t5)` and `esp_get_free_heap_size()` diagnostic fields that were added temporarily to verify T5 was scheduled
- `firmware/firmwareImplementationResults.md` — Phase 3 section added (implementation design, timing analysis, hardware verification checklist, Issue 1 root-cause and fix)

### Verified on hardware
- T5 first poll at t=68 s: boot grace (8 s) + poll interval (60 s) + scheduler jitter (+337 ms)
- FG6485A poll: 509 ms (2 × 200 ms timeout + 100 ms retry), fault set correctly
- S200 poll: 511 ms (2 × 200 ms timeout + 100 ms retry), fault set correctly
- `sensor_reading_t` summary log: `T=0°C RH=0% ws=0.0 m/s wd=0° | avg T=0 RH=0 ws=0.0 wd=0° [win T=1 RH=1 W=1]`
- Q6 overwrite accepted; LOG_SENSOR posted without Q3 overflow
- No WDT resets, panics, or crashes during verification run

### Cross-validated with sensor emulator (greenhouse-Controller-Modbus-sensor-emulator Phase 2)
- Emulator received and CRC-validated all T5-generated frames: FG6485A `01 03 00 00 00 02 C4 0B` ✅, S200 wind `2C 04 00 08 00 0C 77 B0` ✅
- Exactly 2 frames per sensor per cycle observed by emulator — retry count correct
- S200 Frame 3 (heater temp, reg `0x001C`) absent when Frame 2 returns exception — early-exit path confirmed
- 60 s poll interval confirmed end-to-end (emulator timestamps: 10:59:42 → 11:00:42)

---

## [1.4.0] — 2026-05-03

*Phase 1 — Data Foundation (T4) implemented: central NVS config store, sensor ring buffers, RTC read/write, sunrise/sunset, Q4/Q6/TN4 handling, and thread-safe getter API for T1/T2/T3/T6.*

### Added
- `firmware/src/data_manager/data_manager.h` — full T4 public API:
  - `cfg_shadow_t` struct: all NVS-backed config fields plus derived fields (`is_daytime`, `current_unix_ts`, `sunrise_mins_utc`, `sunset_mins_utc`)
  - `dm_ring_buf_t` / `DM_RING_DEPTH = 360` — sensor history ring buffer type
  - `DM_NOTIFY_NTP_SYNCED` — TN4 task notification bit for T10 → T4 NTP sync signal
  - Thread-safe getters: `dm_cfg_snapshot()`, `dm_meas_snapshot()`, `dm_ring_read()`, `dm_get_is_daytime()`, `dm_get_unix_time()`, `dm_get_poll_interval_s()`, `dm_get_travel_s()`, `dm_get_dwell_open_min()`, `dm_get_dwell_close_min()`, `dm_get_led_config()`
- `firmware/src/data_manager/data_manager.cpp` — full T4 implementation:
  - Boot: loads all NVS namespaces (`climate`, `wind`, `motor`, `system`) into `cfg_shadow_t`; applies TZ string via `setenv/tzset`; reads DS1307 RTC under MX1 and calls `settimeofday()` to seed system clock
  - `rtc_dt_to_unix()` — manual UTC-correct conversion (leap year aware, no `timegm()` dependency)
  - Main loop: Q6 handler updates MX2 + MX3 ring, posts `LOG_SENSOR` to Q3, notifies T3 (TN1) and T6 (TN2); Q4 handler validates and applies config updates to NVS + MX4 shadow; TN4 handler syncs DS1307 from NTP time; periodic (~60 s) RTC re-read refreshes `current_unix_ts` and `is_daytime`
  - Location change in Q4 immediately recomputes sunrise/sunset via `update_sun_times()`
  - Boot `LOG_SYSTEM` event posted to Q3

### Changed
- `firmware/firmwareImplementationPlan.md` — Phase 1 marked ✅ done; implementation notes added

### Verified on hardware
- T4 periodic RTC re-read observed at t=60 s and t=120 s: `[T4] RTC: 2026-04-12 17:24:15 UTC  unix=1776014655  daytime=yes`
- DS1307 I2C read functional under MX1; `rtc_dt_to_unix()` produces consistent unix timestamps (Δ between reads matches elapsed wall-clock time)
- `sunrise_is_daytime()` returns correct result (`daytime=yes` at 17:19–17:24 UTC, 52°N)
- No WDT resets, panics, or crashes in 155 s continuous run
- Early boot messages (NVS load log line, boot RTC read) are not visible via USB-CDC due to USB re-enumeration delay (~3–5 s); this is a known USB-CDC limitation, not a code defect — documented as Finding 1 in `firmwareImplementationResults.md`

---

## [1.3.4] — 2026-05-03

*T2 relay controller integration tests expanded to 13 tests (IT-01–IT-13), two structural test defects fixed, and the full suite verified on hardware — all 13 tests pass.*

### Added
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-10 through IT-13:
  - **IT-10** OPEN travel expiry + dwell enforcement: CMD_OPEN CH1 → waits 26 s for travel timer expiry → CH_OPEN; verifies SRC_T6 CMD_CLOSE blocked during the 3 s dwell window; verifies SRC_T6 CMD_CLOSE accepted after dwell expiry
  - **IT-11** CLOSE→OPEN reversal gap: CMD_OPEN during MOVING_CLOSE → CH_GAP_TO_OPEN → 2 s gap → MOVING_OPEN (symmetric counterpart to IT-04)
  - **IT-12** CMD_RESUME no-op: RESUME acknowledged by T2 with no relay state change
  - **IT-13** Invalid channel discarded: CMD_OPEN ch=0 and ch=4 produce `[W]` log entries and no relay change

### Changed
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 structural fix: replaced the two-phase poll (65 s "recal start" assertion + 185 s "recal done" poll) with a single 300 s completion loop; the two-phase approach failed when a second alarm during the guard pushed re-cal start beyond the 65 s window, causing Unity's `longjmp` to skip the completion wait and leave T2 blocked in guard+recal when IT-10 started (Q1 command batch-processing defect — see `firmwareImplementationResults.md` Issue 4)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 and IT-09 manual step prompts, IT-09 completion message, and end-of-test banner all converted from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` was silently dropped by the USB-CDC driver under test task scheduling conditions (see `firmwareImplementationResults.md` Issue 5), rendering both interactive prompts invisible and causing IT-07 to fail (alarm never connected)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 prompt updated with explicit instruction to hold jumper still for ≥1 s (75 ms debounce requirement); duration table updated to show 600 s logic analyser capture window
- `firmware/firmwareImplementationResults.md` — test results table completed (13/13 PASS); logic analyser verification data and contact bounce measurement added; Issues 4 and 5 documented; handover state updated

### Verified on hardware
- All 13 integration tests pass (serial output + logic analyser CSV captured and cross-verified)
- Contact bounce measurement: 329 ms bounce on IT-07 jumper insertion correctly filtered by 75 ms debounce; alarm confirmed 83 ms after stable LOW (nominal 75 ms)
- All relay timing within 35 ms of expected values across boot calibration, guard, re-calibration, travel, gap, and dwell transitions

---

## [1.3.3] — 2026-05-03

*60 s guard time introduced after motor alarm clearance before CLOSE_ALL re-calibration starts. An open issue for alarm contact jitter has been added to the implementation plan.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `handle_alarm_clearance()` now clears `EG1_BIT_MOTOR_ALARM` and logs clearance immediately, then blocks for `ALARM_GUARD_MS = 60 000 ms` (12 × 5 s chunks) before starting re-calibration; re-checks pin at guard expiry and aborts if alarm re-asserted
- `firmware/src/relay_controller/relay_controller.cpp` — added `ALARM_GUARD_MS` and `ALARM_GUARD_CHUNK_MS` constants
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 updated: alarm bit check remains at 15 s (bit cleared before guard); new 65 s poll for re-cal start (waits for guard to expire); recal-complete poll unchanged; expected duration updated to ~500 s

### Added
- `firmware/firmwareImplementationPlan.md` — open issue **#1c Alarm contact jitter**: single end-of-guard pin re-check does not detect bounce within the guard window; mitigations listed; decision deferred

### Documentation
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` (×3 locations), `firmware/firmwareImplementationPlan.md` (#1a, #1b, integration test), `firmware/firmwareImplementationResults.md` — all alarm clearance descriptions updated to include the 60 s guard step and abort-on-re-assertion behaviour

---

## [1.3.2] — 2026-05-03

*Alarm polarity corrected throughout: the RRK-3 opto-coupler output is active-low (GPIO42 LOW = alarm active, HIGH = alarm cleared with INPUT_PULLUP), not active-high as previously documented.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — debounce logic now reads `alarm_signal = (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW)`; onset condition `alarm_signal && !alarm_active`, clearance condition `!alarm_signal && alarm_active`
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 docblock and runtime prompt updated: "Connect GPIO42 to GND" (was "3.3 V"); file header "GPIO42→GND" updated
- `design/technicalHardwareDesignSpecification.md` — GPIO table: "active-low — logic LOW when contact is closed (alarm active)"; Open Issue #1: clarified active-low opto-coupler behaviour
- `design/technicalSoftwareDesignSpecification.md` — Motor alarm detection paragraph: "active-low: contact closed → GPIO 42 LOW; contact open → GPIO 42 HIGH"
- `firmware/firmwareImplementationResults.md` — Motor Alarm sequence table: rows corrected to "GPIO42 LOW → alarm asserted" and "GPIO42 HIGH → alarm cleared"

---

## [1.3.1] — 2026-05-03

*Inter-relay gap extended from 100 ms to 2 s to allow back-EMF to dissipate fully before reversing motor direction.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `RELAY_GAP_MS` increased from `100u` to `2000u`; all inline log strings updated
- `firmware/src/relay_controller/relay_controller.h` — Doxygen updated
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-04 timing updated: mid-gap spot-check at 1150 ms (was 80 ms), final CLOSE-HIGH check at 2650 ms (was 200 ms); all comments updated
- `firmware/firmwareImplementationPlan.md`, `firmware/firmwareImplementationResults.md` — gap value updated throughout
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` — gap value updated

---

## [1.3.0] — 2026-05-03

*Phase 2 firmware implemented and hardware-verified: T2 Relay Controller fully operational with per-channel window FSM, 2 s inter-relay gap enforcement, travel/dwell timers, deferred-ISR motor alarm handling, and a complete on-device Unity integration test suite.*

### Added

#### Firmware — Phase 2: Relay Controller (T2)
- `firmware/src/relay_controller/relay_controller.cpp` — full T2 implementation replacing the Phase 0 stub:
  - Per-channel internal FSM with two transient gap states (`CH_GAP_TO_OPEN`, `CH_GAP_TO_CLOSE`) enforcing a 2 s inter-relay gap on every direction reversal; both relays de-energised during the gap (audible as two distinct clicks spaced ~2 s apart)
  - Travel timers: relay energised for `(travel_mN + 5 s) × 1000 ms`; factory defaults M1/M2 = 26 s, M3 = 176 s; values read from NVS `motor/` namespace at T2 startup via `nvs_cfg_get_i32_or_default()`
  - Dwell timers: `SRC_T3` (Safety Monitor) commands bypass dwell; `SRC_T6` (Climate Control) commands respect it
  - `calib_close_all()`: synchronous blocking CLOSE_ALL at boot and after alarm clearance; de-energises each channel individually at its own travel deadline (M1/M2 at ~26 s, M3 at ~176 s) rather than waiting for the global maximum
  - Deferred-ISR motor alarm: `IRAM_ATTR` ISR on GPIO42 (CHANGE, not suppressed during MOVING); T2 loop confirms after 75 ms debounce by reading live pin state; on assertion: de-energises all 6 relays, sets `EG1_BIT_MOTOR_ALARM`, logs onset; on clearance: clears bit, logs clearance, re-runs `calib_close_all()` (FR-MA01–FR-MA08)
  - Q1 consumer: all commands discarded when `EG1_BIT_MOTOR_ALARM` is set (FR-MA03); 20 ms loop tick
- `firmware/src/relay_controller/relay_controller.h` — complete Doxygen header documenting all T2 responsibilities
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — on-device Unity integration test suite (9 tests):
  - IT-01 NVS factory defaults; IT-02 boot calibration (185 s); IT-03 CMD_OPEN relay energisation; IT-04 direction reversal gap (2 s, audible click … 2 s … click); IT-05 mutual exclusion (OPEN+CLOSE never simultaneously HIGH); IT-06 CLOSE_ALL T3 override; IT-07 alarm onset (interactive, GPIO42 jumper); IT-08 command rejection during alarm; IT-09 alarm clearance + re-calibration (interactive)
  - Minimal heartbeat task (`task_test_heartbeat`) spawned instead of T1 so PIN_HB_LED blinks at 1 Hz during the test run; WDT disabled via `esp_task_wdt_deinit()` (boot calibration exceeds default 5 s timeout)

#### Firmware — test environment
- `firmware/platformio.ini` — `[env:test_t2_relay]` section added; key settings: `test_build_src = yes` (required to compile `src/` during `pio test`), `build_src_filter = +<**> -<main.cpp>` (include all tasks, exclude conflicting `main.cpp`)

### Fixed
- **`calib_close_all()` ran all channels for M3's full 176 s** — rewrote to track per-channel deadlines; M1/M2 relays now de-energise at ~26 s while M3 continues to ~176 s
- **Heartbeat LED static during test run** — test `setup()` now initialises `PIN_HB_LED` as output and spawns a minimal toggle-only heartbeat task

---

## [1.2.0] — 2026-05-03

*Phase 0 firmware implemented and verified on hardware. Firmware implementation plan completed with all gaps and open issues resolved. Design documentation updated for MOTOR_ALARM operating state and C9 scope enforcement.*

### Added

#### Firmware — Phase 0 scaffold (fully verified on hardware)
- `firmware/src/main.cpp` — `setup()`: initialises GPIO, I2C, RTC, NVS, creates all RTOS primitives (Q1–Q6, EG1, MX1–MX5), spawns 12 tasks; `loop()`: self-deletes. T1 Watchdog/Heartbeat fully implemented: 500 ms WDT kick (`esp_task_wdt_add` / `esp_task_wdt_reset`), 1 Hz heartbeat LED toggle (GPIO 41), WS2812B RGB LED (GPIO 38) driven from EG1 state (Red = MOTOR_ALARM, Amber = fault/wind override, Green = normal), day/night brightness with configurable night window (22:00–06:00 default)
- `firmware/src/relay_controller/relay_controller.h/.cpp` — T2 stub
- `firmware/src/safety_monitor/safety_monitor.h/.cpp` — T3 stub
- `firmware/src/data_manager/data_manager.h/.cpp` — T4 stub
- `firmware/src/sensor_poll/sensor_poll.h/.cpp` — T5 stub
- `firmware/src/keypad_scan/keypad_scan.h/.cpp` — T7 stub
- `firmware/src/ui_display/ui_display.h/.cpp` — T8 stub
- `firmware/src/network_manager/network_manager.h/.cpp` — T10 stub
- `firmware/src/web_server/web_server.h/.cpp` — T11 stub
- `firmware/src/mqtt_client/mqtt_client.h/.cpp` — T12 stub
- `firmware/firmwareImplementationResults.md` — Phase 0 implementation results: all six build/boot issues documented with root causes and fixes, verified hardware boot log, verification checklist, Phase 0 → Phase 1 handover state

#### Firmware — pre-Phase-0 modules (already complete before Phase 0 boot)
- `firmware/src/types/app_types.h` — complete shared type system: motor travel constants (M1/M2 = 21 s, M3 = 171 s, 5 s margin), all RTOS handle externs, queue item structs (`window_cmd_t`, `key_event_t`, `log_event_t`, `config_update_t`, `net_status_t`, `sensor_reading_t`), enums (`op_mode_t` with MOTOR_ALARM, `window_state_t`, `log_type_t`, `cmd_source_t` restricted to T3/T6 per C9), EG1 bit definitions (WIND_OVERRIDE, SENSOR_FAULT_T/W, OTA_IN_PROGRESS, MOTOR_ALARM; bit 1 reserved — MANUAL_OVERRIDE not supported by hardware)
- `firmware/src/climate_control/climate_control.h/.cpp` — Gap G resolved: graduated ventilation step table (step 1 = M1, step 2 = M1+M2, step 3 = M1+M2+M3), `vent_step_required_t()` and `vent_step_required_rh()` with close-hysteresis guard, `vent_resolve_conflict()` with three priority modes (CR_TEMP_FIRST, CR_RH_FIRST, CR_DEVIATION)
- `firmware/src/event_logger/event_logger.h/.cpp` — Gap H resolved: `log_post()` drop-oldest Q3 helper (evict-and-retry with portMUX spinlock-protected drop counter), `log_take_dropped_count()` atomic read-and-reset; T9 task stub
- `firmware/src/auth/pin_auth.h/.cpp` — Gap C resolved: PIN hashing via `mbedtls/sha256.h`; SHA-256(16-byte random salt ∥ PIN ASCII); salt stored in NVS `access/pin_salt`; per-role lockout with NVS-persisted expiry timestamps
- `firmware/src/data_manager/sunrise.h/.cpp` — Gap D resolved: NOAA General Solar Position Equations (±2 min accuracy); outputs UTC minutes from midnight; handles polar day/night; zero lat/lon defaults to daytime
- `firmware/firmwareImplementationPlan.md` — complete phased implementation plan (Phases 0–10), all 8 design gaps (A–H) resolved, 6 open issues (#1a/b–#6) resolved, integration test checklist

#### Design documentation
- `design/mocWebUIConciderations.md` — web UI mockup and layout considerations: dashboard, settings page, technical/admin page; responsive mobile-first approach; REST API endpoint mapping

### Changed

#### Design — MOTOR_ALARM operating state (new highest-priority mode)
Resolved Constraint C8: the RRK-3 provides a **single alarm output** that fires only when a motor runs to the **emergency switch** (not on normal manual operation). Manual operation detection via GPIO 42 is not achievable with the current hardware. All affected documents updated:
- `design/functionalRequirementsSpecification.md` — C8 resolved; FR-M08–M11 removed (manual detection not possible); §5.3a added with FR-MA01–FR-MA08 (Motor Alarm requirements: immediate relay de-energisation, highest-priority override, CLOSE_ALL re-calibration on clear, display message, logging); operating modes table updated
- `design/technicalHardwareDesignSpecification.md` — §4.5.2 updated: opto-coupler described as motor emergency stop alarm, not manual override detector
- `design/tasks.md` — T2 function updated (MOTOR_ALARM detection replaces manual override); GPIO 42 ISR description rewritten (not suppressed during MOVING; alarm assert: de-energise all relays + set EG1.MOTOR_ALARM; alarm release: clear flag + CLOSE_ALL re-calibration); EG1 table updated (MANUAL_OVERRIDE removed, MOTOR_ALARM bit 5 added); TN3 task notification removed
- `design/technicalSoftwareDesignSpecification.md` — T2 description updated; manual override detection section replaced with Motor Alarm detection; EG1 table updated; T6 EG1 inhibit flags updated; §5.12 RGB LED Red condition confirmed for MOTOR_ALARM
- `firmware/firmwareImplementationPlan.md` — open issues #1a/#1b updated; Phase 2 T2 GPIO 42 bullet rewritten; Phase 6 T6 EG1 check updated; integration test added

#### Design — C9 scope enforcement (no manual window commands from LCD/web/MQTT)
- `design/functionalRequirementsSpecification.md` — FR-WS05 updated: manual window commands explicitly excluded (C9)
- `design/softwareTestPlan.md` — ST-WI-008 replaced: no longer tests a manual command relay trigger; now verifies the web dashboard contains no window open/close controls
- `design/riskAssessment.md` — manual window via keypad removed from sensor-failure mitigation; MQTT attack chain updated (T12 does not post to Q1; attack surface narrowed to Q4 config updates only)
- `design/implementationPlan.md` — T8 manual window command bullet removed
- `firmware/src/types/app_types.h` — `cmd_source_t` limited to `SRC_T3` and `SRC_T6`; Q1 producer comment updated

#### Firmware configuration
- `firmware/platformio.ini` — `board_build.arduino.memory_type = qio_opi` (LOLIN S3 OPI PSRAM variant); `board_upload.offset_address = 0x20000` (app0 at 0x20000 due to 84 KB NVS); `monitor_dtr = 1` / `monitor_rts = 0`; `lib_extra_dirs = ../drivers`; `lib_ignore = WebServer`; `Adafruit NeoPixel`, `ESPAsyncWebServer`, `AsyncTCP` added to `lib_deps`; `-DCORE_DEBUG_LEVEL=3` and `-DCONFIG_NVS_LOG_CAPACITY=250` added

### Fixed
- **Crash loop on boot** (`rst:0x3 / Saved PC:0x403cdb0a`) — `board_upload.offset_address = 0x20000` added; firmware was being written to `0x10000` while the partition table directed the bootloader to find `app0` at `0x20000`
- **Silent serial output after boot** — all `setup()` diagnostics and T1 heartbeat switched from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` dropped silently when no USB-CDC host had the port open (DTR not asserted), while IDF log calls bypass the DTR check
- **`extern "C"` linkage conflict** in `event_logger.cpp` and `climate_control.cpp` — `extern "C"` blocks removed; all code is C++ throughout
- **`WebServer` LDF conflict** — `lib_ignore = WebServer` added to prevent Arduino's built-in WebServer from being pulled in alongside ESPAsyncWebServer

---

## [1.1.0] — 2026-05-01

*PCB v1.1.0 released following first hardware board test; S200 driver completed; design documentation extended.*

### Added
- `drivers/relay_sequence_test/` — new PlatformIO test project for hardware GPIO verification: sequences all 6 relay outputs (M1/M2/M3 OPEN/CLOSE, GPIO 12–16 and 21), heartbeat LED (GPIO 41), and opto-isolated input (OPTO_INPUT → M1 OPEN follow); includes `README.md` with wiring and usage
- `drivers/s200/` — complete SenseCAP S200 wind sensor driver (LIB-10): `s200.h` / `s200.cpp`, 11 unit tests (UT-S200-001..011), mock Modbus layer, `S200.md` driver documentation
- `hardware/Testing/20250501_HardwareTest.md` — first hardware board test report for PCB v1.1.0: 53 tests across 6 subsystems (voltages, GPIO relays/LEDs/input, 4×4 keyboard, SD card, RTC, LCD); Modbus hardware test pending
- `design/greenhouse_nvs_variables.xlsx` — NVS variable overview spreadsheet covering all namespaces, keys, types, and default values
- `design/hardwareComponentDiagram.puml` + `.png` — hardware component architecture diagram (PlantUML)
- `design/lcd_gui_state_diagram.puml` + `.png` — LCD GUI state diagram (PlantUML)
- `design/web_gui_state_diagram_auth.puml` + `.png` — web GUI authentication flow state diagram (PlantUML)
- `design/web_gui_state_diagram_settings.puml` + `.png` — web GUI settings state diagram (PlantUML)
- `design/web_gui_state_diagram_tech.puml` + `.png` — web GUI technical/admin state diagram (PlantUML)
- `hardware/pcb/Output/20260501_Schema.pdf` — updated schematic PDF
- `hardware/pcb/Output/20260501_Bestukkingstekening.pdf` — updated component placement drawing
- `hardware/pcb/Output/20260501_PrintBedrukkingVoorzijde.pdf` — updated silk screen front PDF

### Changed
- `hardware/pcb/` — PCB design bumped to **v1.1.0**; schematic and layout updated following hardware assembly and test
- `documentation/Sensors/W-Sensecap-S200/` — connector photo replaced (`image.jpg` → `Connector.png`)
- `drivers/driverDevelopmentPlan.md` — updated to reflect all drivers completed

### Removed
- `hardware/pcb/Output/20260424_*.pdf` — superseded by 20260501 fabrication outputs

---

## [1.0.0] — 2026-04-24

### Added
- `realisation/installation.md` — new connector wiring guide covering all 12 PCB connectors (J1–J12): 24 V DC input, AC mains input, motor relay outputs M1/M2/M3, RS485 sensor connections (FG6485A and SenseCAP S200), I2C display, 4×4 keypad, alarm output, RS485 termination jumper, and SD card

### Changed
- `design/technicalDesignSpecification.md` — corrections and open issue resolution following PCB alpha release:
  - **§4.5.1** — relay implementation updated from external relay module board to **6 × SRD-05VDC-SL-C relays integrated on PCB**, each driven by a dedicated **2N7000 N-channel MOSFET**; contact rating updated to 10 A / 250 VAC
  - **§4.9** — LED colours corrected to match PCB: HB heartbeat changed from amber to **green**; relay indicator LEDs changed from red to **amber**; circuit diagram and architecture diagram updated accordingly
  - **Issue #1 closed** — RRK-3 motor feedback signal defined: external relay contact closes on alarm state, drives opto-isolated input J10 (OPTO_INPUT); signal definition referenced to RRK-3 interface documentation
  - **Issue #3 closed (out of scope)** — RS485 sensor cable routing to SenseCAP S200 is the installer's responsibility and outside the controller project scope
  - **Issue #4 closed** — enclosure confirmed as Multicomp Pro **MC001110** (222 × 146 × 55 mm, IP67) following PCB layout and 3D clearance check
  - **Issue #5 closed** — relay module selection resolved by discrete relay integration on PCB (see §4.5.1)
  - **Issue #7 closed** — time source confirmed as **DS1307 RTC** with CR2032 backup, fitted on PCB; TR-HW08 satisfied
  - **Issue #9 added (open)** — J5 pins 5–6 carry HEATING_POS / HEATING_NEG nets for the SenseCAP S200 heater supply; feature not yet documented in TDS; decision on voltage, current, and specification deferred
  - **Issue #8/#9 closed — dropped** — J5 heater supply connection (HEATING_POS / HEATING_NEG, pins 5–6) removed from PCB; no firmware support or documentation required; pins 5–6 of J5 left unconnected

### Added (earlier — 2026-04-02)
- `design/functionalRequirementsSpecification.md` — new constraints and requirements:
  - **C11** — all user-configurable setpoints and thresholds (temperature °C, humidity %, wind speed, wind direction degrees, time durations in minutes) are expressed and stored as integers; fractional values are not supported; fractional sensor readings are rounded before comparison
  - **C12** — temperature control is permanently active; humidity control and wind protection are each independently enable/disable configurable by the administrator; both default to enabled and are persisted across power cycles
  - **FR-C11** — temperature-based climate control shall always be active; it cannot be disabled
  - **FR-C12** — administrator can enable or disable humidity-based climate control; when disabled, RH is ignored for window decisions and conflict resolution is suppressed
  - **FR-WS09** — administrator can enable or disable wind protection (speed and direction); when disabled, no wind-safety close commands are issued
  - **FR-WS10** — persistent LCD warning shown whenever wind protection is inactive
  - **FR-WS11** — disabling wind protection is an admin-only action and shall be logged
  - **FR-CF12** — administrator setting to enable/disable humidity control
  - **FR-CF13** — administrator setting to enable/disable wind protection
  - FR-CR01 updated: conflict resolution is only active when humidity control is enabled
- `design/technicalDesignSpecification.md` §5.1 — added "Setpoint and threshold data types" and "Feature enable/disable flags" design constraints with NVS key names (`rh_ctrl_en`, `wind_prot_en`) and default values
- `design/technicalSoftwareDesignSpecification.md`:
  - §3 Design Constraints — added integer setpoint constraint (`int16_t` NVS storage, rounding rule) and feature enable/disable flag constraints
  - §4.3 T3 Safety Monitor — updated to check `wind_prot_en` flag before evaluating thresholds; suppresses CLOSE_ALL when wind protection is disabled
  - §5.2 Climate Control Logic — RH evaluation conditional on `rh_ctrl_en`; conflict resolution suppressed when humidity disabled; CLOSE_ALL from T3 conditional on `wind_prot_en`; log entry `value_a`/`value_b` fields updated to reflect integer values without scaling
  - §5.10 NVS Configuration Storage Layout — added Type column; `rh_ctrl_en` added to `climate` namespace; `wind_prot_en` added to `wind` namespace; types specified for all namespaces
- `firmware/` directory with PlatformIO project skeleton:
  - `firmware/platformio.ini` — board (`lolin_s3`), Arduino framework, 115200 baud monitor, commented library dependency stubs
  - `firmware/src/README.md` — describes expected source modules and their responsibilities
  - `firmware/test/README.md` — describes unit test structure, Unity framework, and `pio test` usage
- `hardware/` directory with KiCad PCB project structure:
  - `hardware/pcb/README.md` — KiCad tool version, expected project files, design references
  - `hardware/fabrication/README.md` — expected fabrication outputs per release, KiCad export instructions
- `README.md` — completely rewritten to describe the Greenhouse Ventilation Controller (replacing placeholder content from an unrelated project)
- `license.md` — dual-licence information for software and non-software artefacts
- `LICENSE` — canonical licence text for the repository

### Changed
- **Licences updated** throughout the repository:
  - Software (firmware and all code): source-available, non-commercial licence — free to use and modify; redistribution and commercial use not permitted
  - Hardware design files, documentation, and images: CC BY-NC-ND 4.0 (Attribution-NonCommercial-NoDerivatives 4.0 International)
- `design/technicalDesignSpecification.md` §2.1 — "Open Source" section replaced with "Project Licences" reflecting the dual-licence structure
- `design/technicalDesignSpecification.md` §2.5 — repository structure diagram expanded to include `documentation/`, `Archive/`, and all root-level files
- `design/technicalDesignSpecification.md` §3.2 — design principle "open and reproducible" updated to align with source-available rather than open-source framing

---

## [0.2.0] — 2026-03-26

*TDS hardware section complete; FRS v0.2 finalised.*

### Added
- `design/functionalRequirementsSpecification.md` v0.2 — complete functional and technical requirements:
  - Sensing (internal climate: FG6485A T/RH; external weather: SenseCAP S200 wind)
  - Window actuation (M1, M2, M3 via Hotraco RRK-3; timed relay pulses; dwell-time enforcement)
  - Automatic climate control (T and RH setpoints, hysteresis, graduated ventilation strategy)
  - Wind safety (speed threshold, direction exclusion angle, immediate close override)
  - Conflict resolution, window state tracking, operating modes
  - Local user interface (4×4 keypad, 16×2 LCD)
  - WiFi connectivity, MQTT integration, access control, event logging
- `design/technicalDesignSpecification.md` v0.2 — hardware design complete:
  - §4.1 Microcontroller: WEMOS LOLIN S3 (ESP32-S3, 16 MB flash, 8 MB PSRAM)
  - §4.2 Sensors: Seeed SenseCAP S200 (ultrasonic wind, Modbus RS485) and FG6485A (T/RH, Modbus RS485)
  - §4.3 Modbus RS485 bus topology and parameters
  - §4.4 User interface: 4×4 membrane keypad and Waveshare LCD1602 I2C (PCF8574)
  - §4.5 Motor controller interface: 6-ch relay board, potential-free contacts, opto-isolated feedback input
  - §4.6 Real-Time Clock: DS1307, I2C, CR2032 battery backup, external 32.768 kHz crystal
  - §4.7 Power supply: two-stage architecture (230 VAC → 24 VDC → 5 VDC), power budget analysis
  - §4.8 SD card (optional, SPI, FAT32)
  - §4.9 Status LEDs: PWR (green), HB heartbeat (amber), 6 × relay activity (red, shared GPIO)
  - §4.10 Enclosure: Multicomp Pro MC001110, 222 × 146 × 55 mm, IP67, transparent cover
  - §4.11 GPIO and peripheral assignment summary

### Changed
- Sensor selection: SenseCAP S200 confirmed as wind sensor (ultrasonic, no moving parts, single mast)

---

## [0.1.1] — 2026-03-07

*Simulation model refined; physical parameters recorded.*

### Changed
- Simulation model simplified to steady-state plant model; ACH parameters merged; humidity and temperature thresholds unified
- Measured physical greenhouse parameters recorded in simulation environment data

---

## [0.1.0] — 2026-03-06

*First complete design iteration committed.*

### Added
- `Archive/Iteration1/design.md` — first iteration design document including:
  - §3.7 Farmer-accessible configuration parameters
  - Partial window opening decision (not supported — recorded in §1.5 and §5)
  - Anti-oscillation guard (`t_min_dwell`) for motor protection
- `Archive/Iteration1/plantTranspirationRateConsiderations.md` — analysis of plant transpiration rate and its effect on humidity control
- `Archive/Iteration1/setpointConsiderations.md` — recommended T and RH setpoints for typical greenhouse crops
- `Archive/Iteration1/stateDiagram.puml` — PlantUML state diagram for the controller operating modes
- `Archive/Iteration1/Simulation/greenhouse_simulation.py` — Python simulation driven by historical weather data (5 scenarios: daytime solar gain, high humidity, 24 h day–night cycle, T below setpoint / RH critical, motor stall)
- `Archive/Iteration1/Environment/airTemperature_2025-05-01_to_2025-09-01.csv` — historical air temperature data used in simulation
- `Archive/Iteration1/Environment/outside_conditions.py` — outside conditions model for simulation
- `documentation/` — component reference material: sensor datasheets and notes (FG6485A, SenseCAP S200, RHS-10, RTS-2, keypad, anemometer)

### Changed
- Greenhouse physical layout documented: 40 × 16 m, east–west orientation; M1 south roof, M2 north roof, M3 north wall
- Window-to-motor mapping and RRK-3 circuit schematic references added to design

---

## [0.0.1] — 2026-03-05

*Project initialised.*

### Added
- Initial repository structure
- `design/technicalDesignSpecification.md` v0.1 — initial hardware architecture and component candidate evaluation
- `code_of_conduct.md`, `contributing.md` — community standards

