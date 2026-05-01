# Greenhouse Controller — Firmware Implementation Plan

## Context

All 11 hardware drivers (LIB-1 through LIB-11) are complete and tested. The `firmware/src/` directory exists but contains only a README skeleton. This plan describes how to implement the 13-task FreeRTOS application firmware in a logical, dependency-correct order, using the existing drivers as-is. Stubs are used for deferred tasks so every phase produces a buildable, bootable binary.

---

## Module Structure (`firmware/src/`)

```
firmware/src/
├── main.cpp                        # setup(): driver init, RTOS primitives, task spawn
├── types/
│   └── app_types.h                 # ALL shared structs, enums, handle declarations (extern)
├── data_manager/
│   ├── data_manager.h / .cpp       # T4 — NVS load/save, measurements, sunrise/sunset
├── relay_controller/
│   ├── relay_controller.h / .cpp   # T2 — window FSM, relay GPIO, GPIO42 ISR
├── safety_monitor/
│   ├── safety_monitor.h / .cpp     # T3 — wind threshold evaluation, CLOSE_ALL
├── sensor_poll/
│   ├── sensor_poll.h / .cpp        # T5 — Modbus poll loop, sliding averages, fault flags
├── climate_control/
│   ├── climate_control.h / .cpp    # T6 — setpoint eval, hysteresis, conflict resolution
├── keypad_scan/
│   ├── keypad_scan.h / .cpp        # T7 — 20ms scan, key-repeat, Q2 post
├── ui_display/
│   ├── ui_display.h / .cpp         # T8 — LCD FSM, PIN session, menu (≤4 key presses)
├── event_logger/
│   ├── event_logger.h / .cpp       # T9 — log_post() helper, NVS + SD CSV serialisation
├── network_manager/
│   ├── network_manager.h / .cpp    # T10 — WiFi AP/client, NTP, Q5 status
├── web_server/
│   ├── web_server.h / .cpp         # T11 — ESPAsyncWebServer, LittleFS, REST API
├── mqtt_client/
│   ├── mqtt_client.h / .cpp        # T12 — publish/subscribe (stub initially)
└── ota_manager/
    ├── ota_manager.h / .cpp        # T13 — dual-bank OTA, 3-fail rollback (stub initially)
```

**Key support file to add:** `firmware/config/pin_config.h` — extend with `#define PIN_RGB_LED 38`.

---

## Identified Omissions & Gaps

| # | Gap | Status | Resolution |
|---|-----|--------|-----------|
| **A** | No `partitions.csv` — dual OTA+LittleFS layout requires a custom partition table | ✅ Solved | `firmware/partitions.csv` created (nvs 84 KB, otadata 8 KB, app0/app1 2 MB each, lfs0/lfs1 1 MB each); `platformio.ini` updated with `board_build.partitions = partitions.csv`; TSDS §5.9 and THDS §4.1.2 updated. |
| **B** | `PIN_RGB_LED` (GPIO38) missing from `pin_config.h` | ✅ Solved | `#define PIN_RGB_LED 38` added to `firmware/config/pin_config.h` (Indicators section); `adafruit/Adafruit NeoPixel @ ^1.12.3` added to `lib_deps`; I2C comment corrected (0x27 → 0x3E). |
| **C** | No SHA-256 library specified for PIN hashing | ✅ Solved | `firmware/src/auth/pin_auth.h/.cpp` created. Hash: `SHA-256(salt \|\| pin_ascii)` via `mbedtls/sha256.h` (bundled, no extra lib). 16-byte salt from `esp_fill_random()` stored in NVS `access/pin_salt` at first boot. Per-role lockout with NVS-persisted expiry timestamps. TSDS §5.4 and NVS table updated. |
| **D** | No sunrise/sunset algorithm specified | ✅ Solved | `firmware/src/data_manager/sunrise.h/.cpp` created. NOAA General Solar Position Equations (10 steps, ±2 min accuracy). Outputs UTC minutes from midnight. Handles polar day/night and FR-DN05 (zero lat/lon → daytime default). TSDS §4.3, `tasks.md` T4, and FRS FR-DN02 updated. |
| **E** | `ESPAsyncWebServer` / `AsyncTCP` not in `lib_deps` | ⬜ Pending | Add `mathieucarbou/ESPAsyncWebServer` (IDF5-compatible fork) + `AsyncTCP` to `platformio.ini` before Phase 9. |
| **F** | Motor travel time vs. dwell time conflated in NVS schema | ⬜ Pending | Motor travel times → compile-time constants in `app_types.h` (`MOTOR_M1_TRAVEL_MS 21000`, `MOTOR_M2_TRAVEL_MS 21000`, `MOTOR_M3_TRAVEL_MS 171000`); NVS `dwell_open_mN`/`dwell_close_mN` = minimum hold times only. |
| **G** | Graduated ventilation channel assignment undefined | ⬜ Pending | Default step table: Step 1 = M1, Step 2 = M1+M2, Step 3 = M1+M2+M3; compile-time lookup in `climate_control.cpp`. |
| **H** | Q3 drop-oldest not achievable with plain FreeRTOS queue | ⬜ Pending | In `log_post()`: if `xQueueSend(..., 0)` fails, call `xQueueReceive(..., 0)` to evict oldest, then retry send. |

