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
| **E** | `ESPAsyncWebServer` / `AsyncTCP` not in `lib_deps` | ✅ Solved | `mathieucarbou/ESPAsyncWebServer @ ^3.3.6` and `mathieucarbou/AsyncTCP @ ^3.3.2` added to `platformio.ini`; IDF5/Arduino-3 compatible fork chosen. TSDS §5.8 updated with library identity and rationale. |
| **F** | Motor travel time vs. dwell time conflated in NVS schema | ✅ Solved | `firmware/src/types/app_types.h` defines `MOTOR_M1_TRAVEL_S_DEFAULT 21`, `MOTOR_M2_TRAVEL_S_DEFAULT 21`, `MOTOR_M3_TRAVEL_S_DEFAULT 171` (seconds) as factory defaults plus `MOTOR_TRAVEL_S_MIN 5` / `MOTOR_TRAVEL_S_MAX 600` bounds, and `MOTOR_TRAVEL_MARGIN_S_DEFAULT 5` (fixed margin added to every relay pulse). NVS `motor` namespace adds `travel_m1/m2/m3` (int16_t, seconds, range 5–600) loaded by T4 on boot; T2 reads from MX4 at runtime (FR-CF05 satisfied). The relay energisation time is `(travel_mN + MOTOR_TRAVEL_MARGIN_S_DEFAULT) × 1000 ms`; the margin ensures the end-switch fires before the relay drops. De-energising the relay before the end-switch fires stops the window immediately at its current (intermediate) position — therefore only full open/close commands are ever issued. NVS `dwell_*` = minimum hold times only. TSDS §5.2, §4.3 T2/T4, §5.4, §5.10 updated; tasks.md updated. |
| **G** | Graduated ventilation channel assignment undefined | ✅ Solved | `firmware/src/climate_control/climate_control.h/.cpp` created. `NUM_VENT_STEPS 3` added to `app_types.h`. Compile-time `VENT_STEP_TABLE[]`: step 1 = M1, step 2 = M1+M2, step 3 = M1+M2+M3. `vent_step_required_t()` and `vent_step_required_rh()` implement the graduated step algorithm with close-hysteresis guard. RH < RH_min → step 0 (full close; no graduated closing — Gap G design decision). RH in range → `VENT_STEP_NEUTRAL` (−1). `vent_resolve_conflict()` handles neutral / both-open / no-conflict / genuine-conflict cases. TSDS §5.2 updated with full algorithm; tasks.md T6 updated. |
| **H** | Q3 drop-oldest not achievable with plain FreeRTOS queue | ✅ Solved | `firmware/src/event_logger/event_logger.h/.cpp` created. `log_post()` implements two-step evict-and-retry: `xQueueSend` → on fail: `xQueueReceive` (evict oldest) + `g_q3_dropped++` → retry `xQueueSend` → on fail: `g_q3_dropped++`. Counter protected by `portMUX_TYPE` spinlock. `log_take_dropped_count()` atomically reads and resets counter for T9. T9 emits `LOG_SYSTEM` event when count > 0. All producers must use `log_post()`; direct `xQueueSend(Q3,...)` is prohibited. TSDS §5.3 and tasks.md T9 updated. |

### Open Issue Resolutions

