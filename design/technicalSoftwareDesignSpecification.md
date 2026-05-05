# Technical Software Design Specification
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Technical Software Design Specification        |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.2 (draft)                                   |
| Date         | 2026-05-05                                    |
| Status       | Draft                                         |
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
   - 5.7 WiFi — Client Mode (Optional)
   - 5.8 Web Interface
   - 5.9 OTA Firmware Update
   - 5.10 NVS Configuration Storage Layout
   - 5.11 Watchdog and Fault Handling
   - 5.12 System Status RGB LED
6. [Open Issues](#6-open-issues)

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

Control logic modules (climate control, wind safety, conflict resolution, window state machine) shall be decoupled from hardware drivers to enable host-side unit testing via PlatformIO's test runner (TR-SW05). Hardware-dependent drivers (Modbus, relay GPIO, I2C) are abstracted behind interfaces so that logic modules can be tested on a host without target hardware.

### 2.6 Security

- WiFi connections are protected with WPA2 minimum; WPA3 preferred if supported by the ESP32-S3 SDK (TR-NW01).
- HTTPS on the web interface is **not implemented**. TLS termination on the ESP32-S3 is not feasible given the available RAM and CPU headroom. The threat model for TR-NW04 has been assessed and accepted — see §6 Open Issue #4 (TR-NW04).
- User credentials are stored as salted SHA-256 hashes (`SHA-256(salt || pin_ascii)`, mbedTLS); plain-text storage is not permitted (FR-AC06).

---

## 3. Design Constraints from FRS

The following items originate from system-level and functional requirements in the FRS. Each represents a constraint or implementation decision that the software design must satisfy. Source requirement IDs are noted for traceability.

**User interface**
- Menu depth: max 4 key presses from the main screen to any first-level setting (FR-UI07).

**Credential storage**
- User credentials stored as salted SHA-256 hashes (`SHA-256(salt || pin_ascii)`, mbedTLS); plain-text storage not permitted (FR-AC06).
- Configurable login lockout after a set number of failed attempts (FR-AC07).

**Event log**
- Minimum 250 entries retained in persistent storage using a ring buffer (FR-LG06: worst-case 216 events/hour at 30 s poll + headroom); SD card preferred when present, internal flash as fallback (FR-LG07, FR-LG08).

**Settings persistence**
- All configuration settings stored in ESP32-S3 NVS flash partition; retained across power cycles and restarts (FR-CF06, TR-SW01).

**Timekeeping and timezone**
- Time source: **DS1307 RTC** with CR2032 backup fitted on PCB (THDS Open Issue #7 resolved; see THDS §4.6). DS1307 is the authoritative clock when WiFi is unavailable. TR-HW08 is satisfied.
- When WiFi is available: synchronise system time via NTP on boot; on NTP success, T10 calls `do_geo_sync()` to auto-detect timezone via ip-api.com (FR-DN07); POSIX TZ string applied immediately via `setenv/tzset`; persisted to NVS `system/tz_str`. See §4.3 T10 and Open Issue #3.
- When WiFi is unavailable: DS1307 is authoritative; no timestamp gap on power interruption; TZ string from last successful geolocation (or factory default `CET-1CEST,M3.5.0,M10.5.0/3`) applied from NVS at boot.
- Administrator may manually set date/time via the LCD keyboard (FR-UI23) — see §5.5 and `dm_set_manual_time()` in §4.3 T4.

**Firmware update**
- Firmware updates supported without opening the enclosure: OTA over WiFi and via native USB (TR-SW02, TR-IF05).

**Fault recovery**
- Hardware watchdog timer automatically resets the MCU on a software hang; controlled restart sequence re-synchronises window states on recovery (TR-SW03).

**Testability**
- Control logic modules decoupled from hardware drivers for host-side unit testing via PlatformIO test runner (TR-SW05).

**WiFi security**
- WPA2 minimum; WPA3 preferred (TR-NW01).
- HTTPS on the web interface is **not implemented**; see §6 Open Issue #4 for the accepted threat model (TR-NW04).

**Setpoint and threshold data types**
- All user-configurable setpoints and thresholds are stored and processed as **integers** (no fractional part): T_min, T_max (°C), RH_min, RH_max (%), v_max (m/s or Beaufort), wind direction exclusion centre and half-width (degrees), hysteresis bands, and dwell/timer durations (minutes). Fractional sensor readings are rounded to the nearest integer before comparison with setpoints. NVS keys for these parameters use `int16_t` (signed 16-bit integer). (FRS C11, FR-CF01–FR-CF11)

**Feature enable/disable flags**
- Temperature-based climate control is permanently active; no enable/disable flag is stored or checked.
- Humidity-based climate control has a farmer-configurable enable/disable flag (`rh_ctrl_en`, boolean, NVS namespace `climate`). When disabled, T6 skips RH evaluation entirely; conflict resolution (FR-CR01) is also suppressed. (FRS C12, FR-C12, FR-CF12)
- Wind protection has a farmer- and administrator-configurable enable/disable flag (`wind_prot_en`, boolean, NVS namespace `wind`). When disabled, T3 reads wind data but issues no CLOSE_ALL or RESUME commands; the WIND_OVERRIDE event group bit is never set. The LCD warning must remain visible while the flag is false (FR-WS10). (FRS C12, FR-WS09, FR-CF13)
- Both flags default to **enabled** (`true`) on first boot and after factory reset.
- Changes to either flag are logged with timestamp and the operator's identity (FR-WS11).

**Motor alarm detection**
- The RRK-3 alarm relay (dry contact, closes on alarm) is wired to J10; the opto-isolated input drives GPIO 42 configured as INPUT_PULLUP. The opto-coupler output is **active-low**: contact closed (alarm active) → GPIO 42 **LOW**; contact open (no alarm) → GPIO 42 **HIGH**. The alarm fires when any motor fails to stop at its normal end-switch and reaches the emergency switch. Resolved — see Open Issue #1 (Closed). T2 uses a deferred-ISR pattern: `attachInterrupt(PIN_OPTO_INPUT, isr_handler, CHANGE)`; the ISR (`IRAM_ATTR`) records the first edge (volatile flag + tick timestamp) and returns immediately; T2 confirms after 75 ms by reading the live pin state. **Not suppressed during MOVING states** — a motor hitting the emergency switch during a T2-commanded move is the primary alarm scenario. On alarm assert confirmed: T2 immediately de-energises all 6 relays, sets EG1.MOTOR_ALARM, posts log event to Q3 (FR-MA01–FR-MA02). On alarm release confirmed: T2 clears EG1.MOTOR_ALARM, posts log event to Q3, waits a **60 s guard** (motor coast-down; relays remain de-energised), re-checks pin; if still clear, starts CLOSE_ALL re-calibration, then resumes AUTOMATIC (FR-MA06–FR-MA07). T2 checks EG1.MOTOR_ALARM before executing any Q1 command and discards the command if the alarm is active.

**Mutual exclusion of relay commands**
- The firmware must never energise the OPEN and CLOSE relay of the same motor simultaneously. T2 (Relay Controller) is the sole owner of relay GPIO and enforces this constraint before asserting any relay (see §4.3).

---

## 4. Firmware Architecture

### 4.1 Framework Selection

The firmware uses the **Arduino framework** over ESP-IDF via PlatformIO (target board: `lolin_s3`). The Arduino framework provides a familiar API and extensive library ecosystem (Modbus, I2C, MQTT, web server) while PlatformIO manages the underlying ESP-IDF toolchain, flash partitioning, and OTA support. FreeRTOS is available through the ESP-IDF layer and is used directly for task management.

### 4.2 FreeRTOS Task Overview

The firmware is structured as a set of FreeRTOS tasks. Each logical function is assigned a dedicated task with a defined priority, core assignment, and communication interface. `tasks.md` is the authoritative reference for the task architecture; this section summarises the design.

| ID  | Task Name            | Priority          | Core | Function |
|-----|----------------------|-------------------|------|----------|
| T1  | Watchdog / Heartbeat | Highest           | 1    | Hardware watchdog kick; HB LED toggling; RGB status LED update |
| T2  | Relay Controller     | High              | 1    | Relay GPIO; window state machines; dwell timers; mutual exclusion; RRK-3 motor alarm detection |
| T3  | Safety Monitor       | High              | 1    | Wind safety evaluation; issues CLOSE_ALL; overrides climate control |
| T4  | Data Manager         | Medium-high       | 1    | Central store for all configuration settings and measurement data; ring buffers for sensor history |
| T5  | Sensor Poll          | Medium-high       | 1    | Modbus RTU master; polls sensors; posts readings to T4 |
| T6  | Climate Control      | Medium            | 1    | Evaluates setpoints; conflict resolution; posts actuation commands to T2 |
| T7  | Keypad Scan          | Medium-high       | 1    | Matrix scan; debounce; posts key events to T8 |
| T8  | UI / Display         | Medium            | 1    | LCD rendering; menu FSM; session management; posts config changes to T4 |
| T9  | Event Logger         | Low               | 1    | Serialises log writes to NVS ring buffer and SD card |
| T10 | Network Manager      | Low               | 0    | WiFi AP and client lifecycle; NTP trigger; posts status to T8 |
| T11 | Web Server           | Low               | 0    | Serves configuration pages from LittleFS; applies session model; posts config changes to T4 |
| T12 | MQTT Client          | Low               | 0    | Publishes sensor data and status; subscribes to command topics |
| T13 | OTA                  | Low (on demand)   | 0    | Firmware and LittleFS update; manages dual-bank rollback |

### 4.3 Task Descriptions

#### T1 — Watchdog / Heartbeat

**Priority:** Highest | **Core:** 1

- Kicks the hardware watchdog timer at a fixed interval (e.g. every 500 ms).
- Toggles the HB LED: 1 Hz in normal operation; 4 Hz during startup / initialisation.
- Drives the WS2812B RGB status LED (GPIO 38) on each watchdog kick: reads EG1 to determine the current system state, maps it to Green / Amber / Red (see §5.12), and writes the colour with the appropriate brightness (day or night level). Event group reads are lock-free and impose no additional synchronisation cost.
- Must never be starved by lower-priority tasks; its liveness confirms the whole system is running.
- Can be implemented as a FreeRTOS software timer callback rather than a full task.
- **Synchronization:** reads EG1 (all bits — lock-free) for RGB LED colour; acquires MX4 to read LED brightness and night-schedule settings from T4 (cached in local variables; refreshed on each tick or on config update).

---

#### T2 — Relay Controller

**Priority:** High | **Core:** 1

- Sole owner of all 6 relay GPIO output pins (OPEN/CLOSE for M1, M2, M3); no other task may assert relay signals directly.
- All actuation requests arrive via command queue Q1 (from T3 and T6 only — manual window commands from LCD/web/MQTT are out of scope, C9).
- Runs the per-channel window state machine: `CLOSED` → `MOVING` → `OPEN` and reverse.
- Enforces OPEN + CLOSE mutual exclusion on each channel before asserting any relay.
- Reads motor travel times (`travel_mN`, seconds) and dwell times (`dwell_open_mN`, `dwell_close_mN`, minutes) from T4 (MX4) on startup and on each config update; converts travel time to ms for `vTaskDelay`.
- **Travel timer:** energises each relay for `(travel_mN + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000` ms; de-energises on expiry; window is at end position. The margin ensures the end-switch fires before the relay drops. **De-energising the relay before expiry stops the motor immediately at the current (intermediate) position — therefore only complete open or close commands are issued.**
- **Dwell timer:** after travel completes, enforces the minimum hold time before accepting the next command on that channel (FR-A09–FR-A12).
- Monitors the RRK-3 opto-isolated alarm input (GPIO42) via a deferred-ISR pattern: `IRAM_ATTR` ISR records first edge (volatile flag + tick timestamp); T2 task loop confirms after 75 ms by reading live pin state. **Not suppressed during MOVING — a motor reaching the emergency switch during a T2-commanded move is the primary alarm scenario.** On alarm assert confirmed: de-energise all 6 relays immediately, set EG1.MOTOR_ALARM, post log event to Q3 (FR-MA01–FR-MA02). On alarm release confirmed: clear EG1.MOTOR_ALARM, post CLOSE_ALL to Q1 for re-calibration, post log event to Q3, resume AUTOMATIC (FR-MA06–FR-MA07). Checks EG1.MOTOR_ALARM before executing any Q1 command; discards the command if alarm is active.
- **Synchronization:** receives Q1 (actuation commands); checks EG1.MOTOR_ALARM before executing commands; posts to Q3 (log events via `log_post()`); sets/clears EG1.MOTOR_ALARM on GPIO42 alarm assert/release.

---

#### T3 — Safety Monitor

**Priority:** High | **Core:** 1

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

**Priority:** Medium-high | **Core:** 1

T4 is the single source of truth for all runtime data and configuration. All tasks that need to read or write system state do so through T4. This eliminates distributed per-variable mutexes and provides a single serialisation point for NVS persistence.

**Configuration settings**
- Holds all configurable parameters in RAM: setpoints (T_min_day, T_max_day, T_min_night, T_max_night, RH_min_day, RH_max_day, RH_min_night, RH_max_night), wind thresholds, per-channel motor travel times (`travel_mN`, seconds — relay energisation duration) and dwell times (`dwell_open_mN` / `dwell_close_mN`, minutes — minimum hold after travel), hysteresis values, sliding average windows, geographic location (lat/lon), WiFi credentials, PIN hashes, display language, session timeout, WiFi AP timeout.
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
- Ring buffer depth: **360 entries per channel** (T, RH, wind speed, wind direction) = 11.5 KB total internal RAM. Resolved — see Open Issue #2 (Closed).
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

**Priority:** Medium-high | **Core:** 1

- Modbus RTU master on UART1 with SIT65HVD08P transceiver; manages DE/RE direction control pin.
- Polls SenseCAP S200 (wind speed + direction) and FG6485A (T + RH) on a configurable interval (factory default 60 s; technician-configurable 30–3600 s via web GUI).
- On successful read: computes updated sliding-average values for T and RH (ring buffer of size = `avg_window_min × 60 / poll_interval` samples; default window 1 minute = effectively no averaging); writes raw and averaged values to T4; T4 then notifies T3 and T6.
- On fault (timeout, CRC error, out-of-range value): posts a sensor fault event to T9 (logger) and triggers alarm display via T8.
- **Synchronization:** posts to Q6 (sensor readings and updated sliding averages to T4); sets/clears EG1.SENSOR_FAULT_T and EG1.SENSOR_FAULT_W; posts to Q3 (log events); no mutexes held — T4 owns all measurement storage.

---

#### T6 — Climate Control

**Priority:** Medium | **Core:** 1

- Wakes on notification from T4 that new sensor data is available.
- Reads sliding-average T (T_avg) and RH (RH_avg) from T4; reads current day/night period (`is_daytime`) from T4; selects the applicable setpoint pair (T_min_day/T_max_day or T_min_night/T_max_night; RH_min_day/RH_max_day or RH_min_night/RH_max_night).
- Evaluates temperature and humidity against the active setpoints with hysteresis bands.
- Runs conflict resolution algorithm when T and RH demand opposing window actions.
- Posts open/close actuation commands to T2 via command queue.
- Checks operating mode from T4 before acting; inhibited in Standby and Wind-override states.
- **Synchronization:** wakes on TN2 (from T4, new sensor data); acquires MX2 to read current T and RH; acquires MX4 to read setpoints and hysteresis; reads EG1 (MOTOR_ALARM, WIND_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W) before issuing any command; posts to Q1 (actuation commands); posts to Q3 (log events).

---

#### T7 — Keypad Scan

**Priority:** Medium-high | **Core:** 1

- Scans the 4×4 keypad matrix every ~20 ms.
- Applies software debounce.
- Posts validated key-press events to T8 via queue.
- Can be implemented as a FreeRTOS software timer callback rather than a full task.
- **Synchronization:** posts to Q2 (key events to T8); no shared data; no mutexes required.

---

#### T8 — UI / Display

**Priority:** Medium | **Core:** 1

- Manages the LCD1602 display via I2C (shared bus with RTC). Any `delay()` calls in the LCD1602 driver must be replaced with `vTaskDelay(pdMS_TO_TICKS(ms))` so T8 yields to the scheduler rather than spinning.
- Renders the main status screen: T, RH, wind speed and direction, window states, operating mode, active session, active alarms.
- **Cyclic status pages (`STATUS_PAGES = 5`):** auto-rotates every 5 s through pages 0–4:
  - 0: temperature and humidity
  - 1: wind speed and direction
  - 2: window states (OPEN / MOVING / CLOSED per channel)
  - 3: network status (AP / client / IP)
  - 4: current date/time (local, via `localtime_r`) and time source label ("NTP" when `s_net.ntp_synced` true; "RTC" otherwise); pressing `#` on this page initiates the manual time-set flow (FR-UI22, FR-UI23)
- Runs the menu FSM; navigation depth ≤ 4 key presses from the main screen to any first-level setting.
  - **FSM states:** `UI_STATUS`, `UI_MENU_ROOT`, `UI_MENU_CLIMATE`, `UI_MENU_WIND`, `UI_MENU_SYSTEM`, `UI_MENU_ACCESS`, `UI_EDIT_VALUE`, `UI_PIN_ENTRY`, **`UI_SET_DATE`**, **`UI_SET_TIME`**
  - `UI_SET_DATE`: 6-digit DDMMYY entry with inline `_` cursor placeholder; validates DD 01–31, MM 01–12; saves to `s_dt_saved_{year,mon,mday}` on `#`; advances to `UI_SET_TIME`. `*` returns to `UI_STATUS`.
  - `UI_SET_TIME`: 4-digit HHMM entry; validates HH 0–23, MM 0–59. `*` re-enters `UI_SET_DATE` restoring the previously typed digits. `#` builds a `struct tm` from the saved date + typed time, calls `mktime(tm_isdst=-1)` to produce a UTC epoch, then calls `dm_set_manual_time(unix_ts)`.
  - `s_pending_settime` flag: set when `#` is pressed on page 4 without an active admin session; clears and calls `enter_set_date()` after PIN is accepted in `handle_pin()`.
- Manages session state: PIN entry via keyboard, session timeout, PIN validation against T4.
- Posts validated configuration changes and mode changes to T4.
- Receives WiFi status updates from T10 via Q5; stores in `s_net` (`net_status_t`); uses `s_net.ntp_synced` for page-4 source label.
- **Synchronization:** acquires MX1 (I2C) to write LCD; acquires MX2 to read current measurements for display refresh; acquires MX3 to read ring buffers for history view; acquires MX4 to read configuration for settings screens; receives Q2 (key events from T7); receives Q5 (network status from T10, including `ntp_synced`); reads EG1 (alarm flags for display and alarm indication); posts to Q4 (config/mode updates to T4); posts to Q3 (log events: mode changes, setpoint changes, session events).

---

#### T9 — Event Logger

**Priority:** Low | **Core:** 1

- Receives log events from all tasks via a dedicated queue; senders post and return immediately.
- Serialises all writes to the NVS ring buffer and, when present, to the SD card.
- The queue decouples log I/O from higher-priority tasks; no task is blocked by log write latency.
- Queue overflow policy: drop-oldest enforced by `log_post()` in `event_logger.h` (Gap H); see §5.3 for the two-step evict-and-retry mechanism.
- Periodic sensor-value snapshots: T4 posts a `LOG_SENSOR` event to Q3 every time it receives new sensor data from T5 via Q6. T9 consumes these like any other event — no separate timer or MX3 access required.
- **Synchronization:** receives Q3 (log events from all tasks); no mutexes held; no I2C or GPIO access.

---

#### T10 — Network Manager

**Priority:** Low | **Core:** 0

- Manages WiFi AP lifecycle: enable on admin command from T8 or T11; automatic shutdown after configurable timeout (timeout and SSID/PSK managed independently; AP can run concurrently with client connection).
- Manages WiFi client: connect to configured SSID; monitor connection; reconnect on drop; supports DHCP and static IP; exponential backoff (2→4→…→60 s) on repeated failures; `WiFi.setAutoReconnect(false)` (T10 manages reconnection itself).
- Posts connection state changes (connected / disconnected / assigned IP / NTP synced) to T8 via Q5 (`xQueueOverwrite`); `net_status_t` fields: `client_connected` (bool), `ap_active` (bool), `ntp_synced` (bool, latched true after first successful NTP sync), `ip_str[16]` (current IP as string).
- Triggers NTP time synchronisation (`configTime(0, 0, "pool.ntp.org")`) when a client connection is established; polls `time(NULL) > 1700000000L` for up to 30 s; on success: sends TN4 to T4, sets `s_ntp_synced = true`, then calls `do_geo_sync()`.
- **`do_geo_sync()` — automatic geolocation and timezone (FR-DN06, FR-DN07):**
  - Performs HTTP GET `http://ip-api.com/json?fields=status,lat,lon,timezone` (5 s timeout; no SSL; `HTTPClient` from Arduino core — no additional `lib_deps` required).
  - Parses JSON for `status`, `lat` (float), `lon` (float), `timezone` (IANA name string).
  - Converts lat/lon to integer degree + millidegree parts via `float_to_deg_frac()`; posts `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` as `config_update_t` items to Q4 → T4 updates NVS + shadow + calls `update_sun_times()`.
  - Looks up the IANA timezone name in `s_tz_table[]` (~100-entry static array mapping IANA names to POSIX strings) via `iana_to_posix()`; writes the resolved POSIX string to NVS `system/tz_str`; applies immediately via `setenv("TZ", posix_tz, 1); tzset()`.
  - On HTTP failure, JSON parse failure, or unknown timezone name: silently skips the corresponding update; last stored values in NVS are retained.
- Runs on Core 0 alongside the ESP32-S3 internal WiFi stack.
- **Synchronization:** posts to Q5 (network status to T8); posts to Q4 (geolocation lat/lon updates to T4); sends TN4 to T4 on NTP sync success; posts to Q3 (log events); no mutexes held.

---

#### T11 — Web Server

**Priority:** Low | **Core:** 0

- Serves HTML, CSS, and JavaScript from LittleFS via ESPAsyncWebServer (callback-driven).
- Applies the same three-state session model (Normal / Farmer / Admin) and PIN codes as T8.
- Reads configuration and current measurement data from T4; posts validated setting changes to T4.
- Available on both WiFi AP and WiFi client interfaces simultaneously.
- Authentication required before any page is served or any setting is changed.
- **Synchronization:** acquires MX5 (LittleFS) to serialise concurrent HTTP file-serve requests against the active LittleFS partition; reads EG1.OTA_IN_PROGRESS (informational — T13 writes only to the inactive partition so T11 is not blocked during OTA, but the flag may be used to suppress OTA-page interactions); acquires MX2 to read current measurements; acquires MX4 to read configuration; posts to Q4 (validated config/state updates to T4); posts to Q3 (log events).

---

#### T12 — MQTT Client

**Priority:** Low | **Core:** 0

- Publishes current T, RH, wind speed, wind direction, window states, operating mode, and alarm status to the configured MQTT broker at a configurable interval.
- Reads all published values from T4.
- Subscribes to configured command topics; posts received actuation commands to T2 and setting changes to T4.
- Active only when WiFi client is connected and an MQTT broker is configured.
- **Synchronization:** acquires MX2 to read current measurements for publishing; acquires MX4 to read MQTT broker configuration; posts to Q1 (commands received via MQTT); posts to Q4 (settings received via MQTT); posts to Q3 (log events).

---

#### T13 — OTA (on demand)

**Priority:** Low (spawned on demand) | **Core:** 0

- Activated via the web interface (T11).
- Writes incoming firmware image to the inactive firmware bank (A or B).
- Receives web asset zip, buffers it in PSRAM, and extracts it file-by-file to the **inactive** LittleFS partition (the partition paired with the inactive firmware bank). Writes `manifest.json` last.
- The active LittleFS partition is never written during an update; T11 continues to serve the active partition uninterrupted while T13 writes to the inactive one.
- On successful write of both firmware and web assets: marks the inactive firmware bank (and its paired LittleFS partition) as active and triggers a controlled system restart.
- Implements 3-consecutive-fail rollback: if the new firmware fails to complete startup 3 times, the previous bank is restored as active — this also automatically restores the previous matching LittleFS partition.
- Firmware and web asset updates belonging to the same release must both complete before either is activated.
- **Synchronization:** does **not** acquire MX5 during web asset write (inactive LittleFS is not accessed by T11); sets EG1.OTA_IN_PROGRESS on start, clears on completion or failure; posts to Q3 (log events).

---

### 4.4 Core Assignment

| Core | Tasks | Rationale |
|------|-------|-----------|
| **Core 1 (Application)** | T1, T2, T3, T4, T5, T6, T7, T8, T9 | Real-time control, sensor I/O, and local UI; isolated from the WiFi stack |
| **Core 0 (Protocol)** | T10, T11, T12, T13 | WiFi stack, TCP/IP, and all network-facing tasks; the ESP32-S3 WiFi internals run on Core 0 |

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

  T2, T3, T5, T6, T8, T10 ── log events ─────────► T9 Event Logger

  T10 Network ─── status ─────────────────────────► T8 UI / Display
  T10 Network ─── NTP sync ───────────────────────► system clock

  T11 Web ─────── config/mode changes ────────────► T4 Data Manager
  T12 MQTT ───── config/mode changes ─────────────► T4 Data Manager

  T11 Web ─────── OTA trigger ────────────────────► T13 OTA
```

### 4.6 Synchronization Primitives

#### 4.6.1 Mutexes

FreeRTOS mutexes (`xSemaphoreCreateMutex`) implement priority inheritance, which mitigates priority inversion when a high-priority task (e.g. T3) waits on a mutex held by a lower-priority task.

| ID  | Name                      | Protects                                                                 | Writers                       | Readers                              |
|-----|---------------------------|--------------------------------------------------------------------------|-------------------------------|--------------------------------------|
| MX1 | I2C bus                   | Shared I2C bus (SDA/SCL) — LCD display and DS1307 RTC on the same wires | T8 (LCD write), T4 (RTC read) | —                                    |
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
| Q3 | Log event queue             | → T9        | T2, T3, T5, T6, T8, T10, T11, T12, T13     | T9       | Log event struct         | Generous depth; drop-oldest on overflow        |
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
| 0   | WIND_OVERRIDE      | T3     | T3         | T6, T8, T11, T12 (display only) | Wind safety threshold exceeded; all windows being closed |
| 1   | *(reserved)*       | —      | —          | —                       | Previously MANUAL_OVERRIDE — removed; hardware does not support manual operation detection |
| 2   | SENSOR_FAULT_T     | T5     | T5         | T6, T8, T9              | Temperature/humidity sensor fault active                  |
| 3   | SENSOR_FAULT_W     | T5     | T5         | T3, T8, T9              | Wind sensor fault active; T3 treats wind as worst-case   |
| 4   | OTA_IN_PROGRESS    | T13    | T13        | T11                     | OTA update active; T11 defers LittleFS file requests     |
| 5   | MOTOR_ALARM        | T2     | T2         | T3, T6, T8, T11, T12 (display only) | RRK-3 motor emergency stop active; all relays de-energised; all window control suspended; highest priority override |

> **T3 and SENSOR_FAULT_W:** when the wind sensor fault flag is set, T3 shall treat the wind condition as exceeding all thresholds (safe-fail: close all windows) until the fault clears.

> **T2 and MOTOR_ALARM:** MOTOR_ALARM takes priority over all other states. T2 discards all incoming Q1 commands while this flag is set. T3 CLOSE_ALL commands are also discarded — the relays are already de-energised and the alarm state persists until the RRK-3 alarm clears.

#### 4.6.5 Primitive Cross-reference by Task

| Task | Acquires (mutex) | Posts to (queue) | Receives from (queue) | Sends (notification) | Receives (notification) | Reads/Sets (event group) |
|------|-----------------|------------------|-----------------------|----------------------|-------------------------|--------------------------|
| T1   | MX4             | —                | —                     | —                    | —                       | Reads EG1 (all — for RGB status LED colour)  |
| T2   | —               | Q3               | Q1                    | —                    | —                       | Sets/clears EG1.MOTOR_ALARM |
| T3   | MX2             | Q1, Q3           | —                     | —                    | TN1 ← T4               | Sets/clears EG1.WIND_OVERRIDE; reads EG1.SENSOR_FAULT_W, EG1.MOTOR_ALARM |
| T4   | MX1, MX2, MX3, MX4 | —            | Q4, Q6                | TN1 → T3, TN2 → T6   | TN4 ← T10              | —                        |
| T5   | —               | Q3, Q6           | —                     | —                    | —                       | Sets/clears EG1.SENSOR_FAULT_T, EG1.SENSOR_FAULT_W |
| T6   | MX2, MX4        | Q1, Q3           | —                     | —                    | TN2 ← T4               | Reads EG1 (MOTOR_ALARM, WIND_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W) |
| T7   | —               | Q2               | —                     | —                    | —                       | —                        |
| T8   | MX1, MX2, MX3, MX4 | Q3, Q4      | Q2, Q5                | —                    | —                       | Reads EG1 (all)          |
| T9   | —               | —                | Q3                    | —                    | —                       | —                        |
| T10  | —               | Q3, Q4, Q5       | —                     | TN4 → T4             | —                       | —                        |
| T11  | MX2, MX4, MX5  | Q3, Q4           | —                     | —                    | —                       | Reads EG1.OTA_IN_PROGRESS |
| T12  | MX2, MX4        | Q3, Q4           | —                     | —                    | —                       | —                        |
| T13  | MX5             | Q3               | —                     | —                    | —                       | Sets/clears EG1.OTA_IN_PROGRESS |

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
- Poll interval configurable via NVS (default 60 s; range 30–3600 s). T4 posts one `LOG_SENSOR` event to Q3 on every Q6 reception — snapshot interval equals poll interval (FR-LG09); no separate snapshot timer.

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
| event_type | uint8 | 4 | Category: SENSOR, RELAY, MODE_CHANGE, SETPOINT, SESSION, ALARM, SYSTEM |
| initiator | uint8 | 5 | SYSTEM, USER_FARMER, USER_ADMIN, MQTT, WEB |
| channel | uint8 | 6 | Motor channel (M1/M2/M3) or 0 for non-motor events |
| param_id | uint8 | 7 | `log_param_id_t`: identifies the specific CONFIG parameter (C1–C22); 0 for all non-CONFIG events. For C18/C19, `channel` identifies the motor and `param_id` distinguishes open vs close dwell. |
| value_a | int16 | 8 | First payload (sensor value, old setting, reason code) |
| value_b | int16 | 10 | Second payload (new setting, threshold, parameter) |

Total: 12 bytes. No padding needed — four uint8 fields (offset 4–7) fill the alignment gap before `value_a`.

**Storage:**
- Primary: SD card (FAT32), when present. See log rotation policy below.
- Fallback: NVS dedicated log namespace. Ring buffer of minimum 250 fixed-size entries (FR-LG06); oldest entry overwritten when full.
- T9 checks SD card presence on startup and on each write cycle; falls back to NVS if card is absent or returns an error (FR-LG07, FR-LG08).

**SD card log file format:**
- CSV text file; first line is a fixed header row: `timestamp,event_type,initiator,channel,param_id,value_a,value_b`
- Each subsequent line is one log entry. Example: `2024-06-15T10:30:00,SENSOR,SYSTEM,0,0,235,650`
- Average line length: ~60 bytes. Estimated daily volume: ~90 KB (1 440 sensor snapshots at 1-minute default interval + ~100 discrete events).

**SD card log file naming:**
Files are named `YYYYMMDDHHSS.csv`, where:

| Token | Meaning |
|-------|---------|
| YYYY | 4-digit year |
| MM | 2-digit month (01–12) |
| DD | 2-digit day (01–31) |
| HH | 2-digit hour, 24-hour clock (00–23) |
| SS | 2-digit second (00–59) |

The timestamp encodes the moment the file was created. Files are stored in the root directory of the SD card. Lexicographic sort of filenames yields chronological order, which is used by the startup scan and the web log retrieval interface.

**SD card log rotation policy:**

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Maximum file size | 512 KB | At ~90 KB/day typical rate, each file spans ~5–6 days. A power-loss event can corrupt only the currently open file; all closed files are intact. |
| Files retained | 10 most recent | 10 × 512 KB = 5 MB maximum log footprint. Minimum guaranteed history: 9 closed files + 1 partial current file ≈ 45–60 days. |
| Minimum retention floor | 3 files | Files are never deleted below this count, regardless of free space. |
| Low free-space threshold | 2 MB | If SD free space drops below 2 MB and the file count is already at the minimum retention floor, SD logging is suspended; NVS fallback is activated. SD logging resumes on the next successful mount when space has been reclaimed. |

**Rotation procedure (triggered when current file reaches 512 KB):**
1. Flush and close the current log file.
2. Create a new file named with the current timestamp (`YYYYMMDDHHSS.csv`).
3. Write the CSV header row to the new file.
4. If the total file count now exceeds 10, delete the oldest file (lowest lexicographic filename).

**Startup / resume behaviour:**
On SD card mount, T9 scans the log directory for `*.csv` files and sorts them by filename (lexicographic = chronological). If the most recent file is below 512 KB, T9 resumes appending to it. Otherwise a new file is created immediately. If no log files exist, a new file is created.

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

Farmer-level parameters include: day and night temperature setpoints (T_min_day, T_max_day, T_min_night, T_max_night), day and night humidity setpoints (RH_min_day, RH_max_day, RH_min_night, RH_max_night), humidity control enable/disable (`rh_ctrl_en`), wind protection enable/disable (`wind_prot_en`), conflict resolution priority (`cr_priority`), and geographic location for sunrise/sunset calculation (`lat_*`, `lon_*`) — web GUI only (FR-CF16). Administrator/technician-level parameters include: wind safety thresholds (v_max, direction exclusion zone), hysteresis values, dwell times (web GUI only, FR-CF10/CF11), motor travel times (`travel_m1`, `travel_m2`, `travel_m3`, web GUI only, 5–600 s, FR-CF05; defaults `MOTOR_MN_TRAVEL_S_DEFAULT` in `app_types.h`), sensor poll interval (web GUI only, 150–3600 s, FR-CF07), sliding average windows (web GUI only, 1–60 min, FR-CF17), network configuration, and access control settings.

The web interface applies the same three-state model and the same PIN codes as the local keyboard interface.

---

### 5.5 Local User Interface

**Implemented by:** T7 (Keypad Scan) and T8 (UI / Display)

**LCD driver note:** The Waveshare LCD1602 module uses an **AiP31068L** I2C-to-parallel bridge at address **0x3E** (not PCF8574 at 0x27 as originally assumed). LIB-4 (`drivers/LCD1602_I2C/`) is implemented for this module and address.

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

**Menu FSM:**
- Maximum navigation depth: 4 key presses from the main screen to any first-level setting (FR-UI07).
- Menu structure: Main → Category → Parameter → Edit → Confirm.
- `#` key: confirm / enter. `*` key: cancel / back. Numeric keys: input values. `A`/`B`: scroll up/down in lists.
- On session timeout: menu FSM resets to main screen and session closes.

**Status page cycling (`STATUS_PAGES = 5`):**
- Page 0: temperature and humidity readings
- Page 1: wind speed and direction
- Page 2: estimated window states (OPEN / MOVING / CLOSED per channel)
- Page 3: network status (AP active / client IP)
- Page 4: current local date/time (via `localtime_r`); source label "NTP" or "RTC" (from `s_net.ntp_synced`); pressing `#` enters the manual time-set flow (FR-UI22, FR-UI23)

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

- WiFi AP mode is **mandatory** (Must have).
- The AP does not start automatically on boot; it is enabled by the administrator via the local keyboard menu or web interface.
- AP SSID is auto-generated as `"Greenhouse-"` followed by the hexadecimal representation of the last 2 bytes of the WiFi NIC MAC address (e.g., `"Greenhouse-A3F2"` if MAC ends in `A3:F2`). The SSID is not stored in NVS; it is regenerated from the MAC address on each AP start.
- AP password is stored in NVS (`wifi` / `ap_psk`) as **plaintext**. WPA2 requires the raw passphrase during the handshake; hashing is not applicable (contrast with the client PSK, which is stored as a hash because it only needs to match, not be transmitted). Default password: `0123456789`. Configurable by the administrator via the web interface.
- Automatic AP shutdown timeout is configurable by the administrator; the AP disables itself when the timeout expires with no active client connections.
- While the AP is active, the LCD displays "AP active" and the assigned AP IP address.
- The HTTP configuration web interface (§5.8) is accessible to clients connected to the AP.
- WPA2 security minimum (TR-NW01).

---

### 5.7 WiFi — Client Mode (Optional)

**Implemented by:** T10 (Network Manager)

- WiFi client (station) mode is **optional** (Could have).
- The HTTP configuration web interface (§5.8) is accessible to clients on the same network when the controller is connected.
- TCP/IP settings configurable by the administrator:
  - DHCP (automatic address assignment) or static IP.
  - Static configuration: IP address, subnet mask, default gateway, DNS server.
- LCD display shows current WiFi client status:
  - *Disconnected* — client mode enabled but no network connection.
  - *Connected* — connected to AP; displays assigned IP (DHCP) or configured static IP.
- On client connection: T10 triggers NTP synchronisation; on NTP success: sends TN4 to T4 and calls `do_geo_sync()` to auto-detect location and timezone (FR-DN06, FR-DN07). See §4.3 T10 for full `do_geo_sync()` description.
- Time display in all contexts (web dashboard, LCD page 4) uses `localtime_r()` after the TZ string has been applied, so the displayed time automatically reflects the correct timezone and DST offset.

---

### 5.8 Web Interface

**Implemented by:** T11 (Web Server)

**Technology:**
- Web server: **`mathieucarbou/ESPAsyncWebServer @ ^3.3.6`** (callback-driven, non-blocking), with **`mathieucarbou/AsyncTCP @ ^3.3.2`** as its TCP layer. The `mathieucarbou` fork is used instead of the original `me-no-dev/ESPAsyncWebServer` because it is actively maintained for ESP-IDF 5.x / Arduino 3.x compatibility; the original fork does not build cleanly against the ESP32 Arduino 3 core. Both are declared in `firmware/platformio.ini` `lib_deps`.
- HTML, CSS, and JavaScript files stored in LittleFS partition on ESP32-S3 flash, separate from the firmware binary.
- The web interface mirrors the local keyboard interface exactly: same three operating states (§5.4), same PIN codes, same parameter visibility rules.

**Access control:**
- Authentication required before any page is served or any setting is changed (FR-NW06).
- Session cookie issued after successful PIN entry; cookie invalidated on logout or session timeout.
- HTTPS is **not implemented** (TR-NW04 — not feasible on target hardware; threat model accepted, see §6 Open Issue #4).

**Pages:**

- **Dashboard** *(any authenticated session)*: live T, RH, wind speed, wind direction, window states (OPEN / MOVING / CLOSED per channel), operating mode, sunrise/sunset times for current day (FR-DN04), active alarms.

- **Settings** *(sub-sections; access level per row)*:

  | Sub-section | Access | Parameters |
  |-------------|--------|------------|
  | **Climate** | Farmer / Admin | T_min_day, T_max_day, T_min_night, T_max_night (°C); RH_min_day, RH_max_day, RH_min_night, RH_max_night (%); humidity control enable (`rh_ctrl_en`); conflict resolution priority (`cr_priority`); geographic location lat/lon for sunrise/sunset (FR-CF16) |
  | **Wind** | Farmer (enable/disable only) / Admin (all) | Wind protection enable (`wind_prot_en`); v_max (Beaufort); direction exclusion zone centre and half-width (°); wind hysteresis timer (FR-CF09) |
  | **Motors** | Admin only | Motor travel times: M1, M2, M3 individually (seconds, range 5–600 s, factory defaults 21/21/171 s, FR-CF05); open-dwell time per window M1–M3 (minutes, FR-CF10); close-dwell time per window M1–M3 (minutes, FR-CF11) |
  | **Sensors** | Admin only | Sensor poll interval (30–3600 s, factory default 60 s, FR-CF07); sliding average window for T and RH (1–60 min, FR-CF17) |
  | **System** | Admin only | Session timeout (minutes); RGB LED day/night brightness and schedule (`led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`, FR-CF14); NTP timezone string |
  | **Access** | Admin only | Change farmer PIN; change admin PIN; lockout threshold and duration |

  Each editable field shows its current value, the valid range, and the factory default. A **Restore defaults** button is available per sub-section (admin only); factory reset of all settings requires physical confirmation (admin only).

- **Log viewer** *(farmer / admin)*: paginated event log, filterable by event type and time range (FR-LG05).

- **OTA update** *(admin only)*: firmware binary upload and web-asset `.zip` upload (T13).

- **Network** *(admin only)*: WiFi AP configuration (SSID suffix, password, auto-shutdown timeout); WiFi client configuration (SSID, PSK, DHCP / static IP); MQTT broker settings.

**MQTT client (optional, FR-MQ01–FR-MQ05):**
- Configured via the web interface (admin session).
- Publishes: T, RH, wind speed, wind direction, window states, operating mode, alarm flags.
- Subscribes to: OPEN/CLOSE commands per channel, mode change commands.
- Authentication: username/password or client certificate, configurable by administrator.

---

### 5.9 OTA Firmware Update

**Implemented by:** T13 (OTA)

**Flash partition layout** (`firmware/partitions.csv`, 16 MB QSPI flash):

| Label | Type | SubType | Offset | Size | Role |
|-------|------|---------|--------|------|------|
| `nvs` | data | nvs | 0x009000 | 84 KB (0x15000) | Config namespaces + event-log ring buffer |
| `otadata` | data | ota | 0x01E000 | 8 KB (0x2000) | Active/inactive bank metadata (ESP-IDF OTA) |
| `app0` | app | ota_0 | 0x020000 | 2 MB (0x200000) | Firmware Bank A |
| `app1` | app | ota_1 | 0x220000 | 2 MB (0x200000) | Firmware Bank B |
| `lfs0` | data | spiffs | 0x420000 | 1 MB (0x100000) | LittleFS A — web assets paired with Bank A |
| `lfs1` | data | spiffs | 0x520000 | 1 MB (0x100000) | LittleFS B — web assets paired with Bank B |
| *(unused)* | — | — | 0x620000 | ~9.9 MB | Reserved for future expansion |

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
1. Administrator uploads a `.zip` archive of HTML/CSS/JS files via the web interface (admin session required).
2. T13 receives the zip and buffers it entirely in PSRAM.
3. T13 sets **EG1.OTA_IN_PROGRESS**.
4. T13 mounts the **inactive** LittleFS partition independently. The active partition remains mounted by T11 and continues to serve requests uninterrupted — MX5 is not acquired during this phase.
5. T13 extracts each file from the zip and writes it to the inactive LittleFS partition. Existing files are overwritten; new files are created; files absent from the zip are left as orphans (or the partition is formatted first for a clean state — implementation choice).
6. T13 writes `manifest.json` to the inactive partition as the **last step**, only after all files have been extracted and verified:

   ```json
   {
     "asset_version": "MAJOR.MINOR.PATCH",
     "checksum":      "<hex string — CRC32 or SHA-256 of the zip archive>"
   }
   ```

7. T13 unmounts the inactive LittleFS partition and clears **EG1.OTA_IN_PROGRESS**.
8. The inactive LittleFS is now ready. It is activated on the next firmware bank switch (step 3 of the firmware update procedure above), or immediately if only web assets are being updated (T13 switches the active bank pointer without a firmware image change).

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
| `motor` | `travel_m1`, `travel_m2`, `travel_m3`, `dwell_open_m1`, `dwell_open_m2`, `dwell_open_m3`, `dwell_close_m1`, `dwell_close_m2`, `dwell_close_m3` | `int16_t` | **Travel times** (`travel_mN`, seconds, range 5–600): how long T2 energises the relay to move a window from one end-stop to the other. Read by T2 from T4 (MX4); converted to ms for `vTaskDelay`. Defaults: M1=21, M2=21, M3=171 (`MOTOR_MN_TRAVEL_S_DEFAULT` in `app_types.h`). Configurable by technician via web GUI (FR-CF05, admin level). **Dwell times** (`dwell_open_mN` / `dwell_close_mN`, minutes): minimum hold period T2 enforces after travel completes before accepting the next command on that channel. `dwell_open_mN`: min hold at `OPEN` before CLOSE accepted. `dwell_close_mN`: min hold at `CLOSED` before OPEN accepted. Dwell timer starts when the travel timer expires (FR-A09–FR-A12). Default: 0 (no hold enforced). Configurable by technician via web GUI only (FR-CF10, FR-CF11). |
| `access` | `pin_salt` (blob[16]), `pin_farmer_hash` (blob[32]), `pin_admin_hash` (blob[32]), `fail_cnt_f`, `fail_cnt_a`, `lockout_f`, `lockout_a`, `lockout_max`, `lockout_secs` | blob / int32 | `pin_salt`: 16-byte random salt, generated once at first boot. `pin_farmer_hash` / `pin_admin_hash`: SHA-256(salt \|\| pin_ascii) digest. `fail_cnt_f` / `fail_cnt_a`: per-role consecutive failure count. `lockout_f` / `lockout_a`: per-role lockout expiry as Unix timestamp (0 = not locked). `lockout_max`: threshold before lockout (default 5). `lockout_secs`: lockout duration (default 300 s). |
| `wifi` | `ssid`, `psk_hash`, `ap_psk`, `ip_mode`, `ip_addr`, `ip_mask`, `ip_gw`, `ip_dns` | string | WiFi client and AP credentials and network settings; AP SSID is auto-generated from MAC address and not stored. `ap_psk` stored as **plaintext** (WPA2 requires raw key); default `"0123456789"`; configurable by admin via web interface. `psk_hash` (client password) stored as salted SHA-256 hash. |
| `mqtt` | `broker_url`, `port`, `username`, `password`, `topic_prefix`, `interval` | string / uint16 | MQTT broker connection and publish settings. `password` stored as plaintext in NVS (MQTT protocol requires the actual password to authenticate to the broker; hashing is not possible). Accepted risk — same basis as no-HTTPS decision (Issue #5 closed). |
| `system` | `poll_interval`, `session_timeout`, `ap_timeout`, `lang`, `log_pointer`, `schema_ver`, `fw_version`, `led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`, `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac`, `tz_str` | uint16 / string / uint8 / int16 | System-wide configuration; `poll_interval` (uint16, seconds, default 60, technician-settable 30–3600 via web GUI); `lat_deg` + `lat_frac` / `lon_deg` + `lon_frac`: geographic location stored as integer degree and fractional milli-degree parts (e.g. 52.0907°N stored as lat_deg=52, lat_frac=907) for sunrise/sunset calculation; populated manually via web GUI (FR-CF16) or automatically by `do_geo_sync()` (FR-DN06); `tz_str` (string[64]): POSIX TZ string e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"`, factory default `"CET-1CEST,M3.5.0,M10.5.0/3"`, applied at boot via `setenv/tzset` and on each geolocation update (FR-DN07, FR-CF18); `schema_ver` (int32) tracks NVS layout version; `fw_version` (string `"MAJOR.MINOR.PATCH"`) overwritten on every boot; `led_day_brt` / `led_nite_brt` (uint8, 0–255, defaults 200/20); `led_nite_from` / `led_nite_to` (uint8, hour 0–23, defaults 22/6) |
| `log` | Ring buffer entries (binary blob, fixed record size) | blob | Event log fallback when SD card absent |

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

**Hardware watchdog:**
- ESP32-S3 hardware watchdog timer is enabled during initialisation.
- T1 kicks the watchdog every 500 ms; if T1 is starved and the watchdog fires, the MCU resets automatically (TR-SW03).
- The watchdog timeout is set longer than the T1 kick interval but shorter than the maximum acceptable response latency for a fault condition.

**Restart sequence on watchdog reset:**
- On boot after a watchdog reset: the firmware detects the reset reason via `esp_reset_reason()`.
- Controlled restart: T2 closes all relay outputs immediately (CLOSE_ALL on all channels) to re-synchronise the estimated window position (FR-ST02).
- The restart event and reset reason are logged to Q3 before normal operation resumes.
- If 3 consecutive watchdog resets occur without completing the startup health check, T13 OTA rollback logic restores the previous firmware bank.

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
- Driven by the FastLED library (or Adafruit NeoPixel) via the single-wire protocol; no level-shifting or external wiring required.
- LED is mounted on the MCU board inside the MC001110 enclosure; it is visible through the transparent cover (FR-UI20).

**Colour convention and state priority:**

| Priority | Colour | Hex | Condition | FRS |
|----------|--------|-----|-----------|-----|
| Highest | **Red** | `#FF0000` | Critical alarm — `EG1.MOTOR_ALARM` active (RRK-3 emergency stop; all window control suspended), OR system halted (3 consecutive watchdog resets without startup completion) | FR-UI19 |
| Middle | **Amber** | `#FF8000` | Non-critical alarm or warning: `EG1.SENSOR_FAULT_T`, `EG1.SENSOR_FAULT_W`, `EG1.WIND_OVERRIDE`, wind protection disabled (`wind_prot_en == false`), or humidity control disabled (`rh_ctrl_en == false`) | FR-UI18 |
| Lowest | **Green** | `#00FF00` | All above conditions false; system operating normally | FR-UI17 |

If multiple conditions apply simultaneously, the highest-priority colour is shown (FR-UI16).

**State evaluation logic (executed in T1 on each watchdog kick):**

```
if (halt_flag OR EG1.MOTOR_ALARM):
    colour ← RED

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
| Current local hour ∈ [`led_nite_from`, `led_nite_to`) — wrapping midnight | `led_nite_brt` (default 20 / 255) |
| All other hours | `led_day_brt` (default 200 / 255) |

- Brightness is applied to the currently active colour via the FastLED `setBrightness()` API or equivalent.
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

## 6. Open Issues

| # | Issue | Owner | Status |
|---|-------|-------|--------|
| 1 | **Motor alarm signal — software response** — Hardware signal characterised (THDS Issue #1 closed): RRK-3 alarm relay (dry contact, closes on motor emergency stop) → J10 opto input → GPIO 42 (active-low: GPIO LOW = alarm active, INPUT_PULLUP). Normal manual window operation does NOT trigger this signal. **Resolution:** T2 uses a deferred-ISR pattern: `attachInterrupt(PIN_OPTO_INPUT, isr_handler, CHANGE)`; `IRAM_ATTR` ISR records first edge (volatile flag + FreeRTOS tick timestamp); T2 task loop confirms after 75 ms by reading live pin state; NOT suppressed during MOVING. On alarm assert: de-energise all 6 relays, set EG1.MOTOR_ALARM, post log to Q3. On alarm release: clear EG1.MOTOR_ALARM, post CLOSE_ALL re-calibration to Q1, post log to Q3, resume AUTOMATIC. Manual override detection (formerly FR-M08–FR-M11) removed — hardware does not support it. | Software engineer | **Closed** |
| 2 | **Ring buffer depth** — **Resolution:** 360 entries per channel (T, RH, wind speed, wind direction) = 11.5 KB total internal RAM. At the default 60 s poll interval: 6 hours of in-memory history; at the minimum 30 s poll interval: 3 hours. Fits comfortably in ESP32-S3 internal SRAM with substantial headroom. Sufficient for web trend view and MQTT history. | Software engineer | **Closed** |
| 3 | **NTP timezone handling** — Hardware time source resolved (THDS Issue #7 closed): DS1307 RTC is the authoritative offline clock; NTP synchronises on WiFi connect. **Resolution:** DST handling via POSIX TZ string — `setenv("TZ", tz_str, 1); tzset()` applied at boot and on every geolocation update. TZ string stored in NVS `system/tz_str`; factory default `"CET-1CEST,M3.5.0,M10.5.0/3"` (Europe/Amsterdam). **Auto-TZ from geolocation (FR-DN07):** after successful NTP sync, `do_geo_sync()` queries ip-api.com and resolves the IANA timezone name to a POSIX string via a ~100-entry lookup table (`s_tz_table[]` in `network_manager.cpp`); the resolved POSIX string is applied immediately and persisted to NVS. If geolocation or lookup fails, the NVS value from the previous run is used. Technician-configurable via web GUI (FR-CF18). | Software engineer | **Closed** |
| 4 | **Web interface HTTPS — not implemented (TR-NW04 accepted)** — TLS termination on the ESP32-S3 is not feasible: the RAM and CPU overhead of a TLS stack would leave insufficient headroom for concurrent real-time tasks. **Decision:** HTTPS will not be implemented. **Accepted threat model:** the web interface is served over plain HTTP. The risk is mitigated by the following constraints: (a) the WiFi AP is disabled by default and enabled only on explicit admin command; (b) the AP has a configurable automatic timeout; (c) the controller is intended for use on a private, physically controlled greenhouse network and is not exposed to the public internet; (d) all credentials are stored as salted hashes and are never transmitted in plaintext; (e) session cookies are short-lived and invalidated on logout or timeout. This residual risk is accepted by the project owner. | Software engineer | **Closed — accepted** |
| 5 | **MQTT authentication method** — **Resolution:** plain username + password over TCP. Client certificate authentication is not implemented. The MQTT `password` field is stored as plaintext in NVS `mqtt/password` (MQTT protocol requires the actual password; hashing is not applicable). Accepted risk: same threat model as Issue #4 (no-HTTPS decision) — controller is on a private greenhouse network. Risk documented and accepted by the project owner. | Software engineer | **Closed — accepted** |

---

*End of document — version 0.1 draft*