### Open Issue Resolutions

| Issue | Resolution |
|-------|------------|
| **#1 GPIO42 debounce** | `attachInterrupt(PIN_OPTO_INPUT, isr_handler, CHANGE)` in T2; deferred-ISR pattern: ISR only sets volatile flag + timestamp; T2 loop confirms after 75 ms |
| **#1 Calibration cycle** | On `MANUAL_OVERRIDE` clear, T6 posts `CLOSE_ALL` then waits `MOTOR_M3_TRAVEL_MS + 10s` margin before resuming AUTOMATIC |
| **#2 Ring buffer depth** | 360 entries per channel (T, RH, wind_speed, wind_dir) = 11.5 KB total; fits in internal RAM with headroom |
| **#3 NTP timezone** | `setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();` after `configTime()`; store TZ string in NVS `system/tz_str` with that default |
| **#4 MQTT auth** | Username + password over plain TCP (same accepted-risk basis as no-HTTPS); store password in NVS `mqtt/password`; document accepted risk |
| **#5 J5 heater supply** | No GPIO assigned in `pin_config.h` → always-on hardware solution; T5 logs anomalous `heating_temperature_c` values only |
| **#6 Snapshot interval** | Snapshot triggered by sensor data arrival via Q3; period = `poll_interval`; no separate configurable parameter |

---

## Implementation Phases

### Phase 0 — Project Scaffold & Watchdog/Heartbeat (T1)
**Goal:** Builds, boots, watchdog kicking, HB LED blinking, all tasks present as stubs.

Files to create/modify:
- `firmware/partitions.csv` — ✅ **done** (Gap A)
- `firmware/platformio.ini` — ✅ **done** (Gap A: `board_build.partitions`; Gap B: `Adafruit NeoPixel` lib); still needs driver include paths added when src/ layout is finalised
- `firmware/config/pin_config.h` — ✅ **done** (Gap B: `PIN_RGB_LED 38`, I2C address corrected)
- `firmware/src/types/app_types.h` — **create**: complete shared type system (all structs, queue depths, task stack sizes, EG1 bits, extern handles)
- `firmware/src/main.cpp` — **create**: `setup()`: driver inits, create all RTOS primitives, spawn all tasks; `loop()`: empty
- All task `.h/.cpp` files — **create**: stub implementations (`vTaskDelay(portMAX_DELAY)`)
- **T1 fully implemented:** 500ms watchdog kick, HB LED toggle (GPIO41), WS2812 status LED (GPIO38 via `PIN_RGB_LED`) driven from EG1 state using `Adafruit NeoPixel`

Drivers used: LIB-1 (GPIO), LIB-2 (I2C), LIB-3 (RTC probe), LIB-7 (NVS init)

Verification: Board boots, serial shows banner, HB LED blinks 1 Hz, no crash in 5 min.

---

### Phase 1 — Data Foundation (T4)
**Goal:** Central data store operational; NVS round-trips verified; RTC reading.

Files: `data_manager.h/.cpp` — **create**; `sunrise.h/.cpp` — ✅ **done** (Gap D)