| Issue | Resolution |
|-------|------------|
| **#1a GPIO42 motor alarm** | `attachInterrupt(PIN_OPTO_INPUT, isr_handler, CHANGE)` in T2; deferred-ISR pattern: ISR (`IRAM_ATTR`) sets volatile flag + timestamp on first edge; T2 loop confirms after 75 ms by reading current pin state. **Not suppressed during MOVING** — a motor hitting the emergency switch during a T2-commanded move is the primary alarm scenario. On alarm assert: de-energise all 6 relays via `gpio_write()`, set EG1.MOTOR_ALARM, post log to Q3. On alarm release: clear EG1.MOTOR_ALARM, post log to Q3, then observe 60 s guard before re-calibration (see #1b). |
| **#1b Post-alarm re-calibration** | On `MOTOR_ALARM` clear: (1) clear `EG1.MOTOR_ALARM` and log clearance immediately; (2) wait `ALARM_GUARD_MS = 60 000 ms` — motor may still be coasting when operator resets the contact; T2 is blocked, relays remain off; (3) re-check pin at guard expiry — if LOW (re-asserted), abort and return (main loop re-enters onset); (4) if HIGH, call `calib_close_all()` directly (per-channel deadlines); (5) resume AUTOMATIC. |
| **#1c Alarm contact jitter** | **⚠ OPEN ISSUE — needs discussion.** The 60 s guard detects only a re-assertion that is *still present* at guard expiry. If the RRK-3 contact bounces (clears → re-asserts → clears again within the 60 s window), the end-of-guard pin check sees HIGH and proceeds to re-calibrate even though a real alarm may have been present mid-guard. Possible mitigations: (a) latch the alarm until an explicit operator acknowledge command; (b) count ISR edges during guard and abort if any assert edge is seen; (c) extend the guard; (d) require the pin to remain HIGH for N seconds before clearing alarm. Decision deferred — needs discussion with project owner. |
| **#2 Ring buffer depth** | 360 entries per channel (T, RH, wind_speed, wind_dir) = 11.5 KB total; fits in internal RAM with headroom |
| **#3 NTP timezone** | TZ string stored in NVS `system/tz_str`; applied at boot via `setenv("TZ", tz_str, 1); tzset()`. **Auto-TZ from geolocation (v1.13.0):** `do_geo_sync()` in T10 calls ip-api.com after NTP success; looks up IANA timezone via `s_tz_table[]` (~100-entry static map in `network_manager.cpp`); writes resolved POSIX string to NVS and applies immediately via `setenv/tzset`. Factory default `"CET-1CEST,M3.5.0,M10.5.0/3"`. Technician-configurable via web GUI. |
| **#4 MQTT auth** | Username + password over plain TCP (same accepted-risk basis as no-HTTPS); store password in NVS `mqtt/password`; document accepted risk |
| **#5 J5 heater supply** | ~~Dropped~~ — heater supply connection removed from PCB; no GPIO, no T5 logging required |
| **#6 Snapshot interval** | T4 posts a `LOG_SENSOR` event to Q3 on every Q6 reception (FR-LG09: snapshot interval = poll interval; 30–3600 s, default 60 s); no separate configurable parameter. Ring buffer minimum 250 entries (FR-LG06: worst-case 216 events/h at 30 s poll + headroom); `CONFIG_NVS_LOG_CAPACITY = 250`. |

---

## Implementation Phases

### Phase 0 — Project Scaffold & Watchdog/Heartbeat (T1)  ✅ done
**Goal:** Builds, boots, watchdog kicking, HB LED blinking, all tasks present as stubs.

Files to create/modify:
- `firmware/partitions.csv` — ✅ **done** (Gap A)
- `firmware/platformio.ini` — ✅ **done** (Gap A: `board_build.partitions`; Gap B: `Adafruit NeoPixel` lib); still needs driver include paths added when src/ layout is finalised
- `firmware/config/pin_config.h` — ✅ **done** (Gap B: `PIN_RGB_LED 38`, I2C address corrected)
- `firmware/src/types/app_types.h` — ✅ **created** (Gap F: motor travel constants + Phase 0 stubs); Phase 0 completes Sections 2–5 (handles, structs, enums, EG1 bits)
- `firmware/src/main.cpp` — **create**: `setup()`: driver inits, create all RTOS primitives, spawn all tasks; `loop()`: empty
- All task `.h/.cpp` files — **create**: stub implementations (`vTaskDelay(portMAX_DELAY)`)
- **T1 fully implemented:** 500ms watchdog kick, HB LED toggle (GPIO41), WS2812 status LED (GPIO38 via `PIN_RGB_LED`) driven from EG1 state using `Adafruit NeoPixel`

Drivers used: LIB-1 (GPIO), LIB-2 (I2C), LIB-3 (RTC probe), LIB-7 (NVS init)

Verification: Board boots, serial shows banner, HB LED blinks 1 Hz, no crash in 5 min.

---

### Phase 1 — Data Foundation (T4)  ✅ **done**
**Goal:** Central data store operational; NVS round-trips verified; RTC reading.

Files: `data_manager.h/.cpp` — ✅ **done**; `sunrise.h/.cpp` — ✅ **done** (Gap D)

