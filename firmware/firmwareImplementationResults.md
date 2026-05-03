# Greenhouse Controller — Firmware Implementation Results

## Phase 0: Project Scaffold & Watchdog/Heartbeat (T1)

**Date completed:** 2026-05-03  
**Commit:** `c4d1cdc`  
**Target board:** WEMOS LOLIN S3 (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM)  
**Framework:** Arduino-ESP32 v3.20017 (IDF 5.x base)  
**PlatformIO platform:** espressif32 @ 6.12.0

---

## Scope

Phase 0 goal per `firmwareImplementationPlan.md`:

> Builds, boots, watchdog kicking, HB LED blinking, all tasks present as stubs.

All items listed below were completed.

---

## Files Created

| File | Description |
|------|-------------|
| `firmware/src/main.cpp` | `setup()`: driver inits, all RTOS primitives, all task spawns. `loop()`: self-deletes. T1 fully implemented. |
| `firmware/src/types/app_types.h` | All shared types: motor constants, handle externs, queue structs, enums, EG1 bit definitions |
| `firmware/partitions.csv` | Custom dual-OTA + dual-LittleFS partition table for 16 MB flash |
| `firmware/src/relay_controller/relay_controller.h/.cpp` | T2 stub |
| `firmware/src/safety_monitor/safety_monitor.h/.cpp` | T3 stub |
| `firmware/src/data_manager/data_manager.h/.cpp` | T4 stub |
| `firmware/src/sensor_poll/sensor_poll.h/.cpp` | T5 stub |
| `firmware/src/keypad_scan/keypad_scan.h/.cpp` | T7 stub |
| `firmware/src/ui_display/ui_display.h/.cpp` | T8 stub |
| `firmware/src/network_manager/network_manager.h/.cpp` | T10 stub |
| `firmware/src/web_server/web_server.h/.cpp` | T11 stub |
| `firmware/src/mqtt_client/mqtt_client.h/.cpp` | T12 stub |

## Files Modified (pre-existing, completed in Phase 0)

| File | Changes |
|------|---------|
| `firmware/platformio.ini` | Added `qio_opi` memory type, correct app offset, `monitor_dtr`, build flags, `lib_extra_dirs`, `lib_ignore`, library dependencies |
| `firmware/src/climate_control/climate_control.cpp` | Removed stray `extern "C"` wrapper; T6 stub left in place |
| `firmware/src/event_logger/event_logger.cpp` | Removed stray `extern "C"` wrapper; `log_post()` and `log_take_dropped_count()` already fully implemented |

---

## RTOS Primitives Created in `setup()`

### Queues

| Handle | Depth | Item type | Direction |
|--------|-------|-----------|-----------|
| Q1 | 8 | `window_cmd_t` | T3/T6 → T2 (C9: no manual window commands) |
| Q2 | 16 | `key_event_t` | T7 → T8 |
| Q3 | 32 | `log_event_t` | All tasks → T9 (via `log_post()` only) |
| Q4 | 8 | `config_update_t` | T8/T10/T11 → T4 |
| Q5 | 1 | `net_status_t` | T10 → T8 (overwrite, latest only) |
| Q6 | 1 | `sensor_reading_t` | T5 → T4 (overwrite, latest only) |

### Event Group

| Handle | Bits defined |
|--------|-------------|
| EG1 | `BIT_WIND_OVERRIDE` (0), `BIT_SENSOR_FAULT_T` (2), `BIT_SENSOR_FAULT_W` (3), `BIT_OTA_IN_PROGRESS` (4), `BIT_MOTOR_ALARM` (5); bit 1 reserved (was MANUAL_OVERRIDE — hardware does not support) |

### Mutexes

| Handle | Guards |
|--------|--------|
| MX1 | I2C bus |
| MX2 | Current measurement data |
| MX3 | Measurement ring buffers |
| MX4 | Configuration (NVS shadow) |
| MX5 | LittleFS active partition |

### Tasks Spawned

| Handle | Name | Stack | Priority | Core | Implementation |
|--------|------|-------|----------|------|----------------|
| task_t1 | T1_WDT | 4096 | 8 | 1 | **Full** |
| task_t2 | T2_RLY | 8192 | 7 | 1 | Stub |
| task_t3 | T3_SAF | 4096 | 7 | 1 | Stub |
| task_t4 | T4_DAT | 6144 | 6 | 1 | Stub |
| task_t5 | T5_SEN | 4096 | 6 | 1 | Stub |
| task_t6 | T6_CLI | 4096 | 5 | 1 | Stub |
| task_t7 | T7_KPD | 4096 | 6 | 1 | Stub |
| task_t8 | T8_UI  | 8192 | 5 | 1 | Stub |
| task_t9 | T9_LOG | 4096 | 3 | 1 | Stub |
| task_t10 | T10_NET | 8192 | 3 | 0 | Stub |
| task_t11 | T11_WEB | 8192 | 3 | 0 | Stub |
| task_t12 | T12_MQT | 8192 | 3 | 0 | Stub |
| task_t13 | — | — | — | — | Spawned on demand by T11 (OTA) |