Implementation:
- Load all NVS namespaces at boot using `nvs_cfg_get_i32_or_default()` / `nvs_cfg_get_str_or_default()`
- Read DS1307 via `rtc_get_time()` under MX1; warn if `rtc_oscillator_stopped()`
- Compute `is_daytime` from sunrise/sunset algorithm (lat/lon from NVS `system`)
- Task loop: consume Q6 (sensor readings → update MX2 measurements + MX3 ring buffers → send TN1 to T3 + TN2 to T6); consume Q4 (config updates → validate → NVS write → update MX4); consume TN4 (WiFi connected → trigger NTP → `rtc_set_time()`)
- Expose thread-safe getter functions for T3/T6 callers

Drivers used: LIB-7 (nvs_cfg_*), LIB-3 (rtc_*), LIB-2 (via RTC under MX1)

Verification: Inject Q6/Q4 items from `setup()` test harness; verify state via serial; verify NVS persistence across soft reset.

---

### Phase 2 — Relay Controller (T2)
**Goal:** Safe, timing-correct motor control; mutual exclusion enforced; GPIO42 feedback working.

Files: `relay_controller.h/.cpp`

Implementation:
- Init: 6 relay GPIOs as OUTPUT, all de-energised; GPIO42 as INPUT_PULLUP
- Boot sequence: issue CLOSE_ALL on all channels (establishes known CLOSED state; waits full travel time per channel)
- Window FSM per channel: `UNKNOWN → CLOSED → MOVING_OPEN → OPEN → MOVING_CLOSE → CLOSED`
- Before asserting any relay: de-energise complementary relay + 100ms gap
- Motor timer: `vTaskDelay(MOTOR_MN_TRAVEL_MS)` while relay energised; then de-energise and advance FSM
- Dwell timer: load `dwell_open_mN` / `dwell_close_mN` from T4 (MX4); reject commands arriving too early
- Q1 consumer: T3 CLOSE_ALL commands preempt pending T6 commands (check source field)
- GPIO42 ISR: volatile flag + timestamp; T2 main loop confirms after 75ms debounce; on confirmation, set EG1 MANUAL_OVERRIDE, send TN3 to T6, post LOG event to Q3

Drivers used: LIB-1 (gpio_write for 6 relays + gpio_read for GPIO42)

Verification: Logic analyser — never simultaneous OPEN+CLOSE on same channel; confirm 100ms gap; confirm travel timing; confirm GPIO42 debounce with bench toggle.

---

### Phase 3 — Sensor Polling (T5)
**Goal:** Live T/RH and wind data flowing into T4.

Files: `sensor_poll.h/.cpp`

Implementation:
- Poll loop: wait `poll_interval_ms` (read from T4 MX4)
- Read FG6485A via `fg6485a_read_measurements(FG6485A_ADDR, &meas)` — retry once on failure; on second failure set `EG1_BIT_SENSOR_FAULT_T`, post fault log to Q3
- Read S200 via `s200_read_measurements(S200_ADDR, &wind)` — same retry/fault logic for `EG1_BIT_SENSOR_FAULT_W`
- Compute sliding average: circular sum buffer, size = `avg_window_min * 60 / poll_interval`, clamped 1–360
- Post `sensor_reading_t` to Q6 via `xQueueOverwrite()` (depth-1 queue, latest only)
- **Do not use** `fg6485a_task()` or `s200_task()` from LIB-10/11 — T5 is the sole Modbus master

Drivers used: LIB-6 (modbus_rtu), LIB-10 (fg6485a_read_measurements), LIB-11 (s200_read_measurements)

Verification: Serial log shows readings at configured interval; unplug sensor → fault flag within one cycle; replug → clears on next success.

---

### Phase 4 — Safety Monitor (T3)
**Goal:** Wind safety response correct and fast; must be live before climate automation.

Files: `safety_monitor.h/.cpp`