Implementation:
- Load all NVS namespaces at boot using `nvs_cfg_get_i32_or_default()` / `nvs_cfg_get_str_or_default()`
- Read DS1307 via `rtc_get_time()` under MX1; warn if `rtc_oscillator_stopped()`
- Compute `is_daytime` from sunrise/sunset algorithm (lat/lon from NVS `system`)
- Task loop: consume Q6 (sensor readings → update MX2 measurements + MX3 ring buffers → send TN1 to T3 + TN2 to T6); consume Q4 (config updates → validate → NVS write → update MX4); consume TN4 (WiFi connected → trigger NTP → `rtc_set_time()`)
- Expose thread-safe getter functions for T3/T6 callers

Drivers used: LIB-7 (nvs_cfg_*), LIB-3 (rtc_*), LIB-2 (via RTC under MX1)

Verification: Inject Q6/Q4 items from `setup()` test harness; verify state via serial; verify NVS persistence across soft reset.

### Implementation notes (Phase 1)
- `cfg_shadow_t` defined in `data_manager.h` — full NVS shadow with derived fields (`is_daytime`, `current_unix_ts`, `sunrise_mins_utc`, `sunset_mins_utc`).
- `dm_ring_buf_t` — 360-entry `sensor_reading_t` ring (≈ 7.2 KB BSS); `dm_ring_read()` exposes batched read access.
- `rtc_dt_to_unix()` — manual UTC conversion (no `timegm()` dependency); handles leap years 1970–2099.
- `update_sun_times()` — called in-place whenever `current_unix_ts`, `lat_deg/frac`, or `lon_deg/frac` change.
- `setenv("TZ", tz_str, 1); tzset()` applied at boot and would need re-application if tz_str changes at runtime (web server writes NVS directly; T4 picks up on next boot or can add TZ re-apply in Q4 handler for key `tz_str` in a future phase).
- `DM_NOTIFY_NTP_SYNCED` (bit 3) reserved in T4's task notification register; T10 sends this after `configTime()` sync.

---

### Phase 2 — Relay Controller (T2)  ✅ done
**Goal:** Safe, timing-correct motor control; mutual exclusion enforced; GPIO42 feedback working.

Files: `relay_controller.h/.cpp`

Implementation:
- Init: 6 relay GPIOs as OUTPUT, all de-energised; GPIO42 as INPUT_PULLUP
- Boot sequence: issue CLOSE_ALL on all channels (establishes known CLOSED state; waits full travel time per channel)
- Window FSM per channel: `UNKNOWN → CLOSED → MOVING_OPEN → OPEN → MOVING_CLOSE → CLOSED`
- Before asserting any relay: de-energise complementary relay + 2 s gap
- Motor travel timer: read `travel_mN` (seconds) from T4 (MX4) at startup; `vTaskDelay(pdMS_TO_TICKS((travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000))` while relay energised; the margin guarantees the end-switch fires before the relay drops; de-energise on expiry and advance FSM. The relay must remain energised for the full duration — de-energising it early stops the window at an intermediate (unknown) position.
- Dwell timer: read `dwell_open_mN` / `dwell_close_mN` (minutes) from T4 (MX4); reject commands arriving before dwell elapsed
- Q1 consumer: check EG1.MOTOR_ALARM before executing any command — discard if alarm is active; T3 CLOSE_ALL commands preempt pending T6 commands (check source field)
- GPIO42 ISR (MOTOR_ALARM): volatile flag + timestamp; T2 main loop confirms after 75 ms debounce; NOT suppressed during MOVING. On alarm assert: de-energise all 6 relays immediately, set EG1.MOTOR_ALARM, post LOG event to Q3 (FR-MA01–FR-MA02). On alarm release: clear EG1.MOTOR_ALARM, post LOG event to Q3, wait 60 s guard (`ALARM_GUARD_MS`), re-check pin, then call `calib_close_all()` (FR-MA06–FR-MA07). See open issue #1c for jitter limitations.

Drivers used: LIB-1 (gpio_write for 6 relays + gpio_read for GPIO42)

Verification: Logic analyser — never simultaneous OPEN+CLOSE on same channel; confirm 2 s gap; confirm travel timing; confirm GPIO42 debounce with bench toggle.

---

### Phase 3 — Sensor Polling (T5) ✅ done
**Goal:** Live T/RH and wind data flowing into T4.

Files: `sensor_poll.h/.cpp`