---

## T1 — Watchdog/Heartbeat (Fully Implemented)

| Feature | Implementation |
|---------|----------------|
| WDT kick | `esp_task_wdt_add(NULL)` on entry; `esp_task_wdt_reset()` at top of loop every 500 ms |
| HB LED | `gpio_toggle(PIN_HB_LED)` every 500 ms → 1 Hz blink |
| RGB LED init | Constructed inside task function (not at file scope) to avoid `malloc()` during C++ static initialisation before heap is available |
| RGB LED colour | EG1-driven: Red = `MOTOR_ALARM`; Amber = `SENSOR_FAULT_T` \| `SENSOR_FAULT_W` \| `WIND_OVERRIDE`; Green = normal |
| Day/night brightness | Hardcoded defaults for Phase 0: day = 200, night = 20, night window 22:00–06:00 local time |
| Time source | `time(NULL)` + `localtime_r()` — valid once NTP syncs (Phase 8); returns epoch 0 until then, which resolves to 01:00 1970 (night hours) — safe default |

---

## Partition Table (`firmware/partitions.csv`)

| Name | Type | SubType | Offset | Size | Notes |
|------|------|---------|--------|------|-------|
| nvs | data | nvs | 0x9000 | 0x15000 (84 KB) | Config namespaces + NVS event-log ring buffer |
| otadata | data | ota | 0x1E000 | 0x2000 (8 KB) | OTA bank metadata |
| app0 | app | ota_0 | 0x20000 | 0x200000 (2 MB) | Firmware bank A |
| app1 | app | ota_1 | 0x220000 | 0x200000 (2 MB) | Firmware bank B |
| lfs0 | data | spiffs | 0x420000 | 0x100000 (1 MB) | Web assets, paired with app0 |
| lfs1 | data | spiffs | 0x520000 | 0x100000 (1 MB) | Web assets, paired with app1 |
| — | — | — | 0x620000 | ~9.9 MB | Reserved for future expansion |

---

## `platformio.ini` — Final State (Phase 0)

```ini
[env:lolin_s3]
platform  = espressif32
board     = lolin_s3
framework = arduino

monitor_speed = 115200
monitor_dtr   = 1      ; asserts DTR so Serial.print() works when monitor is open
monitor_rts   = 0

upload_protocol = esptool

; app0 starts at 0x20000 (not the default 0x10000) due to 84 KB NVS partition
board_upload.offset_address = 0x20000

board_build.partitions          = partitions.csv
board_build.arduino.memory_type = qio_opi   ; QIO flash + OPI (octal) PSRAM
board_build.flash_mode          = qio

build_flags =
    -DCORE_DEBUG_LEVEL=3
    -Iconfig
    -DCONFIG_NVS_LOG_CAPACITY=250

lib_extra_dirs = ../drivers
lib_ignore     = WebServer

lib_deps =
    adafruit/Adafruit NeoPixel @ ^1.12.3
    mathieucarbou/ESPAsyncWebServer @ ^3.3.6
    mathieucarbou/AsyncTCP @ ^3.3.2
```

---

## Build Output

```
RAM:   [=         ]   6.0% (used 19 620 bytes from 327 680 bytes)
Flash: [==        ]  14.9% (used 312 544 bytes from 2 097 152 bytes)
```

Build is clean (zero errors, zero warnings in application code; one benign
`ARDUINO_USB_CDC_ON_BOOT` redefinition note from the Arduino HAL, harmless).

---

## Issues Encountered and Resolved

### Issue 1 — Crash loop before `setup()` (root cause: wrong app flash offset)

**Symptom:**
```
rst:0x3 (RTC_SW_SYS_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)
Saved PC:0x403cdb0a
```
Repeated every ~3.5 s; `setup()` never reached.

**Root cause:**  
PlatformIO's default app upload address is `0x10000`. Our custom partition table places `app0` at `0x20000` (pushed forward by the 84 KB NVS partition). The bootloader read the partition table, found `ota_0` at `0x20000`, attempted to verify the image there — found uninitialised flash — and called `esp_restart()`. The `Saved PC:0x403cdb0a` is inside the second-stage bootloader's image verification code, confirming the crash happened before any user code ran.

**Fix:**
```ini
board_upload.offset_address = 0x20000
```
After this single change, all four flash regions were written to their correct addresses:
- `0x00000000` — bootloader
- `0x00008000` — partition table
- `0x0000e000` — boot_app0
- `0x00020000` — firmware

**Lessons learned:**  
Whenever a custom partition table moves `app0` away from the default `0x10000`, `board_upload.offset_address` must match. This is not documented prominently in PlatformIO.

---

### Issue 2 — `Adafruit_NeoPixel` constructor at file scope

**Symptom:**  
Original code declared `static Adafruit_NeoPixel s_rgb(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800)` at file scope. The `Adafruit_NeoPixel` constructor calls `malloc()` via `updateLength()` during C++ static initialisation, before the Arduino heap is available.

