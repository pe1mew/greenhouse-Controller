# Task Structure

## Greenhouse Ventilation Controller — FreeRTOS Task Design

| Field        | Value                                    |
|--------------|------------------------------------------|
| Document     | Task Structure                           |
| Project      | Greenhouse Ventilation Controller        |
| Version      | 0.1 (draft)                              |
| Date         | 2026-03-29                               |
| Status       | Draft                                    |
| Related docs | `technicalDesignSpecification.md`        |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Task Overview](#2-task-overview)
3. [Task Descriptions](#3-task-descriptions)
4. [Inter-task Communication](#4-inter-task-communication)
5. [Core Assignment](#5-core-assignment)
6. [Design Notes](#6-design-notes)
7. [Synchronization Primitives](#7-synchronization-primitives)

---

## 1. Introduction

This document defines the FreeRTOS task structure for the greenhouse ventilation controller firmware. Each logical function of the system is assigned to a dedicated task with a defined priority, core assignment, and communication interface.

This document is the authoritative reference for the software task architecture. The Technical Design Specification (TDS §5.2) references this document.

---

## 2. Task Overview

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

---

## 3. Task Descriptions

### T1 — Watchdog / Heartbeat

**Priority:** Highest | **Core:** 1

- Kicks the hardware watchdog timer at a fixed interval (e.g. every 500 ms)
- Toggles the HB LED: 1 Hz in normal operation; 4 Hz during startup / initialisation
- Must never be starved by lower-priority tasks; its liveness confirms the whole system is running
- Can be implemented as a FreeRTOS software timer callback rather than a full task
- **Synchronization:** none — no shared data access; timer callback requires no additional primitives

---

### T2 — Relay Controller

**Priority:** High | **Core:** 1

- Sole owner of all 6 relay GPIO output pins (OPEN/CLOSE for M1, M2, M3); no other task may assert relay signals directly
- All actuation requests arrive via a command queue (from T3, T6, T8, T11, T12)
- Runs the per-channel window state machine: `CLOSED` → `MOVING` → `OPEN` and reverse
- Enforces OPEN + CLOSE mutual exclusion on each channel before asserting any relay
- Manages dwell timers: minimum open-dwell and close-dwell per channel before accepting the next command
- Monitors the RRK-3 opto-isolated feedback input; detects manual override and notifies T6 and T9
- **Synchronization:** receives Q1 (actuation commands); posts to Q3 (log events); sets EG1.MANUAL_OVERRIDE on feedback input trigger; sends TN3 to T6 (manual override detected)

---

### T3 — Safety Monitor

**Priority:** High | **Core:** 1

- Wakes on notification from T4 whenever new wind data is available
- Reads current wind speed and wind direction from T4
- Compares against v_max threshold and wind direction exclusion zone (configuration from T4)
- Posts a `CLOSE_ALL` actuation command to T2 immediately when a threshold is exceeded
- Posts a `RESUME` notification to T6 when conditions return to safe limits
- Always active, independent of Automatic / Standby operating mode
- Priority equal to T2; must preempt T6 to ensure a safety response is never delayed by climate logic
- **Synchronization:** wakes on TN1 (from T4, new wind data); acquires MX2 to read wind speed and direction; posts to Q1 (CLOSE_ALL or RESUME actuation command); sets/clears EG1.WIND_OVERRIDE; posts to Q3 (log events)

---

### T4 — Data Manager

**Priority:** Medium-high | **Core:** 1

T4 is the single source of truth for all runtime data and configuration. All tasks that need to read or write system state do so through T4. This eliminates distributed per-variable mutexes and provides a single serialisation point for NVS persistence.

**Configuration settings**
- Holds all configurable parameters in RAM: setpoints (T_min, T_max, RH_min, RH_max), wind thresholds, dwell times, hysteresis values, WiFi credentials, PIN hashes, display language, session timeout, WiFi AP timeout
- Accepts write requests from T8 (UI) and T11 (web server); validates range before accepting
- Persists changed settings to NVS flash immediately on write
- Loads all settings from NVS on startup; applies defined defaults for any missing keys

**Current measurement data**
- Holds the most recent readings: temperature (T), relative humidity (RH), wind speed, wind direction
- Updated by T5 after each successful Modbus poll cycle
- Read by T3, T6, T8, T9, T11, T12; no task accesses sensor data except through T4

**Measurement history ring buffers**
- Maintains a separate ring buffer for each measured quantity: T, RH, wind speed, wind direction
- Each entry contains: timestamp and measured value
- Ring buffer depth: **to be defined when the data model is finalised**
- Read by T8 (display history), T9 (periodic log snapshots), T11 (web trend view), T12 (MQTT history)

**Operating state**
- Holds current operating mode (Automatic / Standby / Wind-override / Manual-override)
- Holds current session state (Normal / Farmer / Admin)
- Updated by T8 and T11; read by any task that gates behaviour on mode or session
- **Synchronization:** acquires MX1 (I2C) to read DS3231 RTC; holds MX2 while writing current measurement data; holds MX3 while writing ring buffer entries; holds MX4 while reading or writing configuration settings; receives Q4 (config/state updates from T8 and T11); receives Q6 (sensor readings from T5); sends TN1 to T3 after writing new wind data; sends TN2 to T6 after writing new sensor data

---

### T5 — Sensor Poll

**Priority:** Medium-high | **Core:** 1

- Modbus RTU master on UART1 with SIT65HVD08P transceiver; manages DE/RE direction control pin
- Polls SenseCAP S200 (wind speed + direction) and FG6485A (T + RH) on a configurable interval (default 60 s)
- On successful read: writes new values to T4; T4 then notifies T3 and T6
- On fault (timeout, CRC error, out-of-range value): posts a sensor fault event to T9 (logger) and triggers alarm display via T8
- **Synchronization:** posts to Q6 (sensor readings to T4); sets/clears EG1.SENSOR_FAULT_T and EG1.SENSOR_FAULT_W; posts to Q3 (log events); no mutexes held — T4 owns all measurement storage

---

### T6 — Climate Control

**Priority:** Medium | **Core:** 1

- Wakes on notification from T4 that new sensor data is available
- Reads current T and RH from T4; reads setpoints and hysteresis values from T4
- Evaluates temperature and humidity against setpoints with hysteresis bands
- Runs conflict resolution algorithm when T and RH demand opposing window actions
- Posts open/close actuation commands to T2 via command queue
- Checks operating mode from T4 before acting; inhibited in Standby, Wind-override, and Manual-override states
- **Synchronization:** wakes on TN2 (from T4, new sensor data); acquires MX2 to read current T and RH; acquires MX4 to read setpoints and hysteresis; reads EG1 (WIND_OVERRIDE, MANUAL_OVERRIDE, SENSOR_FAULT_T, SENSOR_FAULT_W) before issuing any command; posts to Q1 (actuation commands); receives TN3 (from T2, manual override detected); clears EG1.MANUAL_OVERRIDE after calibration cycle completes; posts to Q3 (log events)

---

### T7 — Keypad Scan

**Priority:** Medium-high | **Core:** 1

- Scans the 4×4 keypad matrix every ~20 ms
- Applies software debounce
- Posts validated key-press events to T8 via queue
- Can be implemented as a FreeRTOS software timer callback rather than a full task
- **Synchronization:** posts to Q2 (key events to T8); no shared data; no mutexes required

---

### T8 — UI / Display

**Priority:** Medium | **Core:** 1

- Manages the LCD1602 display via I2C (shared bus with RTC)
- Renders the main status screen: T, RH, wind speed and direction, window states, operating mode, active session, active alarms
- Runs the menu finite state machine (FSM); navigation depth ≤ 4 key presses from the main screen to any first-level setting
- Manages session state: PIN entry via keyboard, session timeout, PIN validation against T4
- Posts validated configuration changes and mode changes to T4
- Receives WiFi status updates from T10 and displays AP active / client connected / IP address
- **Synchronization:** acquires MX1 (I2C) to write LCD; acquires MX2 to read current measurements for display refresh; acquires MX3 to read ring buffers for history view; acquires MX4 to read configuration for settings screens; receives Q2 (key events from T7); receives Q5 (network status from T10); reads EG1 (alarm flags for display and alarm indication); posts to Q4 (config/mode updates to T4); posts to Q1 (manual window commands); posts to Q3 (log events: mode changes, setpoint changes, session events)

---

### T9 — Event Logger

**Priority:** Low | **Core:** 1

- Receives log events from all tasks via a dedicated queue; senders post and return immediately
- Serialises all writes to the NVS ring buffer and, when present, to the SD card
- The queue decouples log I/O from higher-priority tasks; no task is blocked by log write latency
- Queue overflow policy: **to be decided** (drop oldest is generally preferred over drop newest)
- Writes periodic sensor-value snapshots by reading current measurement data from T4 at a configurable interval
- **Synchronization:** receives Q3 (log events from all tasks); acquires MX3 to read ring buffers for periodic sensor snapshots; no I2C or GPIO access

---

### T10 — Network Manager

**Priority:** Low | **Core:** 0

- Manages WiFi AP lifecycle: enable on admin command from T8 or T11; automatic shutdown after configurable timeout
- Manages WiFi client: connect to configured SSID; monitor connection; reconnect on drop; supports DHCP and static IP
- Posts connection state changes (connected / disconnected / assigned IP) to T8 for display
- Triggers NTP time synchronisation when a client connection is established
- Runs on Core 0 alongside the ESP32-S3 internal WiFi stack
- **Synchronization:** posts to Q5 (network status to T8); posts to Q4 (NTP sync trigger to T4 on client connection); posts to Q3 (log events); no mutexes held

---

### T11 — Web Server

**Priority:** Low | **Core:** 0

- Serves HTML, CSS, and JavaScript from LittleFS via ESPAsyncWebServer (callback-driven)
- Applies the same three-state session model (Normal / Farmer / Admin) and PIN codes as T8
- Reads configuration and current measurement data from T4; posts validated setting changes to T4
- Posts actuation commands to T2 (manual window commands from web UI)
- Available on both WiFi AP and WiFi client interfaces simultaneously
- Authentication required before any page is served or any setting is changed
- **Synchronization:** acquires MX5 (LittleFS) to serve HTML files; reads EG1.OTA_IN_PROGRESS before serving files (defers requests while OTA is active); acquires MX2 to read current measurements; acquires MX4 to read configuration; posts to Q4 (validated config/state updates to T4); posts to Q1 (actuation commands from web UI); posts to Q3 (log events)

---

### T12 — MQTT Client

**Priority:** Low | **Core:** 0

- Publishes current T, RH, wind speed, wind direction, window states, operating mode, and alarm status to the configured MQTT broker at a configurable interval
- Reads all published values from T4
- Subscribes to configured command topics; posts received actuation commands to T2 and setting changes to T4
- Active only when WiFi client is connected and an MQTT broker is configured
- **Synchronization:** acquires MX2 to read current measurements for publishing; acquires MX4 to read MQTT broker configuration; posts to Q1 (commands received via MQTT); posts to Q4 (settings received via MQTT); posts to Q3 (log events)

---

### T13 — OTA (on demand)

**Priority:** Low (spawned on demand) | **Core:** 0

- Activated via the web interface (T11)
- Writes incoming firmware image to the inactive flash bank (A or B)
- Writes incoming LittleFS image to the inactive web-files slot
- On successful write: marks the inactive bank active and triggers a controlled system restart
- Implements 3-consecutive-fail rollback: if the new firmware fails to complete startup 3 times, the previous bank is restored as active and the system boots the known-good version
- Firmware and web-file updates that belong to the same release must both be applied in the same update session before either is activated
- **Synchronization:** acquires MX5 (LittleFS) exclusively during web-file write — T11 is blocked from serving HTML while this is held; sets EG1.OTA_IN_PROGRESS on start, clears on completion or failure; posts to Q3 (log events)

---

## 4. Inter-task Communication

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

---

## 5. Core Assignment

| Core | Tasks | Rationale |
|------|-------|-----------|
| **Core 1 (Application)** | T1, T2, T3, T4, T5, T6, T7, T8, T9 | Real-time control, sensor I/O, and local UI; isolated from the WiFi stack |
| **Core 0 (Protocol)** | T10, T11, T12, T13 | WiFi stack, TCP/IP, and all network-facing tasks; the ESP32-S3 WiFi internals run on Core 0 |

---

## 6. Design Notes

**T4 as single source of truth**
All tasks read and write system state exclusively through T4. This avoids scattered mutexes across the codebase and provides a natural serialisation point for NVS writes. The interface to T4 should be a message queue (request/response) or a mutex-protected accessor API, depending on latency requirements for each caller.

**T2 as sole relay owner**
Only T2 may assert relay GPIO signals. The command queue into T2 is the single arbitration point for all actuation requests from T3, T6, T8, T11, and T12.

**T7 and T1 as timer callbacks**
If stack usage is low and execution time is short, T7 (keypad scan) and T1 (watchdog/heartbeat) can be implemented as FreeRTOS software timer callbacks on the shared timer service task rather than full tasks, reducing memory overhead.

**Ring buffer depth (T4)**
The history depth for the T, RH, wind speed, and wind direction ring buffers is to be defined when the data model is finalised. The depth determines RAM usage and the length of the history available for the web trend view, MQTT history, and periodic log snapshots.

**Queue overflow policy (T9)**
Drop-oldest is generally preferred over drop-newest for the event log queue: recent events are more operationally relevant than older ones in an overflow situation.

**OTA sequencing (T13)**
If an update release contains both firmware and web-file changes, both packages must be transferred and verified before either is activated. T13 shall not switch the active bank until both writes have completed successfully.

---

## 7. Synchronization Primitives

This section is the authoritative definition of all synchronization primitives used in the firmware. Each primitive is identified by a short ID that is referenced in the task descriptions above.

### 7.1 Mutexes

FreeRTOS mutexes (`xSemaphoreCreateMutex`) implement priority inheritance, which mitigates priority inversion when a high-priority task (e.g. T3) waits on a mutex held by a lower-priority task.

| ID  | Name                      | Protects                                                                 | Writers (hold for write)     | Readers (hold for read)              |
|-----|---------------------------|--------------------------------------------------------------------------|------------------------------|--------------------------------------|
| MX1 | I2C bus                   | Shared I2C bus (SDA/SCL) — LCD display and DS3231 RTC on the same wires | T8 (LCD write), T4 (RTC read) | —                                   |
| MX2 | Current measurement data  | Latest T, RH, wind speed, wind direction values in T4                   | T4 (on write from T5)        | T3, T6, T8, T9, T11, T12            |
| MX3 | Measurement ring buffers  | History ring buffers for T, RH, wind speed, wind direction in T4        | T4 (on write from T5)        | T8, T9, T11, T12                    |
| MX4 | Configuration settings    | All configurable parameters in T4                                        | T4 (on validated write from Q4) | T3, T6, T8, T11, T12             |
| MX5 | LittleFS filesystem       | LittleFS partition (HTML and web asset files)                            | T13 (OTA web-file write)     | T11 (serving HTML files)            |

> **MX2 and MX3 are separate** to ensure T3 (safety-critical) is never delayed by a long ring-buffer read in T9 or T11. T3 only acquires MX2 (current values); it never acquires MX3.

> **NVS flash** (ESP-IDF NVS API) is thread-safe internally and requires no application-level mutex. T4 is the only task that writes configuration settings to NVS; T9 is the only task that writes log entries to NVS. Their NVS namespaces are distinct.

---

### 7.2 Queues

FreeRTOS queues (`xQueueCreate`) are thread-safe by design. All queue operations are non-blocking for senders where noted, using `xQueueSend` with a timeout of zero or a short value.

| ID | Name                     | Direction      | Senders              | Receiver | Item                            | Notes                                              |
|----|--------------------------|----------------|----------------------|----------|---------------------------------|----------------------------------------------------|
| Q1 | Actuation command queue  | → T2           | T3, T6, T8, T11, T12 | T2       | Actuation command struct        | T3 posts with highest urgency; never blocking      |
| Q2 | Key event queue          | → T8           | T7                   | T8       | Key code                        | Depth to match max burst; T7 drops on full         |
| Q3 | Log event queue          | → T9           | T2, T3, T5, T6, T8, T10, T11, T12, T13 | T9 | Log event struct | Generous depth; drop-oldest on overflow           |
| Q4 | Config / state update queue | → T4        | T8, T11, T10         | T4       | Config update struct            | T4 validates range; persists to NVS on accept      |
| Q5 | Network status queue     | → T8           | T10                  | T8       | Network status struct           | Small depth (1–2); latest status always relevant   |
| Q6 | Sensor reading queue     | → T4           | T5                   | T4       | Sensor reading struct           | Depth 1; overwrite semantics — only latest matters |

---

### 7.3 Task Notifications

FreeRTOS task notifications (`xTaskNotifyGive` / `xTaskNotify`) are used for point-to-point signalling where a full queue is unnecessary. They are faster and consume less RAM than a binary semaphore.

| ID  | Sender | Receiver | Trigger                                      | Purpose                                                        |
|-----|--------|----------|----------------------------------------------|----------------------------------------------------------------|
| TN1 | T4     | T3       | New wind data written to T4                  | Wake T3 immediately to re-evaluate wind safety conditions      |
| TN2 | T4     | T6       | New sensor data (T or RH) written to T4      | Wake T6 to re-evaluate climate control decisions               |
| TN3 | T2     | T6       | Manual override detected on RRK-3 feedback   | T6 transitions to Manual-override state and begins calibration |
| TN4 | T10    | T4       | WiFi client connection established           | T4 triggers NTP synchronisation and updates system time        |

---

### 7.4 Event Group — System State Flags

A single FreeRTOS event group (`xEventGroupCreate`) holds all system-wide boolean state flags. Any task may read any flag at any time without blocking; setting and clearing is done only by the designated owner task.

**Event group: EG1 — System State**

| Bit | Flag name          | Set by | Cleared by | Read by                          | Meaning when set                                        |
|-----|--------------------|--------|------------|----------------------------------|---------------------------------------------------------|
| 0   | WIND_OVERRIDE      | T3     | T3         | T6, T8, T11, T12                 | Wind safety threshold exceeded; all windows being closed |
| 1   | MANUAL_OVERRIDE    | T2     | T6         | T3, T6, T8, T11                  | Manual window operation detected on RRK-3 feedback      |
| 2   | SENSOR_FAULT_T     | T5     | T5         | T6, T8, T9                       | Temperature/humidity sensor fault active                 |
| 3   | SENSOR_FAULT_W     | T5     | T5         | T3, T8, T9                       | Wind sensor fault active; T3 treats wind as worst-case  |
| 4   | OTA_IN_PROGRESS    | T13    | T13        | T11                              | OTA update active; T11 defers LittleFS file requests    |

> **T3 and SENSOR_FAULT_W:** when the wind sensor fault flag is set, T3 shall treat the wind condition as exceeding all thresholds (safe-fail: close all windows) until the fault clears.

---

### 7.5 Primitive Cross-reference by Task

| Task | Acquires (mutex) | Posts to (queue) | Receives from (queue) | Sends (notification) | Receives (notification) | Reads/Sets (event group) |
|------|-----------------|------------------|-----------------------|----------------------|-------------------------|--------------------------|
| T1   | —               | —                | —                     | —                    | —                       | —                        |
| T2   | —               | Q1 (recv), Q3   | Q1                    | TN3 → T6             | —                       | Sets EG1.MANUAL_OVERRIDE |
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

*End of document — version 0.1 draft*