Implementation:
- Poll loop: wait `poll_interval_ms` (read from T4 MX4)
- Read FG6485A via `fg6485a_read_measurements(FG6485A_ADDR, &meas)` — retry once on failure; on second failure set `EG1_BIT_SENSOR_FAULT_T`, post fault log to Q3
- Read S200 via `s200_read_measurements(S200_ADDR, &wind)` — same retry/fault logic for `EG1_BIT_SENSOR_FAULT_W`
- Compute sliding average: circular sum buffer (arithmetic) for T/RH/wind-speed; unit-vector (sin/cos) buffer for wind direction; size = `avg_window_min × 60 / poll_interval`, clamped 1–360
- Post `sensor_reading_t` to Q6 via `xQueueOverwrite()` (depth-1 queue, latest only)
- **Do not use** `fg6485a_task()` or `s200_task()` from LIB-10/11 — T5 is the sole Modbus master

Drivers used: LIB-6 (modbus_rtu), LIB-10 (fg6485a_read_measurements), LIB-11 (s200_read_measurements)

**Implementation note (updated Phase 4):** `#include <Arduino.h>` must be the first include in any task `.cpp` that uses `ESP_LOGI`. Without it, `ESP_LOGI` routes through raw IDF `esp_log_write`, silently filtered by `CONFIG_LOG_DEFAULT_LEVEL = 1` (ERROR). `<Arduino.h>` brings in `esp32-hal-log.h` which redefines `ESP_LOGI` → `log_printf` (Arduino handler, honouring `CORE_DEBUG_LEVEL=3`). `#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE` only fixes the compile-time gate — it does not bypass the runtime filter. See `firmwareImplementationResults.md` Phase 4 Issue 1.

Verification: Serial log shows readings at configured interval; sensors not yet connected → fault flags set correctly. Full clearance test deferred to hardware bring-up (sensor wiring).

---

### Phase 4 — Safety Monitor (T3) ✅ done
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

### Phase 5 — Event Logger (T9)  ✅ done
**Goal:** All events persistently recorded before automation goes live.

Files: `event_logger.h` — ✅ **done**; `event_logger.cpp` — ✅ **done**.

Implementation:
- `log_post()` and `log_take_dropped_count()` are fully implemented (Gap H ✅); all other tasks must use `log_post()` — never `xQueueSend(Q3, ...)` directly
- T9 task loop: `xQueueReceive(Q3, &evt, portMAX_DELAY)` → `nvs_log_append(&evt)`; if SD available → `storage_sd_write_append(current_file, csv_line)`
- After each drain pass: call `log_take_dropped_count()`; if > 0 → construct `LOG_SYSTEM` event with `value_a = (int16_t)count`; post via `xQueueSend(Q3, &sys_evt, 0)` directly (not `log_post()` — avoids re-entrant eviction)
- SD rotation: track file size via `storage_sd_file_size()`; rotate at 512 KB; delete oldest when count > 10 (`storage_sd_list_csv()` + `storage_sd_delete()`)
- No SD card: graceful fallback to NVS ring buffer; log SYSTEM event on mount failure
- Periodic snapshot: T4 posts a `LOG_SENSOR` event to Q3 every time it receives new data from Q6 (no separate timer in T9 required)

Drivers used: LIB-7 (nvs_log_append), LIB-8 (storage_sd_*)

Verification: CSV file created on SD; 5 manual events appear; rotate test at 512 KB; NVS fallback without SD; fill Q3 to trigger overflow → drop counter > 0 → `LOG_SYSTEM` event in log.

---

### Phase 6 — Climate Control (T6)  ✅ done
**Goal:** Autonomous temperature/humidity-driven ventilation with conflict resolution.

Files: `climate_control.h` — ✅ **done** (Gap G + Phase 6); `climate_control.cpp` — ✅ **done** (Gap G + Phase 6).

Implementation:
- Block on TN2 (new sensor data from T4)
- On TN2: check EG1 flags (MOTOR_ALARM, WIND_OVERRIDE, SENSOR_FAULT_T) — skip if any set
- Acquire MX2: read T_avg, RH_avg; acquire MX4: read `is_daytime`, active setpoints, `hyst_t`, `hyst_rh`, `rh_ctrl_en`, `cr_priority`
- Call `vent_step_required_t(t_avg, t_max, hyst_t, current_step_t)` → `step_t`
- Call `vent_step_required_rh(rh_avg, rh_max, rh_min, hyst_rh, rh_ctrl_en, current_step_rh)` → `step_rh`
- Call `vent_resolve_conflict(step_t, step_rh, cr_priority)` → `resolved_step`
- Call `vent_step_channels(resolved_step)` → `new_mask`; diff against `vent_step_channels(current_step)` → `cur_mask`
- Post incremental commands to Q1: channels in `new_mask & ~cur_mask` → CMD_OPEN; channels in `cur_mask & ~new_mask` → CMD_CLOSE; `new_mask == 0` → CMD_CLOSE_ALL (single command)
- Update `current_step_t` and `current_step_rh`; post log event to Q3 on any step change