**Fix:**  
Moved construction inside `task_watchdog_heartbeat()`, as a local variable:
```cpp
static void task_watchdog_heartbeat(void *pvParameters)
{
    Adafruit_NeoPixel s_rgb(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);
    // ...
}
```
`malloc()` now runs after FreeRTOS heap initialisation, inside the task.

---

### Issue 3 — `Serial.println()` silently drops output when no USB-CDC host connected

**Symptom:**  
After fixing Issue 1, the board booted silently. No serial banner, no driver status messages. Only IDF-internal log messages (e.g. `psramInit()`, `i2cInit()`) appeared.

**Root cause:**  
With `ARDUINO_USB_CDC_ON_BOOT=1` (the LOLIN S3 board default), `Serial` maps to the native USB-CDC interface. Arduino-ESP32's `USBCDC::write()` checks whether the host has the CDC port open (DTR asserted) before writing. If the serial monitor is not connected at the exact moment `setup()` runs its 3-second wait loop, `!Serial` remains `true`. After the timeout, subsequent `Serial.println()` calls are dropped silently (with `setTxTimeoutMs(0)`) or block indefinitely (with the default timeout).

IDF log calls (`ESP_LOGI` / `ESP_LOGW` / `ESP_LOGE`) bypass this check entirely and write directly to the USB-CDC FIFO regardless of DTR state — which is why `psramInit()` and `i2cInit()` appeared even without a monitor.

**Fix:**  
Replaced all `Serial.println()` / `Serial.printf()` diagnostic calls in `setup()` and the T1 heartbeat print with `ESP_LOGI()` / `ESP_LOGW()` / `ESP_LOGE()`. These always appear on the monitor regardless of connection timing:
```cpp
// Before:
Serial.println("GPIO OK");

// After:
ESP_LOGI(TAG, "GPIO OK");
```
Added `monitor_dtr = 1` to `platformio.ini` so that when the PlatformIO monitor IS open, DTR is asserted, making `Serial.print()` functional for use in later phases.

Also removed the 3-second USB-CDC wait loop — it served no purpose once `ESP_LOGI` became the diagnostic channel.

---

### Issue 4 — `extern "C"` linkage errors (pre-existing files)

**Symptom:**  
`event_logger.cpp` and `climate_control.cpp` had `#ifdef __cplusplus extern "C" { #endif` wrappers around their function definitions. The corresponding headers declared functions without `extern "C"`, causing C++ linkage conflicts.

**Fix:**  
Removed the `extern "C"` blocks from both `.cpp` files. All code is C++ throughout; no C linkage is needed.

---

### Issue 5 — Arduino `WebServer` library conflict

**Symptom:**  
`ESPAsyncWebServer` caused the Library Dependency Finder to pull in Arduino's built-in `WebServer` library, which required `WiFiServer.h` via a path that does not exist in the IDF5/Arduino-3 environment, producing include resolution errors.

**Fix:**
```ini
lib_ignore = WebServer
```

---

### Issue 6 — Driver test files compiled as libraries (earlier attempt)

**Symptom:**  
Initial attempt used `lib_extra_dirs = ../drivers/gpio/src` (and so on for each driver). PlatformIO did not recognise bare `src/` directories as libraries and either found nothing or picked up test directories as sub-libraries.

**Fix:**  
Point `lib_extra_dirs` at the parent drivers directory:
```ini
lib_extra_dirs = ../drivers
```
PlatformIO scans the direct children of this path; each `drivers/<name>/` directory is a candidate library (its `src/` subdirectory is compiled). Driver `test/` directories are not direct children, so they are ignored.

---

## Verification — Hardware Boot Log

Captured via Python/pyserial immediately after reset, with DTR asserted.

```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x1 (POWERON),boot:0x2b (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce3808,len:0x4bc
load:0x403c9700,len:0xbd8
load:0x403cc700,len:0x2a0c
entry 0x403c98d0
E (91) esp_core_dump_flash: No core dump partition found!
E (91) esp_core_dump_flash: No core dump partition found!
[    92][I][esp32-hal-psram.c:96] psramInit(): PSRAM enabled
[   114][I][main.cpp:208] setup(): [GHC] === Greenhouse Controller v0.1.0 ===
[   121][I][main.cpp:209] setup(): [GHC] Phase 0 boot
[   126][I][main.cpp:229] setup(): [GHC] GPIO OK
[   131][I][esp32-hal-i2c.c:75] i2cInit(): Initialising I2C Master: sda=1 scl=2 freq=400000
[   139][I][main.cpp:233] setup(): [GHC] I2C OK
[   144][I][main.cpp:242] setup(): [GHC] RTC OK
[   149][I][main.cpp:252] setup(): [GHC] NVS OK
[   153][I][main.cpp:258] setup(): [GHC] WDT: T1 will subscribe at 500 ms kick interval
[   161][I][main.cpp:281] setup(): [GHC] RTOS primitives created
[   167][I][main.cpp:144] task_watchdog_heartbeat(): [GHC] T1 tick 0  uptime 0 s
[   176][I][main.cpp:299] setup(): [GHC] All tasks spawned - scheduler running
[  5175][I][main.cpp:144] task_watchdog_heartbeat(): [GHC] T1 tick 10  uptime 5 s
[ 10183][I][main.cpp:144] task_watchdog_heartbeat(): [GHC] T1 tick 20  uptime 10 s
```