Implementation:
- Block on TN1 (new wind data from T4)
- Acquire MX2: read `wind_speed_max_ms`, `wind_dir_avg_deg`; acquire MX4: read `v_max`, `dir_excl_low`, `dir_excl_high`, `wind_prot_en`
- If `wind_prot_en` false: skip, clear WIND_OVERRIDE if set
- If `EG1_BIT_SENSOR_FAULT_W` set: treat as worst-case (threshold exceeded)
- Direction exclusion zone wraps correctly through 0°
- On threshold exceeded: set EG1 WIND_OVERRIDE, post CLOSE_ALL (source=T3) to Q1, post log to Q3
- On condition clear: clear EG1 WIND_OVERRIDE, post RESUME to Q1, post log to Q3

Drivers used: None directly

Verification: Inject wind > v_max via Q6 from test harness; verify CLOSE_ALL on Q1 (serial log); verify WIND_OVERRIDE bit set.

---

### Phase 5 — Event Logger (T9)
**Goal:** All events persistently recorded before automation goes live.

Files: `event_logger.h/.cpp`

Implementation:
- `log_post(log_event_t*)` helper: if `xQueueSend(Q3, ..., 0)` fails → evict oldest via `xQueueReceive(Q3, &discard, 0)` → retry send (drop-oldest policy)
- T9 task loop: `xQueueReceive(Q3, &evt, portMAX_DELAY)` → `nvs_log_append(&evt)`; if SD available → `storage_sd_write_append(current_file, csv_line)`
- SD rotation: track file size via `storage_sd_file_size()`; rotate at 512 KB; delete oldest when count > 10 (`storage_sd_list_csv()` + `storage_sd_delete()`)
- No SD card: graceful fallback to NVS ring buffer; log SYSTEM event on mount failure
- Periodic snapshot: T4 posts a SENSOR log_event_t to Q3 every time it receives fresh data from Q6 (eliminates separate timer)

Drivers used: LIB-7 (nvs_log_append), LIB-8 (storage_sd_*)

Verification: CSV file created on SD; 5 manual events appear; rotate test at 512 KB; NVS fallback without SD.

---

### Phase 6 — Climate Control (T6)
**Goal:** Autonomous temperature/humidity-driven ventilation with conflict resolution.

Files: `climate_control.h/.cpp`