Graduated ventilation functions are fully implemented in `climate_control.cpp` (Gap G ✅). No stub required for the step table.

Drivers used: None directly

Verification: Inject T > T_max_day via Q6; confirm graduated OPEN commands on Q1 (step 1 then 2 then 3 as T rises); inject conflicting T+RH demands; confirm conflict resolution per `cr_priority`; verify close-hysteresis guard (no step-0 until T < T_max − hyst_t).

---

### Phase 7 — UI Layer (T7 + T8)  ✅ done
**Goal:** Local keypad and LCD interface for commissioning and daily operation.

Files: `keypad_scan.h/.cpp` — ✅ **done**; `ui_display.h/.cpp` — ✅ **done**; `auth/pin_auth.h/.cpp` — ✅ **done** (Gap C)

**T7:**
- `keypad_scan()` every 20ms; if char returned ≠ `KP_NO_KEY`, post `key_event_t` to Q2 (non-blocking)
- Key-repeat: same key held > 500ms → repeat events at 100ms interval

**T8:**
- `lcd_init()` under MX1 at boot
- **Status pages (`STATUS_PAGES = 5`):** auto-rotate every 5 s through pages 0–4: (0) T/RH, (1) wind, (2) window states, (3) network/AP, (4) current local time + source label
- Menu FSM states: `UI_STATUS → UI_MENU_ROOT → UI_MENU_[CLIMATE|WIND|SYSTEM|ACCESS] → UI_EDIT_VALUE / UI_PIN_ENTRY`; plus new states **`UI_SET_DATE`** and **`UI_SET_TIME`** (v1.13.0)
- **Time page (page 4, v1.13.0, FR-UI22):** row 0 = local date/time via `localtime_r`; row 1 = `"Src:NTP  #=SetTm"` or `"Src:RTC  #=SetTm"` depending on `s_net.ntp_synced`
- **Manual time set (v1.13.0, FR-UI23):** `#` on page 4 → check admin session; if not: set `s_pending_settime = true`, enter `UI_PIN_ENTRY`; on PIN pass: `handle_pin()` detects `s_pending_settime`, clears it, calls `enter_set_date()`
  - `UI_SET_DATE`: row 0 = current date reference; row 1 = `"DD/MM/YY #OK *Bk"` with `_` for untyped. Accepts 6 digits (DDMMYY); `#` validates + saves to `s_dt_saved_*`; advances to `UI_SET_TIME`. `*` → `UI_STATUS`
  - `UI_SET_TIME`: row 0 = `"Now: HH:MM"`; row 1 = `"HH:MM    #OK *Bk"`. Accepts 4 digits (HHMM); `*` → back to `UI_SET_DATE`; `#` → `mktime(tm_isdst=-1)` → `dm_set_manual_time(unix_ts)` → `UI_STATUS`
  - `s_dt_len`, `s_dt_buf[9]`, `s_dt_saved_year/mon/mday` are module-static state variables in `ui_display.cpp`
- Max 4 keypresses from status screen to any first-level setting (FR-UI07)
- PIN entry: numeric; call `pin_auth_verify(PIN_ROLE_FARMER/ADMIN, entered_str)` — hashing, salt, and lockout all handled by `auth/pin_auth.h` (Gap C ✅)
- Config edit: read current from T4 (MX4), accept input, validate range, post `config_update_t` to Q4
- Receive Q5 (network status including `ntp_synced`); display WiFi state; use `ntp_synced` for time page source label
- Session timeout: software timer reset on each keypress; on expiry, log session close, return to `UI_STATUS`
- **LCD I2C address: 0x3E (AiP31068L bridge)** — not 0x27

~~Stubs acceptable: WiFi AP toggle screen~~ — **implemented in v1.10.1:** `handle_menu_system()` added; root menu updated to show all 4 items; AP toggle requires admin session; posts Q4 to T4.

Drivers used: LIB-4 (lcd_*), LIB-5 (keypad_scan), LIB-2 (via LCD under MX1)