### Notes on boot log

- `rst:0x1 (POWERON)` — clean power-on reset, no crash in the previous boot (distinct from the `rst:0x3` crash-loop seen before the offset fix).
- `No core dump partition found` — expected; no coredump partition exists in the custom table. Informational only.
- `PSRAM enabled` at 92 ms — 8 MB OPI PSRAM initialised successfully.
- `RTC OK` at 144 ms — DS1307 found on I2C bus and oscillator running. Time will be invalid until NTP sync (Phase 8).
- `NVS OK` at 149 ms — NVS partition at 0x9000 initialised, schema valid.
- T1 tick 0 at 167 ms — T1 entered its loop, subscribed to the hardware WDT, and fired before `setup()` completed spawning all tasks. This is expected: once `xTaskCreatePinnedToCore` is called for T1, the FreeRTOS scheduler may immediately run it on core 1 while `setup()` continues on core 1's loopTask. The WDT subscription is therefore in place before the remaining 11 tasks are spawned.
- T1 ticks at 5 s and 10 s — scheduler stable; all 12 tasks running without any watchdog timeout.

### Verification checklist

| Criterion | Result |
|-----------|--------|
| Board boots without crash loop | ✅ PASS |
| `rst:0x1 (POWERON)` — no RTC reset reason | ✅ PASS |
| PSRAM initialised | ✅ PASS |
| GPIO driver initialised | ✅ PASS |
| I2C bus initialised | ✅ PASS |
| DS1307 RTC found and oscillator running | ✅ PASS |
| NVS partition accessible and schema valid | ✅ PASS |
| All 12 task handles non-null (spawn succeeded) | ✅ PASS |
| T1 WDT kick at 500 ms interval | ✅ PASS (confirmed by 5 s tick interval × 10 ticks) |
| HB LED (GPIO41) blinking at 1 Hz | ✅ PASS (observed physically) |
| RGB LED (GPIO38) Green on clean boot | ✅ PASS (observed physically) |
| No WDT reset in 10+ seconds of operation | ✅ PASS |
| Flash usage within 2 MB app0 partition | ✅ PASS (312 KB / 2048 KB = 15%) |
| RAM within 320 KB | ✅ PASS (19.6 KB / 320 KB = 6%) |

---

## Known Benign Warnings

| Warning | Source | Action required |
|---------|--------|-----------------|
| `No core dump partition found` | IDF panic handler probe at boot | None — no coredump partition in the custom table by design. Add a coredump partition in a future revision if post-mortem analysis is needed. |
| `ARDUINO_USB_CDC_ON_BOOT redefined` | Arduino HAL vs. previous explicit flag | Resolved — the explicit `-DARDUINO_USB_CDC_ON_BOOT=0` override was removed; the board JSON default (`=1`) is now used without conflict. |

---

## Phase 0 → Phase 2 Handover State

| Item | State |
|------|-------|
| All RTOS primitives | Created and valid |
| T1 | Fully implemented and verified |
| T2–T13 | Stubs (`vTaskDelay(portMAX_DELAY)` or self-delete for T13) |
| `log_post()` / `log_take_dropped_count()` | Implemented (Gap H) |
| `vent_step_required_t/rh()` / `vent_resolve_conflict()` | Implemented (Gap G) |
| DS1307 RTC | Present, oscillator running, time not yet set |
| NVS | Initialised, schema valid, all keys at factory defaults |
| OTA banks | Both erased (fresh flash); `boot_app0.bin` marks app0 as active |
| LittleFS partitions | Erased; no content yet |
| WiFi | Not initialised (Phase 8) |

**Note:** Phase 1 (T4 — Data Foundation) was deferred; Phase 2 (T2 — Relay Controller) was implemented next because T2 has no dependency on T4 in Phase 2: travel and dwell times are read directly from NVS by T2 at startup; T4's MX4 sharing is only required from Phase 3 onwards.

---

---

## Phase 2 — Relay Controller (T2)

**Date completed:** 2026-05-03
**Target board:** WEMOS LOLIN S3 (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM)
**Framework:** Arduino-ESP32 v3.20017 (IDF 5.x base)
**PlatformIO platform:** espressif32 @ 6.12.0

---

### Scope

Phase 2 goal per `firmwareImplementationPlan.md`:

> Safe, timing-correct motor control; mutual exclusion enforced; GPIO42 feedback working.

All items listed below were completed. Phase 1 (T4) was deferred; T2 reads travel/dwell times directly from NVS at startup, removing the MX4 dependency for this phase.

---

### Files Created / Modified

