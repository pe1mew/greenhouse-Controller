# Software Test Plan
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Software Test Plan                             |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.1 (draft)                                   |
| Date         | 2026-03-29                                    |
| Status       | Draft                                         |
| Related docs | `technicalSoftwareDesignSpecification.md`      |
|              | `functionalRequirementsSpecification.md`       |
|              | `tasks.md`                                    |

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
17. [Traceability Matrix](#17-traceability-matrix)

---

## 1. Introduction

### 1.1 Purpose
This document defines the test plan for the greenhouse ventilation controller firmware. It specifies test cases at unit, integration, and system level for every software module described in the Technical Software Design Specification (TSDS). Each test case is traceable to a TSDS section and, where applicable, to a Functional Requirements Specification (FRS) requirement.

### 1.2 Scope
This plan covers firmware behaviour only. Hardware-level verification (PCB, wiring, relay contact ratings, enclosure IP rating) is outside the scope of this document.

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

---

## 3. Test Environment

### 3.1 Hardware Requirements

| Item | Purpose |
|------|---------|
| LOLIN S3 (ESP32-S3) with firmware loaded | DUT |
| SIT65HVD08P RS485 transceiver on PCB | Modbus bus driver |
| Modbus RTU simulator (PC + USB-RS485 adapter + script) | Substitute for SenseCAP S200 and FG6485A during IT |
| SenseCAP S200 + FG6485A sensors | Required for ST |
| RRK-3 relay box simulator (6 relay inputs + 1 feedback output) | Motor interface for IT and ST |
| USB-C cable to PC | Serial monitor; native USB OTA |
| WiFi access point (2.4 GHz WPA2) | Client mode IT and ST |
| PC with web browser and MQTT client | Web interface and MQTT ST |
| CR2032 battery in RTC holder | Timekeeping tests |
| SD card (FAT32 formatted) | Event log SD tests |

### 3.2 Software Requirements

| Tool | Purpose |
|------|---------|
| PlatformIO (VSCode extension) | Build, flash, test runner |
| Unity test framework | UT assertions |
| Python 3 + `minimalmodbus` or `pymodbus` | Modbus RTU simulator script |
| `mosquitto` MQTT broker | MQTT IT and ST |
| Postman or `curl` | HTTP/REST web interface tests |
| Serial terminal (e.g. PlatformIO monitor) | Log and diagnostic output |

### 3.3 Firmware Build Variants

| Build variant | Description |
|---------------|-------------|
| `test_host` | Native host build; hardware stubs active; for UT |
| `test_target` | Target build with verbose logging and test hooks; for IT |
| `release` | Production build; no test hooks; for ST and release |

---

## 4. Test Cases — Firmware Architecture

TSDS reference: §4

### 4.1 Task Startup and Scheduling

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-FA-001 | IT | All 13 tasks created and running after boot | Boot DUT; observe serial log | All tasks T1–T13 log "started" within 5 s of boot; no stack overflow reported |
| IT-FA-002 | IT | T1 Watchdog/Heartbeat runs at highest priority | Boot DUT; observe HB LED | HB LED blinks at 4 Hz during startup, transitions to 1 Hz within 10 s |
| IT-FA-003 | IT | Core assignment verified | Boot DUT; log task core IDs via `xTaskGetAffinity` | T1–T9 on Core 1; T10–T13 on Core 0 |
| IT-FA-004 | IT | No stack overflow under full load | Run ST for 30 min with all features active | No stack overflow warning on any task; `uxTaskGetStackHighWaterMark` > 10% remaining on all tasks |

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
| UT-FA-011 | UT | EG1 flags set and cleared by designated owner tasks only | Verify flag ownership by checking set/clear calls in stubs | EG1.WIND_OVERRIDE set/cleared only by T3 stub; EG1.MANUAL_OVERRIDE set by T2, cleared by T6 |
| IT-FA-012 | IT | Priority inheritance: T3 not starved when T4 holds MX2 | Trigger T3 wind evaluation while T4 stub holds MX2 briefly | T4 priority temporarily raised; MX2 released promptly; T3 completes within 50 ms |

---

## 5. Test Cases — Sensor Polling

TSDS reference: §5.1 | FRS: FR-S04, FR-W03

### 5.1 Normal Operation

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-SP-001 | IT | SenseCAP S200 polled and values stored | Connect Modbus simulator returning valid wind speed (5.0 m/s) and direction (270°); wait 1 poll cycle | T4 current measurement: wind speed = 5.0 m/s, direction = 270°; no fault flag set |
| IT-SP-002 | IT | FG6485A polled and values stored | Connect Modbus simulator returning T = 22.5 °C, RH = 65%; wait 1 poll cycle | T4 current measurement: T = 22.5 °C, RH = 65%; no fault flag set |
| IT-SP-003 | IT | Poll interval respected | Log timestamps of consecutive poll cycles | Interval between polls within ±5% of configured value (default 60 s) |
| IT-SP-004 | IT | T4 notified after successful poll | Instrument TN1 and TN2 in test build | TN1 (wind) and TN2 (sensor) task notifications sent to T3 and T6 within 100 ms of poll completion |

### 5.2 Fault Detection

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-SP-005 | IT | No response timeout — wind sensor | Disconnect Modbus simulator for S200; wait 1 poll cycle | EG1.SENSOR_FAULT_W set; fault event posted to Q3; LCD shows wind sensor alarm |
| IT-SP-006 | IT | No response timeout — T/RH sensor | Disconnect Modbus simulator for FG6485A; wait 1 poll cycle | EG1.SENSOR_FAULT_T set; fault event posted to Q3; LCD shows T/RH sensor alarm |
| UT-SP-007 | UT | CRC error triggers retry then fault | Simulate one corrupt frame then silence; assert fault after second failure | One retry attempt; fault flag set after second failure; no false positive on single CRC error |
| UT-SP-008 | UT | Out-of-range value discarded | Inject wind speed = 200 m/s (beyond 60 m/s max) | Reading discarded; EG1.SENSOR_FAULT_W set; previous valid value retained in T4 |
| IT-SP-009 | IT | Fault clears on successful poll | Set fault by disconnecting sensor; reconnect; wait 1 poll cycle | EG1.SENSOR_FAULT_W (or _T) cleared; fault-clear event posted to Q3; LCD alarm clears |

---

## 6. Test Cases — Climate Control Logic

TSDS reference: §5.2 | FRS: FR-C09, FR-C10, FR-CR01–FR-CR04, FR-M08–FR-M11

### 6.1 Operating Mode State Machine

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-001 | UT | Boot state is Automatic | Initialise climate control module with default config stub | Operating mode = Automatic |
| UT-CC-002 | UT | Transition Automatic → Standby on admin command | Post mode-change command to stub; assert mode | Mode = Standby; no relay commands issued while in Standby |
| UT-CC-003 | UT | Transition Standby → Automatic on admin command | Reverse of CC-002 | Mode = Automatic; climate evaluation resumes on next sensor data |
| UT-CC-004 | UT | Wind-override state entered when T3 sets EG1.WIND_OVERRIDE | Set EG1.WIND_OVERRIDE in stub; trigger T6 evaluation | T6 inhibits all open commands; issues CLOSE_ALL; mode = Wind-override |
| UT-CC-005 | UT | Wind-override clears when EG1.WIND_OVERRIDE cleared | Clear EG1.WIND_OVERRIDE; trigger T6 | Mode returns to Automatic; normal climate evaluation resumes |
| UT-CC-006 | UT | Manual-override entered on TN3 notification | Send TN3 to T6 stub | Mode = Manual-override; climate commands inhibited; calibration cycle initiated |
| UT-CC-007 | UT | Calibration cycle closes all channels then resumes Automatic | TN3 received; monitor relay commands from T2 stub | CLOSE_ALL issued on all three channels; after dwell timers expire mode = Automatic |
| UT-CC-008 | UT | Calibration deferred if WIND_OVERRIDE active when manual override clears | Set both WIND_OVERRIDE and MANUAL_OVERRIDE; clear MANUAL_OVERRIDE; assert calibration deferred | Calibration cycle not started until WIND_OVERRIDE also cleared |

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
| UT-CC-014 | UT | Window opens when T > T_max | Set T_max = 25 °C, hysteresis = 1 °C; inject T = 26 °C | OPEN command issued |
| UT-CC-015 | UT | Window stays open until T < T_max − hysteresis | T = 24.5 °C (above T_max − hyst = 24 °C) | No CLOSE command issued |
| UT-CC-016 | UT | Window closes when T < T_max − hysteresis | T = 23.9 °C | CLOSE command issued |
| UT-CC-017 | UT | Window closes when T < T_min | Set T_min = 15 °C; inject T = 14 °C | CLOSE command issued |
| UT-CC-018 | UT | Window opens when RH > RH_max | Set RH_max = 80%, hysteresis = 3%; inject RH = 82% | OPEN command issued |
| UT-CC-019 | UT | No relay chatter at setpoint boundary | Inject T alternating 24.9 / 25.1 °C every poll cycle for 10 cycles | Window state changes ≤ 2 times (hysteresis prevents chatter) |

### 6.4 Conflict Resolution

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-CC-020 | UT | Temperature demands OPEN, humidity demands CLOSE → temperature wins (default priority) | T = 26 °C (above T_max), RH = 60% (below RH_max) | OPEN command issued; conflict logged to Q3 |
| UT-CC-021 | UT | Temperature demands CLOSE, humidity demands OPEN → temperature priority gives CLOSE | T = 14 °C (below T_min), RH = 85% (above RH_max) | CLOSE command issued; conflict logged |
| UT-CC-022 | UT | No conflict when both demand same action | T = 26 °C (OPEN), RH = 85% (OPEN) | Single OPEN command; no conflict logged |

---

## 7. Test Cases — Event Log Manager

TSDS reference: §5.3 | FRS: FR-LG05, FR-LG06, FR-LG07, FR-LG08

### 7.1 Log Entry Storage

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-EL-001 | UT | Log entry struct has correct fixed size | Assert `sizeof(log_entry_t)` in unit test | Size matches documented fixed record size |
| IT-EL-002 | IT | Event posted to Q3 is written to NVS ring buffer | Post a MODE_CHANGE event; read NVS log namespace | Entry present in NVS with correct event_type, initiator, and timestamp |
| IT-EL-003 | IT | NVS ring buffer wraps after 1000 entries | Write 1001 events; read ring buffer | 1000 entries retained; entry #1 overwritten by entry #1001; entry #2 intact |
| IT-EL-004 | IT | SD card preferred over NVS when present | Insert formatted SD card; write 10 events | Events written to SD card file; NVS log not updated |
| IT-EL-005 | IT | Fallback to NVS when SD card absent | Remove SD card; write 10 events | Events written to NVS ring buffer; no crash or error halt |
| IT-EL-006 | IT | SD card presence detected at runtime | Boot without SD; insert card mid-operation; observe log destination | T9 detects card on next write cycle; subsequent events go to SD card |

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

---

## 8. Test Cases — Access Control and Session Management

TSDS reference: §5.4 | FRS: FR-AC06, FR-AC07

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

### 8.2 PIN Storage Security

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-AC-008 | UT | Farmer PIN stored as salted hash, not plain text | Write farmer PIN via config stub; read NVS key `pin_farmer_hash` | Value is a hash string; original PIN not recoverable from stored value |
| UT-AC-009 | UT | Two identical PINs produce different stored hashes (salt uniqueness) | Set same PIN twice with different salts; compare stored values | Stored hashes differ |
| UT-AC-010 | UT | PIN change updates stored hash | Change farmer PIN; verify new PIN accepted and old PIN rejected | New PIN authenticates; old PIN fails |

### 8.3 Login Lockout

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| UT-AC-011 | UT | Lockout activates after N failed attempts (default 5) | Submit wrong PIN 5 times | Input locked; lockout event posted to Q3 |
| UT-AC-012 | UT | Correct PIN rejected during lockout period | Submit correct PIN immediately after lockout | Authentication fails during lockout |
| UT-AC-013 | UT | Lockout expires after configured timeout | Wait for lockout timeout; submit correct PIN | Authentication succeeds after timeout |
| UT-AC-014 | UT | Failed attempt counter resets after successful login | Fail 4 times; succeed once; fail 4 more times | No lockout triggered (counter reset by successful login) |

### 8.4 Administrator Recovery

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-AC-015 | IT | Admin PIN recovery requires hardware jumper | Attempt recovery procedure without jumper fitted | Recovery procedure not triggered |
| IT-AC-016 | IT | Admin PIN reset to factory default on valid recovery | Fit jumper; perform recovery key combination at power-on | Admin PIN reset to factory default; recovery event logged |

---

## 9. Test Cases — Local User Interface

TSDS reference: §5.5 | FRS: FR-UI07

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

---

## 10. Test Cases — WiFi Access Point Mode

TSDS reference: §5.6 | FRS: TR-NW01

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WA-001 | IT | AP does not start on boot | Power cycle DUT; scan for AP SSID for 30 s | AP SSID not visible until explicitly enabled |
| IT-WA-002 | IT | AP starts when enabled via admin menu | Enter admin session; enable AP via menu | AP SSID visible on WiFi scan within 10 s; LCD shows "AP active" |
| IT-WA-003 | IT | Client can connect to AP | Connect test PC to AP | DHCP address assigned; web interface reachable |
| IT-WA-004 | IT | AP uses WPA2 security minimum | Scan AP and inspect security type | WPA2 or WPA3 reported; open/WEP not accepted |
| IT-WA-005 | IT | AP shuts down after configured timeout with no client | Enable AP; leave idle; wait for timeout | AP SSID disappears after configured timeout; LCD clears AP indicator |
| IT-WA-006 | IT | AP timeout resets while client is connected | Enable AP; connect client; wait beyond timeout | AP remains active while client connected; shuts down after client disconnects and timeout elapses |
| IT-WA-007 | IT | AP timeout is configurable | Set AP timeout to 2 min via admin menu; enable AP; leave idle | AP shuts down after 2 min ± 10 s |

---

## 11. Test Cases — WiFi Client Mode

TSDS reference: §5.7

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WC-001 | IT | Client connects to configured SSID with DHCP | Configure SSID and password via admin menu; enable client mode | DUT connects; IP address assigned; LCD shows connected + IP address |
| IT-WC-002 | IT | Client reconnects automatically after AP drop | Connect; disable test AP for 30 s; re-enable | DUT reconnects within 60 s of AP restoration; LCD updates |
| IT-WC-003 | IT | Static IP configuration applied correctly | Set static IP, mask, gateway, DNS via admin; enable client | DUT uses configured static IP; DHCP not attempted |
| IT-WC-004 | IT | NTP sync triggered on client connection | Connect client; observe RTC update | System time synchronised with NTP within 30 s of connection; TN4 sent to T4 |
| IT-WC-005 | IT | LCD shows Disconnected when client mode enabled but no AP available | Enable client mode; ensure configured SSID not available | LCD shows "Disconnected"; no crash or hang |

---

## 12. Test Cases — Web Interface

TSDS reference: §5.8 | FRS: FR-NW06

### 12.1 Authentication

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-001 | ST | Unauthenticated request returns 401 / redirect to login | GET dashboard page without session cookie | HTTP 401 or redirect to login page; no data served |
| ST-WI-002 | ST | Farmer login via web grants farmer-level access | POST correct farmer PIN to login endpoint | Session cookie issued; farmer parameters accessible; admin parameters absent |
| ST-WI-003 | ST | Admin login via web grants full access | POST correct admin PIN | Session cookie issued; all parameters accessible |
| ST-WI-004 | ST | Web session expires after configured timeout | Login; idle for timeout period | Session cookie invalidated; subsequent request redirects to login |

### 12.2 Dashboard and Settings

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-005 | ST | Dashboard shows live sensor values | Open dashboard; compare with LCD main screen | T, RH, wind speed, direction, window states, mode all match LCD; updates within 5 s |
| ST-WI-006 | ST | Setpoint change via web reflected in firmware | Set T_max = 28 °C via web settings page | T4 configuration updated; LCD settings screen shows new value; persisted to NVS |
| ST-WI-007 | ST | Admin-only parameters not visible in farmer session | Login as farmer; inspect settings page | Admin-only parameters absent from page DOM and HTTP responses |
| ST-WI-008 | ST | Manual window command from web triggers relay | Issue OPEN M1 from web dashboard | M1-OPEN relay energises; T2 state machine transitions to MOVING |

### 12.3 MQTT Client

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-WI-009 | ST | MQTT publishes sensor data at configured interval | Configure broker; subscribe to topic; wait | Messages received at configured interval; payload contains T, RH, wind speed, direction |
| ST-WI-010 | ST | MQTT publishes window states and mode | Observe MQTT messages during mode change | Status topic updated within 5 s of mode change |
| ST-WI-011 | ST | MQTT CLOSE_ALL command received and executed | Publish CLOSE_ALL to command topic | All relays receive CLOSE command; T2 state machines transition |

---

## 13. Test Cases — OTA Firmware Update

TSDS reference: §5.9 | FRS: TR-SW02

### 13.1 Firmware Update

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-OT-001 | ST | New firmware image uploaded and applied via web | Login as admin; upload valid firmware binary via OTA page | Upload accepted; DUT reboots; new firmware version reported on dashboard |
| ST-OT-002 | ST | NVS configuration retained after firmware update | Set a custom setpoint; perform OTA update | After reboot, setpoint value unchanged |
| ST-OT-003 | ST | Corrupt firmware image rejected | Upload binary with invalid checksum | Upload rejected with error; DUT remains on current firmware and continues operating |
| ST-OT-004 | ST | Failsafe rollback after 3 failed boots | Upload firmware that halts during startup; power cycle 3 times | After 3rd failed boot, DUT reverts to previous firmware bank; operates normally |
| ST-OT-005 | ST | OTA blocks web file serving during write | Begin web file OTA upload; attempt to load web page simultaneously | Page request deferred (EG1.OTA_IN_PROGRESS set); served after OTA completes |
| ST-OT-006 | ST | Combined firmware + web file update activates only when both complete | Upload firmware image only (no web file); assert no activation | Active bank not switched until web file package also uploaded and verified |

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
| IT-NV-002 | IT | Setting change persisted immediately to NVS | Change T_max via menu; power cycle | After reboot, T_max = previously set value |
| IT-NV-003 | IT | All NVS namespaces written and read correctly | Write one value per namespace; power cycle; read back | All values intact across all namespaces |
| IT-NV-004 | IT | NVS survives OTA firmware update | Set custom values; perform OTA; check values | All values unchanged after update |
| IT-NV-005 | IT | Factory reset clears all NVS and restores defaults | Trigger factory reset; read all NVS keys | All keys at factory default; no residual values from previous configuration |
| UT-NV-006 | UT | Range validation rejects out-of-range write | Submit T_max = 999 °C to T4 config module stub | Write rejected; NVS not updated; error returned to caller |

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
| IT-WD-005 | IT | Wind safety CLOSE_ALL on wind sensor fault | Set EG1.SENSOR_FAULT_W | T3 issues CLOSE_ALL; all windows close; fault alarm on LCD |
| IT-WD-006 | IT | Last known window state maintained during T/RH fault | Open M1; simulate T/RH sensor fault | M1 remains OPEN (state retained); no spurious CLOSE command |

### 15.3 Manual Override

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| IT-WD-007 | IT | Manual override detected on feedback input | Assert RRK-3 feedback GPIO; observe system response | EG1.MANUAL_OVERRIDE set; T6 inhibits commands; LCD shows manual override; event logged |
| IT-WD-008 | IT | Calibration cycle runs after manual override clears | Assert then deassert feedback input; observe relay commands | CLOSE_ALL issued on all channels; dwell timers run; mode returns to Automatic |
| IT-WD-009 | IT | Calibration deferred when wind override also active | Assert feedback input while WIND_OVERRIDE is set; deassert feedback | Calibration not started until WIND_OVERRIDE also cleared |

---

## 16. Test Cases — Security

TSDS reference: §2.4, §3 | FRS: TR-NW01, TR-NW04, FR-AC06

| ID | Level | Description | Steps | Expected result |
|----|-------|-------------|-------|-----------------|
| ST-SE-001 | ST | Plain-text PINs not present in NVS flash dump | Read full NVS partition; search for known PIN strings | No plain-text PIN found; only hash values present (FR-AC06) |
| ST-SE-002 | ST | WiFi AP uses WPA2 or stronger | Scan AP with WiFi analyser | Security type = WPA2 or WPA3; no open or WEP (TR-NW01) |
| ST-SE-003 | ST | Web interface requires authentication before serving any data | Send unauthenticated GET to each API endpoint | All endpoints return HTTP 401 or redirect; no data payload returned (FR-NW06) |
| ST-SE-004 | ST | Session cookie not reusable after logout | Login; record session cookie; logout; replay cookie | Replayed cookie rejected with HTTP 401 |
| ST-SE-005 | ST | HTTP traffic does not expose PIN in plain text | Capture HTTP traffic during PIN submission | PIN not present in plain text in captured frames; hashed or transmitted over protected channel |
| ST-SE-006 | ST | Login lockout applies equally via web and keyboard | Submit wrong PIN 5 times via web login | Web login locked for configured duration; matches keyboard lockout behaviour (FR-AC07) |

---

## 17. Traceability Matrix

| TSDS Section | Requirement IDs | Test case IDs |
|--------------|-----------------|---------------|
| §4 Firmware Architecture | TR-SW05 | IT-FA-001 to IT-FA-004, UT-FA-005 to UT-FA-012 |
| §5.1 Sensor Polling | FR-S04, FR-W03 | IT-SP-001 to IT-SP-009, UT-SP-007, UT-SP-008 |
| §5.2 Climate Control Logic | FR-C09, FR-C10, FR-CR01–FR-CR04, FR-M08–FR-M11 | UT-CC-001 to UT-CC-022 |
| §5.3 Event Log Manager | FR-LG05, FR-LG06, FR-LG07, FR-LG08 | UT-EL-001, IT-EL-002 to IT-EL-008, ST-EL-009 to ST-EL-011 |
| §5.4 Access Control | FR-AC06, FR-AC07 | UT-AC-001 to UT-AC-014, IT-AC-006, IT-AC-007, IT-AC-015, IT-AC-016 |
| §5.5 Local User Interface | FR-UI07 | UT-UI-001 to UT-UI-004, IT-UI-005 to IT-UI-011 |
| §5.6 WiFi AP Mode | TR-NW01 | IT-WA-001 to IT-WA-007 |
| §5.7 WiFi Client Mode | — | IT-WC-001 to IT-WC-005 |
| §5.8 Web Interface | FR-NW06, FR-MQ01–FR-MQ05 | ST-WI-001 to ST-WI-011 |
| §5.9 OTA Firmware Update | TR-SW02 | ST-OT-001 to ST-OT-006, IT-OT-007 |
| §5.10 NVS Configuration Storage | FR-CF06, TR-SW01 | IT-NV-001 to IT-NV-005, UT-NV-006 |
| §5.11 Watchdog and Fault Handling | TR-SW03, FR-ST02, FR-S05, FR-W04 | IT-WD-001 to IT-WD-009 |
| §2.4 Security | TR-NW01, TR-NW04, FR-AC06, FR-NW06 | ST-SE-001 to ST-SE-006 |

---

*End of document — version 0.1 draft*