Verification: Status screen rotates; navigate to T_max_day within 4 keypresses; change value; verify NVS write; PIN lockout after 5 wrong entries; session timeout.

---

### Phase 8 — Network Manager (T10)  ✅ done
**Goal:** WiFi AP/client, NTP sync, DS1307 update, status to T8.

Files: `network_manager.h/.cpp` — ✅ **done**

Implementation:
- WiFi AP: `start_ap()` / `stop_ap()` driven by NVS `wifi/ap_enable`; `poll_ap()` called every 5 s loop tick; SSID `"Greenhouse-XXYY"` (last 2 MAC bytes); auto-shutdown timer from `dm_cfg_snapshot().ap_timeout_min`; WPA2 password from NVS `wifi/ap_psk` (default `"0123456789"`, never open — v1.10.1); AP lifecycle managed independently from client — both can be active simultaneously
- WiFi client FSM: NET_IDLE → NET_CONNECTING → NET_CONNECTED → NET_RUNNING → NET_BACKOFF; exponential backoff 2→4→…→60 s; `WiFi.setAutoReconnect(false)` (T10 manages reconnection itself)
- NTP: `configTime(0, 0, "pool.ntp.org")` then polls `time(NULL) > 1700000000L` for up to 30 s; on success: sets `s_ntp_synced = true`, `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` → T4 calls `rtc_set_time()` under MX1; then calls `do_geo_sync()`
- **Geolocation (`do_geo_sync()`, v1.13.0, FR-DN06/DN07):**
  - `HTTPClient http; http.begin("http://ip-api.com/json?fields=status,lat,lon,timezone"); http.setTimeout(5000); int code = http.GET();`
  - Parse JSON body: extract `status` (must be "success"), `lat` (float), `lon` (float), `timezone` (IANA string)
  - `float_to_deg_frac(lat, &deg, &frac)` — handles negative values and carry; posts `lat_deg` + `lat_frac` to Q4 via `post_q4(NVS_NS_SYSTEM, "lat_deg", deg)` and `post_q4(NVS_NS_SYSTEM, "lat_frac", frac)`; same for lon
  - `iana_to_posix(iana_str)` — linear search of `s_tz_table[]` (~100 entries, `{NULL,NULL}` sentinel); returns POSIX string or NULL if not found
  - If found: `nvs_cfg_set_str(NVS_NS_SYSTEM, "tz_str", posix)` → NVS persisted; `setenv("TZ", posix, 1); tzset()` → applied immediately (affects all `localtime_r()` calls globally; no reboot needed)
  - On any failure (HTTP error, JSON parse error, unknown timezone): log and skip; last stored NVS values retained
  - No additional `lib_deps` required: `HTTPClient.h` is part of the Arduino-ESP32 core (resolved automatically by LDF)
- Q5: `net_status_t` struct extended with `bool ntp_synced` field; `xQueueOverwrite(Q5, &status)` on every state change; T8 reads on next tick and shows IP / "No WiFi" on LCD and uses `ntp_synced` for time page source label
- **Web server time display (v1.13.0):** `web_server.cpp` changed from `gmtime_r` + `"...Z"` suffix to `localtime_r` + no suffix — web dashboard now shows local time

Drivers used: None from driver library (Arduino WiFi + ESP-IDF SNTP + Arduino HTTPClient)

Verification (Phase 8 results): Clean build ✅; board boots without crash ✅; T1 heartbeat steady ✅; NET_IDLE (no SSID) produces no periodic output as expected ✅; Q5 initial post received by T8 ✅.
Additional verification (v1.13.0): NTP sync triggers `do_geo_sync()`; lat/lon visible in web GUI after sync; local time shown on LCD page 4; web dashboard time matches local clock.

---

### Phase 9 — Web Server (T11) ✅ COMPLETE (2026-05-05, v1.11.0)

**Goal:** Browser-accessible dashboard and configuration. ✅ Achieved.

Files: `web_server.h/.cpp`; `platformio.ini`; `firmware/data/index.html`; `firmware/data/style.css`; `firmware/data/app.js` — all written and flashed.