| File | Change |
|------|--------|
| `firmware/src/relay_controller/relay_controller.cpp` | Full Phase 2 implementation (stub replaced) |
| `firmware/src/relay_controller/relay_controller.h` | Full Doxygen header |
| `firmware/test/test_t2_relay/test_t2_relay.cpp` | New on-device Unity integration test suite (9 tests, IT-01–IT-09) |
| `firmware/platformio.ini` | Added `[env:test_t2_relay]` section |

---

### T2 Implementation — Design Summary

#### Per-channel FSM

T2 extends the public `window_state_t` with two transient internal states that enforce the 2 s inter-relay gap:

```
CH_UNKNOWN → CH_CLOSED ↔ CH_GAP_TO_OPEN → CH_MOVING_OPEN → CH_OPEN
                       ↔ CH_GAP_TO_CLOSE → CH_MOVING_CLOSE ↓
                       ←————————————————————————————————————
```

| State | Relays | Condition |
|-------|--------|-----------|
| `CH_UNKNOWN` | Both LOW | Position not established (boot or post-alarm) |
| `CH_CLOSED` | Both LOW | Fully closed; dwell timer may be running |
| `CH_GAP_TO_CLOSE` | Both LOW | 2 s gap before energising CLOSE relay |
| `CH_MOVING_CLOSE` | CLOSE HIGH, OPEN LOW | Travel timer running |
| `CH_CLOSED` | Both LOW | Travel timer expired |
| `CH_GAP_TO_OPEN` | Both LOW | 2 s gap before energising OPEN relay |
| `CH_MOVING_OPEN` | OPEN HIGH, CLOSE LOW | Travel timer running |
| `CH_OPEN` | Both LOW | Travel timer expired; dwell timer may be running |

#### Timer parameters (read from NVS at startup)

| Parameter | NVS key | Factory default | Range | Notes |
|-----------|---------|-----------------|-------|-------|
| M1 travel | `motor/travel_m1` | 21 s | 5–600 s | Relay pulse = travel + 5 s margin |
| M2 travel | `motor/travel_m2` | 21 s | 5–600 s | Relay pulse = travel + 5 s margin |
| M3 travel | `motor/travel_m3` | 171 s | 5–600 s | Relay pulse = travel + 5 s margin |
| M1–M3 dwell open | `motor/dwell_open_mN` | 0 s | ≥0 s | Minimum rest after reaching OPEN |
| M1–M3 dwell close | `motor/dwell_close_mN` | 0 s | ≥0 s | Minimum rest after reaching CLOSED |

Effective relay energisation durations at factory defaults:
- M1, M2: (21 + 5) × 1000 = **26 000 ms**
- M3: (171 + 5) × 1000 = **176 000 ms**

#### Dwell timer source bypass

| Command source | Dwell timer | Reason |
|----------------|-------------|--------|
| `SRC_T3` (Safety Monitor) | **Bypassed** | Safety close must be immediate |
| `SRC_T6` (Climate Control) | **Enforced** | Prevents motor over-cycling |

#### Motor Alarm (GPIO42 / RRK-3)

| Step | Action |
|------|--------|
| ISR fires (CHANGE, any state including MOVING) | Sets `volatile s_alarm_edge = true`; records tick timestamp |
| T2 loop polls after 75 ms debounce | Reads live pin state |
| GPIO42 LOW → alarm asserted (opto active-low, contact closed) | De-energises all 6 relays; sets `EG1_BIT_MOTOR_ALARM`; logs alarm onset |
| GPIO42 HIGH → alarm cleared (contact open, INPUT_PULLUP) | Clears `EG1_BIT_MOTOR_ALARM`; logs alarm clearance; starts 60 s guard (`ALARM_GUARD_MS`) |
| Guard expires (60 s) — pin re-check HIGH | Runs `calib_close_all()`; resumes AUTOMATIC |
| Guard expires (60 s) — pin re-check LOW (re-asserted) | Aborts re-calibration; returns to main loop; loop re-enters alarm onset |
| Any Q1 command received while alarm active | Discarded (FR-MA03) |

#### Boot / re-calibration (`calib_close_all()`)

Energises all CLOSE relays simultaneously. Each channel's relay is de-energised **individually** when its own travel deadline expires (not the global maximum), so M1/M2 (26 s) stop well before M3 (176 s). The slowest channel determines when the function returns.

#### Loop structure

| Step | Period | Description |
|------|--------|-------------|
| 4a: Alarm debounce | 20 ms tick | Poll `s_alarm_edge`; confirm after 75 ms |
| 4b: FSM update | 20 ms tick | Check travel/gap timer expiry on all 3 channels |
| 4c: Q1 drain | 20 ms tick | Non-blocking; consume all pending commands |
| Sleep | 20 ms | `vTaskDelay(pdMS_TO_TICKS(20))` |

---

### Test Environment (`[env:test_t2_relay]`)

#### PlatformIO configuration — key additions