Implementation:
- Block on TN2 (new sensor data) and TN3 (manual override signal from T2)
- On TN3: enter MANUAL_OVERRIDE inhibit; post CLOSE_ALL to Q1; start calibration timer (`MOTOR_M3_TRAVEL_MS + 10s`); on timer expiry, clear EG1 MANUAL_OVERRIDE, post log, resume AUTOMATIC
- On TN2: check EG1 flags (WIND_OVERRIDE, MANUAL_OVERRIDE, SENSOR_FAULT_T) — skip if any set
- Acquire MX2: read T_avg, RH_avg; acquire MX4: read active setpoints (day vs. night via T4's `is_daytime`)
- Temperature evaluation with hysteresis (`hyst_t`); graduated steps per Gap G step table
- Humidity evaluation (only when `rh_ctrl_en = true`) with hysteresis (`hyst_rh`)
- Conflict resolution via `cr_priority` (CR_TEMP_FIRST / CR_RH_FIRST / CR_DEVIATION)
- Post resolved `window_cmd_t` items to Q1; post log to Q3

Stub acceptable: Graduated ventilation can initially be all-or-nothing (all 3 channels); graduated step table added in a follow-on commit.

Drivers used: None directly

Verification: Inject T > T_max_day via Q6; confirm OPEN commands on Q1; inject conflicting T+RH demands; confirm conflict resolution per `cr_priority`.

---

### Phase 7 — UI Layer (T7 + T8)
**Goal:** Local keypad and LCD interface for commissioning and daily operation.

Files: `keypad_scan.h/.cpp`, `ui_display.h/.cpp` — **create**; `auth/pin_auth.h/.cpp` — ✅ **done** (Gap C)

**T7:**
- `keypad_scan()` every 20ms; if char returned ≠ `KP_NO_KEY`, post `key_event_t` to Q2 (non-blocking)
- Key-repeat: same key held > 500ms → repeat events at 100ms interval

**T8:**
- `lcd_init()` under MX1 at boot
- Status screen: auto-rotate every 5s through T/RH, wind, window states, alarms, network
- Menu FSM states: `STATUS_DISPLAY → MENU_ROOT → MENU_[CLIMATE|WIND|ACCESS|WIFI] → EDIT_VALUE / PIN_ENTRY`
- Max 4 keypresses from status screen to any first-level setting (FR-UI07)
- PIN entry: numeric; call `pin_auth_verify(PIN_ROLE_FARMER/ADMIN, entered_str)` — hashing, salt, and lockout all handled by `auth/pin_auth.h` (Gap C ✅)
- Config edit: read current from T4 (MX4), accept input, validate range, post `config_update_t` to Q4
- Manual window commands: in farmer/admin session, post `window_cmd_t` to Q1
- Receive Q5 (network status); display WiFi state
- Session timeout: software timer reset on each keypress; on expiry, log session close, return to STATUS_DISPLAY
- **LCD I2C address: 0x3E (AiP31068L bridge)** — not 0x27

Stubs acceptable: WiFi AP toggle screen, advanced admin settings can display "Not implemented" initially.

Drivers used: LIB-4 (lcd_*), LIB-5 (keypad_scan), LIB-2 (via LCD under MX1)

Verification: Status screen rotates; navigate to T_max_day within 4 keypresses; change value; verify NVS write; PIN lockout after 5 wrong entries; session timeout.

---

### Phase 8 — Network Manager (T10)
**Goal:** WiFi AP/client, NTP sync, DS1307 update, status to T8.

Files: `network_manager.h/.cpp`

Implementation:
- WiFi AP: start on admin command (Q4 `wifi_ap_enable`); SSID `"Greenhouse-XXXX"` (last 2 MAC bytes); auto-shutdown timer from NVS
- WiFi client: connect from NVS SSID/PSK; exponential backoff reconnect (max 60s interval); report state changes to Q5
- NTP: `configTime(0, 0, "pool.ntp.org")`; then `setenv("TZ", tz_str_from_nvs, 1); tzset();`; on sync confirmed, send TN4 to T4
- T4 on TN4: calls `rtc_set_time()` to sync DS1307

Drivers used: None from driver library (Arduino WiFi + ESP-IDF SNTP)

Verification: AP appears in phone scan; auto-shuts after timeout; client connects; NTP sync verified against reference; DS1307 updated.

---

### Phase 9 — Web Server (T11)
**Goal:** Browser-accessible dashboard and configuration.

Files: `web_server.h/.cpp`

Implementation:
- Mount active LittleFS via `littlefs_active_partition()` + `littlefs_mount()`; verify `manifest.json`
- ESPAsyncWebServer on port 80
- REST API: `GET /api/status`, `GET /api/config`, `POST /api/config`, `POST /api/command`, `GET /api/log`
- All endpoints except `/login` require valid session cookie (random 16-byte hex token, server-side map)
- Static files served from LittleFS under MX5; 503 if `EG1_BIT_OTA_IN_PROGRESS` set
- Stubs: `POST /api/ota/*` endpoints return 501 until T13 is implemented

Drivers used: LIB-9 (littlefs_mount, littlefs_read, littlefs_active_partition, under MX5)

Verification: Login page loads; farmer can edit T_max_day; admin settings hidden from farmer; 401 on unauthenticated calls; NVS update verified via LCD.

---

### Phase 10 — OTA Manager (T13)
**Goal:** Safe firmware and web asset updates with automatic rollback.

Files: `ota_manager.h/.cpp`

Implementation:
- Spawned on-demand by T11 when upload begins; `xTaskCreate` in web_server.cpp
- Firmware OTA: `esp_ota_begin()` → stream `esp_ota_write()` → `esp_ota_end()` → `esp_ota_set_boot_partition()` → reboot
- Web asset OTA: receive zip to PSRAM (`ps_malloc`); mount inactive LittleFS; extract via miniz; write `manifest.json` last; unmount; switch partition
- 3-fail rollback: NVS `system/ota_fail_count`; reset on 30s healthy uptime; increment on boot failure; at 3 → `esp_ota_mark_app_invalid_rollback_and_reboot()`
- Set/clear `EG1_BIT_OTA_IN_PROGRESS`; post log events to Q3

Phase 10.1 stub: firmware OTA only; web asset extraction stubbed to Phase 10.2.

Drivers used: LIB-9 (littlefs_mount, littlefs_write, littlefs_unmount, littlefs_active_partition)

Verification: OTA new firmware; confirm reboot to new version; bad firmware → 3-fail rollback; web assets zip → new files after reboot.

---

### Phase 11 — MQTT Client (T12)
**Goal:** Optional telemetry and remote command capability.

Files: `mqtt_client.h/.cpp`

Implementation (or stub if deprioritised):
- PubSubClient or esp-mqtt; credentials from NVS `mqtt` namespace
- Publish T, RH, wind_speed_avg, wind_dir_avg, window states, mode, alarm flags at configurable interval
- Subscribe to `{topic_prefix}/cmd/window` and `{topic_prefix}/cmd/mode`; parse JSON; post to Q1/Q4
- Active only when T10 has client WiFi connection
- Auth: username + password (plain TCP; accepted risk same as HTTPS decision)

Drivers used: None from driver library

Verification: Subscribe on broker; verify publish; publish command; verify relay action.

---

## Inter-Task Communication Reference

| Handle | Type | Depth | Flow |
|--------|------|-------|------|
| Q1 | Queue `window_cmd_t` | 8 | T3/T6/T8/T11/T12 → T2 |
| Q2 | Queue `key_event_t` | 16 | T7 → T8 |
| Q3 | Queue `log_event_t` | 32 | All tasks → T9 |
| Q4 | Queue `config_update_t` | 8 | T8/T10/T11 → T4 |
| Q5 | Queue `net_status_t` | 2 | T10 → T8 |
| Q6 | Queue `sensor_reading_t` | 1 | T5 → T4 (overwrite) |
| TN1 | TaskNotify | — | T4 → T3 (new wind data) |
| TN2 | TaskNotify | — | T4 → T6 (new sensor data) |
| TN3 | TaskNotify | — | T2 → T6 (manual override) |
| TN4 | TaskNotify | — | T10 → T4 (WiFi connected) |
| EG1 | EventGroup | 5 bits | WIND_OVERRIDE, MANUAL_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W, OTA_IN_PROGRESS |
| MX1 | Mutex | — | I2C bus (T4 RTC, T8 LCD) |
| MX2 | Mutex | — | Current measurements |
| MX3 | Mutex | — | Ring buffers |
| MX4 | Mutex | — | Config settings |
| MX5 | Mutex | — | LittleFS |

---

## Integration Test Checklist (post all phases)

1. **Wind safety end-to-end:** Live wind > v_max → all windows close → wind drops → climate resumes
2. **Power-loss recovery:** Cut power mid-operation → restore → CLOSE_ALL boot sequence → NVS intact → log entry for restart
3. **RTC battery backup:** Disconnect mains 30s → restore → DS1307 time intact
4. **Manual override + calibration:** Toggle GPIO42 → MANUAL_OVERRIDE set → calibration CLOSE_ALL → resume AUTOMATIC after M3 travel time
5. **Endurance:** 24-hour run; serial log `ESP.getFreeHeap()` every 5 min (via T9 snapshot); no watchdog resets, no heap leak

---

## Critical Files Summary

| File | Role | Status |
|------|------|--------|
| `firmware/partitions.csv` | Dual OTA+LittleFS partition table | ✅ Done (Gap A) |
| `firmware/platformio.ini` | Partitions ref, NeoPixel lib_dep | ✅ Done (Gap A+B); driver include paths still needed |
| `firmware/config/pin_config.h` | PIN_RGB_LED 38, corrected I2C address | ✅ Done (Gap B) |
| `firmware/src/auth/pin_auth.h/.cpp` | Salted SHA-256 PIN hashing + lockout | ✅ Done (Gap C) |
| `firmware/src/data_manager/sunrise.h/.cpp` | NOAA sunrise/sunset algorithm | ✅ Done (Gap D) |
| `firmware/src/types/app_types.h` | All shared types; everything else depends on it | ⬜ Phase 0 |
| `firmware/src/main.cpp` | RTOS primitives, task spawn, extern handle definitions | ⬜ Phase 0 |