Implementation:
- ✅ Mount active LittleFS via `littlefs_active_partition()` + `littlefs_mount()`; index.html presence checked at boot
- ✅ ESPAsyncWebServer on port 80; AsyncWebSocket on `/ws`; 2 s push loop in T11
- ✅ REST API: GET `/api/status`, `/api/config`, `/api/history?n=N`; POST `/api/login`, `/api/logout`, `/api/config`, `/api/wifi`, `/api/pin`; GET `/api/whoami`
- ✅ All data endpoints require valid session cookie (random 16-byte hex token, server-side map, 4-slot, FreeRTOS mutex)
- ✅ Role-based access: farmer can edit climate setpoints + wind_prot_en; admin has full access
- ✅ Static files served from LittleFS (`index.html`, `style.css`, `app.js`) via `ps_malloc` + `littlefs_read`
- ✅ PIN change via `pin_auth_set()`; WiFi/AP PSK update direct NVS; config updates via Q4

**Deviation from plan:** `/api/command` replaced by WebSocket push for status; farmer/admin distinction via `is_farmer_key()` whitelist; OTA stub endpoints deferred to Phase 10.

Drivers used: LIB-9 (littlefs_storage), LIB-4 (nvs_config), LIB-12 (pin_auth)

Verification: Build clean (0 errors); firmware + LittleFS uploaded; device stable 5+ min; web UI hardware verification pending (browser test).

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
| Q5 | Queue `net_status_t` | 1 (overwrite) | T10 → T8; `net_status_t` fields: `client_connected`, `ap_active`, `ntp_synced` (bool, v1.13.0), `ip_str[16]` |
| Q6 | Queue `sensor_reading_t` | 1 | T5 → T4 (overwrite) |
| TN1 | TaskNotify | — | T4 → T3 (new wind data) |
| TN2 | TaskNotify | — | T4 → T6 (new sensor data) |
| TN4 | TaskNotify | — | T10 → T4 (WiFi connected) |
| EG1 | EventGroup | 6 bits | WIND_OVERRIDE, *(bit 1 reserved)*, SENSOR_FAULT_T, SENSOR_FAULT_W, OTA_IN_PROGRESS, MOTOR_ALARM |
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
4. **Motor alarm end-to-end:** Assert GPIO42 externally → verify EG1.MOTOR_ALARM set → verify all relays de-energised → release GPIO42 → verify EG1.MOTOR_ALARM cleared immediately → verify 60 s guard (relays remain off) → verify CLOSE_ALL re-calibration starts after guard → verify resume to AUTOMATIC → log entries present for alarm onset and clearance
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
| `firmware/src/types/app_types.h` | Motor travel constants (Gap F) + `NUM_VENT_STEPS 3` (Gap G) + Phase 0 stubs | ✅ Constants done; Phase 0 completes handles/structs/enums |
| `firmware/src/climate_control/climate_control.h` | Graduated ventilation API declarations | ✅ Done (Gap G) |
| `firmware/src/climate_control/climate_control.cpp` | VENT_STEP_TABLE, step algorithm, conflict resolution, T6 stub | ✅ Done (Gap G) |
| `firmware/src/event_logger/event_logger.h` | `log_post()` / `log_take_dropped_count()` API + full T9 Doxygen | ✅ Done (Gap H + Phase 5) |
| `firmware/src/event_logger/event_logger.cpp` | Full T9 implementation: drain loop, NVS ring buffer, SD CSV append, rotation, drop-counter surfacing | ✅ Done (Phase 5) |
| `firmware/src/keypad_scan/keypad_scan.h/.cpp` | T7: 20 ms scan, key-repeat, Q2 post | ✅ Done (Phase 7) |
| `firmware/src/ui_display/ui_display.h/.cpp` | T8: LCD FSM, status pages, menu nav, PIN auth, Q4 config post, session timeout | ✅ Done (Phase 7) |
| `firmware/src/network_manager/network_manager.h/.cpp` | T10: WiFi client FSM, AP management, NTP sync, TN4, Q5; `do_geo_sync()`, `s_tz_table[]`, `iana_to_posix()`, `float_to_deg_frac()`, `post_q4()` (v1.13.0) | ✅ Done (Phase 8 + v1.13.0) |
| `firmware/src/web_server/web_server.cpp` | T11: time display corrected to use `localtime_r` + no UTC suffix (v1.13.0) | ✅ Done (Phase 9 + v1.13.0) |
| `firmware/src/main.cpp` | RTOS primitives, task spawn, pin_auth_init(), extern handle definitions | ✅ Done (Phase 0 + Phase 7) |