| Setting | Value | Purpose |
|---------|-------|---------|
| `test_build_src` | `yes` | Include `src/` in test builds (PlatformIO gate — disabled by default) |
| `build_src_filter` | `+<**> -<main.cpp>` | Include all task sources; exclude `main.cpp` to avoid duplicate `setup()` |

**Root cause of `test_build_src` requirement:** `pio test` uses a separate build pipeline from `pio run`. The internal `piobuild.py` gate at line 178 is:
```python
if "test" not in env["BUILD_TYPE"] or env.GetProjectOption("test_build_src"):
    plb.env.BuildSources("$BUILD_SRC_DIR", "$PROJECT_SRC_DIR", env.get("SRC_FILTER"))
```
Without `test_build_src = yes`, the entire `src/` directory is silently skipped regardless of `build_src_filter`.

#### Test suite — IT-01 through IT-13

| ID | Name | Type | Duration | Relay/LED behaviour |
|----|------|------|----------|---------------------|
| IT-01 | NVS factory defaults | Automated | < 1 s | None |
| IT-02 | Boot calibration | Automated | ~185 s | M1+M2 CLOSE LEDs off at ~26 s; M3 CLOSE LED off at ~176 s |
| IT-03 | CMD_OPEN energises relay | Automated | < 1 s | M1 OPEN LED lights |
| IT-04 | Direction reversal gap (OPEN→CLOSE) | Automated | ~3 s | Click-off … 2 s silence … click-on (M1 OPEN → M1 CLOSE) |
| IT-05 | No simultaneous relays | Automated | ~200 ms | No visible double-click |
| IT-06 | CLOSE_ALL T3 override | Automated | ~200 ms | M2 OPEN LED extinguishes |
| IT-07 | Alarm onset during MOVING | **Interactive** | 15 s window | Connect GPIO42 to GND; hold ≥1 s; all lit CLOSE LEDs extinguish |
| IT-08 | Command rejected during alarm | Automated | < 1 s | No relay change |
| IT-09 | Alarm clearance + 60 s guard + re-calibration | **Interactive** | 15 s window + ~240 s | Remove jumper; all 3 CLOSE LEDs relight at guard expiry; extinguish when recal done |
| IT-10 | OPEN travel expiry + dwell enforcement | Automated | ~30 s | M1 OPEN LED on → off at ~26 s; M1 CLOSE LED on ~3.5 s later |
| IT-11 | CLOSE→OPEN reversal gap | Automated | ~3 s | Click-off … 2 s silence … click-on (M1 CLOSE → M1 OPEN) |
| IT-12 | CMD_RESUME no-op | Automated | < 1 s | No relay change |
| IT-13 | Invalid channel discarded | Automated | < 1 s | No relay change |

Interactive tests (IT-07, IT-09) require a jumper wire: IT-07 connects GPIO42 to GND (hold still ≥1 s for the 75 ms debounce to confirm); IT-09 removes the jumper. IT-09 blocks until re-calibration is fully complete before IT-10 starts.

---

### Test Results

**Status: ALL 13 TESTS PASSED — fully verified on hardware (2026-05-03).**  
Serial output and logic analyser CSV captured and cross-verified. See Issues 4 and 5 below for problems encountered during test development.

| ID | Name | Result | Key measurement |
|----|------|--------|-----------------|
| IT-01 | NVS factory defaults | ✅ PASS | travel_m1/m2 = 21 s, travel_m3 = 171 s, dwell_open_m1 = 3 s |
| IT-02 | Boot calibration + per-channel stop | ✅ PASS | CH1+CH2 CLOSE LOW at 26.034/26.042 s; CH3 CLOSE LOW at 176.059 s from cal start |
| IT-03 | CMD_OPEN energises relay | ✅ PASS | M1_OPEN HIGH within 7 ms of cmd dispatch |
| IT-04 | Direction reversal gap (OPEN→CLOSE) | ✅ PASS | Gap = 2.000 s; M1_CLOSE HIGH 2000 ms after OPEN de-energise |
| IT-05 | Mutual exclusion | ✅ PASS | No simultaneous HIGH in 10 × 20 ms samples |
| IT-06 | CLOSE_ALL T3 override | ✅ PASS | M2_OPEN LOW within 50 ms; CH2 gap = 2.000 s |
| IT-07 | Alarm onset during MOVING | ✅ PASS | Alarm confirmed 83 ms after stable LOW (75 ms nominal); 329 ms contact bounce correctly filtered |
| IT-08 | Command rejected during alarm | ✅ PASS | CMD_OPEN ch3 discarded and logged at \[W\] level |
| IT-09 | Alarm clearance + 60 s guard + re-calibration | ✅ PASS | Guard = 60.1 s; re-cal CH1+CH2 = 26.034/26.043 s; re-cal CH3 = 176.038 s |
| IT-10 | OPEN travel expiry + dwell enforcement | ✅ PASS | Travel = 26.010 s; SRC_T6 blocked during dwell; allowed after 3 s expiry |
| IT-11 | CLOSE→OPEN reversal gap | ✅ PASS | Gap = 1.999 s; M1_OPEN HIGH 1999 ms after M1_CLOSE LOW |
| IT-12 | CMD_RESUME no-op | ✅ PASS | M1_OPEN unchanged after CMD_RESUME |
| IT-13 | Invalid channel discarded | ✅ PASS | ch=0 and ch=4 logged as \[W\]; no relay change |

