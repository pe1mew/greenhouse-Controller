# Software Test Plan
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Software Test Plan                             |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.4                                           |
| Date         | 2026-05-07                                    |
| Status       | Updated                                       |
| Related docs | `technicalSoftwareDesignSpecification.md`      |
|              | `functionalRequirementsSpecification.md`       |
|              | `tasks.md`                                    |

> **v0.3 → v0.4 change summary:**
> - §5 (IT-SP-003): poll interval default corrected to 30 s (TSDS §5.1 / FRS FR-S03, FR-CF07 — range 15–120 s, factory default 30 s).
> - §7 (IT-EL-006): expected result updated to reflect 60 s SD automount loop (T9 wakes every 60 s when SD absent). New §7.5 added with IT-EL-015 to IT-EL-018 covering timestamp-based file naming, ISO 8601 CSV timestamps, proactive free-space guard, and 60 s automount window.
> - §9.4 (IT-UI-012): updated to cover 6 status pages (STATUS_PAGES = 6); page 5 (motor states via `t2_get_window_states()`) added. New §9.8 (IT-UI-018): D-key manual page advance.
> - §12.4 (new): ST-WI-016 and ST-WI-017 for Log tab endpoints (`GET /api/log/files`, `GET /api/log/download`).
> - §19 Traceability Matrix: new test IDs added.
> - §20.7 timing table: `poll_interval_s` default corrected to 30 s.
>
> **v0.2 → v0.3 change summary:**
> - §20 (new): Automated Integration Test Infrastructure — harness architecture, fixture protocol, NVS write confirmation, serial patterns reference, eg1 bitmask, timing constants, and run instructions.
> - §3.2: Python dependency list updated (`pytest-timeout` added; env-var overrides documented).
> - §1.2: Automated test suite cross-reference note clarified.
> - Traceability matrix §19: minor wording alignment with §20.
>
> **v0.1 → v0.2 change summary:**
> - Added module codes DN (Day/Night), WS (Wind Safety Flags), RG (RGB LED).
> - §4: IT-FA-001 corrected (T13 spawned on-demand, 12 tasks at boot); IT-FA-013 added (EG1_BIT_CALIBRATING).
> - §5: UT-SP-010 added (sliding average).
> - §6: UT-CC-023 (night setpoints); UT-CC-024/025 (graduated ventilation); UT-CC-026/027 (conflict CR_RH_FIRST / CR_DEVIATION); UT-CC-028 (RH control disabled); §6.5 added.
> - §7: IT-EL-009/010 added (log entry field content).
> - §8: IT-AC-015/016 corrected (GPIO0 BOOT button, not jumper); IT-AC-017/018 added (staged recovery levels 2/3); IT-AC-019 added (web/keyboard session independence).
> - §9: IT-UI-012–016 added (page cycling, time page, manual time, wind-off warning, BOOT button LCD bar).
> - §11: IT-WC-006/007 added (geolocation, timezone immediate apply).
> - §12: ST-WI-001 corrected (public endpoints); ST-WI-012–015 added (public API, history freshness, sunrise/sunset).
> - §13: ST-OT-008 added (asset version mismatch warning).
> - §14: IT-NV-007 added (schema migration).
> - §15: IT-WD-010/011 added (alarm re-assert during guard ≤5 s; alarm at boot).
> - §16: ST-SE-003 corrected (public endpoints are intentional, not a failure).
> - §17 (new): Day/Night Management test cases UT-DN-001–IT-DN-007.
> - §18 (new): RGB Status LED test cases IT-RG-001–IT-RG-009.
> - §19 (renumbered from §17): Traceability Matrix updated with all new cases and FRS references.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Test Strategy](#2-test-strategy)
3. [Test Environment](#3-test-environment)
4. [Test Cases — Firmware Architecture](#4-test-cases--firmware-architecture)
5. [Test Cases — Sensor Polling](#5-test-cases--sensor-polling)
6. [Test Cases — Climate Control Logic](#6-test-cases--climate-control-logic)
7. [Test Cases — Event Log Manager](#7-test-cases--event-log-manager)
8. [Test Cases — Access Control and Session Management](#8-test-cases--access-control-and-session-management)
9. [Test Cases — Local User Interface](#9-test-cases--local-user-interface)
10. [Test Cases — WiFi Access Point Mode](#10-test-cases--wifi-access-point-mode)
11. [Test Cases — WiFi Client Mode](#11-test-cases--wifi-client-mode)
12. [Test Cases — Web Interface](#12-test-cases--web-interface)
13. [Test Cases — OTA Firmware Update](#13-test-cases--ota-firmware-update)
14. [Test Cases — NVS Configuration Storage](#14-test-cases--nvs-configuration-storage)
15. [Test Cases — Watchdog and Fault Handling](#15-test-cases--watchdog-and-fault-handling)
16. [Test Cases — Security](#16-test-cases--security)
17. [Test Cases — Day/Night Management](#17-test-cases--daynight-management)
18. [Test Cases — RGB Status LED](#18-test-cases--rgb-status-led)
19. [Traceability Matrix](#19-traceability-matrix)
20. [Automated Integration Test Infrastructure](#20-automated-integration-test-infrastructure)

---

## 1. Introduction

### 1.1 Purpose
This document defines the test plan for the greenhouse ventilation controller firmware. It specifies test cases at unit, integration, and system level for every software module described in the Technical Software Design Specification (TSDS). Each test case is traceable to a TSDS section and, where applicable, to a Functional Requirements Specification (FRS) requirement.

### 1.2 Scope
This plan covers firmware behaviour only. Hardware-level verification (PCB, wiring, relay contact ratings, enclosure IP rating) is outside the scope of this document.

The automated pytest integration test suite is documented in `test/testPlan.md` (TC-01 to TC-20). It implements a subset of the IT/ST-level test cases in this document and runs against live hardware over serial (COM8) and REST (192.168.20.150 / 192.168.20.226). Where a pytest test case corresponds to a test case here it is noted in the expected-result column as `[→ TC-NN]`. The harness architecture, fixture protocol, and run instructions are described in §20.

### 1.3 Definitions

| Term | Definition |
|------|------------|
| UT | Unit Test — isolated logic test, runs on host PC via PlatformIO test runner |
| IT | Integration Test — two or more modules interacting, runs on target hardware |
| ST | System Test — full end-to-end test on target hardware with all peripherals connected |
| DUT | Device Under Test — the assembled controller with firmware loaded |
| Stub | Software replacement for a hardware driver used during unit testing |
| Pass | Test produces the exact expected result within the specified tolerance |
| Fail | Any deviation from the expected result |
| N/A | Not applicable at the indicated test level |

---

## 2. Test Strategy

### 2.1 Test Levels

| Level | Execution environment | Toolchain | When run |
|-------|-----------------------|-----------|----------|
| **Unit (UT)** | Host PC (native build) | PlatformIO test runner + Unity | On every commit via CI; locally before push |
| **Integration (IT)** | Target hardware (LOLIN S3) | PlatformIO upload + serial monitor | After feature branch merge to `develop` |
| **System (ST)** | Full DUT with sensors, RRK-3 simulator, and WiFi | Manual + scripted HTTP/MQTT client | Before release tag |

### 2.2 Testability Approach
Control logic modules (climate control, wind safety, conflict resolution, window state machine) are decoupled from hardware drivers (TSDS §2.3 / TR-SW05). Hardware-dependent interfaces (Modbus UART, relay GPIO, I2C LCD, RTC) are abstracted behind interfaces and replaced with stubs for unit testing. This allows the full business logic to be exercised on a host PC without target hardware.

### 2.3 Pass/Fail Criteria
A test **passes** when all assertions in the test case hold. A test **fails** when any assertion fails, when the DUT hangs or resets unexpectedly, or when a timing constraint is violated. Failing tests block release.

### 2.4 Test ID Convention

`<Level>-<Module>-<Number>` — e.g. `UT-CC-003` = Unit Test, Climate Control, case 3.

| Module code | Module |
|-------------|--------|
| FA | Firmware Architecture (tasks, queues, synchronization) |
| SP | Sensor Polling |
| CC | Climate Control Logic |
| EL | Event Log Manager |
| AC | Access Control and Session Management |
| UI | Local User Interface |
| WA | WiFi Access Point Mode |
| WC | WiFi Client Mode |
| WI | Web Interface |
| OT | OTA Firmware Update |
| NV | NVS Configuration Storage |
| WD | Watchdog and Fault Handling |
| SE | Security |
| DN | Day/Night Management |
| WS | Wind Safety Feature Flags |
| RG | RGB Status LED |

---

## 3. Test Environment

### 3.1 Hardware Requirements

| Item | Purpose |
|------|---------|
| LOLIN S3 (ESP32-S3) with firmware loaded | DUT |
| SIT65HVD08P RS485 transceiver on PCB | Modbus bus driver |
| Modbus RTU simulator (PC + USB-RS485 adapter + script) | Substitute for SenseCAP S200 and FG6485A during IT |
| Sensor emulator REST API (192.168.20.226) | Pushes T/RH/wind readings over REST for automated IT |
| SenseCAP S200 + FG6485A sensors | Required for ST |
| RRK-3 relay box simulator (6 relay inputs + 1 feedback output) | Motor interface for IT and ST |
| USB-C cable to PC | Serial monitor; native USB OTA |
| WiFi access point (2.4 GHz WPA2) | Client mode IT and ST |
| PC with web browser and MQTT client | Web interface and MQTT ST |
| CR2032 battery in RTC holder | Timekeeping tests |
| SD card (FAT32 formatted) | Event log SD tests |
| Jumper wire (GPIO42 → GND) | Motor alarm simulation (IT-WD-007 to IT-WD-011) |

### 3.2 Software Requirements

| Tool | Purpose |
|------|---------|
| PlatformIO (VSCode extension) | Build, flash, test runner |
| Unity test framework | UT assertions |
| Python 3 + `minimalmodbus` or `pymodbus` | Modbus RTU simulator script |
| Python 3.10+ | Automated integration test runner (`test/testPlan.md`) |
| `pytest` + `pytest-timeout` | Test runner and per-test timeout enforcement |
| `pyserial` | Serial log monitor (`lib/serial_monitor.py`) |
| `requests` | REST client for device and sensor emulator |
| `mosquitto` MQTT broker | MQTT IT and ST |
| Postman or `curl` | HTTP/REST web interface tests |
| Serial terminal (e.g. PlatformIO monitor) | Log and diagnostic output |
| WiFi spectrum analyser | AP security verification (ST-SE-002) |

**Environment variables (integration tests):**

| Variable | Default | Purpose |
|---|---|---|
| `GH_ADMIN_PIN` | `1234` | Admin PIN for `POST /api/login` |
| `GH_FARMER_PIN` | `5678` | Farmer PIN |
| `GH_SERIAL_PORT` | `COM8` | Serial port for `SerialMonitor` |
| `GH_DEVICE_BASE` | `http://192.168.20.150` | Device base URL |
| `GH_EMULATOR_BASE` | `http://192.168.20.226` | Sensor emulator base URL |

### 3.3 Firmware Build Variants

| Build variant | Description |
|---------------|-------------|
| `test_host` | Native host build; hardware stubs active; for UT |
| `test_t2_relay` | On-device integration test for T2 relay controller FSM (runs Unity via serial) |
| `release` (`lolin_s3`) | Production build; for IT, ST, and release |

---

## 4. Test Cases — Firmware Architecture

TSDS reference: §4

### 4.1 Task Startup and Scheduling

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-FA-001 | IT | 12 tasks created and running after boot; T13 absent until OTA triggered | Boot DUT; observe serial log | Tasks T1–T12 each log "started" within 5 s of boot; T13 absent; no stack overflow reported. `[→ TC-01]` |
| IT-FA-002 | IT | T1 Watchdog/Heartbeat runs at highest priority | Boot DUT; observe HB LED | HB LED blinks at 4 Hz during startup, transitions to 1 Hz within 10 s |
| IT-FA-003 | IT | Core assignment verified | Boot DUT; log task core IDs via `xTaskGetAffinity` | T1–T9 on Core 1; T10–T12 on Core 0 |
| IT-FA-004 | IT | No stack overflow under full load | Run ST for 30 min with all features active | No stack overflow warning on any task; `uxTaskGetStackHighWaterMark` > 10% remaining on all tasks |
| IT-FA-013 | IT | EG1_BIT_CALIBRATING set during boot CLOSE_ALL sequence, cleared on completion | Boot DUT; monitor serial; query `/api/status` during calibration | Serial shows `CLOSE_ALL calibration start`; `/api/status` returns `mode: WINDOW_CAL` during calibration; `mode: AUTOMATIC` after `CLOSE_ALL calibration complete` |

### 4.2 Inter-task Queues

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-FA-005 | UT | Q1 actuation command delivered from T6 stub to T2 stub | Inject command via stub; assert T2 stub receives it | Command received within 10 ms; no data corruption |
| UT-FA-006 | UT | Q1 priority: T3 CLOSE_ALL processed before T6 command when both enqueued | Enqueue T6 command then T3 CLOSE_ALL; assert processing order | CLOSE_ALL processed first regardless of enqueue order |
| UT-FA-007 | UT | Q3 log queue drop-oldest on overflow | Fill Q3 beyond capacity with test events; assert oldest entries dropped | Queue does not block sender; oldest items discarded; newest retained |
| UT-FA-008 | UT | Q6 sensor reading queue depth 1: new reading overwrites previous | Post two sensor readings to Q6 without consuming; assert only latest retrieved | T4 stub receives the second (latest) reading |

### 4.3 Synchronization Primitives

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-FA-009 | UT | MX2 prevents concurrent write and read of current measurement data | Simulate simultaneous T4 write and T3 read via stubs | No data torn read; both operations complete correctly |
| UT-FA-010 | UT | MX2 and MX3 independence: T3 not blocked by long MX3 hold | Hold MX3 stub for 200 ms; trigger T3 wind evaluation simultaneously | T3 acquires MX2 and completes wind safety check without waiting for MX3 |
| UT-FA-011 | UT | EG1 flags set and cleared by designated owner tasks only | Verify flag ownership by checking set/clear calls in stubs | EG1.WIND_OVERRIDE set/cleared only by T3 stub; EG1.MOTOR_ALARM set/cleared only by T2 |
| IT-FA-012 | IT | Priority inheritance: T3 not starved when T4 holds MX2 | Trigger T3 wind evaluation while T4 stub holds MX2 briefly | T4 priority temporarily raised; MX2 released promptly; T3 completes within 50 ms |

---

## 5. Test Cases — Sensor Polling

TSDS reference: §5.1 | FRS: FR-S03, FR-S04, FR-S06, FR-S07, FR-W03

### 5.1 Normal Operation

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-SP-001 | IT | SenseCAP S200 polled and values stored | Connect Modbus simulator returning valid wind speed (5.0 m/s) and direction (270°); wait 1 poll cycle | T4 current measurement: wind speed = 5.0 m/s, direction = 270°; no fault flag set |
| IT-SP-002 | IT | FG6485A polled and values stored | Connect Modbus simulator returning T = 22.5 °C, RH = 65%; wait 1 poll cycle | T4 current measurement: T = 22.5 °C, RH = 65%; no fault flag set |
| IT-SP-003 | IT | Poll interval respected | Log timestamps of consecutive poll cycles | Interval between polls within ±5% of configured value (factory default 30 s; range 15–120 s) `[→ TC-14]` |
| IT-SP-004 | IT | T4 notified after successful poll | Instrument TN1 and TN2 in test build | TN1 (wind) and TN2 (sensor) task notifications sent to T3 and T6 within 100 ms of poll completion |

### 5.2 Fault Detection

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-SP-005 | IT | No response timeout — wind sensor | Disconnect Modbus simulator for S200; wait 1 poll cycle | EG1.SENSOR_FAULT_W set; fault event posted to Q3; LCD shows wind sensor alarm `[→ TC-12]` |
| IT-SP-006 | IT | No response timeout — T/RH sensor | Disconnect Modbus simulator for FG6485A; wait 1 poll cycle | EG1.SENSOR_FAULT_T set; fault event posted to Q3; LCD shows T/RH sensor alarm `[→ TC-11]` |
| UT-SP-007 | UT | CRC error triggers retry then fault | Simulate one corrupt frame then silence; assert fault after second failure | One retry attempt; fault flag set after second failure; no false positive on single CRC error |
| UT-SP-008 | UT | Out-of-range value discarded | Inject wind speed = 200 m/s (beyond 60 m/s max) | Reading discarded; EG1.SENSOR_FAULT_W set; previous valid value retained in T4 |
| IT-SP-009 | IT | Fault clears on successful poll | Set fault by disconnecting sensor; reconnect; wait 1 poll cycle | EG1.SENSOR_FAULT_W (or _T) cleared; fault-clear event posted to Q3; LCD alarm clears `[→ TC-11, TC-12]` |

### 5.3 Sliding Average

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-SP-010 | UT | Sliding average of N samples converges to correct mean | Inject N=3 samples with values 20, 22, 22 via T5 stub; read T_avg from T4 | T_avg = (20+22+22)/3 = 21.3, rounded to 21 °C; matches expected mean; not a single raw sample |
| IT-SP-011 | IT | avg_win_t = 3 min: three poll cycles required before average settles | Set avg_win_t=3, poll_interval=60 s; inject T=20 for 2 cycles, then T=26 | After 2nd poll T_avg is still dominated by earlier samples; setpoint comparison does not trigger OPEN until 3rd sample is included (avg crosses threshold) |

---

## 6. Test Cases — Climate Control Logic

TSDS reference: §5.2 | FRS: FR-C01–FR-C12, FR-CR01–FR-CR04, FR-MA01–FR-MA08, FR-M01–FR-M07

### 6.1 Operating Mode State Machine

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-001 | UT | Boot state is Automatic | Initialise climate control module with default config stub | Operating mode = Automatic |
| UT-CC-002 | UT | Transition Automatic → Standby on admin command | Post mode-change command to stub; assert mode | Mode = Standby; no relay commands issued while in Standby |
| UT-CC-003 | UT | Transition Standby → Automatic on admin command | Reverse of CC-002 | Mode = Automatic; climate evaluation resumes on next sensor data |
| UT-CC-004 | UT | Wind-override state entered when T3 sets EG1.WIND_OVERRIDE | Set EG1.WIND_OVERRIDE in stub; trigger T6 evaluation | T6 inhibits all open commands; issues CLOSE_ALL; mode = Wind-override |
| UT-CC-005 | UT | Wind-override clears when EG1.WIND_OVERRIDE cleared | Clear EG1.WIND_OVERRIDE; trigger T6 | Mode returns to Automatic; normal climate evaluation resumes |
| UT-CC-006 | UT | Motor-alarm state entered when EG1.MOTOR_ALARM set | Set EG1.MOTOR_ALARM in stub; trigger T6 evaluation | Mode = Motor-alarm; all window commands inhibited; all relays de-energised |
| UT-CC-007 | UT | CLOSE_ALL re-calibration runs after motor alarm clears | Set then clear EG1.MOTOR_ALARM; monitor relay commands from T2 stub | CLOSE_ALL issued on all three channels; after dwell timers expire mode = Automatic |
| UT-CC-008 | UT | Motor alarm overrides WIND_OVERRIDE (highest priority) | Set both WIND_OVERRIDE and MOTOR_ALARM; assert motor alarm takes priority | All relays de-energised; no window commands from any source; MOTOR_ALARM state active |
| UT-CC-023 | UT | Standby mode does not suppress wind safety (FR-M04) | Set mode = Standby; inject wind speed above v_max via T3 stub | T3 issues CLOSE_ALL even in Standby; wind-override active |

### 6.2 Window State Machine

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-009 | UT | M1 transitions CLOSED → MOVING → OPEN on OPEN command | Issue OPEN M1 command; advance stub time by open-dwell | State sequence: CLOSED → MOVING → OPEN; OPEN relay asserted during MOVING only |
| UT-CC-010 | UT | M1 transitions OPEN → MOVING → CLOSED on CLOSE command | Issue CLOSE M1 command from OPEN state; advance stub time | State sequence: OPEN → MOVING → CLOSED; CLOSE relay asserted during MOVING only |
| UT-CC-011 | UT | OPEN + CLOSE mutual exclusion enforced | Issue simultaneous OPEN and CLOSE commands for M1 | Only one relay asserted at a time; second command rejected or queued; no concurrent assertion |
| UT-CC-012 | UT | Close-dwell prevents immediate reopen | Issue OPEN then immediately CLOSE then OPEN for M1 | Second OPEN command not executed until close-dwell timer expires |
| UT-CC-013 | UT | All three channels (M1, M2, M3) operate independently | Issue OPEN M1, CLOSE M2, OPEN M3 simultaneously | All three channels transition correctly and independently without mutual interference |

### 6.3 Setpoints and Hysteresis

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-014 | UT | Window opens when T > T_max_day (is_daytime = true) | Set T_max_day = 25 °C, hyst_t = 1 °C, is_daytime = true; inject T = 26 °C | OPEN command issued |
| UT-CC-015 | UT | Window stays open until T < T_max − hysteresis | T = 24.5 °C (above T_max_day − hyst = 24 °C); windows open | No CLOSE command issued |
| UT-CC-016 | UT | Window closes when T < T_max − hysteresis | T = 23.9 °C; windows open | CLOSE command issued |
| UT-CC-017 | UT | Window closes when T < T_min_day | Set T_min_day = 15 °C; inject T = 14 °C | CLOSE command issued |
| UT-CC-018 | UT | Window opens when RH > RH_max_day (rh_ctrl_en = true) | Set RH_max_day = 80%, hyst_rh = 3%, rh_ctrl_en = true; inject RH = 82% | OPEN command issued |
| UT-CC-019 | UT | No relay chatter at setpoint boundary | Inject T alternating 24.9 / 25.1 °C every poll cycle for 10 cycles | Window state changes ≤ 2 times (hysteresis prevents chatter) |
| UT-CC-024 | UT | Window closes when RH < RH_min_day (over-dry) | Set RH_min_day = 40%, rh_ctrl_en = true; inject RH = 35% from open state | CLOSE_ALL command issued (RH step = 0) |
| UT-CC-025 | UT | Graduated ventilation: step 1 opens M1 only | Set hyst_t = 3 °C, NUM_VENT_STEPS = 3; inject T = T_max + 1 °C (step = 1) | Only M1 OPEN command issued; M2, M3 remain CLOSED |
| UT-CC-026 | UT | Graduated ventilation: step 2 opens M1 + M2 | Inject T = T_max + hyst_t/3 + 1 °C (step = 2) | M1 and M2 OPEN commands issued; M3 remains CLOSED |
| UT-CC-027 | UT | Graduated ventilation: step 3 opens M1 + M2 + M3 | Inject T = T_max + hyst_t (step = 3) | OPEN commands issued for all three channels |
| UT-CC-028 | UT | Night setpoints used when is_daytime = false | Set T_max_day = 25 °C, T_max_night = 20 °C, is_daytime = false; inject T = 22 °C | OPEN command issued (above T_max_night = 20); would not trigger on T_max_day |
| UT-CC-029 | UT | Day setpoints used when is_daytime = true | Same as CC-028 but is_daytime = true; inject T = 22 °C | No OPEN command (T = 22 < T_max_day = 25) |

### 6.4 Conflict Resolution

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-020 | UT | Temperature demands OPEN, humidity demands CLOSE → temperature wins (CR_TEMP_FIRST, default) | T = 26 °C (above T_max), RH = 60% (below RH_max); cr_priority = 0 | OPEN command issued; conflict logged to Q3 |
| UT-CC-021 | UT | Temperature demands CLOSE, humidity demands OPEN → temperature priority gives CLOSE | T = 14 °C (below T_min), RH = 85% (above RH_max); cr_priority = 0 | CLOSE command issued; conflict logged |
| UT-CC-022 | UT | No conflict when both demand same action | T = 26 °C (OPEN), RH = 85% (OPEN) | Single OPEN command; no conflict logged |
| UT-CC-030 | UT | Conflict resolution CR_RH_FIRST (cr_priority = 1): humidity demand wins | T demands OPEN (T > T_max), RH demands CLOSE (RH < RH_min); cr_priority = 1 | CLOSE command issued (humidity wins) |
| UT-CC-031 | UT | Conflict resolution CR_DEVIATION (cr_priority = 2): higher step wins | T demands OPEN at step 2; RH demands CLOSE (step 0); cr_priority = 2 | OPEN at step 2 issued (deviation-based: max(2, 0) = 2) |

### 6.5 Humidity and Wind Feature Flags

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-032 | UT | RH control disabled: no window commands on RH trigger | Set rh_ctrl_en = false; inject RH = 90% (far above RH_max) | No OPEN command issued; VENT_STEP_NEUTRAL returned for RH; no conflict resolution |
| IT-CC-033 | IT | Wind protection disabled: no CLOSE_ALL on wind threshold breach | Set wind_prot_en = false; inject Speed = 20 m/s | No WIND_OVERRIDE set; mode remains AUTOMATIC; no CLOSE command from T3 `[→ TC-10]` |

---

## 7. Test Cases — Event Log Manager

TSDS reference: §5.3 | FRS: FR-LG01–FR-LG09

### 7.1 Log Entry Storage

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-EL-001 | UT | Log entry struct has correct fixed size | Assert `sizeof(log_entry_t)` in unit test | Size = 12 bytes as documented |
| IT-EL-002 | IT | Event posted to Q3 is written to NVS ring buffer | Post a MODE_CHANGE event; read NVS log namespace | Entry present in NVS with correct event_type, initiator, and timestamp |
| IT-EL-003 | IT | NVS ring buffer wraps at CONFIG_NVS_LOG_CAPACITY | Write capacity+1 events; read ring buffer | capacity entries retained; entry #1 overwritten by entry #(capacity+1); entry #2 intact |
| IT-EL-004 | IT | SD card preferred over NVS when present | Insert formatted SD card; write 10 events | Events written to SD card file; NVS log not updated |
| IT-EL-005 | IT | Fallback to NVS when SD card absent | Remove SD card; write 10 events | Events written to NVS ring buffer; no crash or error halt |
| IT-EL-006 | IT | SD card auto-mounts within 60 s of insertion | Boot without SD; insert card; observe serial and log destination without issuing a manual mount command | Serial shows `[T9] SD automounted` within 60 s; subsequent events written to SD card file; `GET /api/sd/status` returns `mounted: true` |

### 7.2 Log Queue Behaviour

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-EL-007 | UT | Q3 sender is non-blocking | Post to Q3 from a high-priority stub; assert no blocking | Sender returns immediately; no priority inversion |
| UT-EL-008 | UT | Q3 drop-oldest on overflow | Fill Q3; post one more event; assert queue state | Queue length unchanged; oldest entry dropped; newest retained |

### 7.3 Log Retrieval

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-EL-009 | ST | Log viewable via web interface | Log 20 events; open log viewer in browser | All 20 events displayed in reverse-chronological order; event_type, initiator, and timestamp correct |
| ST-EL-010 | ST | Log filterable by event type | Filter log by ALARM; assert only ALARM entries shown | Only ALARM entries returned; other types excluded |
| ST-EL-011 | ST | Log persists across power cycle | Write 50 events; power cycle DUT; open log viewer | All 50 events still present; no entries lost |

### 7.4 Log Entry Content



| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-EL-012 | IT | SETPOINT change log entry contains operator identity and old/new values (FR-LG02–FR-LG04) | Login as farmer; change T_max_day from 25 to 28 via web; read log | Log entry: event_type = SETPOINT, initiator = USER_FARMER, value_a = 25 (old), value_b = 28 (new), param_id matches T_max_day |
| IT-EL-013 | IT | LOG_SENSOR event emitted on each poll cycle with T, RH, wind values (FR-LG09) | Set poll_interval = 30 s; inject T=22, RH=65, Speed=3, Dir=90; wait 1 cycle | LOG_SENSOR entry in Q3/log with correct T (22), RH (65), and wind values; timestamp within 2 s of poll |
| IT-EL-014 | IT | Wind-override onset event logged with SYSTEM initiator (FR-MA08 analog, FR-WS11) | Trigger wind override by exceeding v_max; read log | LOG_ALARM entry: event_type = ALARM, initiator = SYSTEM; onset and clearance both logged |

### 7.5 SD Log File Format and Lifecycle

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-EL-015 | IT | SD log file named with local-time timestamp (YYYYMMDDHHMMSS.csv) | Mount SD card; generate log events; inspect SD root directory | File created with name exactly 14 decimal digits + `.csv` (e.g. `20260507163022.csv`); name encodes local creation time; no `ghc_*` files created |
| IT-EL-016 | IT | CSV timestamp field is ISO 8601 UTC (YYYY-MM-DDTHH:MM:SS) | Download SD log file via `GET /api/log/download?src=sd&file=NAME`; inspect `timestamp` column | Every row's timestamp matches format `YYYY-MM-DDTHH:MM:SS`; value is UTC; differs from filename (local time) by timezone offset |
| IT-EL-017 | IT | Proactive free-space guard: oldest file deleted when SD free < 2 MB (above floor); logging suspended at 3-file floor | Fill SD card to < 2 MB free with > 3 log files present; observe T9 serial | Serial: `SD low space: deleted oldest (N files remaining)` when count > 3; if already at 3 files: `SD low space at retention floor — suspending`; `GET /api/sd/status` shows `mounted: false` at floor |
| IT-EL-018 | IT | SD automounts within 60 s without manual intervention | Boot without SD card; after boot completes insert FAT32 SD card; wait without calling `POST /api/sd/mount` | Serial: `[T9] SD automounted` within 60 s of insertion; subsequent events appear in SD log file; `GET /api/sd/status` returns `mounted: true` |

---

## 8. Test Cases — Access Control and Session Management

TSDS reference: §5.4 | FRS: FR-AC01–FR-AC09

### 8.1 PIN Entry and Session State

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-AC-001 | UT | Correct 4-digit farmer PIN opens farmer session | Submit correct farmer PIN to auth module stub | Session state = Farmer; farmer parameters become accessible |
| UT-AC-002 | UT | Correct 8-digit admin PIN opens admin session | Submit correct admin PIN | Session state = Admin; all parameters accessible |
| UT-AC-003 | UT | Wrong PIN rejected; session remains Normal | Submit incorrect PIN | Session state = Normal; no parameters editable |
| UT-AC-004 | UT | Farmer session cannot access admin parameters | Enter farmer session; attempt to read admin parameter | Admin parameter hidden / access denied |
| UT-AC-005 | UT | Admin session can access and edit all parameters | Enter admin session; read and write farmer and admin parameter | Both accessible; writes accepted |
| IT-AC-006 | IT | Session timeout returns to Normal operation | Enter farmer session; idle for configured timeout period | Session closes automatically; mode = Normal; LCD returns to main status screen |
| IT-AC-007 | IT | Keypad activity resets session idle timer | Enter farmer session; press a key at t = timeout − 5 s | Timer resets; session remains active |
| IT-AC-019 | IT | Web session is independent of keyboard session | Establish farmer session on LCD; login as admin via web simultaneously | Both sessions active concurrently; each enforces its own role; web session expiry does not close LCD session |

### 8.2 PIN Storage Security

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-AC-008 | UT | Farmer PIN stored as salted hash, not plain text | Write farmer PIN via config stub; read NVS key `pin_farmer_hash` | Value is a 32-byte SHA-256 hash blob; original PIN not recoverable from stored value |
| UT-AC-009 | UT | Two identical PINs produce different stored hashes (salt uniqueness) | Set same PIN twice with different salts; compare stored values | Stored hashes differ |
| UT-AC-010 | UT | PIN change updates stored hash | Change farmer PIN; verify new PIN accepted and old PIN rejected | New PIN authenticates; old PIN fails |

### 8.3 Login Lockout

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-AC-011 | UT | Lockout activates after N failed attempts (default 5) | Submit wrong PIN 5 times | Input locked; lockout event posted to Q3 |
| UT-AC-012 | UT | Correct PIN rejected during lockout period | Submit correct PIN immediately after lockout | Authentication fails during lockout |
| UT-AC-013 | UT | Lockout expires after configured timeout | Wait for lockout timeout; submit correct PIN | Authentication succeeds after timeout |
| UT-AC-014 | UT | Failed attempt counter resets after successful login | Fail 4 times; succeed once; fail 4 more times | No lockout triggered (counter reset by successful login) |

### 8.4 Administrator Recovery (GPIO0 BOOT Button)

> **Note (v0.2 correction):** Earlier versions of this plan described a "hardware jumper" recovery mechanism. The implemented design uses the GPIO0 BOOT button (active-low, configured as `INPUT_PULLUP` on the LOLIN S3). Three staged levels are triggered by hold duration. See TSDS §3 and §4.3 T8.

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-AC-015 | IT | Short BOOT button hold (< 5 s) has no recovery effect | Hold BOOT button for 3 s; release; observe system | No PIN reset, no settings reset, no restart; system continues normally; LCD returns to status display |
| IT-AC-016 | IT | Stage 1 (5–10 s hold): all PIN codes reset to factory defaults | Hold BOOT button for 7 s; release | Admin and farmer PINs reset to `12345678` / `1234` factory defaults; active session closed; system continues; LCD shows `PIN Reset!` for 5 s; reset event logged |
| IT-AC-017 | IT | Stage 2 (10–15 s hold): all settings and PINs reset to factory defaults | Hold BOOT button for 12 s; release | All NVS namespaces erased; PINs at factory defaults; system continues with factory defaults; LCD shows `Settings Reset!` for 5 s; reset event logged |
| IT-AC-018 | IT | Stage 3 (15–20 s hold): factory reset followed by MCU restart | Hold BOOT button for 17 s; release | All NVS erased; firmware restarts; boot calibration runs; CLOSE_ALL completes; mode = AUTOMATIC after boot; LCD shows `Restart!` for 3 s before reboot |

---

## 9. Test Cases — Local User Interface

TSDS reference: §5.5 | FRS: FR-UI01–FR-UI09, FR-UI22–FR-UI24, FR-WS06, FR-WS10

### 9.1 Keypad Scanning and Debounce

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-UI-001 | UT | Single clean key press generates one key event | Simulate clean press + release in stub | Exactly one key code posted to Q2 |
| UT-UI-002 | UT | Bouncing contact does not generate multiple events | Simulate 5 rapid transitions within 10 ms then stable | Exactly one key code posted to Q2 |
| UT-UI-003 | UT | Key repeat fires after initial delay then at repeat rate | Hold key in stub for 1 s | One initial event; subsequent repeat events at configured repeat rate |
| UT-UI-004 | UT | All 16 keys produce correct unique key codes | Simulate press of each key | 16 distinct codes; no collisions |

### 9.2 Main Status Screen

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-005 | IT | Main screen shows T, RH, wind speed, direction | Inject sensor values via T4 stub | LCD line 1: temperature and humidity; line 2: wind speed; values match injected data |
| IT-UI-006 | IT | Main screen shows current operating mode | Set mode = Standby | Mode indicator visible on LCD |
| IT-UI-007 | IT | Active alarm displayed on main screen | Set EG1.SENSOR_FAULT_W | Alarm indicator on LCD; blinking character present |
| IT-UI-008 | IT | Main screen refreshes when new sensor data arrives | Update T4 stub every 5 s with changing values | LCD updates within 500 ms of T4 data change |

### 9.3 Menu Navigation Depth

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-009 | IT | Any first-level setting reachable within 4 key presses from main screen | Navigate to each first-level setting; count key presses | Every first-level setting reached in ≤ 4 presses (FR-UI07) |
| IT-UI-010 | IT | `*` key navigates back one level at every menu depth | Enter a nested menu; press `*` at each level | Returns to previous level at each press; returns to main screen from depth 1 |
| IT-UI-011 | IT | `#` key confirms value entry | Edit a setpoint; enter new value; press `#` | Value accepted; written to T4 config; menu returns to previous level |

### 9.4 Status Page Cycling

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-012 | IT | Status pages 0–5 auto-cycle every 5 s in normal operation (FR-UI22) | Boot DUT; observe LCD for 35 s without pressing any key | All 6 pages visible in sequence: page 0 (T/RH) → page 1 (wind) → page 2 (windows) → page 3 (network) → page 4 (time/date) → page 5 (motor states M1/M2/M3); each displayed for ~5 s |
| IT-UI-013 | IT | Page 4 shows current time and source label NTP/RTC (FR-UI22) | Ensure WiFi is disconnected (no NTP sync); observe LCD page 4 | Row 0: current local date/time (YYYY-MM-DD HH:MM format); row 1: source label = "RTC"; after NTP sync label changes to "NTP" |

### 9.5 Manual Date/Time Set

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-014 | IT | Admin session required to set date/time; DDMMYY + HHMM entry accepted (FR-UI23) | Enter admin session; navigate to page 4; press `#`; enter date DDMMYY; press `#`; enter time HHMM; press `#` | LCD prompts for date then time; after `#` on time entry: `dm_set_manual_time()` called; DS1307 updated; page 4 shows new time; RTC label persists until NTP sync |
| IT-UI-015 | IT | `*` during date entry returns to status screen without changing time | Enter admin session; press `#` on page 4; enter partial date; press `*` | `UI_SET_DATE` aborted; LCD returns to `UI_STATUS`; time unchanged |

### 9.6 Wind Protection Disabled Warning

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-016 | IT | Persistent "wind protection disabled" warning visible while wind_prot_en = false (FR-WS10) | Enter admin session; set wind_prot_en = false; observe all LCD status pages | Warning indicator visible on LCD (e.g. in mode line or alarm area) on every status page cycle; warning disappears immediately when wind_prot_en is re-enabled |

### 9.7 BOOT Button LCD Progress Bar

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-017 | IT | BOOT button hold suppresses normal display and shows animated progress bar (FR-UI24) | Hold BOOT button; observe LCD during 0–20 s hold period | Row 1: growing filled-character bar advancing left to right; row 0: blank (0–5 s), `Reset PIN?` (5–10 s), `Reset settings?` (10–15 s), `Restarting?` (15–20 s); normal status pages frozen during hold |

### 9.8 D-Key Page Advance

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-UI-018 | IT | D-key immediately advances to next status page and resets 5 s dwell timer | Boot DUT; let auto-cycle settle on page 0; press `D` key | LCD immediately shows page 1; auto-cycle clock resets (page 1 remains for ~5 s before advancing to page 2); repeated `D` presses cycle through all 6 pages and wrap from page 5 back to page 0 |

---

## 10. Test Cases — WiFi Access Point Mode

TSDS reference: §5.6 | FRS: TR-NW01, FR-NW02

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WA-001 | IT | AP does not start on boot | Power cycle DUT; scan for AP SSID (format: "Greenhouse-XXXX") for 30 s | AP SSID not visible until explicitly enabled |
| IT-WA-002 | IT | AP starts when enabled via admin menu | Enter admin session; enable AP via menu | AP SSID (format: "Greenhouse-" + last 2 MAC bytes in hex) visible on WiFi scan within 10 s; LCD shows "AP active" |
| IT-WA-003 | IT | Client can connect to AP | Connect test PC to AP | DHCP address assigned; web interface reachable |
| IT-WA-004 | IT | AP uses WPA2 security minimum | Scan AP and inspect security type | WPA2 or WPA3 reported; open/WEP not accepted |
| IT-WA-005 | IT | AP shuts down after configured timeout with no client | Enable AP; leave idle; wait for timeout | AP SSID disappears after configured timeout; LCD clears AP indicator |
| IT-WA-006 | IT | AP timeout resets while client is connected | Enable AP; connect client; wait beyond timeout | AP remains active while client connected; shuts down after client disconnects and timeout elapses |
| IT-WA-007 | IT | AP timeout is configurable | Set AP timeout to 2 min via admin menu; enable AP; leave idle | AP shuts down after 2 min ± 10 s |

---

## 11. Test Cases — WiFi Client Mode

TSDS reference: §5.7 | FRS: FR-NW01–FR-NW07, FR-DN06, FR-DN07

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WC-001 | IT | Client connects to configured SSID with DHCP | Configure SSID and password via admin menu; enable client mode | DUT connects; IP address assigned; LCD shows connected + IP address |
| IT-WC-002 | IT | Client reconnects automatically after AP drop | Connect; disable test AP for 30 s; re-enable | DUT reconnects within 60 s of AP restoration; LCD updates |
| IT-WC-003 | IT | Static IP configuration applied correctly | Set static IP, mask, gateway, DNS via admin; enable client | DUT uses configured static IP; DHCP not attempted |
| IT-WC-004 | IT | NTP sync triggered on client connection | Connect client; observe RTC update | System time synchronised with NTP within 30 s of connection; TN4 sent to T4 |
| IT-WC-005 | IT | LCD shows Disconnected when client mode enabled but no AP available | Enable client mode; ensure configured SSID not available | LCD shows "Disconnected"; no crash or hang |
| IT-WC-006 | IT | Geolocation auto-detect updates lat/lon and TZ after NTP sync (FR-DN06, FR-DN07) | Connect to WiFi; observe NTP sync success; wait 10 s | `GET /api/config` shows lat/lon updated from ip-api.com; tz_str updated to POSIX string for detected location; system/tz_str in NVS reflects new timezone |
| IT-WC-007 | IT | Timezone string applied immediately without reboot (FR-DN07) | Change tz_str via `POST /api/config` to a timezone 2 hours ahead; observe displayed time | Web GUI and LCD page-4 time reflect new timezone within one display refresh cycle (≤5 s); no reboot required |

---

## 12. Test Cases — Web Interface

TSDS reference: §5.8 | FRS: FR-NW06, FR-DN04, FR-MQ01–FR-MQ05

### 12.1 Authentication and Public Access

> **Note (v0.2 correction):** As of firmware v1.16.3, the following endpoints are public and do not require authentication: `GET /api/status`, `GET /api/history`, `GET /api/sd/status`. All write endpoints and `/api/config` require a valid session. `ST-SE-003` has been updated accordingly.

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-001 | ST | Unauthenticated request to protected endpoints returns 401 | Send unauthenticated GET to `/api/config`, `/api/whoami`; unauthenticated POST to `/api/config`, `/api/wifi`, `/api/pin` | All return HTTP 401; no data payload served |
| ST-WI-012 | ST | Unauthenticated GET /api/status returns 200 with sensor data | GET `/api/status` without session cookie | HTTP 200; response JSON contains `temp_c`, `rh_pct`, `wind_ms`, `windows`, `mode`; no login required |
| ST-WI-013 | ST | Unauthenticated GET /api/history returns 200 | GET `/api/history?n=20` without session cookie | HTTP 200; `rows` array with up to 20 entries; no login required |
| ST-WI-002 | ST | Farmer login via web grants farmer-level access | POST correct farmer PIN to login endpoint | Session cookie issued; farmer parameters accessible; admin parameters absent |
| ST-WI-003 | ST | Admin login via web grants full access | POST correct admin PIN | Session cookie issued; all parameters accessible |
| ST-WI-004 | ST | Web session expires after configured timeout | Login; idle for timeout period | Session cookie invalidated; subsequent request returns 401 `[→ TC-16]` |

### 12.2 Dashboard and Settings

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-005 | ST | Dashboard shows live sensor values | Open dashboard; compare with LCD main screen | T, RH, wind speed, direction, window states, mode all match LCD; updates within 5 s |
| ST-WI-006 | ST | Setpoint change via web reflected in firmware | Set T_max_day = 28 °C via web settings page | T4 configuration updated; LCD settings screen shows new value; persisted to NVS `[→ TC-14]` |
| ST-WI-007 | ST | Admin-only parameters not visible in farmer session | Login as farmer; inspect settings page | Admin-only parameters absent from page DOM and HTTP responses `[→ TC-15]` |
| ST-WI-008 | ST | Web dashboard does not expose window open/close controls | Open dashboard as farmer; inspect page DOM | No window open/close buttons or controls present; manual window commands out of scope (C9) |
| ST-WI-014 | ST | Dashboard shows today's sunrise and sunset times (FR-DN04) | Login; open dashboard with valid location set (lat/lon ≠ 0) | Sunrise and sunset times visible in local time; values match NOAA calculation for configured location ± 5 min |
| ST-WI-015 | ST | Sensor history table — newest row has highest timestamp (newest at top) | Insert known T values at known times; GET /api/history?n=10 | First row in `rows[]` array has the largest `ts` value; rows descend in time |

### 12.3 MQTT Client

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-009 | ST | MQTT publishes sensor data at configured interval | Configure broker; subscribe to topic; wait | Messages received at configured interval; payload contains T, RH, wind speed, direction |
| ST-WI-010 | ST | MQTT publishes window states and mode | Observe MQTT messages during mode change | Status topic updated within 5 s of mode change |
| ST-WI-011 | ST | MQTT CLOSE_ALL command received and executed | Publish CLOSE_ALL to command topic | All relays receive CLOSE command; T2 state machines transition |

### 12.4 Log Tab

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-016 | ST | `GET /api/log/files` admin-only; returns NVS count and sorted SD file list | Login as admin with SD mounted; `GET /api/log/files` | HTTP 200; response contains `nvs_count` (integer ≥ 0) and `sd_files` array; SD filenames match `YYYYMMDDHHMMSS.csv` pattern; list sorted lexicographically oldest→newest; unauthenticated or farmer session returns 403 |
| ST-WI-017 | ST | `GET /api/log/download` returns valid CSV with ISO 8601 timestamps | Login as admin; `GET /api/log/download?src=nvs` and `GET /api/log/download?src=sd&file=NAME` | Both return `text/csv` with header `timestamp,type,initiator,ch,param,value_a,value_b`; every `timestamp` field matches `YYYY-MM-DDTHH:MM:SS`; NVS filename is `nvs_log.csv`; SD filename preserved; path-traversal attempts (`../` or `/`) return 400 |

---

## 13. Test Cases — OTA Firmware Update

TSDS reference: §5.9 | FRS: TR-SW02

### 13.1 Firmware Update

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-OT-001 | ST | New firmware image uploaded and applied via web | Login as admin; upload valid firmware binary via OTA page | Upload accepted; DUT reboots; new firmware version reported on dashboard `[→ TC-18]` |
| ST-OT-002 | ST | NVS configuration retained after firmware update | Set a custom setpoint; perform OTA update | After reboot, setpoint value unchanged |
| ST-OT-003 | ST | Corrupt firmware image rejected | Upload binary with invalid checksum | Upload rejected with error; DUT remains on current firmware and continues operating |
| ST-OT-004 | ST | Failsafe rollback after 3 failed boots | Upload firmware that halts during startup; power cycle 3 times | After 3rd failed boot, DUT reverts to previous firmware bank; operates normally `[→ TC-19]` |
| ST-OT-005 | ST | OTA blocks web file serving during write | Begin web file OTA upload; attempt to load web page simultaneously | Page request deferred (EG1.OTA_IN_PROGRESS set); served after OTA completes |
| ST-OT-006 | ST | Combined firmware + web file update activates only when both complete | Upload firmware image only (no web file); assert no activation | Active bank not switched until web file package also uploaded and verified |
| ST-OT-008 | ST | Asset version mismatch after manual intervention shows dashboard warning | Flash firmware binary; manually flash LittleFS image with different asset_version; boot | Dashboard shows version-mismatch warning banner; event logged; operation continues; no crash |

### 13.2 USB OTA

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-OT-007 | IT | Firmware flashable via native USB without opening enclosure | Connect USB-C; flash via PlatformIO | Flash succeeds; DUT boots new firmware |

---

## 14. Test Cases — NVS Configuration Storage

TSDS reference: §5.10 | FRS: FR-CF06, TR-SW01

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-NV-001 | IT | All default values applied on first boot (blank NVS) | Erase NVS partition; boot DUT | All parameters at documented factory defaults; no crash |
| IT-NV-002 | IT | Setting change persisted immediately to NVS | Change T_max_day via menu; power cycle | After reboot, T_max_day = previously set value |
| IT-NV-003 | IT | All NVS namespaces written and read correctly | Write one value per namespace; power cycle; read back | All values intact across all namespaces |
| IT-NV-004 | IT | NVS survives OTA firmware update | Set custom values; perform OTA; check values | All values unchanged after update |
| IT-NV-005 | IT | Factory reset clears all NVS and restores defaults | Trigger factory reset via BOOT button (Stage 3); read all NVS keys | All keys at factory default; no residual values from previous configuration |
| UT-NV-006 | UT | Range validation rejects out-of-range write | Submit T_max_day = 999 °C to T4 config module stub | Write rejected; NVS not updated; error returned to caller |
| IT-NV-007 | IT | NVS schema version mismatch handled gracefully (§5.10 migration) | Flash firmware with NVS_SCHEMA_VERSION incremented by 1; boot without erasing NVS | Existing NVS settings retained (per-key migration); schema_ver updated; fw_version updated; `NVS_CFG_ERR_MIGRATION` logged via Q3; system operates normally |

---

## 15. Test Cases — Watchdog and Fault Handling

TSDS reference: §5.11 | FRS: TR-SW03, FR-ST02, FR-S05, FR-W04

### 15.1 Watchdog

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WD-001 | IT | Watchdog resets MCU on T1 starvation | Suspend T1 via test hook; wait for watchdog timeout | MCU resets; restart event logged on next boot |
| IT-WD-002 | IT | Restart sequence closes all relays after watchdog reset | Open M1 relay; trigger watchdog reset via test hook | After reboot, CLOSE_ALL issued on all channels; M1 closed; event logged |
| IT-WD-003 | IT | Reset reason logged correctly | Trigger watchdog reset; read event log | Event log entry with event_type = SYSTEM and reset reason = WATCHDOG |

### 15.2 Sensor Fault Handling

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WD-004 | IT | Climate control inhibited on T/RH sensor fault | Set EG1.SENSOR_FAULT_T; inject temperature that would trigger OPEN | No OPEN command issued; window state unchanged; LCD shows fault |
| IT-WD-005 | IT | Wind safety CLOSE_ALL on wind sensor fault | Set EG1.SENSOR_FAULT_W | T3 issues CLOSE_ALL; all windows close; fault alarm on LCD `[→ TC-13]` |
| IT-WD-006 | IT | Last known window state maintained during T/RH fault | Open M1; simulate T/RH sensor fault | M1 remains OPEN (state retained); no spurious CLOSE command |

### 15.3 Motor Alarm

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WD-007 | IT | Motor alarm detected on RRK-3 emergency stop signal | Assert GPIO42 externally (LOW); observe system response | EG1.MOTOR_ALARM set; all relays de-energised; T6 inhibits all commands; LCD shows motor alarm; onset event logged `[→ TC-20]` |
| IT-WD-008 | IT | CLOSE_ALL re-calibration runs after motor alarm clears | Assert then deassert GPIO42; observe relay commands | EG1.MOTOR_ALARM cleared; CLOSE_ALL issued on all channels; dwell timers run; clearance event logged; mode returns to Automatic `[→ TC-20]` |
| IT-WD-009 | IT | Motor alarm asserted during T2-commanded OPEN move de-energises relays immediately | Start M1 OPEN command; assert GPIO42 mid-move | Relay de-energised immediately; MOTOR_ALARM set; move does not complete |
| IT-WD-010 | IT | Motor alarm re-asserted during 60 s guard period detected within ≤ 5 s | Trigger alarm; wait for alarm clearance; within the 60 s guard re-assert GPIO42 | New onset detected ≤ 5 s after re-assertion; MOTOR_ALARM re-set; guard aborted; relays remain de-energised; second onset event logged |
| IT-WD-011 | IT | Motor alarm active at power-on — boot-time CLOSE_ALL calibration skipped | Assert GPIO42 before power-on; power on DUT | CLOSE_ALL calibration not initiated; MOTOR_ALARM set immediately on boot; serial: no `CLOSE_ALL calibration start`; LCD shows motor alarm |

---

## 16. Test Cases — Security

TSDS reference: §2.4, §3 | FRS: TR-NW01, TR-NW04, FR-AC06, FR-NW06

> **Note (v0.2 correction):** ST-SE-003 is updated to reflect that `/api/status`, `/api/history`, and `/api/sd/status` are intentionally public (no authentication required) as of v1.16.3. Only write endpoints and `/api/config` remain auth-protected.

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-SE-001 | ST | Plain-text PINs not present in NVS flash dump | Read full NVS partition; search for known PIN strings | No plain-text PIN found; only hash values present (FR-AC06) |
| ST-SE-002 | ST | WiFi AP uses WPA2 or stronger | Scan AP with WiFi analyser | Security type = WPA2 or WPA3; no open or WEP (TR-NW01) |
| ST-SE-003 | ST | Write endpoints and /api/config require authentication; public read endpoints return data without auth | Send unauthenticated GET to `/api/config`, `/api/whoami`; unauthenticated POST to `/api/config`, `/api/wifi`, `/api/pin` | All protected endpoints return HTTP 401; `/api/status`, `/api/history`, `/api/sd/status` return HTTP 200 (intentional public access design) |
| ST-SE-004 | ST | Session cookie not reusable after logout | Login; record session cookie; logout; replay cookie | Replayed cookie rejected with HTTP 401 `[→ TC-17]` |
| ST-SE-005 | ST | HTTP traffic does not expose PIN in plain text | Capture HTTP traffic during PIN submission | PIN not present in plain text in captured frames |
| ST-SE-006 | ST | Login lockout applies equally via web and keyboard | Submit wrong PIN 5 times via web login | Web login locked for configured duration; matches keyboard lockout behaviour (FR-AC07) |

---

## 17. Test Cases — Day/Night Management

TSDS reference: §4.3 T4, §5.2 | FRS: FR-DN01–FR-DN07, FR-C01–FR-C08

> These tests verify the sunrise/sunset calculation, the day/night period determination, and the automatic climate setpoint profile selection. The geolocation and timezone tests also apply here.

### 17.1 Sunrise/Sunset Calculation

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-DN-001 | UT | Sunrise/sunset calculation accuracy for known input | Call sunrise/sunset algorithm with lat=52.09°N, lon=5.12°E, date=2026-06-21 (summer solstice, UTC) | Sunrise ≈ 243 min UTC (04:03); sunset ≈ 1219 min UTC (20:19); result within ±5 min of NOAA reference value |
| UT-DN-002 | UT | is_daytime = true when UTC time is between sunrise and sunset | Set current UTC = sunrise + 60 min; call is_daytime evaluation | is_daytime = true |
| UT-DN-003 | UT | is_daytime = false when UTC time is before sunrise | Set current UTC = sunrise − 60 min; call is_daytime evaluation | is_daytime = false |
| UT-DN-004 | UT | is_daytime = false when UTC time is after sunset | Set current UTC = sunset + 60 min; call is_daytime evaluation | is_daytime = false |

### 17.2 Setpoint Profile Switching

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-DN-005 | IT | Day setpoints active during daytime; night setpoints active at night | Set T_max_day=25, T_max_night=20; inject T=22 during day period; inject T=22 during night period | T=22 triggers no OPEN during day (22 < 25); T=22 triggers OPEN during night (22 > 20) |
| IT-DN-006 | IT | Location (lat/lon) change triggers sunrise/sunset recalculation within 60 s | Change lat_deg/lat_frac via web GUI; wait 60 s; query `/api/status` | `sunrise_utc` and `sunset_utc` fields in status response reflect new location; change visible within 60 s |

### 17.3 Default and Fallback Behaviour

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-DN-007 | IT | No location configured (lat=0, lon=0) → daytime setpoints always active (FR-DN05) | Erase NVS; boot DUT; observe is_daytime at 02:00 UTC | is_daytime = true (daytime setpoints are the safe default); no night setpoints applied |

### 17.4 Timezone and Time Display

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-DN-008 | IT | Web GUI shows sunrise and sunset in local time after geolocation (FR-DN04) | Configure lat/lon for Amsterdam (52°N, 5°E); connect WiFi; open dashboard | Sunrise and sunset fields show local time (UTC + DST offset); values correct for current date and location ± 5 min |
| IT-DN-009 | IT | POSIX TZ string applied immediately when changed (FR-DN07) | Manually set tz_str to `"EST5"` (UTC−5) via POST /api/config; observe time display | Time display changes to UTC−5 immediately; no reboot required; RTC still stores UTC; local display offset by −5 h |

---

## 18. Test Cases — RGB Status LED

TSDS reference: §5.12 | FRS: FR-UI16–FR-UI21, FR-CF14

> LED state is observed directly on the enclosure (visible through transparent cover). For automated testing the `/api/status` `eg1` field and `wind_prot_en` / `rh_ctrl_en` config values are used to infer the expected colour.

### 18.1 Colour State Mapping

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-RG-001 | IT | Normal operation — no faults or warnings → Green (FR-UI17) | Ensure all sensors healthy, wind_prot_en=true, rh_ctrl_en=true, no alarm | RGB LED shows green |
| IT-RG-002 | IT | EG1.SENSOR_FAULT_T set → Amber (FR-UI18) | Simulate T/RH sensor fault (disconnect or emulator live mode); observe LED | RGB LED changes to amber within 2 poll cycles |
| IT-RG-003 | IT | EG1.SENSOR_FAULT_W set → Amber | Simulate wind sensor fault; observe LED | RGB LED shows amber |
| IT-RG-004 | IT | EG1.WIND_OVERRIDE set → Amber | Trigger wind speed override (Speed > v_max); observe LED | RGB LED shows amber |
| IT-RG-005 | IT | wind_prot_en = false → Amber (FR-UI18, TSDS §5.12) | Set wind_prot_en = false via web; observe LED | RGB LED shows amber (wind protection disabled is a non-critical warning) |
| IT-RG-006 | IT | rh_ctrl_en = false → Amber | Set rh_ctrl_en = false via web; observe LED | RGB LED shows amber (humidity control disabled is a non-critical warning) |
| IT-RG-007 | IT | EG1.MOTOR_ALARM set → Red; overrides all amber conditions (FR-UI19) | Assert GPIO42 alarm; observe LED | RGB LED shows red regardless of any concurrent amber conditions |
| IT-RG-008 | IT | MOTOR_ALARM cleared; amber condition still active → LED returns to Amber | Assert GPIO42 (Red); deassert; sensor fault still active | After alarm clears LED returns to amber (SENSOR_FAULT remains); only returns to green when all amber conditions also clear |
| IT-RG-009 | IT | Multiple amber conditions active → Amber (not Red) | Set SENSOR_FAULT_T and wind_prot_en=false simultaneously | LED shows amber; no inadvertent escalation to red |

### 18.2 Night Brightness Dimming

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-RG-010 | IT | LED brightness reduced during configured night hours (FR-UI21, FR-CF14) | Set led_nite_from=22, led_nite_to=6, led_nite_brt=20; advance DUT time to 23:00 local | Observable brightness reduction; LED colour unchanged (still green/amber/red); brighter during day hours |
| IT-RG-011 | IT | LED brightness returns to day level outside night window | Advance DUT time to 08:00 local; observe LED | LED brightness returns to led_day_brt level |

---

## 19. Traceability Matrix

| TSDS Section | FRS / Req IDs | Test case IDs |
|--------------|---------------|---------------|
| §4 Firmware Architecture | TR-SW05 | IT-FA-001 to IT-FA-004, IT-FA-013, UT-FA-005 to UT-FA-012 |
| §5.1 Sensor Polling | FR-S03, FR-S04, FR-S06, FR-S07, FR-W03 | IT-SP-001 to IT-SP-011, UT-SP-007, UT-SP-008, UT-SP-010 |
| §5.2 Climate Control Logic — mode FSM | FR-C09, FR-C10, FR-C11, FR-C12, FR-M01–FR-M07, FR-MA01–FR-MA08 | UT-CC-001 to UT-CC-013, UT-CC-023, UT-CC-028, UT-CC-029, IT-CC-033 |
| §5.2 Climate Control Logic — setpoints | FR-C01–FR-C12, FR-S06 | UT-CC-014 to UT-CC-022, UT-CC-024 to UT-CC-027, UT-CC-030, UT-CC-031 |
| §5.2 Conflict Resolution | FR-CR01–FR-CR04 | UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-030, UT-CC-031 |
| §5.3 Event Log Manager | FR-LG01–FR-LG09 | UT-EL-001, IT-EL-002 to IT-EL-006, IT-EL-012 to IT-EL-018, UT-EL-007, UT-EL-008, ST-EL-009 to ST-EL-011 |
| §5.4 Access Control | FR-AC01–FR-AC09 | UT-AC-001 to UT-AC-014, IT-AC-006, IT-AC-007, IT-AC-015 to IT-AC-019 |
| §5.5 Local User Interface | FR-UI01–FR-UI09, FR-UI22–FR-UI24, FR-WS06, FR-WS10 | UT-UI-001 to UT-UI-004, IT-UI-005 to IT-UI-018 |
| §5.6 WiFi AP Mode | TR-NW01, FR-NW02 | IT-WA-001 to IT-WA-007 |
| §5.7 WiFi Client Mode | FR-NW01–FR-NW07, FR-DN06, FR-DN07 | IT-WC-001 to IT-WC-007 |
| §5.8 Web Interface | FR-NW06, FR-DN04, FR-MQ01–FR-MQ05 | ST-WI-001 to ST-WI-017 |
| §5.9 OTA Firmware Update | TR-SW02 | ST-OT-001 to ST-OT-006, ST-OT-008, IT-OT-007 |
| §5.10 NVS Configuration Storage | FR-CF06, TR-SW01 | IT-NV-001 to IT-NV-005, IT-NV-007, UT-NV-006 |
| §5.11 Watchdog and Fault Handling | TR-SW03, FR-ST02, FR-S05, FR-W04 | IT-WD-001 to IT-WD-011 |
| §5.12 RGB Status LED | FR-UI16–FR-UI21, FR-CF14 | IT-RG-001 to IT-RG-011 |
| §4.3 T4 Day/Night + §5.2 | FR-DN01–FR-DN07, FR-C01–FR-C08 | UT-DN-001 to UT-DN-004, IT-DN-005 to IT-DN-009 |
| §2.4 Security | TR-NW01, TR-NW04, FR-AC06, FR-NW06 | ST-SE-001 to ST-SE-006 |

### Unimplemented / Won't-have coverage

| Requirement | Reason not covered |
|-------------|-------------------|
| FR-A07/FR-A08 | Partial opening not feasible with current hardware (C3); out of scope |
| FR-MQ03–FR-MQ04 | MQTT command subscription and broker credentials — "Could have"; deferred |
| TR-NW04 | HTTPS not implemented; accepted risk documented in TSDS §6 Issue #4 |
| FR-UI09 | Display language configurable — "Could have"; not yet implemented |

---

---

## 20. Automated Integration Test Infrastructure

This section documents the architecture, fixtures, and conventions of the pytest-based integration test suite (`test/testPlan.md`). Test case step tables and pass criteria live in `test/testPlan.md`; this section describes _how_ the harness works.

### 20.1 File layout

```
test/
  testPlan.md                    ← authoritative TC-01 to TC-20 specification
  conftest.py                    ← pytest fixtures and helper functions
  lib/
    __init__.py
    serial_monitor.py            ← background serial reader + pattern matcher
    device_api.py                ← REST client for device (192.168.20.150)
    emulator_api.py              ← REST client for sensor emulator (192.168.20.226)
  test_01_boot.py
  test_02_climate_temp.py
  test_03_climate_rh.py
  test_04_wind_override.py
  test_05_sensor_fault.py
  test_06_config_api.py
  test_07_session.py
  test_08_motor_alarm.md         ← TC-18, TC-19, TC-20: manual procedures
```

### 20.2 Harness modules

#### `lib/serial_monitor.py` — `SerialMonitor`

A background thread reads the device's USB-CDC serial output into a `deque(maxlen=2000)`. Each entry is a `(monotonic_ts, line)` tuple. Two methods are exposed to test bodies:

| Method | Signature | Returns |
|--------|-----------|---------|
| `mark()` | `() → float` | Current `time.monotonic()` — use as an `after` anchor |
| `wait_for(pattern, timeout, after)` | `(str, float, float) → str \| None` | First line matching the regex that arrived after `after`; `None` on timeout |
| `wait_for_none(pattern, duration, after)` | `(str, float, float) → bool` | `True` if the pattern does **not** appear within `duration` seconds |

All serial assertions pass `after=t0` (a mark taken before the triggering action) to prevent false matches on residual buffered output from earlier test steps.

#### `lib/emulator_api.py` — `EmulatorApi`

Wraps `POST /api/data` and `POST /config/sensor` on the sensor emulator (192.168.20.226).

| Method | Purpose |
|--------|---------|
| `set_rest_mode()` | Both sensors → REST mode (emulator serves injected values) |
| `set_live_mode_t_rh()` | fg6485a → Live (Modbus pass-through; simulates loss of T/RH sensor) |
| `set_live_mode_wind()` | S200 → Live (simulates loss of wind sensor) |
| `push(T, RH, Speed, Direction)` | Inject individual sensor values; omitted fields retain previous value |
| `push_neutral()` | Push `T=20.0, RH=60.0, Speed=0.5, Dir=180.0` |

#### `lib/device_api.py` — `DeviceApi`

Wraps the device REST API using a persistent `requests.Session` (auth cookie retained).

| Method | Purpose |
|--------|---------|
| `login(pin, role)` | `POST /api/login`; asserts `ok: true` |
| `logout()` | `POST /api/logout` |
| `whoami()` | `GET /api/whoami` → `Response` |
| `set_config(ns, key, value)` | `POST /api/config` → `Response` |
| `get_config()` | `GET /api/config` → `dict` |
| `get_status()` | `GET /api/status` → `dict` |
| `get_ota_status()` | `GET /api/ota/status` → `dict` |
| `config_value(key)` | Extracts a single key value from the config dict |

### 20.3 NVS write confirmation protocol

`POST /api/config` returns HTTP 200 immediately; the value is committed to flash and applied to the running firmware state after ~400 ms per key (queue depth permitting). The `wait_for_config` helper polls `GET /api/config` until the returned value matches, before any test body pushes sensor data or asserts firmware behaviour:

```
write key → poll GET /api/config (up to 8 s, 400 ms interval) → proceed
```

This prevents race conditions between the REST write and the firmware's NVS commit + reload path.

### 20.4 Fixtures

| Fixture | Scope | Purpose |
|---------|-------|---------|
| `serial_mon` | session | Starts `SerialMonitor` on `GH_SERIAL_PORT`; shared across all tests |
| `device` | session | Logs in as admin at session start; `DeviceApi` instance reused |
| `emulator` | session | Calls `set_rest_mode()`; `EmulatorApi` instance reused |
| `fast_config` | function | Writes fast values (see §20.6 timing table) and confirms them; restores originals on teardown |
| `neutral_sensors` | function | Pushes `push_neutral()` before test body and after teardown |
| `wait_for_automatic_mode` | helper | Polls `GET /api/status` until `mode == "AUTOMATIC"`; called at end of setup phase in tests that modify mode |
| `save_and_restore` | function | Snapshots specific keys before test; restores unconditionally on teardown (TC-14 use) |

`fast_config` restores these keys after every test that uses it:

| Namespace | Key | Restored to |
|-----------|-----|-------------|
| `system` | `poll_interval_s` | Pre-test `GET /api/config` value |
| `motor` | `travel_m1` | Pre-test value (default 21 s) |
| `motor` | `travel_m2` | Pre-test value (default 21 s) |
| `motor` | `travel_m3` | Pre-test value (default 171 s) |
| `climate` | `avg_win_t` | Pre-test value (default 3) |
| `climate` | `avg_win_rh` | Pre-test value (default 3) |

Keys **not** explicitly restored are left as written; the next test's fixture writes and confirms its own required values, so residual values from a previous test cannot affect later outcomes.

### 20.5 Serial patterns reference

| Event | Regex |
|-------|-------|
| Boot banner | `=== Greenhouse Controller v` |
| All tasks started | `All tasks spawned` |
| CLOSE_ALL calibration complete | `CLOSE_ALL calibration complete` |
| Window opening | `MOVING_OPEN\|CMD_OPEN` |
| Window closing | `MOVING_CLOSE\|CMD_CLOSE\|CMD_CLOSE_ALL` |
| Wind override set | `WIND_OVERRIDE set` |
| Wind override cleared | `WIND_OVERRIDE cleared` |
| T/RH fault | `T/RH sensor FAULT` |
| T/RH fault cleared | `T/RH sensor fault cleared` |
| Wind sensor fault | `Wind sensor FAULT` |
| Wind sensor fault cleared | `Wind sensor fault cleared` |
| Motor alarm asserted | `MOTOR_ALARM asserted` |
| Motor alarm cleared | `MOTOR_ALARM cleared` |
| Config written | `Q4 applied:` |
| OTA boot healthy (fail counter reset) | `fail counter reset to 0` |

### 20.6 `eg1` event-group bit reference

The `eg1` field in `GET /api/status` is the raw FreeRTOS event-group bitmask (reported as a decimal integer):

| Bit | Hex mask | Meaning |
|-----|----------|---------|
| 0 | `0x01` | `WIND_OVERRIDE` active |
| 2 | `0x04` | `SENSOR_FAULT_T` — T/RH sensor fault |
| 3 | `0x08` | `SENSOR_FAULT_W` — wind sensor fault |
| 5 | `0x20` | `MOTOR_ALARM` active |
| 6 | `0x40` | `EG1_BIT_CALIBRATING` — CLOSE_ALL calibration in progress |

Test bodies that assert bit-level fault state use `(status['eg1'] & mask) != 0`.

### 20.7 Timing reference

| Parameter | Default (NVS) | `fast_config` value | Rationale |
|-----------|---------------|---------------------|-----------|
| `poll_interval_s` | 30 s | 30 s | Matches factory default; explicitly written to ensure fixture state is known |
| `travel_m1` | 21 s | 5 s | Window traverses in 5 s |
| `travel_m2` | 21 s | 5 s | Window traverses in 5 s |
| `travel_m3` | 171 s | 5 s | Window traverses in 5 s |
| `avg_win_t` | 3 | 1 | Single sample = immediate T response |
| `avg_win_rh` | 3 | 1 | Single sample = immediate RH response |
| `WAIT_AFTER_PUSH` (constant) | — | 35 s | poll_interval (30) + 5 s buffer |
| `FAULT_WAIT` (constant) | — | 80 s | 2 × poll_interval + 10 s buffer |
| `BOOT_TIMEOUT` (constant) | — | 60 s | Boot + CLOSE_ALL with travel=5 s |

### 20.8 Running the automated suite

**Prerequisites:**

```powershell
# Install dependencies
python -m pip install pytest pytest-timeout pyserial requests

# Set PINs (or edit conftest.py constants)
$env:GH_ADMIN_PIN  = "your-admin-pin"
$env:GH_FARMER_PIN = "your-farmer-pin"
```

Device must be reachable at `192.168.20.150`, emulator at `192.168.20.226`, and serial port `COM8` connected.

**TC-01 note:** TC-01 waits for the firmware boot banner on serial. The conftest pre-writes `travel_m1/m2/m3 = 5 s` to NVS, then prompts:

```
ACTION REQUIRED — TC-01 Boot test
Reset or power-cycle the device NOW, then press ENTER
as soon as the device starts booting.
```

Press ENTER as the device starts booting. The `_reset_mark` timestamp is recorded at that point; all three serial `wait_for` calls in TC-01 use it as the look-back anchor.

**Run commands:**

```powershell
# Full suite
cd test
python -m pytest -v -s --timeout=300

# Single file
python -m pytest test_02_climate_temp.py -v -s --timeout=120

# Skip TC-01 (device already running)
python -m pytest --ignore=test_01_boot.py -v -s --timeout=300
```

`--timeout=300` covers TC-16 (165 s wall-clock) with margin. A plain-text result log is written to `test/results.log`.

**After any run, verify fixture teardown:**

```powershell
curl http://192.168.20.150/api/config
```

Confirm `travel_m1/m2/m3` and `avg_win_t/rh` are back to their pre-test values. If a test was interrupted before teardown, restore manually via the device web UI or `POST /api/config`.

---

*End of document — version 0.4*
