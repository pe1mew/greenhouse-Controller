# Technical Software Design Specification
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Technical Software Design Specification        |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.1 (draft)                                   |
| Date         | 2026-03-29                                    |
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
- User credentials are stored using a one-way hashing algorithm (bcrypt or SHA-256 with salt); plain-text storage is not permitted (FR-AC06).

---

## 3. Design Constraints from FRS

The following items originate from system-level and functional requirements in the FRS. Each represents a constraint or implementation decision that the software design must satisfy. Source requirement IDs are noted for traceability.

**User interface**
- Menu depth: max 4 key presses from the main screen to any first-level setting (FR-UI07).

**Credential storage**
- User credentials stored using one-way hashing (bcrypt or SHA-256 with salt); plain-text storage not permitted (FR-AC06).
- Configurable login lockout after a set number of failed attempts (FR-AC07).

**Event log**
- Minimum 1000 entries retained in persistent storage using a ring buffer; SD card preferred when present, internal flash as fallback (FR-LG06, FR-LG07, FR-LG08).

**Settings persistence**
- All configuration settings stored in ESP32-S3 NVS flash partition; retained across power cycles and restarts (FR-CF06, TR-SW01).

**Timekeeping and timezone**
- Time source selection is an open design decision (Open Issue #7 in `technicalHardwareDesignSpecification.md` §5); the chosen hardware solution must satisfy TR-HW08.
- When WiFi is available: synchronise system time via NTP on boot and periodically; configure timezone (Europe/Amsterdam, CET/CEST) with automatic DST transitions.
- When WiFi is unavailable or before first sync: use the hardware time source (RTC, GNSS, or DCF77) as the authoritative clock.
- If no hardware time source is fitted: log timestamps shall be marked as invalid until NTP sync succeeds; firmware shall not block startup waiting for time.

**Firmware update**
- Firmware updates supported without opening the enclosure: OTA over WiFi and via native USB (TR-SW02, TR-IF05).

**Fault recovery**
- Hardware watchdog timer automatically resets the MCU on a software hang; controlled restart sequence re-synchronises window states on recovery (TR-SW03).

**Testability**
- Control logic modules decoupled from hardware drivers for host-side unit testing via PlatformIO test runner (TR-SW05).

**WiFi security**
- WPA2 minimum; WPA3 preferred (TR-NW01).
- HTTPS on the web interface is **not implemented**; see §6 Open Issue #4 for the accepted threat model (TR-NW04).

**Manual override detection**
- Mechanism for detecting manual window override via RRK-3 opto-isolated feedback input; software response and calibration cycle on resumption of control (FR-M08–FR-M11, Open Issue #1).

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
| T1  | Watchdog / Heartbeat | Highest           | 1    | Hardware watchdog kick; HB LED toggling |
| T2  | Relay Controller     | High              | 1    | Relay GPIO; window state machines; dwell timers; mutual exclusion; manual override detection |
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
- Must never be starved by lower-priority tasks; its liveness confirms the whole system is running.
- Can be implemented as a FreeRTOS software timer callback rather than a full task.
- **Synchronization:** none — no shared data access; timer callback requires no additional primitives.

---

#### T2 — Relay Controller

**Priority:** High | **Core:** 1

- Sole owner of all 6 relay GPIO output pins (OPEN/CLOSE for M1, M2, M3); no other task may assert relay signals directly.
- All actuation requests arrive via command queue Q1 (from T3, T6, T8, T11, T12).
- Runs the per-channel window state machine: `CLOSED` → `MOVING` → `OPEN` and reverse.
- Enforces OPEN + CLOSE mutual exclusion on each channel before asserting any relay.
- Manages dwell timers: minimum open-dwell and close-dwell per channel before accepting the next command.
- Monitors the RRK-3 opto-isolated feedback input; detects manual override and notifies T6 and T9.
- **Synchronization:** receives Q1 (actuation commands); posts to Q3 (log events); sets EG1.MANUAL_OVERRIDE on feedback input trigger; sends TN3 to T6 (manual override detected).

---

#### T3 — Safety Monitor

**Priority:** High | **Core:** 1

- Wakes on notification from T4 whenever new wind data is available.
- Reads current wind speed and wind direction from T4.
- Compares against v_max threshold and wind direction exclusion zone (configuration from T4).
- Posts a `CLOSE_ALL` actuation command to T2 immediately when a threshold is exceeded.
- Posts a `RESUME` notification to T6 when conditions return to safe limits.
- Always active, independent of Automatic / Standby operating mode.
- Priority equal to T2; must preempt T6 to ensure a safety response is never delayed by climate logic.
- **Synchronization:** wakes on TN1 (from T4, new wind data); acquires MX2 to read wind speed and direction; posts to Q1 (CLOSE_ALL or RESUME actuation command); sets/clears EG1.WIND_OVERRIDE; posts to Q3 (log events).

---

#### T4 — Data Manager

**Priority:** Medium-high | **Core:** 1

T4 is the single source of truth for all runtime data and configuration. All tasks that need to read or write system state do so through T4. This eliminates distributed per-variable mutexes and provides a single serialisation point for NVS persistence.

**Configuration settings**
- Holds all configurable parameters in RAM: setpoints (T_min, T_max, RH_min, RH_max), wind thresholds, dwell times, hysteresis values, WiFi credentials, PIN hashes, display language, session timeout, WiFi AP timeout.
- Accepts write requests from T8 (UI) and T11 (web server); validates range before accepting.
- Persists changed settings to NVS flash immediately on write.
- Loads all settings from NVS on startup; applies defined defaults for any missing keys.

**Current measurement data**
- Holds the most recent readings: temperature (T), relative humidity (RH), wind speed, wind direction.
- Updated by T5 after each successful Modbus poll cycle.
- Read by T3, T6, T8, T9, T11, T12; no task accesses sensor data except through T4.

**Measurement history ring buffers**
- Maintains a separate ring buffer for each measured quantity: T, RH, wind speed, wind direction.
- Each entry contains: timestamp and measured value.
- Ring buffer depth: to be defined when the data model is finalised.
- Read by T8 (display history), T9 (periodic log snapshots), T11 (web trend view), T12 (MQTT history).

**Operating state**
- Holds current operating mode (Automatic / Standby / Wind-override / Manual-override).
- Holds current session state (Normal / Farmer / Admin).
- Updated by T8 and T11; read by any task that gates behaviour on mode or session.
- **Synchronization:** acquires MX1 (I2C) to read DS3231 RTC; holds MX2 while writing current measurement data; holds MX3 while writing ring buffer entries; holds MX4 while reading or writing configuration settings; receives Q4 (config/state updates from T8 and T11); receives Q6 (sensor readings from T5); sends TN1 to T3 after writing new wind data; sends TN2 to T6 after writing new sensor data.

---

#### T5 — Sensor Poll

**Priority:** Medium-high | **Core:** 1

- Modbus RTU master on UART1 with SIT65HVD08P transceiver; manages DE/RE direction control pin.
- Polls SenseCAP S200 (wind speed + direction) and FG6485A (T + RH) on a configurable interval (default 60 s).
- On successful read: writes new values to T4; T4 then notifies T3 and T6.
- On fault (timeout, CRC error, out-of-range value): posts a sensor fault event to T9 (logger) and triggers alarm display via T8.
- **Synchronization:** posts to Q6 (sensor readings to T4); sets/clears EG1.SENSOR_FAULT_T and EG1.SENSOR_FAULT_W; posts to Q3 (log events); no mutexes held — T4 owns all measurement storage.

---

#### T6 — Climate Control

**Priority:** Medium | **Core:** 1

- Wakes on notification from T4 that new sensor data is available.
- Reads current T and RH from T4; reads setpoints and hysteresis values from T4.
- Evaluates temperature and humidity against setpoints with hysteresis bands.
- Runs conflict resolution algorithm when T and RH demand opposing window actions.
- Posts open/close actuation commands to T2 via command queue.
- Checks operating mode from T4 before acting; inhibited in Standby, Wind-override, and Manual-override states.
- **Synchronization:** wakes on TN2 (from T4, new sensor data); acquires MX2 to read current T and RH; acquires MX4 to read setpoints and hysteresis; reads EG1 (WIND_OVERRIDE, MANUAL_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W) before issuing any command; posts to Q1 (actuation commands); receives TN3 (from T2, manual override detected); clears EG1.MANUAL_OVERRIDE after calibration cycle completes; posts to Q3 (log events).

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

- Manages the LCD1602 display via I2C (shared bus with RTC).
- Renders the main status screen: T, RH, wind speed and direction, window states, operating mode, active session, active alarms.
- Runs the menu finite state machine (FSM); navigation depth ≤ 4 key presses from the main screen to any first-level setting.
- Manages session state: PIN entry via keyboard, session timeout, PIN validation against T4.
- Posts validated configuration changes and mode changes to T4.
- Receives WiFi status updates from T10 and displays AP active / client connected / IP address.
- **Synchronization:** acquires MX1 (I2C) to write LCD; acquires MX2 to read current measurements for display refresh; acquires MX3 to read ring buffers for history view; acquires MX4 to read configuration for settings screens; receives Q2 (key events from T7); receives Q5 (network status from T10); reads EG1 (alarm flags for display and alarm indication); posts to Q4 (config/mode updates to T4); posts to Q1 (manual window commands); posts to Q3 (log events: mode changes, setpoint changes, session events).

---

#### T9 — Event Logger

**Priority:** Low | **Core:** 1

- Receives log events from all tasks via a dedicated queue; senders post and return immediately.
- Serialises all writes to the NVS ring buffer and, when present, to the SD card.
- The queue decouples log I/O from higher-priority tasks; no task is blocked by log write latency.
- Queue overflow policy: drop-oldest is preferred over drop-newest (recent events are more operationally relevant in an overflow situation).
- Writes periodic sensor-value snapshots by reading current measurement data from T4 at a configurable interval.
- **Synchronization:** receives Q3 (log events from all tasks); acquires MX3 to read ring buffers for periodic sensor snapshots; no I2C or GPIO access.

---

#### T10 — Network Manager

**Priority:** Low | **Core:** 0

- Manages WiFi AP lifecycle: enable on admin command from T8 or T11; automatic shutdown after configurable timeout.
- Manages WiFi client: connect to configured SSID; monitor connection; reconnect on drop; supports DHCP and static IP.
- Posts connection state changes (connected / disconnected / assigned IP) to T8 for display.
- Triggers NTP time synchronisation when a client connection is established.
- Runs on Core 0 alongside the ESP32-S3 internal WiFi stack.
- **Synchronization:** posts to Q5 (network status to T8); posts to Q4 (NTP sync trigger to T4 on client connection); posts to Q3 (log events); no mutexes held.

---

#### T11 — Web Server

**Priority:** Low | **Core:** 0

- Serves HTML, CSS, and JavaScript from LittleFS via ESPAsyncWebServer (callback-driven).
- Applies the same three-state session model (Normal / Farmer / Admin) and PIN codes as T8.
- Reads configuration and current measurement data from T4; posts validated setting changes to T4.
- Posts actuation commands to T2 (manual window commands from web UI).
- Available on both WiFi AP and WiFi client interfaces simultaneously.
- Authentication required before any page is served or any setting is changed.
- **Synchronization:** acquires MX5 (LittleFS) to serve HTML files; reads EG1.OTA_IN_PROGRESS before serving files (defers requests while OTA is active); acquires MX2 to read current measurements; acquires MX4 to read configuration; posts to Q4 (validated config/state updates to T4); posts to Q1 (actuation commands from web UI); posts to Q3 (log events).

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
- Writes incoming firmware image to the inactive flash bank (A or B).
- Writes incoming LittleFS image to the inactive web-files slot.
- On successful write: marks the inactive bank active and triggers a controlled system restart.
- Implements 3-consecutive-fail rollback: if the new firmware fails to complete startup 3 times, the previous bank is restored as active and the system boots the known-good version.
- Firmware and web-file updates belonging to the same release must both be applied in the same update session before either is activated.
- **Synchronization:** acquires MX5 (LittleFS) exclusively during web-file write — T11 is blocked from serving HTML while this is held; sets EG1.OTA_IN_PROGRESS on start, clears on completion or failure; posts to Q3 (log events).

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

  T11 Web ─────── actuation commands ─────────────► T2 Relay Controller
  T12 MQTT ───── actuation commands ──────────────► T2 Relay Controller

  T11 Web ─────── OTA trigger ────────────────────► T13 OTA
```

### 4.6 Synchronization Primitives

#### 4.6.1 Mutexes

FreeRTOS mutexes (`xSemaphoreCreateMutex`) implement priority inheritance, which mitigates priority inversion when a high-priority task (e.g. T3) waits on a mutex held by a lower-priority task.

| ID  | Name                      | Protects                                                                 | Writers                       | Readers                              |
|-----|---------------------------|--------------------------------------------------------------------------|-------------------------------|--------------------------------------|
| MX1 | I2C bus                   | Shared I2C bus (SDA/SCL) — LCD display and DS3231 RTC on the same wires | T8 (LCD write), T4 (RTC read) | —                                    |
| MX2 | Current measurement data  | Latest T, RH, wind speed, wind direction values in T4                   | T4 (on write from T5)         | T3, T6, T8, T9, T11, T12            |
| MX3 | Measurement ring buffers  | History ring buffers for T, RH, wind speed, wind direction in T4        | T4 (on write from T5)         | T8, T9, T11, T12                    |
| MX4 | Configuration settings    | All configurable parameters in T4                                        | T4 (on validated write from Q4) | T3, T6, T8, T11, T12             |
| MX5 | LittleFS filesystem       | LittleFS partition (HTML and web asset files)                            | T13 (OTA web-file write)      | T11 (serving HTML files)            |

> **MX2 and MX3 are separate** to ensure T3 (safety-critical) is never delayed by a long ring-buffer read in T9 or T11. T3 only acquires MX2 (current values); it never acquires MX3.

> **NVS flash** (ESP-IDF NVS API) is thread-safe internally and requires no application-level mutex. T4 is the only task that writes configuration settings to NVS; T9 is the only task that writes log entries to NVS. Their NVS namespaces are distinct.

#### 4.6.2 Queues

FreeRTOS queues (`xQueueCreate`) are thread-safe by design. All queue operations are non-blocking for senders where noted, using `xQueueSend` with a timeout of zero or a short value.

| ID | Name                        | Direction   | Senders                                    | Receiver | Item                     | Notes                                          |
|----|-----------------------------|-------------|--------------------------------------------|----------|--------------------------|------------------------------------------------|
| Q1 | Actuation command queue     | → T2        | T3, T6, T8, T11, T12                       | T2       | Actuation command struct | T3 posts with highest urgency; never blocking  |
| Q2 | Key event queue             | → T8        | T7                                         | T8       | Key code                 | Depth to match max burst; T7 drops on full     |
| Q3 | Log event queue             | → T9        | T2, T3, T5, T6, T8, T10, T11, T12, T13    | T9       | Log event struct         | Generous depth; drop-oldest on overflow        |
| Q4 | Config / state update queue | → T4        | T8, T11, T10                               | T4       | Config update struct     | T4 validates range; persists to NVS on accept  |
| Q5 | Network status queue        | → T8        | T10                                        | T8       | Network status struct    | Small depth (1–2); latest status always relevant |
| Q6 | Sensor reading queue        | → T4        | T5                                         | T4       | Sensor reading struct    | Depth 1; overwrite semantics — only latest matters |

#### 4.6.3 Task Notifications

FreeRTOS task notifications (`xTaskNotifyGive` / `xTaskNotify`) are used for point-to-point signalling where a full queue is unnecessary. They are faster and consume less RAM than a binary semaphore.

| ID  | Sender | Receiver | Trigger                                      | Purpose                                                        |
|-----|--------|----------|----------------------------------------------|----------------------------------------------------------------|
| TN1 | T4     | T3       | New wind data written to T4                  | Wake T3 immediately to re-evaluate wind safety conditions      |
| TN2 | T4     | T6       | New sensor data (T or RH) written to T4      | Wake T6 to re-evaluate climate control decisions               |
| TN3 | T2     | T6       | Manual override detected on RRK-3 feedback   | T6 transitions to Manual-override state and begins calibration |
| TN4 | T10    | T4       | WiFi client connection established           | T4 triggers NTP synchronisation and updates system time        |

#### 4.6.4 Event Group — System State Flags

A single FreeRTOS event group (`xEventGroupCreate`) holds all system-wide boolean state flags. Any task may read any flag at any time without blocking; setting and clearing is done only by the designated owner task.

**Event group: EG1 — System State**

| Bit | Flag name          | Set by | Cleared by | Read by                 | Meaning when set                                         |
|-----|--------------------|--------|------------|-------------------------|----------------------------------------------------------|
| 0   | WIND_OVERRIDE      | T3     | T3         | T6, T8, T11, T12        | Wind safety threshold exceeded; all windows being closed |
| 1   | MANUAL_OVERRIDE    | T2     | T6         | T3, T6, T8, T11         | Manual window operation detected on RRK-3 feedback       |
| 2   | SENSOR_FAULT_T     | T5     | T5         | T6, T8, T9              | Temperature/humidity sensor fault active                  |
| 3   | SENSOR_FAULT_W     | T5     | T5         | T3, T8, T9              | Wind sensor fault active; T3 treats wind as worst-case   |
| 4   | OTA_IN_PROGRESS    | T13    | T13        | T11                     | OTA update active; T11 defers LittleFS file requests     |

> **T3 and SENSOR_FAULT_W:** when the wind sensor fault flag is set, T3 shall treat the wind condition as exceeding all thresholds (safe-fail: close all windows) until the fault clears.

#### 4.6.5 Primitive Cross-reference by Task

| Task | Acquires (mutex) | Posts to (queue) | Receives from (queue) | Sends (notification) | Receives (notification) | Reads/Sets (event group) |
|------|-----------------|------------------|-----------------------|----------------------|-------------------------|--------------------------|
| T1   | —               | —                | —                     | —                    | —                       | —                        |
| T2   | —               | Q3               | Q1                    | TN3 → T6             | —                       | Sets EG1.MANUAL_OVERRIDE |
| T3   | MX2             | Q1, Q3           | —                     | —                    | TN1 ← T4               | Sets/clears EG1.WIND_OVERRIDE; reads EG1.SENSOR_FAULT_W |
| T4   | MX1, MX2, MX3, MX4 | —            | Q4, Q6                | TN1 → T3, TN2 → T6   | TN4 ← T10              | —                        |
| T5   | —               | Q3, Q6           | —                     | —                    | —                       | Sets/clears EG1.SENSOR_FAULT_T, EG1.SENSOR_FAULT_W |
| T6   | MX2, MX4        | Q1, Q3           | —                     | —                    | TN2 ← T4, TN3 ← T2    | Reads EG1 (all); clears EG1.MANUAL_OVERRIDE |
| T7   | —               | Q2               | —                     | —                    | —                       | —                        |
| T8   | MX1, MX2, MX3, MX4 | Q1, Q3, Q4  | Q2, Q5                | —                    | —                       | Reads EG1 (all)          |
| T9   | MX3             | —                | Q3                    | —                    | —                       | —                        |
| T10  | —               | Q3, Q4, Q5       | —                     | TN4 → T4             | —                       | —                        |
| T11  | MX2, MX4, MX5  | Q1, Q3, Q4       | —                     | —                    | —                       | Reads EG1.OTA_IN_PROGRESS |
| T12  | MX2, MX4        | Q1, Q3, Q4       | —                     | —                    | —                       | —                        |
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
- SenseCAP S200 (Modbus address 1): reads wind speed register and wind direction register each cycle.
- FG6485A (Modbus address 2): reads temperature register and humidity register each cycle.
- Poll interval configurable via NVS (default 60 s); minimum interval to be determined during integration testing.

**Fault detection and response:**

| Fault condition | Detection | Response |
|----------------|-----------|----------|
| No response within timeout | Timer expiry after transmit | Set EG1.SENSOR_FAULT_T or EG1.SENSOR_FAULT_W; post log event to Q3 |
| CRC error | Modbus frame validation | Retry once; on second failure set fault flag and post log event |
| Out-of-range value | Sanity check on decoded value | Discard reading; set fault flag; post log event |
| Fault clears | Successful read after fault | Clear fault flag; post log event |

Link to FRS requirements: FR-S04 (sensor fault detection), FR-W03 (wind sensor fault).

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
         │                manual detected (T2 → TN3)
         │                                  │
         │                        [Manual-override]
         │                                  │
         │              calibration complete AND wind safe
         │                                  │
         └──────────────────────────────────┘
```

**Window state machine (per channel M1, M2, M3):**

| State   | Entry condition | Exit condition | Action |
|---------|----------------|----------------|--------|
| CLOSED  | Power-on; close command completes dwell | Open command received | Assert OPEN relay |
| MOVING  | Relay energised | Dwell timer expires | Deassert relay; transition to OPEN or CLOSED |
| OPEN    | Open dwell complete | Close command received | Assert CLOSE relay |

- Open-dwell and close-dwell times are configurable per channel via NVS.
- OPEN + CLOSE mutual exclusion is enforced in T2 before asserting any relay output.

**Climate setpoints and hysteresis:**
- T_min, T_max: temperature range for window open/close (configurable, farmer level).
- RH_min, RH_max: humidity range for window open/close (configurable, farmer level).
- Hysteresis band on each setpoint prevents rapid toggling near threshold.
- Graduated ventilation: windows opened in steps proportional to deviation from setpoint (FR-C09, FR-C10).

**Conflict resolution (FR-CR01–FR-CR04):**
When temperature demands OPEN and humidity demands CLOSE (or vice versa), the conflict resolution algorithm selects the safer action:
1. Wind safety always overrides both (T3 issues CLOSE_ALL regardless of climate demand).
2. When temperature and humidity conflict, priority is configurable: default is temperature priority.
3. The active conflict and the resolution applied are logged to Q3.

**Manual override detection (FR-M08–FR-M11):**
- T2 detects a state change on the RRK-3 opto-isolated feedback input.
- T2 sets EG1.MANUAL_OVERRIDE and sends TN3 to T6.
- T6 transitions to Manual-override state; climate commands are inhibited.
- On operator resumption: T6 initiates a calibration cycle (close all windows to re-synchronise position estimate), then returns to Automatic mode — unless WIND_OVERRIDE is active.

---

### 5.3 Event Log Manager

**Implemented by:** T9 (Event Logger)

**Log entry structure:**

| Field | Type | Description |
|-------|------|-------------|
| timestamp | uint32 (Unix epoch) | Time of event; marked invalid if no time source has synced |
| event_type | uint8 enum | Category: SENSOR, RELAY, MODE_CHANGE, SETPOINT, SESSION, ALARM, SYSTEM |
| initiator | uint8 enum | SYSTEM, USER_FARMER, USER_ADMIN, MQTT, WEB |
| channel | uint8 | Motor channel (M1/M2/M3) or 0 for non-motor events |
| value_a | int16 | Optional: sensor value or setpoint (scaled, e.g. °C × 10) |
| value_b | int16 | Optional: second sensor value or threshold |
| reserved | uint8[2] | Padding to maintain fixed record size |

**Storage:**
- Primary: SD card (FAT32), when present. Circular file with header tracking write pointer.
- Fallback: NVS dedicated log namespace. Ring buffer of minimum 1000 fixed-size entries; oldest entry overwritten when full.
- T9 checks SD card presence on startup and on each write cycle; falls back to NVS if card is absent or returns an error (FR-LG07, FR-LG08).

**Retrieval:**
- Web interface: paginated log view, filterable by event type and time range (FR-LG05).
- USB serial diagnostic port: raw log dump command.

**Queue management:**
- All tasks post to Q3 non-blocking; T9 is the sole consumer.
- Overflow policy: drop-oldest entry in the queue (most recent events preserved).

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

- PINs are stored in NVS as salted SHA-256 hashes; plain-text is never stored or transmitted.
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

The web interface applies the same three-state model and the same PIN codes as the local keyboard interface.

---

### 5.5 Local User Interface

**Implemented by:** T7 (Keypad Scan) and T8 (UI / Display)

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

**Alarm display:**
- Sensor fault (T or wind): displayed on line 2 with blinking indicator.
- Wind safety override active: displayed prominently with wind speed reading.
- Manual override detected: displayed on line 2.
- WiFi AP active: displayed on line 2 with IP address if in client mode.

---

### 5.6 WiFi — Access Point Mode

**Implemented by:** T10 (Network Manager)

- WiFi AP mode is **mandatory** (Must have).
- The AP does not start automatically on boot; it is enabled by the administrator via the local keyboard menu or web interface.
- AP SSID and password are configurable by the administrator (stored in NVS; password hashed).
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
- On client connection: T10 sends TN4 to T4, triggering NTP synchronisation.

---

### 5.8 Web Interface

**Implemented by:** T11 (Web Server)

**Technology:**
- Web server: ESPAsyncWebServer (callback-driven, non-blocking).
- HTML, CSS, and JavaScript files stored in LittleFS partition on ESP32-S3 flash, separate from the firmware binary.
- The web interface mirrors the local keyboard interface exactly: same three operating states (§5.4), same PIN codes, same parameter visibility rules.

**Access control:**
- Authentication required before any page is served or any setting is changed (FR-NW06).
- Session cookie issued after successful PIN entry; cookie invalidated on logout or session timeout.
- HTTPS is **not implemented** (TR-NW04 — not feasible on target hardware; threat model accepted, see §6 Open Issue #4).

**Pages:**
- Dashboard: live T, RH, wind speed, wind direction, window states, operating mode, active alarms.
- Settings: farmer parameters (farmer/admin session) and admin parameters (admin session only).
- Log viewer: paginated event log, filterable by event type and time range.
- OTA update: firmware and web-file upload (admin session only).
- Network: WiFi AP and client configuration (admin session only).

**MQTT client (optional, FR-MQ01–FR-MQ05):**
- Configured via the web interface (admin session).
- Publishes: T, RH, wind speed, wind direction, window states, operating mode, alarm flags.
- Subscribes to: OPEN/CLOSE commands per channel, mode change commands.
- Authentication: username/password or client certificate, configurable by administrator.

---

### 5.9 OTA Firmware Update

**Implemented by:** T13 (OTA)

**Flash partition layout:**

| Partition | Role |
|-----------|------|
| Bank A | Firmware image slot A |
| Bank B | Firmware image slot B |
| NVS | Configuration settings (persistent across updates) |
| LittleFS | HTML and web asset files for the administrative interface |

- The system boots from whichever bank is marked **active** in the partition table.

**Firmware update procedure:**
1. Administrator uploads new firmware image via web interface (admin session required).
2. T13 writes the image to the inactive bank.
3. On successful write and integrity check: inactive bank is marked active.
4. System reboots into the new firmware.

**Failsafe rollback:**
- If the newly booted firmware fails to complete its startup health check 3 consecutive times, the previous bank is automatically restored as active and the system reboots into the known-good firmware.
- Rollback events are logged.

**Web file update:**
- HTML and web asset files in LittleFS are updated separately via the same OTA mechanism.
- MX5 is held exclusively during the write; T11 defers file-serve requests while EG1.OTA_IN_PROGRESS is set.

**Combined firmware + web file update:**
- When a release includes both firmware and UI changes, both packages must be transferred and verified before either is activated.
- T13 does not switch the active bank until both writes have completed successfully.

---

### 5.10 NVS Configuration Storage Layout

**Managed by:** T4 (Data Manager)

NVS uses ESP-IDF namespaces to separate configuration domains. All keys use UTF-8 strings of ≤ 15 characters (ESP-IDF NVS limit).

| Namespace | Key examples | Description |
|-----------|-------------|-------------|
| `climate` | `t_min`, `t_max`, `rh_min`, `rh_max`, `hyst_t`, `hyst_rh` | Temperature and humidity setpoints and hysteresis |
| `wind` | `v_max`, `dir_excl_low`, `dir_excl_high` | Wind speed threshold and direction exclusion zone |
| `motor` | `dwell_open_m1`, `dwell_close_m1`, … `dwell_close_m3` | Per-channel dwell times (ms) |
| `access` | `pin_farmer_hash`, `pin_admin_hash`, `pin_salt`, `lockout_count`, `lockout_time` | PIN hashes, lockout configuration |
| `wifi` | `ssid`, `psk_hash`, `ap_ssid`, `ap_psk`, `ip_mode`, `ip_addr`, `ip_mask`, `ip_gw`, `ip_dns` | WiFi client and AP credentials and network settings |
| `mqtt` | `broker_url`, `port`, `username`, `password_hash`, `topic_prefix`, `interval` | MQTT broker connection and publish settings |
| `system` | `poll_interval`, `session_timeout`, `ap_timeout`, `lang`, `log_pointer` | System-wide configuration |
| `log` | Ring buffer entries (binary blob, fixed record size) | Event log fallback when SD card absent |

**Default values:**
- Applied on first boot (no NVS key present) or after factory reset.
- Factory reset clears all NVS namespaces and restores defaults; requires deliberate admin action and is logged.

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

**Manual override handling:**
- T2 detects a change on the RRK-3 feedback input and sets EG1.MANUAL_OVERRIDE.
- T6 receives TN3, transitions to Manual-override state, inhibits all climate commands, and logs the event.
- After the operator releases manual control: T6 initiates a calibration cycle (CLOSE_ALL on all channels, wait for dwell timers, resume Automatic mode).
- If WIND_OVERRIDE is also active when manual override clears, the calibration cycle is deferred until WIND_OVERRIDE also clears.

---

## 6. Open Issues

| # | Issue | Owner | Status |
|---|-------|-------|--------|
| 1 | **Motor feedback signal — software response** — The exact nature of the RRK-3 feedback signal is undefined (see hardware open issue #1). Once the signal is characterised, the software response in T2 (edge-triggered vs. level-triggered detection, debounce duration) and the T6 calibration cycle behaviour must be defined and implemented. | Software engineer | Blocked on HW issue #1 |
| 2 | **Ring buffer depth** — The depth of the T4 measurement history ring buffers (T, RH, wind speed, wind direction) is to be defined when the data model and RAM budget are finalised. Depth determines RAM usage and the length of history available for web trend view, MQTT history, and periodic log snapshots. | Software engineer | Open |
| 3 | **NTP timezone handling** — The firmware must handle CET/CEST (Europe/Amsterdam) DST transitions automatically. The implementation approach (POSIX TZ string, manual transition table, or DCF77 DST flag) depends on the time source decision (hardware open issue #7). | Software engineer | Blocked on HW issue #7 |
| 4 | **Web interface HTTPS — not implemented (TR-NW04 accepted)** — TLS termination on the ESP32-S3 is not feasible: the RAM and CPU overhead of a TLS stack would leave insufficient headroom for concurrent real-time tasks. **Decision:** HTTPS will not be implemented. **Accepted threat model:** the web interface is served over plain HTTP. The risk is mitigated by the following constraints: (a) the WiFi AP is disabled by default and enabled only on explicit admin command; (b) the AP has a configurable automatic timeout; (c) the controller is intended for use on a private, physically controlled greenhouse network and is not exposed to the public internet; (d) all credentials are stored as salted hashes and are never transmitted in plaintext; (e) session cookies are short-lived and invalidated on logout or timeout. This residual risk is accepted by the project owner. | Software engineer | **Closed — accepted** |
| 5 | **MQTT authentication method** — Username/password or client certificate authentication for the MQTT client. The choice depends on the broker environment; both options should be configurable. | Software engineer | Open |

---

*End of document — version 0.1 draft*