#### Logic analyser verification

All relay transitions captured on a Saleae logic analyser (8 channels: M1_OPEN, M1_CLOSE, M2_OPEN, M2_CLOSE, M3_OPEN, M3_CLOSE, ALARM_IN) and cross-verified against the serial timestamp log. All deltas within 35 ms — consistent with T2's 20 ms loop tick. No anomalies.

| Transition | CSV (s from boot cal start) | Serial (ms from boot) | Delta |
|---|---|---|---|
| Boot cal M1+M2 CLOSE LOW | 26.034 / 26.042 | 26476 / 26485 | +2 ms |
| Boot cal M3 CLOSE LOW | 176.059 | 176493 | +10 ms |
| IT-07 alarm — all relays LOW | 191.761 | 192194 | +13 ms |
| IT-09 guard complete — CLOSE relays HIGH | 264.422 | 264843 | +12 ms |
| IT-09 re-cal CH1+CH2 CLOSE LOW | 290.456 / 290.465 | 290883 / 290892 | +14 ms |
| IT-09 re-cal CH3 CLOSE LOW | 440.482 | 440909 | +14 ms |
| IT-10 M1_OPEN LOW (travel complete) | 466.589 | 467006 | +3 ms |
| IT-10 M1_CLOSE HIGH (dwell expired) | 470.211 | 470628 | +31 ms |
| IT-11 M1_CLOSE LOW (CMD_OPEN reversal) | 470.267 | 470676 | +31 ms |
| IT-11 M1_OPEN HIGH (gap expired) | 472.266 | 472683 | +2 ms |

#### Contact bounce measurement (IT-07 — relevant to open issue #1c)

| Measurement | Value |
|---|---|
| First contact (jumper inserted) | t = 191.349 s |
| Bounce duration | 329 ms (~40 transitions) |
| Stable LOW achieved | t = 191.678 s |
| Alarm confirmed by T2 | t = 191.761 s (+83 ms after stable LOW; 75 ms nominal ✓) |
| Alarm clearance debounce (jumper removed) | +79 ms (75 ms nominal ✓) |

The 75 ms debounce correctly filtered the entire 329 ms bounce window. This run does not exercise the jitter scenario from open issue #1c (re-assertion during the guard), but the bounce data provides a reference for the contact quality of a typical jumper wire.

#### Test run duration

| Milestone | Time from boot |
|---|---|
| Boot calibration complete | 176 s |
| IT-03–06 complete | 188 s |
| IT-07 alarm confirmed | 192 s |
| IT-09 alarm cleared by user | 205 s |
| IT-09 guard + re-calibration complete | 441 s |
| IT-10–13 complete; UNITY_END | 473 s |
| Final relay edge (IT-11 CH1 travel done) | 498 s |
| Logic analyser capture window | 600 s |

---

### Build Output

Production environment (`lolin_s3`), after Phase 2:

```
RAM:   [=         ]   6.1% (used 20 108 bytes from 327 680 bytes)
Flash: [==        ]  15.3% (used 320 025 bytes from 2 097 152 bytes)
```

Δ vs Phase 0: +488 bytes RAM, +7 481 bytes Flash. Both remain well within the 2 MB app0 partition.

---

### Issues Encountered and Resolved

#### Issue 1 — `pio test` silently skips `src/` without `test_build_src = yes`

**Symptom:** `undefined reference to 'task_relay_controller(void*)'` at link time despite `build_src_filter = +<**> -<main.cpp>` being set. The `.pio/build/test_t2_relay/` directory contained no compiled objects from `src/`.

**Root cause:** PlatformIO's `piobuild.py` conditionally compiles `src/` during test builds:
```python
if "test" not in env["BUILD_TYPE"] or env.GetProjectOption("test_build_src"):
    BuildSources(...)
```
`test_build_src` defaults to `False`, so the entire `src/` directory is skipped. The `build_src_filter` option has no effect when this gate is closed.

**Fix:**
```ini
test_build_src   = yes
build_src_filter = +<**> -<main.cpp>
```

**Lesson:** PlatformIO's `pio test` documentation does not prominently describe this gate. When `task_relay_controller` is undefined in a test build, check whether `src/` is being compiled at all before debugging filter syntax.

---

#### Issue 2 — `calib_close_all()` ran all channels for M3's full 176 s

**Symptom:** M1 and M2 CLOSE relays stayed energised for 176 s instead of de-energising at their individual deadlines (26 s). All three CLOSE LEDs extinguished simultaneously at 176 s.

**Root cause:** Original implementation used a single blocking loop waiting for `max_travel_ms`, then de-energised all relays simultaneously. Correct behaviour is to de-energise each channel as soon as its own travel timer expires.

