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

## Phase 0 → Phase 1 Handover State

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

**Next phase:** Phase 1 — Data Foundation (T4): NVS round-trips, RTC reading, ring buffers, Q6/Q4 consumers.
