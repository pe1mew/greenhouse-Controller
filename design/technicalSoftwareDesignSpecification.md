# Technical Software Design Specification
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Technical Software Design Specification        |
| Project      | Greenhouse Ventilation Controller              |
| Status       | End-state architecture specification           |
| Related docs | `functionalRequirementsSpecification.md`       |
|              | `technicalHardwareDesignSpecification.md`      |
|              | `tasks.md`                                    |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture and Development Principles](#2-architecture-and-development-principles)
   - 2.1 Project Licences
   - 2.2 Firmware Toolchain
   - 2.3 Version Control
   - 2.4 Repository Structure
   - 2.5 Testability
   - 2.6 Security
3. [Design Constraints from FRS](#3-design-constraints-from-frs)
4. [Firmware Architecture](#4-firmware-architecture)
   - 4.1 Framework Selection
   - 4.2 FreeRTOS Task Overview
   - 4.3 Task Descriptions
   - 4.4 Core Assignment
   - 4.5 Inter-task Communication
   - 4.6 Synchronization Primitives
5. [Software Modules](#5-software-modules)
   - 5.1 Sensor Polling — Modbus RTU
   - 5.2 Climate Control Logic
   - 5.3 Event Log Manager
   - 5.4 Access Control and Session Management
   - 5.5 Local User Interface
   - 5.6 WiFi — Access Point Mode
   - 5.7 WiFi — Client Mode
   - 5.8 Web Interface
   - 5.9 OTA Firmware Update
   - 5.10 NVS Configuration Storage Layout
   - 5.11 Watchdog and Fault Handling
   - 5.12 System Status RGB LED
   - 5.13 Status Website POST (T14)
   - 5.14 Persistent Circuit Breaker (T14 internal — deferred)
   - 5.15 Bulkhead Policy and Status-POST Supervisor (T15 — dormant)

---

## 1. Introduction

### 1.1 Purpose
This document describes the software design of the greenhouse ventilation controller firmware. It defines the task architecture, inter-task communication, synchronization primitives, and the design of each software module. It translates the software-related requirements in the Functional Requirements Specification (FRS) and the hardware constraints documented in `technicalHardwareDesignSpecification.md` into concrete implementation decisions.

### 1.2 Scope
This document covers the full firmware design: the FreeRTOS task structure (§4), the software modules implementing each system function (§5), and the design constraints derived from the FRS (§3). Hardware design is documented separately in `technicalHardwareDesignSpecification.md`.

### 1.3 Definitions

| Term | Definition |
|------|------------|
| FreeRTOS | Real-time operating system kernel used on the ESP32-S3 |
| Task | FreeRTOS scheduling unit; analogous to a thread |
| Queue | FreeRTOS inter-task message buffer (thread-safe FIFO) |
| Mutex | FreeRTOS mutual exclusion semaphore with priority inheritance |
| Event group | FreeRTOS bit array for broadcasting boolean state flags |
| Task notification | Lightweight FreeRTOS point-to-point signal; faster than a binary semaphore |
| NVS | Non-Volatile Storage — ESP-IDF key-value store in flash |
| LittleFS | Lightweight filesystem stored in an ESP32 flash partition |
| OTA | Over-The-Air firmware update |
| FSM | Finite State Machine |
| MQTT | Message Queuing Telemetry Transport — lightweight IoT publish/subscribe protocol |
| NTP | Network Time Protocol |
| CRC | Cyclic Redundancy Check — error detection used in Modbus RTU |
| DE/RE | Driver Enable / Receiver Enable — RS485 direction control |
| RTC | Real-Time Clock |
| T | Air temperature (°C) |
| RH | Relative humidity (%) |

---

## 2. Architecture and Development Principles

### 2.1 Project Licences

The firmware source code is covered by a source-available, non-commercial licence. The hardware design and documentation licences are documented separately in `technicalHardwareDesignSpecification.md` §2.1.

| Aspect | Licence |
|--------|---------|
| **Software licence** | Source-available, non-commercial licence. Free to use and modify for personal and non-commercial purposes. Redistribution and commercial use are **not** permitted. |
| **Rationale** | The software licence allows inspection and personal adaptation without enabling commercial exploitation or unauthorised redistribution. |

### 2.2 Firmware Toolchain — PlatformIO + Visual Studio Code

The firmware is developed using **PlatformIO** as the build system and package manager, with **Visual Studio Code (VSCode)** as the editor. See `technicalHardwareDesignSpecification.md` §2.3 for full details.

### 2.3 Version Control

All firmware source code is managed in the project Git repository. See `technicalHardwareDesignSpecification.md` §2.2 for the branching strategy and tagging convention.

### 2.4 Repository Structure

```
greenhouse-controller/          ← Git repository root (GitHub / GitLab)
│
├── firmware/                   ← PlatformIO project (edit in VSCode)
│   ├── platformio.ini          ← Board, framework, library dependencies
│   ├── src/                    ← Application source code
│   └── test/                   ← Unit tests (PlatformIO test runner)
│
├── hardware/
│   ├── pcb/                    ← KiCad project files (.kicad_sch, .kicad_pcb, ...)
│   └── fabrication/            ← Gerbers, BOM, pick-and-place (generated per release)
│
├── design/                     ← Markdown design documents (FRS, THDS, TSDS)
│
├── documentation/              ← Component reference material
│   ├── Sensors/                ← Sensor datasheets and integration notes
│   ├── Motors/                 ← Motor and relay box documentation
│   └── VentilationSystem/      ← Ventilation system reference material
│
├── Archive/                    ← Historical design iterations (read-only reference)
│   └── Iteration1/             ← First design iteration: concept, simulation, environment data
│
├── README.md
├── LICENSE
├── license.md
├── changelog.md
├── contributing.md
└── code_of_conduct.md
```

### 2.5 Testability

The firmware ships with a host-side acceptance test harness in the `test/` directory. The harness uses `pytest` and exercises the controller's HTTP and WebSocket surface against a running unit, asserting on canonical-JSON shape, role-gated endpoint behaviour, the boot sequence, climate setpoint round-trip, wind override, sensor fault handling, configuration round-trip, and session lifecycle. Each test file is independent and may be run in isolation.

Pure-logic unit tests of the climate / wind / conflict-resolution algorithms are deferred. The acceptance test suite covers the operator-observable behaviour those algorithms produce.

### 2.6 Security

- WiFi connections are protected with WPA2 minimum; WPA3 preferred if supported by the ESP32-S3 SDK (TR-NW01).
- HTTPS on the local web interface is **not implemented**. The local web GUI runs over HTTP on port 80 and is protected by shared-secret authentication (session cookie issued by `/api/login`) and confinement to same-LAN traffic. TLS termination on the ESP32-S3 for the inbound web server is not feasible given the RAM and CPU headroom available alongside the climate-critical task graph; the threat model for TR-NW04 has been assessed and accepted on this basis.
- **HTTPS is required for the outbound status-website channel.** T14's status POST and SD log upload (§5.13) use `esp_http_client` over `esp_tls` against the remote status server. The configured status URL is validated at write time to begin with `https://`; non-HTTPS URLs are rejected. HTTPS enforcement is the responsibility of the remote server (the controller does not pin certificates); the shared-secret `sourceidentifier` header (carrying the per-unit `status_secret` from NVS) travels inside the TLS-protected request body and headers and is therefore not exposed on the wire.
- User credentials are stored as salted SHA-256 hashes (`SHA-256(salt || pin_ascii)`, mbedTLS); plain-text storage is not permitted (FR-AC06).

---

## 3. Design Constraints from FRS

The following items originate from system-level and functional requirements in the FRS. Each represents a constraint or implementation decision that the software design must satisfy. Source requirement IDs are noted for traceability.

**User interface**
- Menu depth: max 4 key presses from the main screen to any first-level setting (FR-UI07).

**Credential storage**
- User credentials stored as salted SHA-256 hashes (`SHA-256(salt || pin_ascii)`, mbedTLS); plain-text storage not permitted (FR-AC06).
- Configurable login lockout after a set number of failed attempts (FR-AC07).
- Hardware credential recovery via GPIO0 BOOT button resets PINs (and optionally all NVS settings) to factory defaults without authentication; requires physical access to the device (FR-AC08, FR-AC09).

**Event log**
- SD card is the sole persistent log store (FR-LG06, FR-LG07). The log is written as CSV with rotating files (default 30 × 1024 KB — FR-LG06; `SD_MAX_FILES` × `SD_ROTATE_BYTES` in `event_logger.h`), oldest file deleted automatically when the cap is exceeded. There is no NVS ring-buffer fallback (FR-LG08): on SD-card absence, removal, or write failure, T9 suspends logging cleanly and surfaces the condition via the local UI and the web GUI; climate-critical operation continues unaffected; on SD re-insertion, logging resumes in a fresh file timestamped with the resumption moment.

**Settings persistence**
- All configuration settings stored in ESP32-S3 NVS flash partition; retained across power cycles and restarts (FR-CF06, TR-SW01).

**Timekeeping and timezone**
- Time source: **DS1307 RTC** with CR2032 backup fitted on PCB. DS1307 is the authoritative clock when WiFi is unavailable (TR-HW08).
- When WiFi is available: synchronise system time via NTP on boot, then again on a 24-hour cadence; on initial NTP success, T10 calls `do_geo_sync()` to auto-detect timezone via ip-api.com (FR-DN07); POSIX TZ string applied immediately via `setenv/tzset`; persisted to NVS `system/tz_str`. See §4.3 T10.
- When WiFi is unavailable: DS1307 is authoritative; no timestamp gap on power interruption; TZ string from last successful geolocation (or factory default `CET-1CEST,M3.5.0,M10.5.0/3`) applied from NVS at boot.
- Administrator may manually set date/time via the LCD keyboard (FR-UI23) — see §5.5 and `dm_set_manual_time()` in §4.3 T4.

**Firmware update**
- Firmware updates supported without opening the enclosure: OTA over WiFi and via native USB (TR-SW02, TR-IF05).

**Fault recovery**
- Hardware watchdog timer automatically resets the MCU on a software hang; controlled restart sequence re-synchronises window states on recovery (TR-SW03).
- Hardware credential recovery via GPIO0 (LOLIN S3 BOOT button): a sustained press triggers a staged PIN and NVS reset without requiring any prior authentication; physical enclosure access is the only prerequisite. Three escalating levels are triggered by hold duration (5–10 s / 10–15 s / 15–20 s). See §4.3 T8 for the full implementation (FR-AC08, FR-AC09, FR-UI24).

**Testability**
- Host-side acceptance test suite under `test/` exercises the HTTP and WebSocket surface using `pytest` (TR-SW05). See §2.5.

**WiFi security**
- WPA2 minimum; WPA3 preferred (TR-NW01).
- Local web GUI is HTTP on the LAN, protected by shared-secret session cookie + same-LAN confinement (TR-NW04). Outbound status POST to the remote server is HTTPS (§5.13).

**Setpoint and threshold data types**
- All user-configurable setpoints and thresholds are stored and processed as **integers** (no fractional part): T_min, T_max (°C), RH_min, RH_max (%), v_max (m/s or Beaufort), wind direction exclusion centre and half-width (degrees), hysteresis bands, and dwell/timer durations (minutes). NVS keys for these parameters use `int16_t` (signed 16-bit integer). Sensor readings are carried at native precision (0.1 °C for temperature, sub-degree for wind direction via vector averaging); comparisons against integer setpoints use the rounded integer value. (FRS C11, FR-CF01–FR-CF11)

**Feature enable/disable flags**
- Temperature-based climate control is permanently active; no enable/disable flag is stored or checked.
- Humidity-based climate control has a farmer-configurable enable/disable flag (`rh_ctrl_en`, boolean, NVS namespace `climate`). When disabled, T6 skips RH evaluation entirely; conflict resolution (FR-CR01) is also suppressed. (FRS C12, FR-C12, FR-CF12)
- Wind protection has a farmer- and administrator-configurable enable/disable flag (`wind_prot_en`, boolean, NVS namespace `wind`). When disabled, T3 reads wind data but issues no CLOSE_ALL or RESUME commands; the WIND_OVERRIDE event group bit is never set. The LCD warning must remain visible while the flag is false (FR-WS10). (FRS C12, FR-WS09, FR-CF13)
- Both flags default to **enabled** (`true`) on first boot and after factory reset.
- Changes to either flag are logged with timestamp and the operator's identity (FR-WS11).

**Motor alarm detection**
- The RRK-3 alarm relay (dry contact, closes on alarm) is wired to J10; the input drives GPIO 42 configured as input with internal pull-up enabled (`GPIO_PULLUP_ENABLE`). The potential free output is **active-low**: contact closed (alarm active) → GPIO 42 **LOW**; contact open (no alarm) → GPIO 42 **HIGH**. The alarm fires when any motor fails to stop at its normal end-switch and reaches the emergency switch. T2 uses a deferred-ISR pattern via the ESP-IDF GPIO ISR service: `gpio_isr_handler_add(PIN_OPTO_INPUT, isr_handler, NULL)` after `gpio_install_isr_service(0)`. The IRAM-resident ISR records the first edge (volatile flag + FreeRTOS tick timestamp) and returns immediately; T2 confirms after 75 ms by reading the live pin state via `gpio_get_level()`. **Not suppressed during MOVING states** — a motor hitting the emergency switch during a T2-commanded move is the primary alarm scenario. On alarm assert confirmed: T2 immediately de-energises all 6 relays, sets EG1.MOTOR_ALARM, posts log event to Q3 (FR-MA01–FR-MA02). On alarm release confirmed: T2 clears EG1.MOTOR_ALARM, posts log event to Q3, waits a **60 s guard** (motor coast-down; relays remain de-energised), re-checks pin; if still clear, starts CLOSE_ALL re-calibration (EG1.CALIBRATING set for the duration), then resumes AUTOMATIC (FR-MA06–FR-MA07). T2 checks EG1.MOTOR_ALARM before executing any Q1 command and discards the command if the alarm is active.

**Mutual exclusion of relay commands**
- The firmware must never energise the OPEN and CLOSE relay of the same motor simultaneously. T2 (Relay Controller) is the sole owner of relay GPIO and enforces this constraint before asserting any relay (see §4.3).

---

## 4. Firmware Architecture

### 4.1 Framework Selection

The firmware is built directly against **ESP-IDF** via PlatformIO (`framework = espidf`, target board `lolin_s3`). FreeRTOS is provided by the IDF layer and is used directly for task management, queues, mutexes, event groups, and task notifications.

The build is component-based. Each hardware driver lives in `drivers/<name>/src/` and is exposed to the firmware build through a proxy `firmware/components/<name>/CMakeLists.txt` that forwards sources and include directories. Managed components (currently `espressif/led_strip`) are pulled in via `idf_component.yml` manifests; the on-disk cache lives in `firmware/managed_components/` and is gitignored.

Tier-1 hardening compiler flags (`-Wall`, `-Wextra`, `-Wformat=2`, `-Wshadow`, `-Wstack-usage=2200`, `-Wlogical-op`, `-Wstrict-overflow=2`, `-Wnull-dereference`) are applied component-scoped to the firmware's own source tree only. ESP-IDF framework components compile with their own flag baseline.

Filesystem support:
- **LittleFS** via the `joltwallet/littlefs` managed component, used for the web-asset bundle on each of two A/B partitions for paired-OTA support (§5.9).
- **FAT32 over SPI** via the IDF-bundled `fatfs` + `sdmmc` + `driver` components, mounted at `/sdcard` for the event log (§5.3) and the coredump-download workflow (§5.11).

Networking:
- WiFi via `esp_wifi` + `esp_netif` + `esp_event` (event-driven STA + APSTA).
- HTTP server via `esp_http_server` (§5.8).
- Outbound HTTPS via `esp_http_client` + `esp_tls` against the IDF-bundled mbedtls (§5.13).
- SNTP via `esp_netif_sntp` for initial sync and 24-hour cadence resync (§5.7).

Filesystem-managed dependencies and the toolchain pin are reproducible from source: `firmware/platformio.ini` carries the platform pin (`espressif32@<version>`) and every paired-OTA release archives the matching `firmware-<version>.elf` + `firmware-<version>.map` + `bootloader-<version>.bin` + `partitions-<version>.bin` alongside the `.bin` for forensic reproducibility (see `bin/build_release.ps1`).

### 4.2 FreeRTOS Task Overview

The firmware is structured as a set of FreeRTOS tasks. Each logical function is assigned a dedicated task with a defined priority and communication interface; core placement is delegated to the FreeRTOS SMP scheduler (see §4.4). `tasks.md` is the authoritative reference for the task architecture; this section summarises the design.

| ID  | Task Name              | Priority          | Affinity | Function |
|-----|------------------------|-------------------|----------|----------|
| T1  | Watchdog / Heartbeat   | Highest           | any  | Subscribes to and kicks the IDF Task Watchdog; drives the heartbeat LED + WS2812B RGB status LED; emits periodic heap-instrumentation rows to the event log (free internal, free PSRAM, largest contiguous internal block, heap-integrity check, task stack high-water-mark sweep). |
| T2  | Relay Controller       | High              | any  | Relay GPIO; per-channel window state machine; dwell timers; mutual exclusion; RRK-3 motor alarm detection. Persists terminal window state to NVS on every transition (§5.15). |
| T3  | Safety Monitor         | High              | any  | Wind safety evaluation; issues CLOSE_ALL; overrides climate control. |
| T4  | Data Manager           | Medium-high       | any  | Central store for all configuration settings and measurement data; ring buffers for sensor history; owner of the NVS-backed cfg shadow; coredump-presence cached at boot. |
| T5  | Sensor Poll            | Medium-high       | any  | Modbus RTU master; polls FG6485A T/RH + S200 wind; posts readings to T4. Sole owner of the Modbus bus. |
| T6  | Climate Control        | Medium            | any  | Evaluates setpoints; conflict resolution; posts actuation commands to T2. |
| T7  | Keypad Scan            | Medium-high       | any  | Matrix scan; debounce; posts key events to T8. |
| T8  | UI / Display           | Medium            | any  | LCD rendering; menu FSM; session management; posts config changes to T4. |
| T9  | Event Logger           | Low               | any  | Drains Q3; writes log entries as CSV rows to SD card; manages rotating-file retention. |
| T10 | Network Manager        | Low               | any  | WiFi STA + APSTA lifecycle; SNTP (boot + 24 h cadence); IP-based geo + TZ; posts net status to T8. Owns the boot-time `nm_wifi_init_blocking()` entry point called by `app_main` before this task is spawned. |
| T11 | Web Server             | Low               | any  | `esp_http_server` serving the local GUI from LittleFS + a REST API surface; spawns a dedicated WebSocket-push child task for the 2-second canonical-JSON broadcast on `/ws`. |
| T12 | MQTT Client            | Low               | any  | *(Could-have, reserved task slot.)* Architecture leaves room for an MQTT bridge that publishes sensor and status data to a configured broker and subscribes to a command topic set. Not currently active in the firmware build; the `task_t12` handle is declared and remains `NULL` until activation. |
| T13 | OTA                    | Low (on demand)   | any  | Firmware and LittleFS update; manages dual-bank A/B rollback; firmware-only fallback timer for unpaired uploads; spawns a dedicated reboot worker task to host `esp_restart()` outside the FreeRTOS timer-service context (§5.9). |
| T14 | Status website POST    | Low               | any  | Outbound HTTPS POST to the remote status server every `cfg.status_interval_s`; SD log upload at the configured daily slot and on T9 rotation (multi-file drain). Builds the canonical status JSON via the shared `build_canonical_status_json()` (§5.13). |
| T15 | Status-POST supervisor | (deferred)        | any  | *(Dormant — see §5.15.)* Bulkhead supervisor for T14: heartbeat + cumulative-heap-drop monitor with respawn budget escalating to planned reboot. Source preserved on disk; excluded from the build. May be withdrawn pending soak outcome. |

### 4.3 Task Descriptions

#### T1 — Watchdog / Heartbeat

**Priority:** Highest

T1 is a full FreeRTOS task (not a software-timer callback). It is the system's liveness anchor and its instrumentation source.

**Per-tick responsibilities (500 ms tick):**
- Subscribes once to the IDF Task Watchdog Timer (`esp_task_wdt_add(NULL)`) and kicks it (`esp_task_wdt_reset()`) on every tick.
- Toggles the green heartbeat LED at 1 Hz in steady state, 4 Hz during start-up initialisation, steady-on if the firmware has stopped before the watchdog fires, off otherwise.
- Drives the WS2812B RGB status LED via the `led_strip` managed component (RMT TX backend): reads EG1 to determine the current system state, maps it to the colour convention in §5.12, and writes the chosen colour at the day or night brightness level taken from T4 (cached locally; refreshed on cfg update).
- Detects the coredump-available condition cached by T4 at boot and reflects it in the RGB colour for one short post-boot window if other higher-priority flags are inactive.

**Periodic instrumentation (every 60 s):**
- Emits three heap-snapshot rows to Q3: total free internal heap, total free PSRAM, largest contiguous free internal block. The largest-block metric is the gh-class "heap fragmentation" signal — sampled at the same cadence as total-free so fragmentation (largest block falling while total-free remains stable) is visible from the SD log alone.
- 30 seconds offset from the heap-snapshot moment, performs a `heap_caps_check_integrity_all()` sweep and emits a corruption event if the integrity check fails.

**Periodic instrumentation (every 10 minutes):**
- Stack high-water-mark sweep across every known task handle; writes one row to Q3 per task that has crossed a low-stack threshold.

**OTA-healthy gating:**
- After `OTA_HEALTHY_MS` of uninterrupted ticks following a fresh boot, calls `ota_mark_healthy()` to clear the boot-failure counter that drives the 3-fail rollback (§5.9). One NVS write per boot via a local boolean.

**Synchronization:** reads EG1 (all bits — lock-free); reads cfg via `dm_cfg_snapshot()` (acquires MX4 internally) on cfg-change; posts to Q3 via `log_post()` for heap rows, integrity events, and stack-HWM warnings.

---

#### T2 — Relay Controller

**Priority:** High

- Sole owner of all 6 relay GPIO output pins (OPEN/CLOSE for M1, M2, M3); no other task may assert relay signals directly.
- All actuation requests arrive via command queue Q1 (from T3 and T6 only — manual window commands from LCD/web/MQTT are out of scope, C9).
- Runs the per-channel window state machine: `CLOSED` → `MOVING` → `OPEN` and reverse.
- Enforces OPEN + CLOSE mutual exclusion on each channel before asserting any relay.
- Reads motor travel times (`travel_mN`, seconds) and dwell times (`dwell_open_mN`, `dwell_close_mN`, minutes) from T4 (MX4) on startup and on each config update; converts travel time to ms for `vTaskDelay`.
- **Travel timer:** energises each relay for `(travel_mN + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000` ms; de-energises on expiry; window is at end position. The margin ensures the end-switch fires before the relay drops. **De-energising the relay before expiry stops the motor immediately at the current (intermediate) position — therefore only complete open or close commands are issued.**
- **Dwell timer:** after travel completes, enforces the minimum hold time before accepting the next command on that channel (FR-A09–FR-A12).
- Monitors the RRK-3 opto-isolated alarm input (GPIO42) via a deferred-ISR pattern: an `IRAM_ATTR` ISR registered with `gpio_isr_handler_add()` records the first edge (volatile flag + tick timestamp) and returns immediately; the T2 task loop confirms after 75 ms by reading the live pin state. **Not suppressed during MOVING — a motor reaching the emergency switch during a T2-commanded move is the primary alarm scenario.** On alarm assert confirmed: de-energise all 6 relays immediately, set EG1.MOTOR_ALARM, post log event to Q3 (FR-MA01–FR-MA02). On alarm release confirmed: clear EG1.MOTOR_ALARM, post CLOSE_ALL to Q1 for re-calibration, post log event to Q3, resume AUTOMATIC (FR-MA06–FR-MA07). Checks EG1.MOTOR_ALARM before executing any Q1 command; discards the command if alarm is active.
- **Synchronization:** receives Q1 (actuation commands); checks EG1.MOTOR_ALARM before executing commands; posts to Q3 (log events via `log_post()`); sets/clears EG1.MOTOR_ALARM on GPIO42 alarm assert/release.

---

#### T3 — Safety Monitor

**Priority:** High

- Wakes on notification from T4 whenever new wind data is available.
- Reads current wind speed and wind direction from T4.
- Checks the `wind_prot_en` flag (from T4) before evaluating thresholds. If wind protection is disabled, T3 takes no action and clears EG1.WIND_OVERRIDE if previously set.
- Compares against v_max threshold and wind direction exclusion zone (configuration from T4) — only when `wind_prot_en` is true.
- Posts a `CLOSE_ALL` actuation command to T2 immediately when a threshold is exceeded.
- Posts a `RESUME` notification to T6 when conditions return to safe limits.
- Always active (task remains scheduled), but evaluation and actuation are suppressed when wind protection is disabled.
- Independent of Automatic / Standby operating mode.
- Priority equal to T2; must preempt T6 to ensure a safety response is never delayed by climate logic.
- **Synchronization:** wakes on TN1 (from T4, new wind data); acquires MX2 to read wind speed and direction; posts to Q1 (CLOSE_ALL or RESUME actuation command); sets/clears EG1.WIND_OVERRIDE; posts to Q3 (log events).

---

#### T4 — Data Manager

**Priority:** Medium-high

T4 is the single source of truth for all runtime data and configuration. All tasks that need to read or write system state do so through T4. This eliminates distributed per-variable mutexes and provides a single serialisation point for NVS persistence.

**Configuration settings**
- Holds all configurable parameters in RAM: setpoints (T_min_day, T_max_day, T_min_night, T_max_night, RH_min_day, RH_max_day, RH_min_night, RH_max_night), wind thresholds, per-channel motor travel times (`travel_mN`, seconds — relay energisation duration) and dwell times (`dwell_open_mN` / `dwell_close_mN`, minutes — minimum hold after travel), hysteresis values, sliding average windows, geographic location (lat/lon), WiFi credentials, PIN hashes, display language, session timeout, AP idle timeout, status-website URL/secret/interval/expose-mask, daily log-upload HH:MM.
- Accepts write requests from T8 (UI) and T11 (web server); validates range before accepting.
- Persists changed settings to NVS flash immediately on write.
- Loads all settings from NVS on startup; applies defined defaults for any missing keys.

**Day/night period management**
- Computes sunrise and sunset times from geographic location (latitude, longitude) and the current UTC date using the **NOAA General Solar Position Equations** (simplified, ±2 min accuracy; sufficient for ≤60° latitude). Implemented in `firmware/src/data_manager/sunrise.h/.cpp` (FR-DN01, FR-DN02).
- Algorithm steps: Julian Day → Julian Century → geometric mean longitude / anomaly → equation of center → apparent longitude → declination → equation of time → hour angle at sunrise → UTC minutes from midnight. Accuracy for the Netherlands (≈52°N): within ±1–2 minutes year-round.
- Inputs: `lat_deg + lat_frac / 1000.0f` and `lon_deg + lon_frac / 1000.0f` from NVS `system` namespace; Unix timestamp from DS1307 RTC.
- FR-DN05: if latitude and longitude are both zero (no location configured), daytime setpoints are applied as the safe default.
- Determines current period by comparing the UTC time-of-day (derived from the Unix timestamp) against the computed sunrise/sunset windows.
- Exposes `is_daytime` (boolean) and `sunrise_mins_utc` / `sunset_mins_utc` (minutes from midnight UTC) to all tasks via shared state under MX4.
- Emits one `LOG_SUN` row via `log_post()` whenever the cached sunrise/sunset values change (change-detected at the top of `update_sun_times()`; sentinel `-1` cache means the first call after boot always emits). The row carries the **local-time** minutes-from-midnight (matching the values exposed by `/api/status`), so historical SD files are self-sufficient for per-day night-shading without needing a live `/api/status` lookup. In steady-state operation this produces one row per local day (the midnight rollover triggers the recompute and detects the ~1–2 min/day shift); see §5.3 for the row format.
- T6 reads `is_daytime` to select the correct setpoint pair (day or night) before each evaluation cycle.
- Web GUI displays `sunrise_mins_utc` and `sunset_mins_utc` converted to local time for farmer verification (FR-DN04).

**Current measurement data**
- Holds the most recent raw readings: temperature (T), relative humidity (RH), wind speed, wind direction.
- Also holds the sliding-average values for T and RH (T_avg, RH_avg), updated incrementally on each new poll result.
- Updated by T5 after each successful Modbus poll cycle.
- T6 uses the averaged values (T_avg, RH_avg) for setpoint comparison; raw values are logged and displayed.
- Read by T3, T6, T8, T11, T12; no task accesses sensor data except through T4. T9 does not read MX2 directly — current measurement values reach T9 as fields inside `LOG_SENSOR` events posted to Q3 by T4.

**Measurement history ring buffers**
- Maintains a separate ring buffer for each measured quantity: T, RH, wind speed, wind direction.
- Each entry contains: timestamp and measured value.
- Ring buffer depth: **360 entries per channel** (T, RH, wind speed, wind direction) = 11.5 KB total internal RAM. At the default 30 s poll interval this yields 3 h of in-memory history; at the minimum 15 s poll interval, 1.5 h. The depth is sized to fit comfortably in ESP32-S3 internal SRAM with substantial headroom while still serving the web trend view and any future MQTT history publish channel.
- Read by T8 (display history), T11 (web trend view), T12 (MQTT history). T9 no longer reads ring buffers for snapshots — T4 posts a `LOG_SENSOR` event to Q3 on each new poll result instead.

**Operating state**
- Holds current operating mode (Automatic / Standby / Wind-override / Manual-override).
- Holds current session state (Normal / Farmer / Admin).
- Updated by T8 and T11; read by any task that gates behaviour on mode or session.
- **Synchronization:** acquires MX1 (I2C) to read DS1307 RTC; holds MX2 while writing current measurement data; holds MX3 while writing ring buffer entries; holds MX4 while reading or writing configuration settings; receives Q4 (config/state updates from T8, T10, and T11); receives Q6 (sensor readings from T5); sends TN1 to T3 after writing new wind data; sends TN2 to T6 after writing new sensor data.

**`dm_set_manual_time(time_t unix_ts)` — public API (FR-UI23)**
- Called by T8 after the operator confirms a manual date/time entry via `UI_SET_DATE` → `UI_SET_TIME`.
- Step 1: Updates the POSIX system clock via `settimeofday(&tv, NULL)`.
- Step 2: Converts `unix_ts` → `rtc_datetime_t` using `gmtime_r()` (DS1307 stores UTC); writes to DS1307 via `rtc_set_time()` under MX1 (500 ms timeout).
- Step 3: Updates `s_cfg.current_unix_ts` under MX4 (200 ms timeout).
- T8 calls `mktime(tm_isdst=-1)` before calling this function to convert user-entered local time to a UTC epoch using the currently active TZ environment variable. This ensures DS1307 always stores UTC regardless of the configured timezone.

---

#### T5 — Sensor Poll

**Priority:** Medium-high

- Modbus RTU master on UART1 with SIT65HVD08P transceiver; manages DE/RE direction control pin.
- Polls SenseCAP S200 (wind speed + direction) and FG6485A (T + RH) on a configurable interval (factory default 30 s; technician-configurable 15–120 s via web GUI).
- On successful read: computes updated sliding-average values for T and RH (ring buffer of size = `avg_window_min × 60 / poll_interval` samples; default window 1 minute = effectively no averaging); writes raw and averaged values to T4; T4 then notifies T3 and T6.
- On fault (timeout, CRC error, out-of-range value): posts a sensor fault event to T9 (logger) and triggers alarm display via T8.
- **Synchronization:** posts to Q6 (sensor readings and updated sliding averages to T4); sets/clears EG1.SENSOR_FAULT_T and EG1.SENSOR_FAULT_W; posts to Q3 (log events); no mutexes held — T4 owns all measurement storage.

---

#### T6 — Climate Control

**Priority:** Medium

- Wakes on notification from T4 that new sensor data is available.
- Reads sliding-average T (T_avg) and RH (RH_avg) from T4; reads current day/night period (`is_daytime`) from T4; selects the applicable setpoint pair (T_min_day/T_max_day or T_min_night/T_max_night; RH_min_day/RH_max_day or RH_min_night/RH_max_night).
- Evaluates temperature and humidity against the active setpoints with hysteresis bands.
- Runs conflict resolution algorithm when T and RH demand opposing window actions.
- Posts open/close actuation commands to T2 via command queue.
- Checks operating mode from T4 before acting; **inhibited** by any of the EG1 bits in the "do nothing" mask. End-state mask (rc.1.5.1+): `MOTOR_ALARM | WIND_OVERRIDE | SENSOR_FAULT_T | STANDBY`. The first three are safety / data-validity gates; `STANDBY` is the operator-initiated pause (gh#28). The rc.1.5.0 transient `MANUAL_SESSION` bit (gh#29) was removed in rc.1.5.1 — the admin manual-motor LCD menu now auto-sets STANDBY on entry and clears it on exit (without recalibration, preserving the admin's manual positions per FR-MM07).
- **Synchronization:** wakes on TN2 (from T4, new sensor data); acquires MX2 to read current T and RH; acquires MX4 to read setpoints and hysteresis; reads EG1 (MOTOR_ALARM, WIND_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W, STANDBY) before issuing any command; posts to Q1 (actuation commands); posts to Q3 (log events).

---

#### T7 — Keypad Scan

**Priority:** Medium-high

- Scans the 4×4 keypad matrix every ~20 ms.
- Applies software debounce.
- Posts validated key-press events to T8 via queue.
- Can be implemented as a FreeRTOS software timer callback rather than a full task.
- **Synchronization:** posts to Q2 (key events to T8); no shared data; no mutexes required.

---

#### T8 — UI / Display

**Priority:** Medium

- Manages the LCD1602 display via I2C (shared bus with RTC). Any `delay()` calls in the LCD1602 driver must be replaced with `vTaskDelay(pdMS_TO_TICKS(ms))` so T8 yields to the scheduler rather than spinning.
- Renders the main status screen: T, RH, wind speed and direction, window states, operating mode, active session, active alarms.
- **Cyclic status pages (`STATUS_PAGES = 7`):** auto-rotates every 5 s through pages 0–6:
  - 0: temperature and humidity
  - 1: wind speed and direction
  - 2: operating mode + active session (`Mode: AUTO/WIND/ALARM/STANDBY/Window Cal.` + `Sess: NONE/Farmer/Admin` ± `OTA`)
  - 3: network status (AP / client / IP)
  - 4: current date/time (local, via `localtime_r`) and time source label ("NTP" when `s_net.ntp_synced` true; "RTC" otherwise); pressing `#` on this page initiates the manual time-set flow (FR-UI22, FR-UI23)
  - 5: motor (window) states — row 0: `M1    M2    M3  `; row 1: per-channel state abbreviation (`OPEN` / `CLOS` / `MOV>` / `MOV<` / `UNK `); reads `window_state_t[3]` from T2 via `t2_get_window_states()` (FR-UI04)
  - 6: firmware version (read-only diagnostic — `fw_ver` + `asset_version`)
- **D-key page advance:** pressing `D` while in the auto-rotation loop (`UI_STATUS`) immediately advances to the next page (`s_status_page = (s_status_page + 1) % STATUS_PAGES`) and resets the 5-second display timer (`s_status_ticks = 0`); the new page then holds for a full 5 s before the next automatic advance.
- Runs the menu FSM; navigation depth ≤ 4 key presses from the main screen to any first-level setting.
  - **FSM states:** `UI_STATUS`, `UI_MENU_ROOT`, `UI_MENU_CLIMATE`, `UI_MENU_WIND`, `UI_MENU_SYSTEM`, `UI_MENU_ACCESS`, `UI_EDIT_VALUE`, `UI_PIN_ENTRY`, **`UI_SET_DATE`**, **`UI_SET_TIME`**
  - `UI_SET_DATE`: 6-digit DDMMYY entry with inline `_` cursor placeholder; validates DD 01–31, MM 01–12; saves to `s_dt_saved_{year,mon,mday}` on `#`; advances to `UI_SET_TIME`. `*` returns to `UI_STATUS`.
  - `UI_SET_TIME`: 4-digit HHMM entry; validates HH 0–23, MM 0–59. `*` re-enters `UI_SET_DATE` restoring the previously typed digits. `#` builds a `struct tm` from the saved date + typed time, calls `mktime(tm_isdst=-1)` to produce a UTC epoch, then calls `dm_set_manual_time(unix_ts)`.
  - `s_pending_settime` flag: set when `#` is pressed on page 4 without an active admin session; clears and calls `enter_set_date()` after PIN is accepted in `handle_pin()`.
- Manages session state: PIN entry via keyboard, session timeout, PIN validation against T4.
- Posts validated configuration changes and mode changes to T4.
- Receives WiFi status updates from T10 via Q5; stores in `s_net` (`net_status_t`); uses `s_net.ntp_synced` for page-4 source label.
- **IO0 hardware recovery sequence (FR-AC08, FR-AC09, FR-UI24):** GPIO0 (LOLIN S3 BOOT button, active-low, configured as `INPUT_PULLUP`) is polled once per 100 ms main-loop tick. While held, `render_reset_bar()` writes a growing bar to the LCD and the loop immediately issues `continue`, suppressing all normal renders (Q5 dirty-flag updates, status-page rotation, key dispatch) until the button is released.
  - **Bar rendering:** row 0 = contextual stage label; row 1 = 16-cell bar where filled cells use HD44780 ROM A00 character `\xFF` (full block) and unfilled cells use CGRAM slot 1 (5×8 outline-square glyph `{0x1F,0x11,0x11,0x11,0x11,0x11,0x1F,0x00}`, loaded under MX1 after `lcd_init()` at T8 startup via `lcd_create_char()`). Slot 0 is avoided (C null terminator). `filled = (ticks × 16) / 200`.
  - **Stage table** (each stage = 50 ticks = 5 s at 100 ms/tick):

    | Ticks | Hold time | Row 0 label | Release action |
    |-------|-----------|-------------|----------------|
    | 0–49 | 0–5 s | *(blank)* | No action; normal display resumes. |
    | 50–99 | 5–10 s | `Reset PIN?` | `nvs_cfg_erase_namespace(NVS_NS_ACCESS)` + `pin_auth_init()` → PINs reset to defaults (`1234` / `12345678`); active session closed; system continues. Confirmation: `PIN Reset!` shown 5 s. |
    | 100–149 | 10–15 s | `Reset settings?` | All NVS namespaces erased (climate, wind, motor, access, wifi, mqtt, system) + `pin_auth_init()` + session close; system continues with factory defaults. Confirmation: `Settings Reset!` shown 5 s. |
    | 150–199 | 15–20 s | `Restarting?` | Same full NVS erase + `pin_auth_init()` + `ESP.restart()`. Confirmation: `Restart!` shown 3 s before reboot. Auto-executes at 200 ticks without requiring release. |

- **Synchronization:** acquires MX1 (I2C) to write LCD; acquires MX2 to read current measurements for display refresh; acquires MX3 to read ring buffers for history view; acquires MX4 to read configuration for settings screens; receives Q2 (key events from T7); receives Q5 (network status from T10, including `ntp_synced`); reads EG1 (alarm flags for display and alarm indication); posts to Q4 (config/mode updates to T4); posts to Q3 (log events: mode changes, setpoint changes, session events).

---

#### T9 — Event Logger

**Priority:** Low

- Receives log events from all tasks via a dedicated queue; senders post and return immediately.
- Serialises all writes to, when present, the SD card.
- The queue decouples log I/O from higher-priority tasks; no task is blocked by log write latency.
- Queue overflow policy: drop-oldest enforced by `log_post()` in `event_logger.h` (Gap H); see §5.3 for the two-step evict-and-retry mechanism.
- Periodic sensor-value snapshots: T4 posts a `LOG_SENSOR` event to Q3 every time it receives new sensor data from T5 via Q6. T9 consumes these like any other event — no separate timer or MX3 access required.
- **Synchronization:** receives Q3 (log events from all tasks); no mutexes held; no I2C or GPIO access.

---

#### T10 — Network Manager

**Priority:** Low

T10 owns the WiFi subsystem end-to-end via the native ESP-IDF stack (`esp_wifi` + `esp_netif` + `esp_event`). Two distinct entry points: a blocking boot-time bring-up called from `app_main` *before* the long-running task is spawned, and the long-running task itself.

**Boot-time entry: `nm_wifi_init_blocking(connect_timeout_ms)`**
- Reads SSID and PSK from NVS `wifi` namespace; if no SSID is configured the WiFi stack is still initialised to STA mode (the radio comes up, no STA association is attempted; the AP-mode recovery path remains available).
- Calls `esp_netif_init()`, `esp_event_loop_create_default()`, `esp_netif_create_default_wifi_sta()`, `esp_wifi_init()` with default config, registers a single unified handler for `WIFI_EVENT` and `IP_EVENT` against the default event loop, sets STA mode and config, calls `esp_wifi_start()`.
- Blocks on a FreeRTOS event group for `IP_EVENT_STA_GOT_IP` (success) or `WIFI_EVENT_STA_DISCONNECTED` after a fast-retry budget (failure) or the caller-supplied timeout.
- On `STA_GOT_IP`: runs initial SNTP synchronisation against `pool.ntp.org` for up to ~10 s, then returns.
- The unified event handler is registered once here and **stays alive after this function returns** — it continues to drive all subsequent disconnect events via the exponential-backoff reconnect timer described below.

**Long-running task: `task_network_manager`**

Started by `app_main` *after* `nm_wifi_init_blocking()` has returned. Runs a 5-second monitor loop:
- Each cycle, snapshots the current netif/wifi state into a `net_status_t` (`client_connected`, `ap_active`, `ntp_synced`, `ip_str[16]`) and overwrites Q5 (depth 1) so T8's LCD WiFi page always reads the freshest state with no blocking.
- Polls the NVS `wifi/ap_enable` flag and toggles AP mode on the rising/falling edge. AP and STA run **simultaneously** in `WIFI_MODE_APSTA` when AP is enabled; SSID is `Greenhouse-<XXYY>` (last two bytes of WiFi STA MAC); PSK is read from NVS `wifi/ap_psk` (falls back to a default if unset, WPA2-PSK with a minimum 8-character key). When AP is disabled the radio returns to STA-only.
- Sends `DM_NOTIFY_NTP_SYNCED` (TN4) to T4 once on the first successful NTP sync so T4 writes the post-SNTP system time back to the DS1307 RTC.
- Runs `do_geo_sync()` once per boot after the first NTP sync: HTTP GET against `http://ip-api.com/json?fields=status,lat,lon,timezone`, parses lat/lon and IANA timezone, posts lat/lon updates to Q4 → T4 → NVS + sunrise recompute, looks up the IANA name in the static table (`iana_to_posix()`), writes the resolved POSIX string to NVS `system/tz_str`, applies immediately via `setenv("TZ", posix_tz, 1); tzset()`. All failure modes silently skip; last stored values are retained.
- Runs periodic NTP resync on a **24-hour cadence** after the initial sync. The DS1307 RTC holds time precisely enough for multi-day operation, but slow drift accumulates; periodic resync brings the wall clock back to NTP accuracy. Audit-logged.

**Reconnect behaviour**

The unified WIFI_EVENT handler (registered by `nm_wifi_init_blocking`) drives reconnects via a FreeRTOS one-shot timer with exponential back-off (2 s → 4 s → 8 s → 16 s → 32 s → 60 s cap). On each successful `IP_EVENT_STA_GOT_IP` the back-off is reset. The handler is light: it signals the boot-time event group on first arrival and arms or re-arms the timer for subsequent disconnects. The timer callback fires `esp_wifi_connect()` directly.

**STA address mode**

DHCP only. Static-IP fields are not part of the current spec.

**Synchronization:** posts to Q5 (network status to T8); posts to Q4 (geolocation lat/lon updates to T4); sends TN4 to T4 on first NTP sync success; posts to Q3 (log events for STA up/down, NTP synced/timeout, AP up/down, geo success); no mutexes held in the steady-state monitor loop.

---

#### T11 — UI / Web Server

**Priority:** Low | **Stack:** 8 KB (main handler task) + 4 KB (WS-push child task)

T11 runs the local web GUI and its REST API via `esp_http_server` (HTTP on port 80; HTTPS is not used for the inbound local web — see §2.6). Static assets (HTML/CSS/JS/manifest) are served from the active LittleFS partition; the REST surface drives every operator-facing action.

**Architecture**
- A single `httpd_handle_t` server with all URI handlers registered at task start.
- A separate child task `task_ws_push` is spawned (4 KB stack, priority 4, `tskNO_AFFINITY`). The child holds the same `httpd_handle_t`, wakes every 2 000 ms, builds the canonical status JSON (§5.13) when at least one WebSocket client is connected, and broadcasts it to every `/ws` socket via `httpd_ws_send_frame_async()`. Splitting the broadcast into its own task isolates the periodic JSON-build cost from the HTTP accept/dispatch loop.
- The inbound `/ws` URI handler (registered as `is_websocket = true`) completes the HTTP-upgrade handshake on the first GET and drains client-sent frames silently on subsequent calls (the dashboard sends no client→server WS messages; draining is protocol housekeeping so the socket stays alive across pings).

**URI groups served by the REST surface**

| Group | Endpoints | Auth |
|---|---|---|
| Static | `/`, `/style.css`, `/app.js`, `/manifest.json` | public |
| Auth | `/api/whoami`, `/api/login`, `/api/logout` | login is public, others session-aware |
| Data | `/api/status`, `/api/history` | public |
| Config | `/api/config` (GET, POST), `/api/config/limits`, `/api/wifi`, `/api/pin`, `/api/web` | session-gated (farmer / admin per field) |
| SD | `/api/sd/status`, `/api/sd/mount`, `/api/sd/unmount` | admin |
| Log | `/api/log/files`, `/api/log/download` | session-gated |
| Coredump | `/api/coredump/status`, `/api/coredump/download`, `/api/coredump/erase` | admin (rate-limited; audit-logged) |
| OTA | `/api/ota/status`, `/api/ota/firmware`, `/api/ota/assets` | admin |
| WebSocket | `/ws` | public (payload identical to `/api/status`) |

**Session model**
- In-memory table of `MAX_SESSIONS` slots, each holding a random 16-hex-char token + role (`farmer` or `admin`) + expiry.
- Browsers store the token in a `Set-Cookie: session=TOKEN; Path=/; HttpOnly` cookie.
- Each authenticated request slides the expiry forward by `cfg.session_timeout_min × 60 s`.
- PIN authentication backed by §5.4 (salted SHA-256 + lockout). Local-keyboard PIN and web PIN use the same NVS-stored hashes; the role enforcement at endpoint level uses `admin_only_or_send_error()` and equivalent helpers.

**Cold-start performance**
- The dashboard's `app.js` issues a synchronous `fetch('/api/status')` at page load and at the end of a successful `doLogin()` and pipes the response through the same `handleStatus()` function the WebSocket dispatcher uses. This closes the 0–2 s gap between page render and the first WS push.

**Synchronization:** acquires MX5 (LittleFS active partition) to serialise file reads against T13's partition-switch on OTA commit; reads EG1.OTA_IN_PROGRESS for informational purposes (T13 writes only to the inactive partition so T11 is not blocked during OTA); acquires MX2 to read current measurements; acquires MX3 to read the history ring; acquires MX4 to read configuration; posts to Q4 (validated config writes to T4); posts to Q3 (log events for every audit-relevant action). The WS-push child is read-only against the same primitives.

---

#### T12 — MQTT Client

**Priority:** Low | **Status:** *Could-have, reserved task slot*

T12 is reserved as a future bridge to an external MQTT broker. The architecture leaves room for it: a `task_t12` FreeRTOS handle is declared in `app_types.h` and remains `NULL` until activation; `LOG_BY_MQTT` is allocated as an initiator code in the event-log enumeration; the NVS `mqtt` namespace exists as a placeholder for broker connection settings (broker URL, port, credentials, base topic, publish interval).

When activated, the task is intended to:
- Publish current T, RH, wind speed, wind direction, window states, operating mode, and alarm status to a configured MQTT broker at a configurable interval. Source-of-truth for the published values is T4 (read via `dm_meas_snapshot()` and `dm_status_snapshot()`).
- Subscribe to a configured command topic set; route received actuation commands to T2 via Q1 and configuration changes to T4 via Q4.
- Run only while the WiFi STA is connected and a broker is configured; degrade silently on broker unreachable.

**Synchronization (when activated):** acquires MX2 to read current measurements for publishing; acquires MX4 to read broker configuration; posts to Q1 for inbound actuation commands; posts to Q4 for inbound configuration changes; posts to Q3 for log events with `LOG_BY_MQTT` initiator.

---

#### T13 — OTA (on demand)

**Priority:** Low (spawned on demand)

T13 is spawned from T11 when a firmware or asset upload is received and tears itself down once the OTA cycle completes (or a fallback timer fires). It is not part of the steady-state task graph.

**Firmware upload path**
- Activated by `POST /api/ota/firmware` (admin only). The handler streams the raw image body chunk-by-chunk into T13's writer via `ota_firmware_write()`; the writer feeds `esp_ota_write()` against the inactive bank.
- On `ota_firmware_end()`: ESP-IDF computes and verifies the image SHA, marks the new bank as the next-boot partition pending commit.

**Asset upload path**
- Activated by `POST /api/ota/assets` (admin only) after the firmware step. The handler streams the STORE-only ZIP body chunk-by-chunk into a PSRAM accumulator (`ota_assets_accumulate(data, len, offset)`); on completion, T13 extracts the ZIP entries directly into the **inactive** LittleFS partition (the partition paired with the inactive firmware bank) using the IDF `joltwallet/littlefs` VFS bindings, writing `manifest.json` last so the partition is only made consistent at the very end. T11 continues to serve from the **active** partition throughout.

**Reboot path — `reboot_worker_task` carve-off**
- When both firmware and assets are committed, T13 arms a 1-second one-shot reboot timer. The timer callback **does not call `esp_restart()` directly**: `esp_restart()` performs a WiFi-teardown chain (`esp_wifi_stop` → 802.11 ioctls → `queue_send_wrapper`) that consumes several KB of stack — more than the FreeRTOS timer-service task's ~2 KB allotment. Instead, the timer callback spawns a dedicated `reboot_worker_task` with a 4 KB stack and priority 5; the worker logs the impending reboot, calls `esp_restart()`, and never returns. On `xTaskCreate()` failure the timer falls back to in-timer `esp_restart()` (best-effort).

**Firmware-only fallback timer**
- If a firmware upload completes verification but no paired asset upload arrives within `FW_DONE_FALLBACK_MS` (120 s), T13 commits the firmware alone (`esp_ota_set_boot_partition`) and reboots via the same `reboot_worker_task` path. The asset partition stays at the previous version; on the next boot the new firmware runs against the old asset bundle. This guards against half-updates without forcing the operator to re-flash on a flaky upload.

**Rollback**
- Implements consecutive-boot-failure rollback (3-fail budget): if the new firmware fails to reach `ota_mark_healthy()` (called by T1 after `OTA_HEALTHY_MS` of stable uptime) 3 times in a row, the previous bank is restored as active on the next boot. The paired LittleFS partition follows the firmware automatically.

**Synchronization**
- Acquires MX5 only at the moment of partition-pointer commit (atomic flip from inactive→active); T11's reads of the active partition are not blocked during the write phase because T13 writes the inactive partition only.
- Sets EG1.OTA_IN_PROGRESS on start; clears on commit or failure; posts to Q3 (log events for each OTA stage and outcome).

#### T14 — Status Website POST

**Priority:** Low (3) | **Stack:** 12 KB

T14 is the outbound telemetry path to the status website. It is the only task that performs HTTPS handshakes and the only task that streams SD-card log files off-device. It owns one `esp_http_client` handle (with `keep_alive_enable = true`) and one `esp_tls_t` session; the handle is created once at task entry and retained for the lifetime of the task.

**Status POST cycle**
- Every `cfg.status_interval_s` (default 600 s; range 60–86400 s; `0` disables outbound POST entirely), T14 builds a status JSON payload via the shared `build_canonical_status_json()` helper used by `/api/status` and the `/ws` push. The payload is identical across all three channels except for `STATUS_EXPOSE_ALL` versus the configured `cfg.status_expose_mask` (bitmask selecting which top-level objects to include: sensors, modes, alarms, setpoints, network, sys).
- The cycle is triggered by a one-shot FreeRTOS software timer that re-arms itself at the end of each cycle; `0` disables both the initial arm and the re-arm so the task idles indefinitely.
- Request shape: `POST <cfg.status_url>` with `Content-Type: application/json`, `User-Agent: greenhouse-controller/<FIRMWARE_VERSION>`, and the `sourceidentifier: <cfg.status_secret>` shared-secret header. The secret is never logged; it lives in NVS namespace `status` and is masked in `/api/config` GET responses.
- HTTPS is enforced by the remote endpoint; the controller does not enforce certificate pinning. `esp_tls` is configured with `skip_common_name_check = true` and `crt_bundle_attach = NULL` (server cert validation is delegated to the remote operator's certificate-rotation policy). The shared-secret header carried inside the TLS-encrypted body provides authenticity; the TLS channel provides confidentiality.
- HTTP 200/201/204 = success. Any 4xx/5xx response is logged as `LOG_NET status_post_fail value_a=<http_status>` and counts against the breaker budget. Network errors (DNS, connect, TLS handshake) are logged as `LOG_NET status_post_fail value_a=-1`.

**SD log upload cycle**
- T14 owns the upload of completed SD log files to the status website. Trigger conditions are (a) a daily upload slot configured by `cfg.log_upload_hhmm` (HH×100 + MM, e.g. `0835` = 08:35), and (b) any T9 event-log rotation event (the prior log file's `closeout` marker is detected on Q3).
- For each pending file under `/sdcard/log/`, T14 opens the file via `SDFileChunkedStream`, allocates a `LOG_UPLOAD_CHUNK_BYTES + 1u` heap buffer (one extra byte so the chunked reader can null-terminate without overrunning), then streams the file via `esp_http_client_open(fsize)` → `esp_http_client_write()` per chunk → `esp_http_client_fetch_headers()`.
- Successful upload (HTTP 200): T14 deletes the local file via `f_unlink()` (after `f_sync()` to ensure the deletion is durable).
- Failed upload: file is retained; T14 advances to the next file; the failed file is retried in the next upload window.
- **Multi-file drain:** T14 walks the directory in one pass per trigger and uploads all eligible files in lexicographic order (chronological by filename `YYYY-MM-DD.log`). The drain is single-threaded — T14 never opens more than one upload connection at a time.
- **Dedup latch:** for a given trigger source (daily-slot OR rotation), T14 sets a one-shot latch at trigger time and clears it once the drain completes (success or terminal failure). A second trigger arriving while the latch is set is ignored. This protects against the daily-slot timer firing during a rotation-triggered upload that has not yet completed.

**Heap-drop measurement (deferred)**
- The signed-balance heap-drop detector originally specified in §5.14 is **deferred**. T14 records pre/post free-heap around each `esp_http_client_perform()` for logging only (`LOG_NET value_a=<free_after>`); the budget accounting that would feed T15 supervisor decisions is not active in the end-state design captured here.

**Synchronization**
- Reads MX2 (current sensor values), MX3 (history rings), MX4 (configuration) when building the status JSON. No mutex held across the network call (build the JSON, drop the mutex, then transmit).
- Posts to Q3 (log events for upload outcomes, breaker state changes, status-POST result codes).
- No event-group bits owned by T14.

#### T15 — Status-POST Supervisor (dormant)

**Priority:** Low (3) | **Stack:** 4 KB

T15 is **dormant in the end-state architecture**. The task slot is reserved and the source file (`firmware/src/status_post_supervisor/`) is preserved for a future bulkhead policy; no `xTaskCreate()` call activates it in the steady-state build.

**Intended role (deferred design, not implemented):**
- Heartbeat monitor: T14 would post a heartbeat to a private inter-task channel on each successful POST cycle. T15 would observe missed heartbeats and, after a configurable budget (`status_post_max_silent_cycles`), declare T14 wedged.
- Heap-drop budget: T15 would track the cumulative signed-heap-drop measurements T14 reports per cycle and, once the cumulative drop crosses `cfg.heap_drop_budget_kb`, declare T14's heap footprint untenable.
- Respawn budget: T15 would terminate and respawn T14 up to `cfg.status_post_max_respawns` times within a sliding window; on budget exhaustion T15 would escalate to a planned reboot via the same `reboot_worker_task` carve-off used by T13.

The deferral rationale is documented in §5.15. The end-state expectation is that the ESP-IDF HTTPS client's keep-alive plus bounded mbedTLS buffers eliminate the per-cycle heap-drop pattern that originally motivated T15, making the supervisor unnecessary.

---

### 4.4 Core Assignment

All application tasks are created with `tskNO_AFFINITY` so the FreeRTOS SMP scheduler may place them on either core based on the live ready-queue state. The ESP-IDF WiFi/lwIP internals run on the protocol core by configuration; no application task is statically pinned.

The rationale for not pinning is that the workload mix is dominated by network-bound waits (T10, T11, T14) and short bursts of compute (T2, T3, T6); the scheduler's runtime balancing is preferred over a static partition that would over-serialise one core while leaving the other idle. The WiFi-driver-on-protocol-core constraint is satisfied by ESP-IDF's own internal task placement and does not require application-task pinning to enforce.

### 4.5 Inter-task Communication

```
  T5 Sensor Poll ──────────────────────────────────► T4 Data Manager
  T8 UI ───────── config writes ───────────────────► T4 Data Manager
  T11 Web ─────── config writes ───────────────────► T4 Data Manager
                                                      │
                    ┌─────────────────────────────────┤ reads
                    ▼                   ▼             ▼
                 T3 Safety          T6 Climate     T8 UI
                 Monitor            Control        Display
                    │                   │             │
                    └──── actuation ────┴─────────────┴──► T2 Relay Controller
                          queue                              │
                                                          relays ──► RRK-3
                                                             │
                                                    feedback ──► override detect
                                                             │
                                                    notify ──► T6, T9

  T7 Keypad ──── key events ──────────────────────► T8 UI / Display

  T2, T3, T5, T6, T8, T10, T11, T14 ─ log events ─► T9 Event Logger

  T10 Network ─── status ─────────────────────────► T8 UI / Display
  T10 Network ─── NTP sync ───────────────────────► system clock

  T11 Web ─────── config/mode changes ────────────► T4 Data Manager
  T11 Web ─────── WS push (every 2 s) ────────────► browser clients

  T4 Data ─────── reads (canonical JSON) ─────────► T14 Status POST
  T14 Status ─── HTTPS POST every cfg.status_interval_s ─► status website
  T14 Status ─── SD log upload (daily slot + T9 rotation) ► status website

  T11 Web ─────── OTA trigger ────────────────────► T13 OTA
```

### 4.6 Synchronization Primitives

#### 4.6.1 Mutexes

FreeRTOS mutexes (`xSemaphoreCreateMutex`) implement priority inheritance, which mitigates priority inversion when a high-priority task (e.g. T3) waits on a mutex held by a lower-priority task.

| ID  | Name                      | Protects                                                                 | Writers                       | Readers                              |
|-----|---------------------------|--------------------------------------------------------------------------|-------------------------------|--------------------------------------|
| MX1 | I2C bus                   | Shared I2C bus (SDA/SCL) — LCD display and DS1307 RTC on the same wires | T8 (LCD write via `i2c_master_transmit`), T4 (RTC read/write) | T1 may probe RTC presence at boot |
| MX2 | Current measurement data  | Latest T, RH, wind speed, wind direction values in T4                   | T4 (on write from T5)         | T3, T6, T8, T11, T12                |
| MX3 | Measurement ring buffers  | History ring buffers for T, RH, wind speed, wind direction in T4        | T4 (on write from T5)         | T8, T11, T12                        |
| MX4 | Configuration settings    | All configurable parameters in T4                                        | T4 (on validated write from Q4) | T3, T6, T8, T11, T12             |
| MX5 | LittleFS filesystem       | Active LittleFS partition (HTML and web asset files served by T11)       | T11 (concurrent HTTP requests) | T11 (concurrent HTTP requests)     |

> **MX2 and MX3 are separate** to ensure T3 (safety-critical) is never delayed by a long ring-buffer read in T9 or T11. T3 only acquires MX2 (current values); it never acquires MX3.

> **NVS flash** (ESP-IDF NVS API) is thread-safe internally and requires no application-level mutex. T4 is the only task that writes configuration settings to NVS; T9 is the only task that writes log entries to NVS. Their NVS namespaces are distinct.

#### 4.6.2 Queues

FreeRTOS queues (`xQueueCreate`) are thread-safe by design. All queue operations are non-blocking for senders where noted, using `xQueueSend` with a timeout of zero or a short value.

| ID | Name                        | Direction   | Senders                                    | Receiver | Item                     | Notes                                          |
|----|-----------------------------|-------------|--------------------------------------------|----------|--------------------------|------------------------------------------------|
| Q1 | Actuation command queue     | → T2        | T3, T6                                      | T2       | Actuation command struct | T3 posts with highest urgency; never blocking  |
| Q2 | Key event queue             | → T8        | T7                                         | T8       | Key code                 | Depth to match max burst; T7 drops on full     |
| Q3 | Log event queue             | → T9        | T2, T3, T5, T6, T8, T10, T11, T13, T14     | T9       | Log event struct         | Generous depth; drop-oldest on overflow        |
| Q4 | Config / state update queue | → T4        | T8, T11, T10                               | T4       | Config update struct     | T4 validates range; persists to NVS on accept  |
| Q5 | Network status queue        | → T8        | T10                                        | T8       | `net_status_t` struct    | Depth 1 (`xQueueOverwrite`); latest status always relevant; struct fields: `client_connected` (bool), `ap_active` (bool), `ntp_synced` (bool), `ip_str[16]` |
| Q6 | Sensor reading queue        | → T4        | T5                                         | T4       | Sensor reading struct    | Depth 1; overwrite semantics — only latest matters |

#### 4.6.3 Task Notifications

FreeRTOS task notifications (`xTaskNotifyGive` / `xTaskNotify`) are used for point-to-point signalling where a full queue is unnecessary. They are faster and consume less RAM than a binary semaphore.

| ID  | Sender | Receiver | Trigger                                      | Purpose                                                        |
|-----|--------|----------|----------------------------------------------|----------------------------------------------------------------|
| TN1 | T4     | T3       | New wind data written to T4                  | Wake T3 immediately to re-evaluate wind safety conditions      |
| TN2 | T4     | T6       | New sensor data (T or RH) written to T4      | Wake T6 to re-evaluate climate control decisions               |
| TN4 | T10    | T4       | WiFi client connection established           | T4 triggers NTP synchronisation and updates system time        |

#### 4.6.4 Event Group — System State Flags

A single FreeRTOS event group (`xEventGroupCreate`) holds all system-wide boolean state flags. Any task may read any flag at any time without blocking; setting and clearing is done only by the designated owner task.

**Event group: EG1 — System State**

| Bit | Flag name          | Set by | Cleared by | Read by                 | Meaning when set                                         |
|-----|--------------------|--------|------------|-------------------------|----------------------------------------------------------|
| 0   | WIND_OVERRIDE      | T3     | T3         | T6, T8, T11, T14 (display/payload only) | Wind safety threshold exceeded; all windows being closed |
| 1   | *(reserved)*       | —      | —          | —                       | Previously MANUAL_OVERRIDE — removed; hardware does not support manual operation detection |
| 2   | SENSOR_FAULT_T     | T5     | T5         | T6, T8, T9, T11, T14   | Temperature/humidity sensor fault active                  |
| 3   | SENSOR_FAULT_W     | T5     | T5         | T3, T8, T9, T11, T14   | Wind sensor fault active; T3 treats wind as worst-case   |
| 4   | OTA_IN_PROGRESS    | T13    | T13        | T11                     | OTA update active; T11 defers LittleFS file requests     |
| 5   | MOTOR_ALARM        | T2     | T2         | T3, T6, T8, T11, T14 (display/payload only) | RRK-3 motor emergency stop active; all relays de-energised; all window control suspended; highest priority override |
| 6   | CALIBRATING        | T2     | T2         | T1, T8, T11, T14       | CLOSE_ALL boot calibration in progress; window positions transitioning from `UNKNOWN` to `CLOSED`; RGB LED shows Blue |

> **T3 and SENSOR_FAULT_W:** when the wind sensor fault flag is set, T3 shall treat the wind condition as exceeding all thresholds (safe-fail: close all windows) until the fault clears.

> **T2 and MOTOR_ALARM:** MOTOR_ALARM takes priority over all other states. T2 discards all incoming Q1 commands while this flag is set. T3 CLOSE_ALL commands are also discarded — the relays are already de-energised and the alarm state persists until the RRK-3 alarm clears.

> **CALIBRATING:** set by T2 at the start of the CLOSE_ALL boot sequence, cleared once all window state machines reach `CLOSED`. While set, T6 holds in standby (no automatic commands); T11 exposes the state in `/api/status.alarms.calibrating`; T14 includes it in the canonical status JSON; T1 drives the RGB LED Blue.

#### 4.6.5 Primitive Cross-reference by Task

| Task | Acquires (mutex) | Posts to (queue) | Receives from (queue) | Sends (notification) | Receives (notification) | Reads/Sets (event group) |
|------|-----------------|------------------|-----------------------|----------------------|-------------------------|--------------------------|
| T1   | MX4             | Q3               | —                     | —                    | —                       | Reads EG1 (all — for RGB status LED colour); reads EG1.CALIBRATING for Blue state |
| T2   | —               | Q3               | Q1                    | —                    | —                       | Sets/clears EG1.MOTOR_ALARM, EG1.CALIBRATING |
| T3   | MX2             | Q1, Q3           | —                     | —                    | TN1 ← T4               | Sets/clears EG1.WIND_OVERRIDE; reads EG1.SENSOR_FAULT_W, EG1.MOTOR_ALARM |
| T4   | MX1, MX2, MX3, MX4 | —            | Q4, Q6                | TN1 → T3, TN2 → T6   | TN4 ← T10              | —                        |
| T5   | —               | Q3, Q6           | —                     | —                    | —                       | Sets/clears EG1.SENSOR_FAULT_T, EG1.SENSOR_FAULT_W |
| T6   | MX2, MX4        | Q1, Q3           | —                     | —                    | TN2 ← T4               | Reads EG1 (MOTOR_ALARM, WIND_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W, CALIBRATING) |
| T7   | —               | Q2               | —                     | —                    | —                       | —                        |
| T8   | MX1, MX2, MX3, MX4 | Q3, Q4      | Q2, Q5                | —                    | —                       | Reads EG1 (all)          |
| T9   | —               | —                | Q3                    | —                    | —                       | —                        |
| T10  | —               | Q3, Q4, Q5       | —                     | TN4 → T4             | —                       | —                        |
| T11  | MX2, MX3, MX4, MX5 | Q3, Q4      | —                     | —                    | —                       | Reads EG1 (all — for `/api/status` and WS push payload) |
| T12  | *(reserved)*    | —                | —                     | —                    | —                       | *(reserved)*             |
| T13  | MX5             | Q3               | —                     | —                    | —                       | Sets/clears EG1.OTA_IN_PROGRESS |
| T14  | MX2, MX3, MX4  | Q3               | —                     | —                    | —                       | Reads EG1 (all — for canonical status JSON) |
| T15  | *(dormant)*     | —                | —                     | —                    | —                       | *(dormant)*              |

---

## 5. Software Modules

### 5.1 Sensor Polling — Modbus RTU

**Implemented by:** T5 (Sensor Poll)

**Driver interface:**
- Modbus RTU master on UART1 with SIT65HVD08P transceiver.
- DE/RE direction control via dedicated GPIO: HIGH during transmit frame, LOW during receive window.
- Bus parameters: 9600 baud, 8N1 (configurable via NVS; default matches sensor factory settings).

**Poll schedule:**
- SenseCAP S200 (Modbus address 1): reads wind speed register and wind direction register each cycle. **No dedicated S200 driver library exists.** T5 calls LIB-6 (`modbus_read_holding_registers()`) directly with the S200 register map from the sensor user guide (`documentation/Sensors/W-Sensecap-S200/`).
- FG6485A (Modbus address 2): reads temperature register and humidity register each cycle. **LIB-10** (`drivers/FG6485A/`) provides the driver. LIB-10 also exposes `fg6485a_task()` — a standalone FreeRTOS periodic polling task. T5 may integrate this directly or call the driver read API within its own loop; the integration approach is to be decided during implementation.
- Poll interval configurable via NVS (default 30 s; range 15–120 s). T4 posts one `LOG_SENSOR` event to Q3 on every Q6 reception — snapshot interval equals poll interval (FR-LG09); no separate snapshot timer.

**Fault detection and response:**

| Fault condition | Detection | Response |
|----------------|-----------|----------|
| No response within timeout | Timer expiry after transmit | Set EG1.SENSOR_FAULT_T or EG1.SENSOR_FAULT_W; post log event to Q3 |
| CRC error | Modbus frame validation | Retry once; on second failure set fault flag and post log event |
| Out-of-range value | Sanity check on decoded value | Discard reading; set fault flag; post log event |
| Fault clears | Successful read after fault | Clear fault flag; post log event |

Link to FRS requirements: FR-S04 (sensor fault detection), FR-W03 (wind sensor fault).

**FG6485A heater supply — not implemented:**
The J5 heater supply connection (HEATING_POS / HEATING_NEG) has been removed from the PCB. The FG6485A heater element is not powered. T5 does not read or log `heating_temperature_c`. No heater-related fault detection is implemented.

---

### 5.2 Climate Control Logic

**Implemented by:** T6 (Climate Control)

**Operating mode state machine:**

```
         ┌──────────────────────────────────┐
         │                                  │
    [Standby] ◄──── admin command ────► [Automatic]
         │                                  │
         │                      wind threshold exceeded
         │                                  │
         │                           [Wind-override]
         │                                  │
         │                   wind safe AND no manual
         │                                  │
         │                             [Automatic]
         │
         │                RRK-3 alarm assert (T2 → EG1.MOTOR_ALARM)
         │                                  │
         │                        [Motor-alarm]
         │                                  │
         │              alarm clears → CLOSE_ALL re-calibration
         │                                  │
         └──────────────────────────────────┘
```

**Window state machine (per channel M1, M2, M3):**

| State         | Entry condition | Exit condition | T2 action |
|---------------|----------------|----------------|-----------|
| `UNKNOWN`     | Boot | CLOSE_ALL boot sequence completes | Assert CLOSE relay; advance to `MOVING_CLOSE` |
| `CLOSED`      | Travel timer expired after CLOSE command; or boot CLOSE_ALL complete | OPEN command received AND close-dwell elapsed | Assert OPEN relay; advance to `MOVING_OPEN` |
| `MOVING_OPEN` | OPEN relay energised | **Travel timer** (`(MOTOR_MN_TRAVEL_S_DEFAULT + MOTOR_TRAVEL_MARGIN_S_DEFAULT) × 1000 ms`) expires | De-energise relay; advance to `OPEN` |
| `OPEN`        | Travel timer expired after OPEN command | CLOSE command received AND open-dwell elapsed | Assert CLOSE relay; advance to `MOVING_CLOSE` |
| `MOVING_CLOSE`| CLOSE relay energised | **Travel timer** (`(MOTOR_MN_TRAVEL_S_DEFAULT + MOTOR_TRAVEL_MARGIN_S_DEFAULT) × 1000 ms`) expires | De-energise relay; advance to `CLOSED` |

**Travel timer vs. dwell timer — critical distinction:**

| Timer | What it times | Value source | Unit | Purpose |
|-------|--------------|--------------|------|---------|
| Travel timer | Relay energisation (window in motion) | NVS `motor/travel_mN` + `MOTOR_TRAVEL_MARGIN_S_DEFAULT` (`app_types.h`); read by T2 from T4 (MX4) | seconds | De-energise relay after end-stop is reached; margin ensures end-switch fires before relay drops; de-energising early stops the motor at an intermediate position |
| Open-dwell timer | Hold period at `OPEN` before CLOSE accepted | NVS `motor/dwell_open_mN`; read by T2 from T4 (MX4) | minutes | Prevents rapid reversal; protects mechanics |
| Close-dwell timer | Hold period at `CLOSED` before OPEN accepted | NVS `motor/dwell_close_mN`; read by T2 from T4 (MX4) | minutes | Prevents rapid reversal; protects mechanics |

- OPEN + CLOSE mutual exclusion is enforced in T2 before asserting any relay output; a 2 s gap is inserted after de-energising the outgoing relay.
- Dwell timers start when the travel timer expires (window reaches end position), not when the command is issued (FR-A09–FR-A12).

**Climate setpoints and hysteresis:**
- T_min_day / T_max_day and T_min_night / T_max_night: day and night temperature thresholds (configurable, farmer level). T6 selects the active pair based on `is_daytime` from T4. Stored and compared as integer °C. Always active; cannot be disabled.
- RH_min_day / RH_max_day and RH_min_night / RH_max_night: day and night humidity thresholds (configurable, farmer level). T6 selects the active pair based on `is_daytime`. Stored and compared as integer %. Only evaluated when the `rh_ctrl_en` flag is true.
- All setpoints are integers; fractional sensor readings are rounded to the nearest integer before comparison.
- Hysteresis band on each setpoint prevents rapid toggling near threshold. Hysteresis values are also integers.

**Graduated ventilation (FR-C09, FR-C10) — `NUM_VENT_STEPS = 3`:**

Windows are opened in up to 3 cumulative steps, each adding one more channel:

| Step | Channels open |
|------|--------------|
| 0    | None (fully closed) |
| 1    | M1 only |
| 2    | M1 + M2 |
| 3    | M1 + M2 + M3 |

The channel assignment is a compile-time table in `climate_control.cpp` (`VENT_STEP_TABLE[]`). `NUM_VENT_STEPS` is defined in `app_types.h`.

**Step selection algorithm** (used for both temperature and RH-open demands):

```
step_width    = max(hyst / NUM_VENT_STEPS, 1)          -- integer division, floor to 1
deviation     = value − setpoint_max                    -- may be negative
raw_step      = ceil(deviation / step_width)            -- integer ceiling; 0 if deviation ≤ 0
required_step = clamp(raw_step, 0, NUM_VENT_STEPS)
```

**Close-hysteresis guard:** once any step > 0 is active, T6 will NOT reduce to step 0 until the measured value falls below `setpoint_max − hyst`. Step reductions within the active range (e.g. 3 → 2 → 1) are applied immediately. This guard prevents oscillation near the setpoint.

**Humidity-close demand (Gap G design decision):** when RH < RH_min (too dry), the required step is always 0 — graduated closing is **not** implemented. A step-0 close demand keeps conflict resolution symmetric: both T and RH demands are expressed as a step number (0 = close, 1–N = open at step N), with the sentinel `VENT_STEP_NEUTRAL` (−1) meaning "RH is in range — no demand from this source."

**Humidity-open demand:** when RH > RH_max (too humid), the same graduated step algorithm is applied using `hyst_rh` as the hysteresis band.

**Humidity disabled:** when `rh_ctrl_en` is false, `vent_step_required_rh()` always returns `VENT_STEP_NEUTRAL`; conflict resolution and RH evaluation are skipped entirely.

The functions `vent_step_required_t()`, `vent_step_required_rh()`, and `vent_resolve_conflict()` are declared in `climate_control.h` and implemented in `climate_control.cpp`.

**Conflict resolution (FR-CR01–FR-CR04):**

Rules applied in order by `vent_resolve_conflict(step_t, step_rh, cr_priority)`:

1. **Wind safety override:** T3 issues CLOSE_ALL regardless of climate demand (independent of this algorithm) — unless wind protection is disabled (`wind_prot_en` = false).
2. **RH neutral:** if `step_rh == VENT_STEP_NEUTRAL`, return `step_t` unchanged (RH has no vote).
3. **Both demand OPEN** (`step_t > 0` and `step_rh > 0`): return the higher step regardless of `cr_priority` — more ventilation satisfies both demands.
4. **No conflict** (`step_t == step_rh`): return as-is.
5. **Genuine conflict** (one OPEN, one CLOSE=0) — apply `cr_priority`:
   - `0 = CR_TEMP_FIRST` — temperature wins (return `step_t`).
   - `1 = CR_RH_FIRST` — humidity wins (return `step_rh`, which may be 0).
   - `2 = CR_DEVIATION` — higher step wins (return `max(step_t, step_rh)`).

Conflict resolution is only active when `rh_ctrl_en` is true. The active conflict and the resolution applied are logged to Q3.

**Motor alarm detection (FR-MA01–FR-MA08):**
- T2 detects the RRK-3 alarm relay (GPIO42) using a deferred-ISR pattern: `IRAM_ATTR` ISR records first edge (volatile flag + FreeRTOS tick timestamp); T2 task loop polls the flag with a ≤10 ms resolution and confirms after 75 ms by reading the live pin state. **Not suppressed during MOVING states** — a motor hitting the emergency switch during a T2-commanded move is the primary alarm scenario.
- **On alarm assert confirmed:** T2 immediately de-energises all 6 relays, sets EG1.MOTOR_ALARM, posts log event to Q3. T2 then discards all incoming Q1 commands while MOTOR_ALARM is set.
- **On alarm release confirmed:** T2 clears EG1.MOTOR_ALARM, posts log event to Q3, then observes a **60 s guard time** (motor may still be coasting; relays remain de-energised, T2 blocked). At guard expiry the live pin is re-checked: if LOW (re-asserted) T2 aborts and the main loop re-enters alarm onset; if HIGH, T2 calls CLOSE_ALL re-calibration, then resumes AUTOMATIC.
- T6 checks EG1.MOTOR_ALARM on every TN2 wake and skips evaluation if the alarm is active.

*Note: Manual override detection (formerly FR-M08–FR-M11) has been removed. The RRK-3 alarm relay does not signal normal manual window operation; it signals motor emergency stop only. Detection of manual window operation is not achievable with the current hardware.*

---

### 5.3 Event Log Manager

**Implemented by:** T9 (Event Logger)

**Log entry structure:**

| Field | Type | Offset | Description |
|-------|------|--------|-------------|
| timestamp | uint32 | 0 | Unix epoch seconds; marked invalid if no time source has synced |
| event_type | uint8 | 4 | Category: SENSOR_HR, SUN, RELAY, MODE_CHANGE, SETPOINT, SESSION, ALARM, SYSTEM. (`SENSOR` is retained in the enum but no longer emitted; see "Sensor logging" below.) |
| initiator | uint8 | 5 | SYSTEM, USER_FARMER, USER_ADMIN, MQTT, WEB |
| channel | uint8 | 6 | Motor channel (M1/M2/M3), `SENSOR_HR` sub-row id (0=T+RH, 1=wind, 2=window-bitmask), or 0 for non-channel events |
| param_id | uint8 | 7 | `log_param_id_t`: identifies the specific CONFIG parameter (C1–C22); 0 for all non-CONFIG events. For C18/C19, `channel` identifies the motor and `param_id` distinguishes open vs close dwell. |
| value_a | int16 | 8 | First payload (sensor value, old setting, reason code) |
| value_b | int16 | 10 | Second payload (new setting, threshold, parameter) |

Total: 12 bytes. No padding needed — four uint8 fields (offset 4–7) fill the alignment gap before `value_a`.

**Sensor logging — `LOG_SENSOR_HR` triplet.** Each sensor poll cycle (30 s default; range 15–120 s) emits **three rows** rather than one, sharing the same Unix timestamp from the originating Q6 reading. The three rows are discriminated by the `channel` field:

| channel | Subject | `value_a` | `value_b` |
|--------:|---------|-----------|-----------|
| 0 | Temperature + Humidity | `t_c10` — temperature × 10 (0.1 °C precision; e.g. `234` = 23.4 °C) | `rh` — humidity, 0..100 % |
| 1 | Wind | `wind_dms` — wind speed × 10 (deci-m/s; e.g. `35` = 3.5 m/s) | `wind_dir_deg` — wind direction, 0..359 ° |
| 2 | Window-state bitmask | 16-bit packed state — see encoding below | 0 (reserved) |

The 16-bit window-state bitmask (channel = 2, `value_a`) packs all three channels' public `window_state_t` plus three EG1 safety bits:

```
bits  1..0  = M1 state    (0=CLOSED, 1=MOVING_OPEN, 2=OPEN, 3=MOVING_CLOSE)
bits  3..2  = M2 state    (same encoding)
bits  5..4  = M3 state    (same encoding)
bits 11..6  = reserved (0)
bit  12     = EG1_BIT_WIND_OVERRIDE  (1 if T3 has forced CLOSE_ALL)
bit  13     = EG1_BIT_MOTOR_ALARM    (1 if RRK-3 emergency-stop active)
bit  14     = EG1_BIT_CALIBRATING    (1 if boot CLOSE_ALL is running OR
                                       STANDBY-exit recalibration is running)
bit  15     = reserved (0) — rc.1.5.0 introduced EG1_BIT_STANDBY (bit 7 in
              EG1) and EG1_BIT_MANUAL_SESSION (bit 8 in EG1), but neither is
              mirrored into the SENSOR_HR ch=2 bitmask. STANDBY visibility
              in the SD log goes through LOG_MODE_CHANGE rows; manual-motor
              activity goes through LOG_RELAY rows (with source attribution
              in the T2 per-command log line). The bit-15 slot stays
              reserved for a possible future operator-mode tracking
              extension if per-sample granularity ever proves needed.
```

T2's internal FSM uses an extended `ch_state_t` with two transient `GAP_TO_OPEN` / `GAP_TO_CLOSE` states for direction-reversal delays; these collapse to the matching MOVING state in the public `window_state_t` returned by `t2_get_window_states()` and packed by `t2_get_window_bitmask()`. GAP visibility (~2 s per reversal) is lost in the log; the analysis pipeline treats GAP-folded transitions as MOVING.

The pre-existing single-row `LOG_SENSOR` format (`value_a = int16 °C`, `value_b = int16 % RH`) is **sunset and no longer emitted**. Its enum value is retained in `log_type_t` and its `"SENSOR"` type-column string is retained in the CSV row formatter so that historical SD files served via `/api/log/download` continue to display unchanged.

**Sun-time logging — `LOG_SUN`.** A single row records sunrise and sunset (local-time minutes from midnight). Emitted by T4 only when the cached values change:

| channel | Subject | `value_a` | `value_b` |
|--------:|---------|-----------|-----------|
| 0 | Sunrise / sunset (local) | `sunrise_min` — minutes from local midnight, 0..1439 | `sunset_min` — minutes from local midnight, 0..1439 |

In steady-state operation this produces one row per local day (the midnight rollover triggers the recompute, which detects the ~1–2 min/day shift typical of spring/autumn). The trigger also fires once at boot (cache sentinel vs first computed value) and once per operator coordinate edit via Q4. The row makes per-day night-shading in the analysis pipeline self-contained — historical days no longer need a live `/api/status` lookup for their dawn/dusk values.

**Storage:**
- **SD card (FAT32) is the sole event-log persistence target.** See log rotation policy below.
- T9 checks SD card presence on startup and on each write cycle. If the card is absent, unmounted, or returns a write error, T9 suspends event logging (the call to `log_post()` still succeeds and the event is consumed from Q3, but no on-disk record is written) until the next successful mount. The card-absent condition is signalled on the LCD and in `/api/sd/status`.
- The NVS `log` namespace once reserved as an event-log ring-buffer fallback is **not used** in the end-state design and shall not be defined in the NVS layout (§5.10). Removing the fallback simplifies the persistence path, avoids the wear-levelling cost of high-frequency NVS writes, and aligns the design with operator practice of treating the SD card as a mandatory installation component.

**SD card log file format:**
- CSV text file; first line is a fixed header row: `timestamp,type,initiator,ch,param,value_a,value_b`
- Each subsequent line is one log entry. Examples:
  - `2026-05-23T13:30:22,SENSOR_HR,SYS,0,0,234,65` — T+RH (23.4 °C / 65 % RH)
  - `2026-05-23T13:30:22,SENSOR_HR,SYS,1,0,35,158` — wind (3.5 m/s / 158 °)
  - `2026-05-23T13:30:22,SENSOR_HR,SYS,2,0,42,0` — bitmask 0x002A (all windows OPEN, no overrides)
  - `2026-05-23T00:01:12,SUN,SYS,0,0,330,1296` — sunrise 05:30 / sunset 21:36 local
- The `timestamp` field is an ISO 8601 **local-time** string (`YYYY-MM-DDTHH:MM:SS`), formatted via `localtime_r()` + `strftime("%Y-%m-%dT%H:%M:%S")`. Average line length: ~55 bytes. Estimated daily volume: ~483 KB (8 640 sensor sub-rows at 30 s default interval × 3 sub-rows per cycle + ~1 `SUN` row + ~150 discrete events).

**SD card log file naming:**
Files are named `YYYYMMDDHHMMSS.csv`, where:

| Token | Meaning |
|-------|---------|
| YYYY | 4-digit year |
| MM | 2-digit month (01–12) |
| DD | 2-digit day (01–31) |
| HH | 2-digit hour, 24-hour clock (00–23) |
| MM | 2-digit minute (00–59) |
| SS | 2-digit second (00–59) |

The timestamp encodes the moment the file was created (local time). Files are stored in the root directory of the SD card. Lexicographic sort of filenames yields chronological order, which is used by the startup scan and the web log retrieval interface. T9 applies an `is_ts_filename()` filter (exactly 14 decimal digits + `.csv`) so that old sequential-index files (`ghc_NNNN.csv`) from a previous firmware version are silently ignored and do not interfere with rotation or the file count.

**SD card log rotation policy:**

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Maximum file size | 1 024 KB (1 MB) | At ~483 KB/day typical rate, each file spans ~2 days. A power-loss event can corrupt only the currently open file; all closed files are intact. |
| Files retained | 30 most recent | 30 × 1 MB = 30 MB maximum log footprint — comfortable against any reasonable SD card. Minimum guaranteed on-card history: ~63 days, well over the daily T14 upload's catch-up window. |
| Minimum retention floor | 5 files | The free-space guard never deletes below this count. |
| Low free-space threshold | 4 MB | If SD free space drops below 4 MB and the file count is above the floor, the oldest file is deleted to reclaim space. If already at the floor (5 files) and space is still below 4 MB, SD logging is suspended. SD logging resumes on the next successful mount command. |

**Rotation procedure (triggered when current file reaches 1 MB):**
1. Create a new file named with the current local timestamp (`YYYYMMDDHHMMSS.csv`).
2. Write the CSV header row to the new file.
3. If the total timestamp-file count now exceeds 30, delete the lexicographically oldest file.
4. Check free space (`storage_sd_free_bytes()`): if < 4 MB, invoke the free-space guard (delete oldest or suspend).

**Write-failure reclaim:** if `storage_sd_write_append()` returns `STORAGE_ERR_FULL` or `STORAGE_ERR_IO`, T9 attempts a single oldest-file deletion and retries the write. If the retry also fails, T9 suspends event logging and surfaces the condition in `/api/sd/status`; subsequent `log_post()` calls drain Q3 without writing until the next successful mount.

**Startup / resume behaviour:**
On SD card mount, T9 calls `storage_sd_list_csv(".csv", ...)` and filters results through `is_ts_filename()` (14 decimal digits + `.csv`). The lexicographically largest matching filename is the most recent file. If its size is below `SD_ROTATE_BYTES` (1 MB), T9 resumes appending to it; otherwise a new timestamp file is created. If no matching files exist, a new file is created immediately.

**Corruption resilience:**
- Power loss during a write may leave the last partial CSV line incomplete; all preceding complete lines remain parseable.
- FAT32 sector-level corruption (512 B) within a closed file affects at most one log entry; the remainder of the file is unaffected because each CSV line is self-contained and line-delimited.

**Retrieval:**
- Web interface: paginated log view, filterable by event type and time range (FR-LG05).
- USB serial diagnostic port: raw log dump command.

**Queue management and drop-oldest overflow policy (Gap H):**

FreeRTOS queues have no native drop-oldest mode for multi-element queues (`xQueueOverwrite()` is valid only for depth-1 queues). All producers post to Q3 through the `log_post()` helper declared in `event_logger.h`. Calling `xQueueSend(Q3, ...)` directly from any task is prohibited; `log_post()` is the single enforcement point for the overflow policy.

`log_post()` implements the two-step evict-and-retry pattern:

```
1. xQueueSend(Q3, evt, 0)
   → pdPASS → done (common path)
   → pdFAIL → queue full; continue

2. xQueueReceive(Q3, &discard, 0)   -- evict oldest; drop counter ++
3. xQueueSend(Q3, evt, 0)           -- retry
   → pdFAIL → rare concurrent-sender race; new event lost; drop counter ++
```

The drop counter (`g_q3_dropped`) is a `volatile uint32_t` protected by a FreeRTOS spinlock (`portMUX_TYPE`). The spinlock critical sections are sub-microsecond on the ESP32-S3; they do not materially affect the latency of calling tasks.

The `xQueueReceive` and retry `xQueueSend` are not atomic. If two tasks simultaneously reach step 2, one may take the freed slot and the other's retry will fail. This is an accepted trade-off given Q3's depth (32 entries) and T9's drain rate; both the evicted old entry and the second sender's new event are counted in the drop counter. A mutex around the full evict-and-retry sequence would eliminate the race at the cost of added latency in high-priority callers (T3, T6); this is deferred until load testing demonstrates it is necessary.

T9 calls `log_take_dropped_count()` (which atomically reads and resets `g_q3_dropped`) after each drain pass. If the count is non-zero, T9 emits one synthetic `LOG_SYSTEM` event with `value_a` = drop count using `xQueueSend(Q3, ..., 0)` directly (not via `log_post()`, to avoid re-entrant eviction). This makes queue pressure visible in the log record without losing a current event to report it.

---

### 5.4 Access Control and Session Management

**Implemented by:** T8 (local keyboard) and T11 (web interface)

**Operating states:**

| State | Description |
|-------|-------------|
| **Normal operation** | No user logged in. Windows are controlled, status is displayed. No settings can be changed. |
| **Farmer session** | Farmer PIN accepted. Farmer-level parameters are editable. Admin-only parameters are hidden from display. |
| **Administrator session** | Admin PIN accepted. All parameters accessible: farmer parameters in read-write; admin parameters in read-write. |

**PIN specification:**

| Role | Format | Length |
|------|--------|--------|
| Farmer | Numeric | 4 digits |
| Administrator | Numeric | 8 digits |

- PINs are stored in NVS as salted SHA-256 hashes; plain-text is never stored or transmitted (FR-AC06). Implementation: `SHA-256(salt || pin_ascii)` using `mbedtls/sha256.h` (bundled with ESP-IDF — no extra library dependency). The 16-byte random salt is generated once at first boot via `esp_fill_random()` and stored in NVS `access/pin_salt`. See `firmware/src/auth/pin_auth.h`.
- PIN entry via the 4×4 keypad (numeric keys 0–9).
- Session timeout: configurable idle period (admin setting); on expiry the session closes and the controller returns to Normal operation.

**PIN management:**
- Farmer may change their own PIN only.
- Administrator may change both the farmer PIN and the administrator PIN.

**Login lockout (FR-AC07):**
- Configurable maximum failed attempts (default: 5) before the input is locked for a configurable timeout (default: 5 minutes).
- Lockout applies independently to farmer and administrator PIN entry.
- Lockout events are logged to Q3.

**Administrator password recovery:**
- A recovery procedure shall be implemented that requires deliberate physical action (candidate: hold specific key combination at power-on while a hardware jumper is fitted) to prevent accidental activation.
- The recovery procedure resets the administrator PIN to the factory default and logs the event.

**Role-based parameter visibility:**

| Visibility class | Normal operation | Farmer session | Administrator session |
|------------------|-----------------|----------------|-----------------------|
| *Free* parameters | Read-only | Read-only | Read-only |
| *Farmer* parameters | Hidden | Read-write | Read-write |
| *Administrator* parameters | Hidden | Hidden | Read-write |

Farmer-level parameters include: day and night temperature setpoints (T_min_day, T_max_day, T_min_night, T_max_night), day and night humidity setpoints (RH_min_day, RH_max_day, RH_min_night, RH_max_night), humidity control enable/disable (`rh_ctrl_en`), wind protection enable/disable (`wind_prot_en`), conflict resolution priority (`cr_priority`), and geographic location for sunrise/sunset calculation (`lat_*`, `lon_*`) — web GUI only (FR-CF16). Administrator/technician-level parameters include: wind safety thresholds (v_max, direction exclusion zone), hysteresis values, dwell times (web GUI only, FR-CF10/CF11), motor travel times (`travel_m1`, `travel_m2`, `travel_m3`, web GUI only, 5–300 s, FR-CF05; defaults `MOTOR_MN_TRAVEL_S_DEFAULT` in `firmware/config/cfg_defaults.h`), sensor poll interval (web GUI only, 15–120 s, FR-CF07), sliding average windows (web GUI only, 1–60 min, FR-CF17), network configuration, and access control settings.

The web interface applies the same three-state model and the same PIN codes as the local keyboard interface.

---

### 5.5 Local User Interface (LCD / Keypad)

**Implemented by:** T7 (Keypad Scan) and T8 (UI / Display)

**LCD driver note:** The Waveshare LCD1602 module uses an **AiP31068L** I2C-to-parallel bridge at address **0x3E**. LIB-4 (`drivers/LCD1602_I2C/`) is implemented for this module and address.

**Keypad handling:**
- Matrix scan period: ~20 ms (software timer or dedicated task).
- Software debounce: key must be stable for 2 consecutive scan cycles before it is accepted.
- Key-repeat: configurable initial delay and repeat rate for navigation keys (up/down in menus).
- Validated key-press events posted to T8 via Q2.

**Main status screen (default display):**

```
Line 1: [T: xx.x°C  RH: xx%]
Line 2: [W: x.x m/s  Mxx   ]
```
Where `Mxx` encodes active window states (e.g. `M1O` = M1 open, `M2C` = M2 closed) and active alarms are indicated by a blinking character in line 2.

**Precision policy for T on the main screen:** T is rendered with one decimal place (`xx.x°C`) directly from the `t_c10` integer field carried in T4's measurement struct (units of 0.1 °C). T8 must not round `t_c10` to the nearest integer before formatting; the LCD render path computes `t_c10 / 10` and `t_c10 % 10` independently to produce the integer and tenths digits, matching the precision shown on the web dashboard and the WS push.

**Menu FSM:**
- Maximum navigation depth: 4 key presses from the main screen to any first-level setting (FR-UI07).
- Menu structure: Main → Category → Parameter → Edit → Confirm.
- `#` key: confirm / enter. `*` key: cancel / back. Numeric keys: input values. `A`/`B`: scroll up/down in lists.
- On session timeout: menu FSM resets to main screen and session closes.

**Status page cycling (`STATUS_PAGES = 7`):**
- Page 0: temperature and humidity readings
- Page 1: wind speed and direction
- Page 2: operating mode + active session (`Mode: AUTO/WIND/ALARM/STANDBY/Window Cal.` on row 0; `Sess: NONE/Farmer/Admin` ± `OTA` on row 1). **rc.1.5.0+ (gh#28):** `#` enters `UI_MODE_TOGGLE` (1=Auto, 2=Stby, *=back). Either Farmer or Admin PIN accepted — `handle_pin()` discriminates by digit count on submission (4 = Farmer, 8 = Admin). Commit calls `dm_set_standby(want, init, surface=1)`; that helper toggles `EG1_BIT_STANDBY`, persists to `system/mode_standby` NVS, emits a `LOG_MODE_CHANGE` audit row, and on STANDBY exit posts `CMD_RECALIBRATE` to Q1 (T2 runs `calib_close_all()` synchronously).
- Page 3: network status (AP active / client IP)
- Page 4: current local date/time (via `localtime_r`); source label "NTP" or "RTC" (from `s_net.ntp_synced`); pressing `#` enters the manual time-set flow (FR-UI22, FR-UI23)
- Page 5: motor (window) states — row 0: `M1    M2    M3  `; row 1: four-character state per channel (`OPEN` / `CLOS` / `MOV>` / `MOV<` / `UNK `); state read via `t2_get_window_states()` (FR-UI04). **rc.1.5.0+ (gh#29):** `#` enters `UI_MOTOR_PICK` (1=M1, 2=M2, 3=M3, *=back) → `UI_MOTOR_ACTION` (1=Open, 2=Close, *=back). **Admin PIN only.** **rc.1.5.2+ semantics**: menu entry auto-enters STANDBY via `dm_set_standby_ex(true, LOG_BY_ADMIN, 1, false)` (only if STANDBY wasn't already on; the auto-set fact is tracked in `s_manual_set_standby_on_entry`). T6 stays gated by the standard `EG1_BIT_STANDBY` for the entire admin PIN session — not just while the admin is keypressing in the menu. **Menu does NOT auto-dismiss on idle.** **`*=back` does NOT clear STANDBY** — it only navigates back to the auto-rotating status screens; STANDBY persists. STANDBY auto-clears (no recal) when the admin session ends via `session_close()` (5-minute timeout from last keypress or explicit logout). Re-entering the menu within the same session preserves the flag (`true` stays `true`). If STANDBY was already on before the menu was entered, the flag stays false and the session-end leaves STANDBY untouched. Actions post `window_cmd_t` to Q1 with `source = SRC_OPERATOR_MANUAL`; T2 dwell-timer check is bypassed for that source. Safety gates remain authoritative: `WIND_OVERRIDE` blocks OPEN (CLOSE accepted); `MOTOR_ALARM` and `CALIBRATING` block all manual commands.
- Page 6: firmware version (`fw_ver` + `asset_version` from `system_id`); read-only diagnostic.

**D-key page advance:** pressing `D` on any status page immediately increments `s_status_page` (modulo `STATUS_PAGES`) and resets `s_status_ticks` to 0, giving the new page a full 5 s dwell before the next auto-advance.

**Manual time-set flow (FR-UI23, admin session required):**
1. `#` pressed on status page 4 → if no admin session active: enter PIN flow first (`s_pending_settime = true`); else: enter `UI_SET_DATE` directly.
2. `UI_SET_DATE`: row 0 shows current date as reference (`YYYY-MM-DD`); row 1 shows `DD/MM/YY #OK *Bk` with `_` for untyped digits. Accepts exactly 6 numeric digits (DDMMYY). `#` validates and saves date to `s_dt_saved_*`; advances to `UI_SET_TIME`. `*` returns to `UI_STATUS`.
3. `UI_SET_TIME`: row 0 shows `Now: HH:MM`; row 1 shows `HH:MM #OK *Bk`. Accepts exactly 4 numeric digits (HHMM). `*` returns to `UI_SET_DATE`. `#` builds `struct tm` from saved date + typed time with `tm_isdst = -1`, calls `mktime()` to produce UTC epoch, calls `dm_set_manual_time(unix_ts)`, then returns to `UI_STATUS`.

**Alarm display:**
- Motor alarm active (`EG1.MOTOR_ALARM`): displayed prominently on line 1 with "MOTOR ALARM — CONTROL SUSPENDED" message (FR-MA05).
- Sensor fault (T or wind): displayed on line 2 with blinking indicator.
- Wind safety override active: displayed prominently with wind speed reading.
- WiFi AP active: displayed on line 2 with IP address if in client mode.

---

### 5.6 WiFi — Access Point Mode

**Implemented by:** T10 (Network Manager)

- WiFi AP mode is **mandatory** (Must have). T10 always brings the radio up in `WIFI_MODE_APSTA` so both interfaces are simultaneously available.
- The AP enable state is held in NVS (`wifi` / `ap_enable`, `int32` 0/1, default **0** — admin must explicitly opt in). T10's `poll_ap()` reads this key every `NET_POLL_MS` from the main loop; on any edge versus the previous tick, T10 calls `start_ap()` or `stop_ap()` accordingly. The `ap_active` flag in Q5 reflects the current radio state. Default-off is a security choice: auto-enabling on a failed STA association would expose an unconfigured greenhouse to anyone in radio range, so admin action is required to open the AP.
- AP SSID is auto-generated as `"Greenhouse-"` followed by the hexadecimal representation of the last 2 bytes of the WiFi NIC MAC address (e.g. `"Greenhouse-A3F2"` if the MAC ends in `A3:F2`). The SSID is not stored in NVS; it is regenerated from the MAC address on each AP start.
- AP password is stored in NVS (`wifi` / `ap_psk`) as **plaintext**. WPA2 requires the raw passphrase during the handshake; hashing is not applicable (contrast with the client PSK, which is stored as a hash because it only needs to match, not be transmitted). Default password: `0123456789`. Configurable by the administrator via the web interface.
- **AP auto-shutdown** (uptime timer): configurable per `cfg.ap_timeout_min`, stored in NVS as `system` / `ap_timeout` (`int32` minutes, default 30, range `[0, CFG_MAX_TIMEOUT_MIN]`; **`0` = stay up indefinitely**). The timer is a flat measurement of elapsed FreeRTOS ticks since `start_ap()` — it is **not** idle-based and does not consult the associated-station list. Once `elapsed_ms >= ap_timeout_min × 60 000`, T10's `poll_ap()` calls `stop_ap()` and posts `wifi/ap_enable = 0` via Q4 so the next poll does not restart the AP; the operator must re-enable `ap_enable` from the web interface to bring the AP back up.
- While the AP is active, the LCD displays "AP active" and the assigned AP IP address; the LCD AP-status line is driven from the Q5 `ap_active` flag, not from `esp_wifi_get_mode()`.
- The local HTTP web interface (§5.8) is accessible to clients connected to the AP on the soft-AP IP (`192.168.4.1` by default). The captive-portal redirect URL is the dashboard root `/`.
- WPA2 security minimum (TR-NW01).

---

### 5.7 WiFi — Client Mode

**Implemented by:** T10 (Network Manager)

- WiFi client (station) mode is **mandatory** (Must have) — the outbound status POST (T14) and NTP synchronisation depend on it. STA is enabled whenever `cfg.sta_ssid` is non-empty.
- The HTTP configuration web interface (§5.8) is accessible to clients on the same network when the controller is connected.
- **IP configuration: DHCP only.** Static-IP configuration is intentionally not supported — the controller is designed for residential / small-business networks where DHCP is the norm, and the configuration surface is kept small. Operators requiring a fixed IP shall do so via DHCP reservation on the upstream router.
- LCD display shows current WiFi client status, driven from Q5:
  - *Disconnected* — STA credentials configured but no association or no IP yet.
  - *Connected* — associated and DHCP IP obtained; LCD shows the assigned address.
- T10 is event-driven: it subscribes to `WIFI_EVENT` and `IP_EVENT` via `esp_event_handler_register()` and translates those callbacks into Q5 posts. Polling `esp_wifi_is_connected()` is not used in the steady-state path; the `EG1_BIT_NETIF_READY` style gating that existed before the IDF migration is now structurally implicit because IDF emits `IP_EVENT_STA_GOT_IP` only after the netif is fully wired up.
- Reconnect backoff: on `WIFI_EVENT_STA_DISCONNECTED` T10 attempts an immediate reconnect; subsequent failures use an exponential backoff up to 30 s, then a steady 30 s retry cadence until success.
- On `IP_EVENT_STA_GOT_IP`: T10 triggers NTP synchronisation. On NTP success it sends TN4 to T4 and calls `do_geo_sync()` to auto-detect location and timezone (FR-DN06, FR-DN07). See §4.3 T10 for the full `do_geo_sync()` description.
- A periodic NTP resync runs every 24 hours after the first sync (and after each `WIFI_EVENT_STA_DISCONNECTED → IP_EVENT_STA_GOT_IP` recovery) to bound the drift of the on-board RTC. Each resync emits a `LOG_NET` audit event.
- Time display in all contexts (web dashboard, LCD page 4, T14 payload) uses `localtime_r()` after the TZ string has been applied so the displayed time automatically reflects the correct timezone and DST offset.

---

### 5.8 Web Interface

**Implemented by:** T11 (Web Server)

**Technology:**
- Web server: ESP-IDF `esp_http_server` (component `esp_http_server`), driven from a single FreeRTOS task (T11) plus one child task for the WebSocket push (see §4.3 T11).
- HTML, CSS, JavaScript, image and font assets live in the dual A/B LittleFS partitions on the ESP32-S3 flash, served from the partition matched to the active firmware bank. Asset versioning is enforced by the build pipeline so that `sys.fw_ver` and `sys.asset_version` always match on the active bank; T11 surfaces both fields so the dashboard can display a MISMATCH badge if the operator force-flashes only one half.
- The web interface mirrors the local keyboard interface exactly: same three operating states (§5.4), same PIN codes, same parameter visibility rules.

**Transport:**
- The local web interface is served over plain HTTP on TCP port 80. It is intended for use on the LAN behind the operator's firewall (or on the controller's own AP). Confidentiality on the LAN is out of scope; PIN-based authentication and session cookies (§5.4) provide authorisation.
- HTTPS is **not** offered on the local interface. The reason is heap footprint: terminating TLS in `esp_http_server` would require a sustained mbedTLS context per active session and would compete with T14's outbound `esp_tls` context. Operators requiring transport encryption on the local network shall front the controller with a reverse proxy.
- The outbound status POST channel (T14) is a different matter — see §2.6 and §5.13.

**Access control:**
- Authentication required before any setting is changed or any non-public page is served (FR-NW06).
- Session cookie (`session=<32-hex-char-token>`, `HttpOnly`, `SameSite=Lax`) issued after a successful `POST /api/login`; the token indexes the in-RAM session table (`firmware/src/auth/session.*`). Cookie cleared by `POST /api/logout`.
- A request without a valid session that hits a non-public route receives HTTP 401. Public routes are the static dashboard shell (`/`, `/style.css`, `/app.js`, `/manifest.json`), the login endpoint (`POST /api/login`), the whoami probe (`GET /api/whoami`), the network-status JSON (`GET /api/status`), and the live WebSocket (`GET /ws`). All other API routes require an authenticated session at the appropriate role.
- Cookie parsing is done by hand from the `Cookie:` request header using `httpd_req_get_hdr_value_str()` since `esp_http_server` provides no high-level cookie helper.

**URI table:**

The full set of routes registered by T11 at `httpd_start()`:

| Method | URI | Role | Purpose |
|--------|-----|------|---------|
| GET | `/` | public | Static dashboard shell (`index.html` from LittleFS with cache-bust `?v=<FIRMWARE_VERSION>` injection) |
| GET | `/style.css` | public | Static asset |
| GET | `/app.js` | public | Static asset |
| GET | `/manifest.json` | public | PWA manifest |
| GET | `/favicon.ico` | public | Static asset |
| POST | `/api/login` | public | PIN check; issues session cookie |
| POST | `/api/logout` | any session | Invalidates the current session |
| GET | `/api/whoami` | public | Returns `{role: "none"\|"farmer"\|"admin"}` for the current session |
| GET | `/api/status` | public | Full canonical status JSON (sensors, modes, alarms, setpoints, network, sys) |
| GET | `/api/history` | any session | Ring-buffer history for T, RH, wind speed, wind direction |
| GET | `/api/config` | any session | Current configuration; admin-only fields masked for farmer sessions |
| POST | `/api/config` | farmer/admin per field | Single-field write; T11 validates role per parameter before forwarding to T4 |
| GET | `/api/wifi` | admin | Returns STA SSID (read-only) + AP enable + AP SSID + AP PSK masked |
| POST | `/api/wifi` | admin | Update STA credentials or AP enable/PSK |
| POST | `/api/pin` | admin | Change farmer or admin PIN |
| GET | `/api/sd/status` | any session | SD card mount state, free bytes, file count |
| POST | `/api/sd/mount` | admin | Manual SD mount |
| POST | `/api/sd/unmount` | admin | Manual SD unmount (graceful — `f_sync()` before unmount) |
| GET | `/api/log/files` | admin | Returns `{sd_files:[...]}` for the log-download dropdown |
| GET | `/api/log/download` | admin | Streams a `.csv` log file to the browser; path-traversal guard rejects `/` and `..` |
| POST | `/api/ota/firmware` | admin | Streams firmware image to T13's writer; multipart accumulator |
| POST | `/api/ota/assets` | admin | Streams web-asset ZIP to T13's PSRAM accumulator |
| POST | `/api/ota/commit` | admin | Marks the new bank as next-boot; arms reboot timer |
| POST | `/api/ota/cancel` | admin | Aborts the in-progress upload; rolls back the partition writer |
| POST | `/api/web` | admin | Direct web-asset ZIP push (no firmware pair); used for asset-only updates |
| POST | `/api/factory_reset` | admin | Erases configuration namespaces; requires physical-confirmation header |
| POST | `/api/reboot` | admin | Soft reboot via `reboot_worker_task` carve-off (same path as T13's reboot) |
| GET | `/ws` | public (read-only) | WebSocket upgrade; pushes canonical status JSON every 2 s |

The MQTT integration page that was reserved in earlier specifications is not exposed in the URI table; see T12 (§4.3) for the deferred-Could-be status.

**Pages:**

- **Dashboard** *(any authenticated session, plus public read-only via WS)*: live T (0.1 °C precision), RH, wind speed, wind direction (averaged), window states (OPEN / MOVING / CLOSED per channel), operating mode, sunrise/sunset times for current day (FR-DN04), active alarms, calibrating indicator while EG1.CALIBRATING is set.

- **Settings** *(sub-sections; access level per row)*:

  | Sub-section | Access | Parameters |
  |-------------|--------|------------|
  | **Climate** | Farmer / Admin | T_min_day, T_max_day, T_min_night, T_max_night (°C); RH_min_day, RH_max_day, RH_min_night, RH_max_night (%); humidity control enable (`rh_ctrl_en`); conflict resolution priority (`cr_priority`); geographic location lat/lon for sunrise/sunset (FR-CF16) |
  | **Wind** | Farmer (enable/disable only) / Admin (all) | Wind protection enable (`wind_prot_en`); v_max (Beaufort); direction exclusion zone centre and half-width (°); wind hysteresis timer (FR-CF09) |
  | **Motors** | Admin only | Motor travel times: M1, M2, M3 individually (seconds, range 5–300 s, factory defaults 21/21/171 s, FR-CF05); open-dwell time per window M1–M3 (minutes, FR-CF10); close-dwell time per window M1–M3 (minutes, FR-CF11) |
  | **Sensors** | Admin only | Sensor poll interval (15–120 s, factory default 30 s, FR-CF07); sliding average window for T and RH (1–60 min, FR-CF17) |
  | **System** | Admin only | Session timeout (minutes); RGB LED day/night brightness and schedule (`led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`, FR-CF14); NTP timezone string; status-website URL; status interval; status secret (write-only); status expose bitmask; daily log-upload time (HHMM) |
  | **Access** | Admin only | Change farmer PIN; change admin PIN; lockout threshold and duration |

  Each editable field shows its current value, the valid range, and the factory default. A **Restore defaults** button is available per sub-section (admin only); factory reset of all settings requires physical confirmation (admin only).

- **Log** *(admin only)*: dedicated tab that consolidates SD card management and event log download:
  - **SD card controls:** mount and unmount the SD card.
  - **Log download:** a dropdown populated by `GET /api/log/files` (returns `{sd_files:[...]}`) lists each `.csv` file found on the SD card.
  - A **Download CSV** button triggers a browser file download via `GET /api/log/download?file=NAME` (filename preserved). Path-traversal guard on the server rejects any filename containing `/` or `..`. Returns HTTP 503 if SD is unmounted, 404 if file not found (FR-LG05).

- **OTA update** *(admin only)*: firmware binary upload and web-asset `.zip` upload (T13).

- **Network** *(admin only)*: WiFi AP configuration (auto-generated SSID, password, idle-timeout); WiFi client configuration (SSID, PSK; DHCP only — no static IP fields).

---

### 5.9 OTA Firmware Update

**Implemented by:** T13 (OTA)

**Flash partition layout** (`firmware/partitions.csv`, 16 MB QSPI flash):

| Label | Type | SubType | Offset | Size | Role |
|-------|------|---------|--------|------|------|
| `nvs` | data | nvs | 0x009000 | 84 KB (0x15000) | Configuration namespaces |
| `otadata` | data | ota | 0x01E000 | 8 KB (0x2000) | Active/inactive bank metadata (ESP-IDF OTA) |
| `app0` | app | ota_0 | 0x020000 | 2 MB (0x200000) | Firmware Bank A |
| `app1` | app | ota_1 | 0x220000 | 2 MB (0x200000) | Firmware Bank B |
| `lfs0` | data | spiffs | 0x420000 | 1 MB (0x100000) | LittleFS A — web assets paired with Bank A |
| `lfs1` | data | spiffs | 0x520000 | 1 MB (0x100000) | LittleFS B — web assets paired with Bank B |
| `coredump` | data | coredump | 0x620000 | 64 KB (0x10000) | ESP-IDF coredump partition |
| *(unused)* | — | — | 0x630000 | ~9.8 MB | Reserved for future expansion |

> **LittleFS subtype note:** The ESP-IDF partition table has no dedicated `littlefs` subtype. Both `lfs0` and `lfs1` use subtype `spiffs`; the LittleFS library locates the partition by label (name), not subtype.

- The system boots from whichever firmware bank is marked **active** in `otadata`.
- The active LittleFS partition is always the one with the same letter as the active firmware bank: Bank A (`app0`) → `lfs0`; Bank B (`app1`) → `lfs1`. This coupling is fixed and unconditional; T13 enforces it on every bank switch.
- T11 mounts only the active LittleFS partition. The inactive LittleFS partition is never mounted by T11.

**Firmware update procedure:**
1. Administrator uploads new firmware image via web interface (admin session required).
2. T13 writes the image to the inactive firmware bank.
3. On successful write and integrity check: inactive bank is marked active.
4. System reboots. Both the firmware bank and the paired LittleFS partition switch together.

**Failsafe rollback:**
- If the newly booted firmware fails to complete its startup health check 3 consecutive times, the previous bank is automatically restored as active and the system reboots into the known-good firmware.
- Because LittleFS is coupled to the firmware bank, rolling back the firmware bank automatically restores the matching web assets. No separate web asset rollback is needed.
- Rollback events are logged.

**Web asset update procedure:**
1. Administrator uploads a STORE-only `.zip` archive of HTML/CSS/JS/font/image files via the web interface (admin session required).
2. T11's `/api/ota/assets` handler streams the request body chunk-by-chunk into T13's PSRAM accumulator via `ota_assets_accumulate(data, len, offset)`. The full archive is held in PSRAM only — never in flash — so an aborted or failed upload leaves the on-flash inactive partition untouched.
3. T13 sets **EG1.OTA_IN_PROGRESS**.
4. T13 formats the **inactive** LittleFS partition for a clean state, then mounts it. The active partition remains mounted by T11 and continues to serve requests uninterrupted — MX5 is not acquired during this phase.
5. T13 walks the in-PSRAM ZIP central directory and extracts each entry directly to the inactive LittleFS partition. Because the archive is STORE-only (no deflate), each entry is a contiguous byte range that can be copied without an inflate pass.
6. T13 writes `manifest.json` to the inactive partition as the **last step**, only after all files have been extracted and the partition flushed:

   ```json
   {
     "asset_version": "MAJOR.MINOR.PATCH",
     "checksum":      "<hex string — CRC32 or SHA-256 of the zip archive>"
   }
   ```

7. T13 unmounts the inactive LittleFS partition, frees the PSRAM buffer, and clears **EG1.OTA_IN_PROGRESS**.
8. The inactive LittleFS is now ready. It is activated on the next firmware bank switch (step 3 of the firmware update procedure above), or immediately if only web assets are being updated (T13 switches the active bank pointer without a firmware image change).

**Reboot and firmware-only fallback:**
- The reboot that finalises an OTA cycle is performed by `reboot_worker_task` carved off the FreeRTOS timer-service task (see §4.3 T13). Calling `esp_restart()` from the timer callback directly would exceed the timer-service task's 2 KB stack because the WiFi-teardown chain inside `esp_restart()` consumes several KB.
- If a firmware upload completes verification but no paired asset upload arrives within `FW_DONE_FALLBACK_MS` (120 s), T13 commits the firmware alone and reboots via the same `reboot_worker_task` path. The asset partition stays at the previous version; on the next boot the new firmware runs against the older asset bundle and surfaces a MISMATCH badge until the operator pushes matching assets.

**Web asset version tracking:**
- On startup, T11 reads `manifest.json` from its active LittleFS partition and compares `asset_version` against `system/fw_version` (from NVS):

  | Condition | T11 behaviour |
  |-----------|---------------|
  | `manifest.json` absent | Assets are incomplete; T11 serves a fallback error page and logs the event |
  | `asset_version` ≠ `fw_version` | Version mismatch; T11 serves pages but shows a warning on the dashboard and logs the event |
  | `asset_version` == `fw_version` | Normal operation |

- Because firmware and web assets are always activated together as a pair, a version mismatch after a clean update is not expected. A mismatch indicates either a partial update or a manual intervention.

**Combined firmware + web file update:**
- When a release includes both firmware and web asset changes, both must be written to their respective inactive partitions and verified before either is activated.
- T13 performs the firmware write and the web asset zip extraction in sequence. The firmware bank switch (and paired LittleFS activation) happens only after both have completed and `manifest.json` has been written.

---

### 5.10 NVS Configuration Storage Layout

**Managed by:** T4 (Data Manager)

NVS uses ESP-IDF namespaces to separate configuration domains. All keys use UTF-8 strings of ≤ 15 characters (ESP-IDF NVS limit).

Setpoint and threshold values (temperature, humidity, wind speed, wind direction, dwell durations) are stored as **`int16_t`** (signed 16-bit integer). Fractional values are not stored; the UI and sensor reading pipeline round to the nearest integer before writing to NVS.

**Factory-default constants in `app_types.h`:**
Motor full-travel time defaults (`MOTOR_M1_TRAVEL_S_DEFAULT 21`, `MOTOR_M2_TRAVEL_S_DEFAULT 21`, `MOTOR_M3_TRAVEL_S_DEFAULT 171`, unit: seconds) are defined in `firmware/src/types/app_types.h`. They are written to NVS on first boot and after factory reset. At runtime T2 reads the live travel times from T4 (MX4) — i.e. the current NVS `motor/travel_mN` values — and converts to milliseconds for `vTaskDelay`. Technicians can adjust travel times via the web GUI (FR-CF05). See §5.2 for the travel timer / dwell timer distinction.

| Namespace | Key examples | Type | Description |
|-----------|-------------|------|-------------|
| `climate` | `t_min_day`, `t_max_day`, `t_min_ngt`, `t_max_ngt`, `rh_min_day`, `rh_max_day`, `rh_min_ngt`, `rh_max_ngt`, `hyst_t`, `hyst_rh`, `rh_ctrl_en`, `cr_priority`, `avg_win_t`, `avg_win_rh` | `int16_t` / `uint8_t` | Day and night setpoints for temperature (°C) and humidity (%) (integers); hysteresis bands (integers); `rh_ctrl_en`: humidity control enable (0 = disabled, 1 = enabled, default 1); `cr_priority`: conflict resolution (0 = temperature first [default], 1 = humidity first, 2 = deviation-based); `avg_win_t` / `avg_win_rh`: sliding average window in minutes for T and RH (1–60, default 1) |
| `wind` | `v_max`, `dir_excl_low`, `dir_excl_high`, `wind_prot_en` | `int16_t` / `uint8_t` | Wind speed threshold (m/s or Beaufort) and direction exclusion zone (degrees) (integers); `wind_prot_en`: wind protection enable flag (0 = disabled, 1 = enabled, default 1) |
| `motor` | `travel_m1`, `travel_m2`, `travel_m3`, `dwell_open_m1`, `dwell_open_m2`, `dwell_open_m3`, `dwell_close_m1`, `dwell_close_m2`, `dwell_close_m3` | `int16_t` | **Travel times** (`travel_mN`, seconds, range 5–300): how long T2 energises the relay to move a window from one end-stop to the other. Read by T2 from T4 (MX4); converted to ms for `vTaskDelay`. Defaults: M1=21, M2=21, M3=171 (`MOTOR_MN_TRAVEL_S_DEFAULT` in `firmware/config/cfg_defaults.h`). Bounds enforced by `cfg_clamp()` from `firmware/config/cfg_limits.h::CFG_{MIN,MAX}_TRAVEL_S`. Configurable by technician via web GUI (FR-CF05, admin level). **Dwell times** (`dwell_open_mN` / `dwell_close_mN`, minutes): minimum hold period T2 enforces after travel completes before accepting the next command on that channel. `dwell_open_mN`: min hold at `OPEN` before CLOSE accepted. `dwell_close_mN`: min hold at `CLOSED` before OPEN accepted. Dwell timer starts when the travel timer expires (FR-A09–FR-A12). Default: 0 (no hold enforced). Configurable by technician via web GUI only (FR-CF10, FR-CF11). |
| `access` | `pin_salt` (blob[16]), `pin_farmer_hash` (blob[32]), `pin_admin_hash` (blob[32]), `fail_cnt_f`, `fail_cnt_a`, `lockout_f`, `lockout_a`, `lockout_max`, `lockout_secs` | blob / int32 | `pin_salt`: 16-byte random salt, generated once at first boot. `pin_farmer_hash` / `pin_admin_hash`: SHA-256(salt \|\| pin_ascii) digest. `fail_cnt_f` / `fail_cnt_a`: per-role consecutive failure count. `lockout_f` / `lockout_a`: per-role lockout expiry as Unix timestamp (0 = not locked). `lockout_max`: threshold before lockout (default 5). `lockout_secs`: lockout duration (default 300 s). |
| `wifi` | `ssid`, `psk_hash`, `ap_enable`, `ap_psk` | string / int32 | WiFi client and AP credentials. STA: `ssid` and `psk_hash` (salted SHA-256 of PSK). DHCP is the only IP-acquisition mode supported (static-IP keys are not defined). AP: `ap_enable` (int32, 0/1, **default 0** — admin must explicitly opt in) toggles soft-AP at runtime; AP SSID is auto-generated from the MAC address and not stored; `ap_psk` is stored as **plaintext** (WPA2 requires the raw key), default `"0123456789"`, configurable by admin via web interface. The AP auto-shutdown timer is stored in the `system` namespace as `ap_timeout` (see below), not here. |
| `mqtt` | *(reserved — no keys defined in the end-state design)* | — | The `mqtt` namespace is reserved for a future Could-be MQTT integration (T12). It is not provisioned with keys and shall not be written by the current firmware. |
| `status` | `url`, `secret`, `interval_s`, `expose_mask`, `log_up_hhmm` | string / uint32 / uint8 / uint16 | Status website POST configuration (T14). `url` (string ≤127 chars): full HTTPS URL. `secret` (string ≤63 chars): shared secret transmitted in the `sourceidentifier` request header; masked in `/api/config` GET responses. `interval_s` (uint32, default 600, range 60–86400; `0` disables outbound POST). `expose_mask` (uint8): bitmask selecting which top-level objects of the canonical JSON are included in the POST (1=sensors, 2=modes, 4=alarms, 8=setpoints, 16=network, 32=sys; default 0x3F = all). `log_up_hhmm` (uint16, HH×100+MM, e.g. 0835; daily SD log-upload trigger time). |
| `system` | `poll_interval`, `session_timeout`, `ap_timeout`, `lang`, `schema_ver`, `fw_version`, `led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`, `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac`, `tz_str` | int32 / string / uint8 / int16 | System-wide configuration; `poll_interval` (int32, seconds, default 30, technician-settable 15–120 via web GUI); `session_timeout` (int32, minutes); `ap_timeout` (int32, minutes, default 30, range `[0, CFG_MAX_TIMEOUT_MIN]`; `0` = stay up indefinitely) — flat AP-uptime timer enforced by T10's `poll_ap()` (§5.6); `lat_deg` + `lat_frac` / `lon_deg` + `lon_frac`: geographic location stored as integer degree and fractional milli-degree parts (e.g. 52.0907°N stored as lat_deg=52, lat_frac=907) for sunrise/sunset calculation; populated manually via web GUI (FR-CF16) or automatically by `do_geo_sync()` (FR-DN06); `tz_str` (string[64]): POSIX TZ string e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"`, factory default `"CET-1CEST,M3.5.0,M10.5.0/3"`, applied at boot via `setenv/tzset` and on each geolocation update (FR-DN07, FR-CF18); `schema_ver` (int32) tracks NVS layout version; `fw_version` (string `"MAJOR.MINOR.PATCH"`) overwritten on every boot; `led_day_brt` / `led_nite_brt` (uint8, 0–255, defaults 200/20); `led_nite_from` / `led_nite_to` (uint8, hour 0–23, defaults 22/6) |

**Default values:**
- Applied on first boot (no NVS key present) or after factory reset.
- Factory reset clears all NVS namespaces and restores defaults; requires deliberate admin action and is logged.

**Schema versioning and firmware update behaviour:**

A `schema_ver` integer is stored in the `system` namespace. `nvs_cfg_init()` compares it to the compile-time `NVS_SCHEMA_VERSION` on every boot.

| Boot condition | Behaviour | Return value |
|----------------|-----------|--------------|
| First boot / blank flash | Writes `system/schema_ver`; writes `system/fw_version` | `NVS_CFG_OK` |
| Stored version matches firmware | Overwrites `system/fw_version` with current version | `NVS_CFG_OK` |
| Stored version differs from firmware | Writes new `system/schema_ver`; overwrites `system/fw_version`; **namespaces are not erased** | `NVS_CFG_ERR_MIGRATION` |

`system/fw_version` is a `"MAJOR.MINOR.PATCH"` string that is overwritten on **every** boot. It always reflects the currently running firmware and can be read by T11 (web dashboard) and T9 (event logger) without parsing the binary image.

Migration strategy across firmware updates:

| Situation | Outcome |
|-----------|---------|
| Key exists in NVS and is still used | Read as-is — **user setting is preserved** |
| Key absent in NVS (new setting in this firmware) | First `_or_default` call writes the factory default |
| Key exists in NVS but is no longer used | Never read — orphaned; no functional impact |

The `log` namespace is never touched by schema migration. Event history is always preserved.

**Type-change exception:** If a key's storage type changes between firmware versions, ESP-IDF returns `ESP_ERR_NVS_TYPE_MISMATCH`. That individual key must be erased and rewritten explicitly by T4 on startup; a blanket namespace erase is not used.

T4 checks the return of `nvs_cfg_init()`. If `NVS_CFG_ERR_MIGRATION` is returned it logs the event via Q3 and continues normal startup.

---

### 5.11 Watchdog and Fault Handling

**Implemented by:** T1 (Watchdog/Heartbeat) and T2 (Relay Controller)

**Task watchdog:**
- The ESP-IDF Task Watchdog Timer (TWDT) is enabled at boot via `esp_task_wdt_init()` with a 30 s timeout and `panic_on_timeout = true`.
- Every long-running task (T1, T2, T3, T4, T5, T6, T8, T9, T10, T11, T14) registers itself with the TWDT via `esp_task_wdt_add(NULL)` at task entry and calls `esp_task_wdt_reset()` on every loop iteration. T1's iteration is the most frequent at 500 ms; the slower tasks (T9, T10) reset on their own cadence well inside the 30 s window.
- If any subscribed task fails to reset within the timeout, the TWDT fires a panic, which is captured by the coredump backend (see below) and triggers an `ESP_RST_TASK_WDT` reset.

**T1 instrumentation duties:**
- T1 is the sole task that runs `esp_task_wdt_reset()` at a high cadence (500 ms) and also drives the RGB LED state machine (§5.12).
- T1 emits periodic instrumentation events to Q3:
  - Per-tick: nothing (silent).
  - **Per 60 s:** `LOG_SYSTEM` with `value_a = uxTaskGetStackHighWaterMark()` for T1's own stack, and a free-heap sample (`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`).
  - **Per 10 min:** a fuller heap snapshot (free + largest-free-block) plus a per-task high-water-mark dump for the long-running tasks.
  - **OTA-healthy mark:** after `OTA_HEALTHY_MS` (default 60 s) of stable uptime in a freshly booted firmware bank, T1 calls `esp_ota_mark_app_valid_cancel_rollback()` so the OTA framework treats this boot as successful. This is the application-side participation in the rollback-on-three-failures policy (§5.9).

**Coredump capture:**
- A dedicated 64 KB `coredump` partition (label `coredump`, subtype `coredump`) is included in the partition table (§5.9).
- The ESP-IDF coredump backend (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, ELF format) is enabled in `sdkconfig.defaults`.
- On panic or task-watchdog firing the backend writes the full core image to the partition before the reset.
- On the next boot T1 checks for a stored coredump via `esp_core_dump_image_check()`; if present it emits a `LOG_SYSTEM coredump_present value_a=<size>` event so the operator can pull the image off via `idf.py coredump-info` / `coredump-debug` and clear the partition.

**Restart sequence on reset:**
- On every boot the firmware detects the reset reason via `esp_reset_reason()` and emits one `LOG_SYSTEM boot_reason` event with the reason code in `value_a`.
- Controlled restart: T2 closes all relay outputs immediately (CLOSE_ALL on all channels) to re-synchronise the estimated window position (FR-ST02); EG1.CALIBRATING is set for the duration of the boot CLOSE_ALL sequence.
- If 3 consecutive resets occur without T1 reaching the OTA-healthy mark, the ESP-IDF OTA rollback restores the previous firmware bank automatically (§5.9).

**Sensor fault handling:**
- When EG1.SENSOR_FAULT_T is set: T6 inhibits climate control commands; the last known window state is maintained; LCD displays fault indication (FR-S05).
- When EG1.SENSOR_FAULT_W is set: T3 treats wind as exceeding all thresholds (safe-fail: CLOSE_ALL) (FR-W04).
- Faults clear automatically when T5 receives a valid reading.

**Motor alarm handling (FR-MA01–FR-MA08):**
- T2 detects the RRK-3 alarm signal on GPIO42 and immediately de-energises all relays, sets EG1.MOTOR_ALARM, and logs the alarm onset.
- While MOTOR_ALARM is set: T2 discards all incoming Q1 commands; T6 and T3 are inhibited from issuing any window commands.
- LCD displays "MOTOR ALARM — CONTROL SUSPENDED" (FR-MA05); RGB LED shows Red (FR-UI19).
- When the alarm clears: T2 clears EG1.MOTOR_ALARM and logs alarm clearance immediately; then observes a **60 s guard** (motor coast-down) before starting CLOSE_ALL re-calibration; resumes Automatic mode after re-calibration (FR-MA07).

---

### 5.12 System Status RGB LED

**Implemented by:** T1 (Watchdog / Heartbeat)

**Hardware:**
- WS2812B single-wire addressable LED integrated on the **LOLIN S3** module at **GPIO 38** (`LED_BUILTIN`).
- Driven directly by the ESP-IDF RMT driver (`driver/rmt_tx`): one channel, 24-bit GRB frame per refresh, no external library dependency. The RMT-encoded waveform is generated in T1's local memory and transmitted via `rmt_transmit()`.
- LED is mounted on the MCU board inside the MC001110 enclosure; it is visible through the transparent cover (FR-UI20).

**Colour convention and state priority:**

| Priority | Colour | Hex | Condition | FRS |
|----------|--------|-----|-----------|-----|
| Highest | **Red** | `#FF0000` | Critical alarm — `EG1.MOTOR_ALARM` active (RRK-3 emergency stop; all window control suspended), OR system halted (3 consecutive watchdog resets without startup completion) | FR-UI19 |
| High | **Blue** | `#0000FF` | `EG1.CALIBRATING` active — boot CLOSE_ALL sequence in progress; window positions transitioning from `UNKNOWN` to `CLOSED`. Shown after MOTOR_ALARM in priority because operators need to distinguish "controller is moving relays right now to re-establish position" from a steady warning. | FR-UI16 |
| Middle | **Amber** | `#FF8000` | Non-critical alarm or warning: `EG1.SENSOR_FAULT_T`, `EG1.SENSOR_FAULT_W`, `EG1.WIND_OVERRIDE`, wind protection disabled (`wind_prot_en == false`), or humidity control disabled (`rh_ctrl_en == false`) | FR-UI18 |
| Lowest | **Green** | `#00FF00` | All above conditions false; system operating normally | FR-UI17 |

If multiple conditions apply simultaneously, the highest-priority colour is shown (FR-UI16).

**State evaluation logic (executed in T1 on each watchdog kick):**

```
if (halt_flag OR EG1.MOTOR_ALARM):
    colour ← RED

else if (EG1.CALIBRATING):
    colour ← BLUE

else if (EG1.SENSOR_FAULT_T OR EG1.SENSOR_FAULT_W
         OR EG1.WIND_OVERRIDE
         OR (rh_ctrl_en == false) OR (wind_prot_en == false)):
    colour ← AMBER

else:
    colour ← GREEN
```

`halt_flag` is set after 3 consecutive watchdog resets without completing the startup health check; it can also be set by any future mechanism that places the system in a fully halted state.

**Day/night brightness dimming (Should — FR-UI21, FR-CF14):**

| Condition | Brightness applied |
|-----------|-------------------|
| Current local hour ∈ (`led_nite_from`, `led_nite_to`) — wrapping midnight | `led_nite_brt` (default 20 / 255) |
| All other hours | `led_day_brt` (default 200 / 255) |

- Brightness is applied to the currently active colour by scaling each of the 8-bit GRB components in T1's RMT encoder before the frame is transmitted.
- T1 reads the four NVS settings (`led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`) from T4 via MX4; values are cached in T1 local variables and refreshed on each tick to pick up any runtime configuration change.
- Current local time is read from the ESP32 system clock (maintained by T4 after DS1307 read and NTP sync); no additional mutex is required for a `time()` / `localtime()` call on ESP32.

**NVS keys (all in `system` namespace):**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `led_day_brt` | `uint8_t` | 200 | RGB LED brightness during daytime (0–255) |
| `led_nite_brt` | `uint8_t` | 20 | RGB LED brightness during night hours (0–255) |
| `led_nite_from` | `uint8_t` | 22 | Night period start hour, local time (0–23) |
| `led_nite_to` | `uint8_t` | 6 | Night period end hour, local time (0–23) |

These four keys are part of the "Should" feature set (FR-UI21, FR-CF14); they default to sensible values on first boot and can be changed by the administrator via the keypad menu or web interface.

---

### 5.13 Status Website POST (T14)

**Responsibility.** T14 is the sole producer of outbound HTTPS POSTs to the configured external status website. It serves two distinct traffic types over one persistent client:

- `POST <cfg.status_url>` — periodic status snapshot every `cfg.status_interval_s` (default 600 s; range 60–86400 s; `0` disables outbound POST entirely).
- `POST <cfg.status_url>` (Content-Type `text/csv`) — SD log-file upload at the configured daily slot `cfg.log_upload_hhmm`, plus a rotation-triggered drain when T9 closes a log file.

**HTTPS transport.** T14 uses `esp_http_client` configured for `HTTP_TRANSPORT_OVER_SSL`. One handle is created at task entry with `keep_alive_enable = true`, `buffer_size = 1024`, `buffer_size_tx = 1024`, `skip_cert_common_name_check = true`, and `event_handler = http_event_cb`. The handle persists for the lifetime of the task; subsequent calls to `esp_http_client_perform()` reuse the keep-alive TCP socket and (where the server cooperates) the mbedTLS session ticket emitted by the previous handshake.

The remote server's HTTPS enforcement and certificate-rotation policy are the operator's responsibility (TR-NW04 reference). The controller does not pin a server certificate; transport authenticity is delegated to the system-level CA bundle compiled in via `crt_bundle_attach`, and authenticity at the application layer is provided by the `sourceidentifier` shared-secret header carried inside the TLS-encrypted body.

**Request shape:**

```
POST /<status_url_path> HTTP/1.1
Host: <status_url_host>
User-Agent: greenhouse-controller/<FIRMWARE_VERSION>
Content-Type: application/json
sourceidentifier: <cfg.status_secret>
Content-Length: <n>

<canonical JSON body>
```

For log uploads, `Content-Type` is `text/csv`, `Content-Length` is the file size on disk, and the body is the raw CSV file streamed chunk-by-chunk.

**Canonical JSON builder.** All three status channels (`/api/status`, `/ws`, T14 POST) call the same `build_canonical_status_json()` helper. The helper takes a `STATUS_EXPOSE_*` mask plus an `include_disabled_setpoints` boolean. The mask selects which top-level JSON objects appear in the payload (sensors / modes / alarms / setpoints / network / sys). T11's local endpoints pass `STATUS_EXPOSE_ALL`; T14 passes `cfg.status_expose_mask`. Because all three channels run through the same builder, the payload byte-shapes are identical for any given mask choice — the dashboard's `handleStatus()` does not need to branch on source.

**Timeouts.** `esp_http_client_set_timeout_ms()` is set to 5000 ms for the status POST (small body) and 30000 ms for log uploads (large body). DNS resolution and TLS handshake share the same budget. If a cycle elapses without a completed POST, the cycle is recorded as failed and the breaker counter advances.

**Streaming log upload.** Each upload begins with `SDFileChunkedStream` opening the target `.csv` file and a `LOG_UPLOAD_CHUNK_BYTES + 1u` heap buffer (the `+1u` reserves space for the chunked reader to null-terminate without overrunning). The transmit loop is:

```
esp_http_client_open(client, fsize);
while ((n = sd_stream_read(buf, LOG_UPLOAD_CHUNK_BYTES)) > 0) {
    esp_http_client_write(client, buf, n);
}
esp_http_client_fetch_headers(client);
```

Peak heap during a 5 MB upload is bounded by the chunk size (4 KB) plus the mbedTLS record buffer (~17 KB), independent of file size.

**Multi-file drain.** Each trigger (daily slot or T9 rotation) causes T14 to walk `/sdcard/log/` once in lexicographic order and upload every eligible file in series. Successful uploads result in `f_unlink()` (after `f_sync()`); failed uploads leave the file in place for the next trigger. A dedup latch prevents concurrent triggers from interleaving uploads.

**Status-expose bitmask.** `cfg.status_expose_mask` is a `uint8_t` with bit positions:

| Bit | Name | Object included when set |
|-----|------|--------------------------|
| 0 | SENSORS | `sensors:{t_c10, t_avg_c10, rh, wind_kmh, wind_dir_deg, …}` |
| 1 | MODES | `modes:{op_mode, m1, m2, m3, …}` |
| 2 | ALARMS | `alarms:{motor, sensor_t, sensor_w, wind_override, calibrating}` |
| 3 | SETPOINTS | `setpoints:{t_min_day, t_max_day, …, rh_min_day, …}` |
| 4 | NETWORK | `network:{wifi_ip, ap_active, ntp_synced}` |
| 5 | SYS | `sys:{fw_ver, asset_version, free_heap, uptime_s, reset_reason}` |
| 6–7 | *(reserved)* | — |

Default `0x3F` exposes every object. Operators may narrow the payload on bandwidth-constrained links by clearing bits.

**Public state-getters** (lock-free reads of primitive types; no mutex required):

| Function | Returns | Used by |
|---|---|---|
| `status_post_last_str(buf, cap)` | "OK 2026-05-10 14:30:22" or "" | `/api/status` builder |
| `status_post_last_log_str(buf, cap)` | same shape, for the log upload | `/api/status` builder |
| `status_post_backoff_active()` | `true` if the breaker is open | `status_json.cpp` and `ui_display.cpp` (LCD page 3 `BK` badge) |
| `status_post_heartbeat()` | monotonic uint32 incremented at the top of every loop iteration | Reserved for T15 (dormant) |
| `status_post_heap_drop_bytes()` | saturating uint32 of cumulative free-heap drop measured around every HTTPS call (logged only) | Reserved for T15 (dormant) |
| `status_post_force_teardown()` | idempotent close of the `esp_http_client` handle | Reserved for T15 (dormant) |

### 5.14 Persistent Circuit Breaker (T14 internal — deferred)

**Status: deferred.** The persistent NVS-backed circuit breaker that escalates outbound-POST hold times in the face of repeated failures is documented here for completeness but is **not active** in the end-state design captured in this specification. The motivation for the breaker was to limit retry energy during long external-network outages; with the ESP-IDF HTTPS stack's keep-alive plus bounded mbedTLS buffers, the per-cycle cost of a failed attempt is small enough that a simple linear retry has been deemed sufficient.

**Intended design (deferred — for future re-enablement):**
- Two independent breakers within T14: one for the periodic status POST, one for the SD-log upload, each tracking consecutive successes and failures.
- Escalation schedule: `{0, 60, 300, 1800, 3600}` seconds of hold time. Three consecutive failures advance the breaker one step; five consecutive successes regress it one step. Maximum hold = 1 hour.
- Persistence of the breaker phase and the next-reopen Unix timestamp in NVS so a reboot during an outage does not reset accumulated state.
- Pre-NTP guard: the breaker cannot advance until the system clock has been synchronised at least once, because the next-reopen comparison requires a valid wall-clock.

**Current behaviour.** T14 retries on the natural cycle (`cfg.status_interval_s` for the status POST; next trigger for the log upload). Failure counts are surfaced as `LOG_NET` events but are not used to throttle future attempts.

### 5.15 Bulkhead Policy and Status-POST Supervisor (T15 — dormant)

**Status: dormant.** T15's source file (`firmware/src/status_post_supervisor/status_post_supervisor.cpp`) is preserved in the tree as a structural placeholder, but no `xTaskCreate()` call activates it in the end-state build. The bulkhead policy is captured here as the design that would be re-enabled if a future regression re-introduces a per-cycle heap-drop pattern.

**Design intent (deferred — for future re-enablement).** Provide a structural guarantee that secondary-network failures (T14 hang, TLS-stack memory leak, lwIP socket-leak) cannot disrupt the climate-critical primary loop. The supervisor would be small, lock-free, and isolated from T14's address-space contents: observing via the public getters in §5.13 and acting via the public setters there.

**Intended detection signals:**

| Signal | Threshold | Outcome |
|---|---|---|
| `status_post_heartbeat()` not advancing for 60 s | wedge timeout | Respawn T14 |
| `status_post_heap_drop_bytes()` ≥ 64 KB | heap-drop limit | Planned reboot |
| > 1 respawn in 5 minutes | min respawn gap | Planned reboot |
| > 10 respawns in current hour | hourly respawn limit | Planned reboot |

**Intended respawn sequence (deferred):**

1. Confirm the respawn budgets above are not exhausted (else: planned reboot).
2. Call `status_post_force_teardown()` — idempotent close of the persistent `esp_http_client` handle. The handle survives `vTaskDelete` (it lives in BSS, not on T14's task stack); without this explicit close the next incarnation would inherit a half-closed socket pointing at lwIP state the killed task never released.
3. `vTaskDelete(task_t14)`.
4. Short delay for FreeRTOS's idle task to reclaim the deleted task's stack + TCB.
5. `xTaskCreate(task_status_post, "T14_WEB", 12288, NULL, TASK_PRIO_LOW, &task_t14)` — recreate.

**Intended planned-reboot sequence (deferred):**

1. Set a planned-reboot flag in NVS so the next boot's reset-reason log can disambiguate this from a panic.
2. Short delay for NVS commit and log buffer flush.
3. Invoke the same `reboot_worker_task` carve-off used by T13 (§4.3 T13). `esp_reset_reason()` on the next boot reports `ESP_RST_SW (3)`, distinguishable from `ESP_RST_PANIC (4)` and `ESP_RST_INT_WDT (5)`.
4. Clear the planned-reboot flag once T14 completes one successful POST in the new boot.

**Watchdog discipline.** Were T15 active, it would subscribe to the task watchdog at entry and break its 30 s polling interval into 1 s chunks each preceded by `esp_task_wdt_reset()`. This prevents the supervisor itself from starving the WDT while waiting on a slow signal.

**Cross-references for the related primary-side state (active in the current build):**

- Per-channel persisted window state (`t2_st_ch0/1/2` in `motor`) — written by T2 on every transition to a terminal state (`CH_CLOSED`/`CH_OPEN`); written to `NVS_STATE_UNKNOWN` before every relay-energise. At boot, if all three channels are `CH_CLOSED` and the GPIO42 motor-alarm pin is not asserted, `calib_close_all()` is skipped (saves up to 171 s on the planned-reboot recovery path). Logged as `LOG_SYSTEM, value_a=10`. Implements FR-BK05.
- T1's periodic heap probe emits `LOG_SYSTEM value_a=7` (free internal KB), `value_a=8` (free PSRAM KB), and `value_a=12` (largest contiguous internal block, KB). A widening gap between `value_a=7` and `value_a=12` is the diagnostic signature of heap fragmentation under repeated TLS handshakes.

**Operator-visible surfaces (active in the current build, regardless of T15 status):**

- LCD page 3 (Network) row 1: when `status_post_backoff_active()` returns true, the row reads `WiFi: conn    BK` instead of `WiFi: connected `. The 4-char slack of the original layout absorbs the badge without truncating the IP address on row 2.
- Status JSON: `mode.flags[]` array includes the string literal `"net_backoff_active"` while the breaker is open. The local web GUI renders it as a yellow warning badge on the Status tab's Alarms card.
- Event log: breaker phase transitions write a `LOG_SYSTEM` row with sub-code in `value_b` — see `event_logger.h` for the sub-code table.

**Known structural limitation.** Hard faults *inside* ESP-IDF / mbedTLS / lwIP cannot be intercepted from the application layer on this single-chip architecture. The dormant bulkhead design, were it activated, would make such faults *bounded* (the breaker throttles the trigger rate; the supervisor ensures the recovery is a 2-second blip, not a 171-second outage). Eliminating the faults themselves would require hardware separation or a co-processor — explicitly out of scope.

---

For the live list of design and integration issues see `firmware/issues.md` and the GitHub issue tracker. This specification is the end-state design; resolved-and-ratified issues are reflected directly in the body of the document, and open issues are not duplicated here.

---

*End of document.*