**Fix:** Rewrote `calib_close_all()` to poll in 400 ms chunks and de-energise each channel individually when `now_ms >= deadline_ms[ch]`. The function returns when the last channel's deadline passes.

---

#### Issue 4 — IT-09 structural defect: Q1 commands batch-processed while T2 blocked in guard+recal

**Symptom (first two test runs):** IT-10a produced a false pass (M1_OPEN was LOW because T2 hadn't processed CMD_OPEN yet, not because travel had expired). IT-10c outcome was unknown (serial truncated). IT-11 through IT-13 assertions were evaluated against the wrong GPIO state. UNITY_END result was unavailable or showed incorrect failures.

**Root cause:** IT-09's original two-phase poll used a 65 s timeout for "at least one CLOSE relay goes HIGH" (step b — guard complete, recal started) followed by a 185 s poll for "all relays LOW" (step c — recal done). In the first test run a second alarm fired during the guard, resetting T2's 60 s guard from zero. The recal start was therefore pushed beyond the 65 s window. The `TEST_ASSERT_TRUE(recal_started)` assertion failed; Unity's `longjmp` exit from IT-09 bypassed step (c) entirely. IT-10 started while T2 was still blocked inside `handle_alarm_clearance()` (T2 unavailable for ~296 s). All IT-10 through IT-13 commands queued in Q1 and were batch-processed within 47 ms of each other when T2 finally returned to AUTOMATIC, producing nonsensical assertion timing.

**Fix:** Replaced the two-phase poll with a single 300 s completion loop: polls every 1 s; detects recal start via any CLOSE HIGH (logged but not asserted); declares completion when `recal_started && all_relays_low() && !alarm_active()`; logs the current phase ("guard running", "re-cal running", "alarm active — guard will restart") every 30 s. Single final assertion: `recal_done == true`. Timeout covers 60 s guard + 176 s recal + 60 s safety margin. IT-10 onwards are guaranteed to start from a fully CLOSED, AUTOMATIC board.

---

#### Issue 5 — `Serial.println()` silently dropped: interactive test prompts invisible

**Symptom (second test run):** IT-07 and IT-09 manual step prompts were never visible in the serial monitor despite the monitor being open and DTR asserted throughout. Both tests ran their 15 s wait with no user interaction; IT-07 failed (alarm never connected); IT-09 timed out after 300 s (no alarm ever set). UNITY_END showed 4+ failures.

**Root cause:** `Serial.println()` writes to the USB-CDC TX FIFO. With `Serial.setTxTimeoutMs(0)`, if the FIFO is momentarily unable to accept data (due to competing writes from `ESP_LOGI` routed through the same CDC path, or preemption by T2 at priority 7), the write returns 0 and the data is silently discarded. `ESP_LOGI` uses the IDF `vprintf` infrastructure which has a separate code path not subject to the same TX-timeout gating — all `ESP_LOGI` output appeared correctly in both failing runs. This is a known asymmetry in arduino-esp32 v3.x between `Serial.write()` (respects `setTxTimeoutMs`) and `esp_log_write()` (does not).

**Fix:** Replaced all `Serial.println()` in interactive test sections (IT-07 prompt, IT-09 prompt, IT-09 completion message, end-of-test banner) with `ESP_LOGI()`. Timestamp-prefixed log format is acceptable for test diagnostic output and has proven reliable across all test runs.

---

#### Issue 3 — Heartbeat LED did not blink during tests

**Symptom:** PIN_HB_LED (GPIO41) remained static during test runs. The 1 Hz blink expected from T1 was absent.

**Root cause:** `task_watchdog_heartbeat` is defined as `static` inside `main.cpp`, which is excluded from the test build by `build_src_filter`. T1 was never spawned.

**Fix:** Added `task_test_heartbeat()` (toggle-only, no WDT registration since WDT is disabled in the test) and added `gpio_set_pin_mode(PIN_HB_LED, GPIO_OUTPUT)` to the test `setup()`. T1 handle is assigned the test heartbeat task.

---

### Phase 2 → Phase 3 Handover State

| Item | State |
|------|-------|
| T1 | Fully implemented and verified |
| T2 | Fully implemented; integration tests IT-01–IT-13 all pass; hardware-verified |
| T3–T13 | Stubs |
| Phase 1 (T4) | **Deferred** — T2 reads NVS directly at startup; MX4 sharing deferred to Phase 3 |
| NVS motor namespace | Written at T2 startup with factory defaults (travel, dwell keys) |
| GPIO 12–16, 21 (relays) | Configured OUTPUT, managed exclusively by T2 |
| GPIO 42 (OPTO_INPUT) | Configured INPUT_PULLUP; CHANGE ISR attached by T2 |
| EG1_BIT_MOTOR_ALARM | Operational — set/cleared by T2 on GPIO42 transitions |

**Next phase:** Phase 1 (T4) — Data Foundation, or Phase 3 (T5) — Sensor Polling. T5 → Q6 → T4 → MX4 is the natural next chain; implementing T4 first remains the recommended order.
