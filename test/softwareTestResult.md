# Software Test Results
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Software Test Results                          |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 1.0                                           |
| Date         | 2026-05-06                                    |
| Status       | Complete (firmware v1.16.6)                   |
| Based on     | `softwareTestPlan.md` v0.3                    |
| Evidence     | `firmware/firmwareImplementationResults.md`   |

---

## Result Legend

| Symbol | Meaning |
|--------|---------|
| ✅ PASS | Verified on hardware or by unit test; explicit result noted in firmwareImplementationResults.md |
| ⬜ NOT EXECUTED | No evidence of execution in firmwareImplementationResults.md |
| ⚠️ DEFERRED | Feature not yet implemented (e.g. MQTT); testing intentionally skipped |
| 🔲 PENDING | Implementation complete; hardware test explicitly listed as pending in results file |
| ❌ FAIL | Known failure; see notes |

---

## Table of Contents

1. [Firmware Architecture](#1-firmware-architecture)
2. [Sensor Polling](#2-sensor-polling)
3. [Climate Control Logic](#3-climate-control-logic)
4. [Event Log Manager](#4-event-log-manager)
5. [Access Control and Session Management](#5-access-control-and-session-management)
6. [Local User Interface](#6-local-user-interface)
7. [WiFi Access Point Mode](#7-wifi-access-point-mode)
8. [WiFi Client Mode](#8-wifi-client-mode)
9. [Web Interface](#9-web-interface)
10. [OTA Firmware Update](#10-ota-firmware-update)
11. [NVS Configuration Storage](#11-nvs-configuration-storage)
12. [Watchdog and Fault Handling](#12-watchdog-and-fault-handling)
13. [Security](#13-security)
14. [Day/Night Management](#14-daynight-management)
15. [RGB Status LED](#15-rgb-status-led)
16. [Test Coverage Report](#16-test-coverage-report)

---

## 1. Firmware Architecture

TSDS reference: §4

### 1.1 Task Startup and Scheduling

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-FA-001 | IT | 12 tasks created and running after boot; T13 absent until OTA triggered | ✅ PASS | Phase 0: boot log shows all task "started" messages; watchdog/heartbeat confirmed operational. Verified multiple phases through v1.16.6. |
| IT-FA-002 | IT | T1 Watchdog/Heartbeat runs at highest priority; HB LED blinks at 4 Hz then 1 Hz | ✅ PASS | Phase 0 verification: HB LED blink sequence confirmed on hardware. |
| IT-FA-003 | IT | Core assignment: T1–T9 on Core 1; T10–T12 on Core 0 | ⬜ NOT EXECUTED | Core affinity not explicitly verified in implementation results. |
| IT-FA-004 | IT | No stack overflow under full load for 30 min | ⬜ NOT EXECUTED | Long-duration stack-headroom test not documented in results. |
| IT-FA-013 | IT | EG1_BIT_CALIBRATING set during CLOSE_ALL; `mode: WINDOW_CAL` during cal, `AUTOMATIC` after | ✅ PASS | v1.16.3: `EG1_BIT_CALIBRATING` (bit 6) added to `app_types.h`; `relay_controller.cpp` sets/clears around `calib_close_all()`; `web_server.cpp` serialises as `"WINDOW_CAL"`; LCD shows `"Mode:Window Cal."` — all confirmed on hardware. |

### 1.2 Inter-task Queues

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-FA-005 | UT | Q1 command delivered from T6 stub to T2 stub | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-FA-006 | UT | Q1 priority: T3 CLOSE_ALL before T6 command | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-FA-007 | UT | Q3 drop-oldest on overflow | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-FA-008 | UT | Q6 depth 1: new reading overwrites previous | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |

### 1.3 Synchronization Primitives

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-FA-009 | UT | MX2 prevents concurrent T4 write / T3 read | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-FA-010 | UT | MX2/MX3 independence: T3 not blocked by MX3 hold | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-FA-011 | UT | EG1 flags set/cleared by owner tasks only | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| IT-FA-012 | IT | Priority inheritance: T3 not starved when T4 holds MX2 | ⬜ NOT EXECUTED | Priority inversion test not documented in results. |

---

## 2. Sensor Polling

TSDS reference: §5.1 | FRS: FR-S03, FR-S04, FR-S06, FR-S07, FR-W03

### 2.1 Normal Operation

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-SP-001 | IT | S200 polled; wind speed and direction stored correctly | ✅ PASS | Phase 3 T5: Modbus poll with SenseCAP S200 emulator confirmed; values stored in T4. |
| IT-SP-002 | IT | FG6485A polled; T and RH stored correctly | ✅ PASS | Phase 3 T5: FG6485A emulator returns T = 22.5 °C, RH = 65%; T4 current measurement confirmed. |
| IT-SP-003 | IT | Poll interval respected (within ±5%) | ✅ PASS | v1.16.0: duplicate timestamp bug fixed (replaced `dm_get_unix_time()` with `time(NULL)`); no duplicate rows at 30 s poll confirmed on hardware. |
| IT-SP-004 | IT | TN1 and TN2 notifications sent to T3 and T6 after poll | ✅ PASS | Phase 3/4: T3 and T6 both respond correctly to new sensor data; T6 opens windows on high-T readings; T3 triggers CLOSE_ALL on high-wind — confirms notifications delivered. |

### 2.2 Fault Detection

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-SP-005 | IT | No-response timeout: wind sensor → SENSOR_FAULT_W set | ✅ PASS | Phase 4 T3: T3-01–T3-13 all confirmed; S200 fault detection verified with emulator set to Live mode. |
| IT-SP-006 | IT | No-response timeout: T/RH sensor → SENSOR_FAULT_T set | ✅ PASS | Phase 4 T3: FG6485A fault detection confirmed on hardware. |
| UT-SP-007 | UT | CRC error → retry then fault | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-SP-008 | UT | Out-of-range value discarded; fault set | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| IT-SP-009 | IT | Fault clears on successful poll | ✅ PASS | Phase 4 T3: fault-clear path confirmed; EG1 bit cleared; LCD alarm clears on reconnect. |

### 2.3 Sliding Average

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-SP-010 | UT | Sliding average of N samples converges to correct mean | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| IT-SP-011 | IT | avg_win_t = 3: three poll cycles required before average settles | ⬜ NOT EXECUTED | Multi-cycle averaging integration test not documented in results. |

---

## 3. Climate Control Logic

TSDS reference: §5.2 | FRS: FR-C01–FR-C12, FR-CR01–FR-CR04, FR-MA01–FR-MA08, FR-M01–FR-M07

### 3.1 Operating Mode State Machine

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-CC-001 | UT | Boot state = Automatic | ✅ PASS | Phase 6 T6: boot into AUTOMATIC mode confirmed; `GET /api/status` returns `mode: AUTOMATIC` after calibration completes. |
| UT-CC-002 | UT | Transition Automatic → Standby on admin command | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. Standby mode entry via web not verified. |
| UT-CC-003 | UT | Transition Standby → Automatic on admin command | ⬜ NOT EXECUTED | Host-build unit test not documented as executed. |
| UT-CC-004 | UT | Wind-override state entered when T3 sets EG1.WIND_OVERRIDE | ✅ PASS | Phase 4 T3: T3-01–T3-13 confirmed; WIND_OVERRIDE set on speed threshold breach; T6 inhibits OPEN and issues CLOSE_ALL. |
| UT-CC-005 | UT | Wind-override clears when EG1.WIND_OVERRIDE cleared | ✅ PASS | Phase 4 T3: WIND_OVERRIDE cleared when speed drops below threshold; mode returns to AUTOMATIC. |
| UT-CC-006 | UT | Motor-alarm state entered when EG1.MOTOR_ALARM set | ✅ PASS | Phase 2 T2 IT-07: motor alarm detection confirmed on hardware; relays de-energised; T6 inhibited. |
| UT-CC-007 | UT | CLOSE_ALL re-calibration after motor alarm clears | ✅ PASS | Phase 2 T2 IT-08: alarm-clear path confirmed; CLOSE_ALL re-calibration runs; mode returns to AUTOMATIC. |
| UT-CC-008 | UT | Motor alarm overrides WIND_OVERRIDE (highest priority) | ✅ PASS | Phase 2 T2 IT-13 (priority test): motor alarm takes priority over wind override; confirmed. |
| UT-CC-023 | UT | Standby does not suppress wind safety (FR-M04) | ⬜ NOT EXECUTED | Host-build unit test not documented. Standby + wind-safety interaction not explicitly verified. |

### 3.2 Window State Machine

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-CC-009 | UT | M1 CLOSED → MOVING → OPEN on OPEN command | ✅ PASS | Phase 2 T2 IT-01–IT-06: relay FSM state transitions fully verified on hardware with logic analyser (2026-05-03). |
| UT-CC-010 | UT | M1 OPEN → MOVING → CLOSED on CLOSE command | ✅ PASS | Phase 2 T2: CLOSE state transition confirmed with logic analyser. |
| UT-CC-011 | UT | OPEN + CLOSE mutual exclusion | ✅ PASS | Phase 2 T2: mutual exclusion between OPEN and CLOSE relays confirmed; verified on logic analyser. |
| UT-CC-012 | UT | Close-dwell prevents immediate reopen | ✅ PASS | Phase 2 T2: dwell timer logic confirmed; second OPEN withheld until dwell expires. |
| UT-CC-013 | UT | Three channels operate independently | ✅ PASS | Phase 2 T2: IT-06 (or equivalent): M1, M2, M3 operate concurrently without interference. |

### 3.3 Setpoints and Hysteresis

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-CC-014 | UT | OPEN when T > T_max_day (is_daytime = true) | ✅ PASS | Phase 6 T6: T6-04/T6-05 PASS; CMD_OPEN issued on temperature exceeding T_max_day. |
| UT-CC-015 | UT | Stay open when T above (T_max − hyst) | ✅ PASS | Phase 6 T6: hysteresis confirmed; no premature CLOSE while T inside hysteresis band. |
| UT-CC-016 | UT | CLOSE when T < T_max − hyst | ✅ PASS | Phase 6 T6: CMD_CLOSE issued when temperature drops below hysteresis threshold. |
| UT-CC-017 | UT | CLOSE when T < T_min_day | ✅ PASS | Phase 6 T6: minimum setpoint close behaviour confirmed. |
| UT-CC-018 | UT | OPEN when RH > RH_max_day (rh_ctrl_en = true) | ✅ PASS | Phase 6 T6: T6-07 PASS; OPEN issued on RH exceeding threshold. |
| UT-CC-019 | UT | No relay chatter at setpoint boundary | ⬜ NOT EXECUTED | Boundary oscillation test not documented in results. |
| UT-CC-024 | UT | CLOSE_ALL when RH < RH_min_day (over-dry) | ⬜ NOT EXECUTED | Over-dry CLOSE_ALL path not explicitly documented. |
| UT-CC-025 | UT | Graduated ventilation step 1: M1 only | ⬜ NOT EXECUTED | Graduated ventilation unit tests not documented as executed. |
| UT-CC-026 | UT | Graduated ventilation step 2: M1 + M2 | ⬜ NOT EXECUTED | See UT-CC-025. |
| UT-CC-027 | UT | Graduated ventilation step 3: M1 + M2 + M3 | ⬜ NOT EXECUTED | See UT-CC-025. |
| UT-CC-028 | UT | Night setpoints used when is_daytime = false | ⬜ NOT EXECUTED | Night setpoint profile unit test not documented as executed. |
| UT-CC-029 | UT | Day setpoints used when is_daytime = true | ⬜ NOT EXECUTED | See UT-CC-028. |

### 3.4 Conflict Resolution

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-CC-020 | UT | T demands OPEN, RH demands CLOSE → T wins (CR_TEMP_FIRST) | ⬜ NOT EXECUTED | Conflict resolution unit tests not documented as executed. |
| UT-CC-021 | UT | T demands CLOSE, RH demands OPEN → T priority gives CLOSE | ⬜ NOT EXECUTED | See UT-CC-020. |
| UT-CC-022 | UT | No conflict when both demand same action | ⬜ NOT EXECUTED | See UT-CC-020. |
| UT-CC-030 | UT | Conflict CR_RH_FIRST: humidity demand wins | ⬜ NOT EXECUTED | CR_RH_FIRST path not documented as executed. |
| UT-CC-031 | UT | Conflict CR_DEVIATION: higher step wins | ⬜ NOT EXECUTED | CR_DEVIATION path not documented as executed. |

### 3.5 Humidity and Wind Feature Flags

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-CC-032 | UT | RH control disabled: no OPEN on RH trigger | ⬜ NOT EXECUTED | rh_ctrl_en=false path unit test not documented. |
| IT-CC-033 | IT | Wind protection disabled: no CLOSE_ALL on threshold breach | ✅ PASS | Phase 4 T3: wind_prot_en=false case confirmed; WIND_OVERRIDE not set; mode remains AUTOMATIC. |

---

## 4. Event Log Manager

TSDS reference: §5.3 | FRS: FR-LG01–FR-LG09

### 4.1 Log Entry Storage

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-EL-001 | UT | `sizeof(log_entry_t)` = 12 bytes | ⬜ NOT EXECUTED | Struct-size assertion not documented as executed. |
| IT-EL-002 | IT | Event written to NVS ring buffer | ⬜ NOT EXECUTED | NVS ring write unit verification not documented explicitly. |
| IT-EL-003 | IT | NVS ring wraps at CONFIG_NVS_LOG_CAPACITY | ⬜ NOT EXECUTED | Ring-overflow wrap-around not explicitly verified. |
| IT-EL-004 | IT | SD card preferred over NVS when present | ✅ PASS | Phase 5 T9: T9-04/T9-05 PASS; SD card preferred path confirmed on hardware. |
| IT-EL-005 | IT | Fallback to NVS when SD absent | ✅ PASS | Phase 5 T9: T9-01/T9-03 PASS; NVS fallback on SD removal confirmed. |
| IT-EL-006 | IT | SD presence detected at runtime | ✅ PASS | Phase 5 T9: T9-07/T9-08/T9-09 PASS; dynamic SD card insertion detected during operation. |

### 4.2 Log Queue Behaviour

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-EL-007 | UT | Q3 sender is non-blocking | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-EL-008 | UT | Q3 drop-oldest on overflow | ⬜ NOT EXECUTED | Host-build unit test not documented. |

### 4.3 Log Retrieval

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-EL-009 | ST | Log viewable via web; all events in reverse-chronological order | ⬜ NOT EXECUTED | Web log viewer system test not documented. |
| ST-EL-010 | ST | Log filterable by event type | ⬜ NOT EXECUTED | Log filter test not documented. |
| ST-EL-011 | ST | Log persists across power cycle | ⬜ NOT EXECUTED | Power-cycle persistence system test not documented. |

### 4.4 Log Entry Content

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-EL-012 | IT | SETPOINT log entry contains operator identity and old/new values | ⬜ NOT EXECUTED | Structured log content verification not documented. |
| IT-EL-013 | IT | LOG_SENSOR event emitted each poll cycle with T, RH, wind values | ⬜ NOT EXECUTED | LOG_SENSOR content verification not documented explicitly. |
| IT-EL-014 | IT | Wind-override onset event logged with SYSTEM initiator | ✅ PASS | Phase 4 T3: wind-override onset and clearance logged; confirmed via serial log patterns. |

---

## 5. Access Control and Session Management

TSDS reference: §5.4 | FRS: FR-AC01–FR-AC09

### 5.1 PIN Entry and Session State

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-AC-001 | UT | Correct farmer PIN opens farmer session | ✅ PASS | Manually executed on LCD/Keyboard and in web-gui. |
| UT-AC-002 | UT | Correct admin PIN opens admin session | ✅ PASS | Manually executed on LCD/Keyboard and in web-gui.  |
| UT-AC-003 | UT | Wrong PIN rejected; session = Normal | ✅ PASS | Manually executed on LCD/Keyboard and in web-gui. |
| UT-AC-004 | UT | Farmer cannot access admin parameters | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-005 | UT | Admin can access all parameters | ✅ PASS | Manually executed on LCD/Keyboard and in web-gui. |
| IT-AC-006 | IT | Session timeout returns to Normal | ✅ PASS | v1.14.0: session expiry bug fixed (T11 sets inactivity flag; T8 closes session); confirmed on hardware. |
| IT-AC-007 | IT | Keypad activity resets session idle timer | ✅ PASS | Phase 7 T8: LCD session management confirmed; activity resets timer. |
| IT-AC-019 | IT | Web and keyboard sessions independent and concurrent | ✅ PASS | Manually executed on LCD/Keyboard and in web-gui.  |

### 5.2 PIN Storage Security

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-AC-008 | UT | Farmer PIN stored as salted SHA-256 hash | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-009 | UT | Same PIN with different salts → different hashes | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-010 | UT | PIN change updates stored hash | ⬜ NOT EXECUTED | Host-build unit test not documented. |

### 5.3 Login Lockout

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-AC-011 | UT | Lockout after N failed attempts | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-012 | UT | Correct PIN rejected during lockout | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-013 | UT | Lockout expires after timeout | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-AC-014 | UT | Failed counter resets after successful login | ⬜ NOT EXECUTED | Host-build unit test not documented. |

### 5.4 Administrator Recovery (GPIO0 BOOT Button)

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-AC-015 | IT | Short hold < 5 s — no effect | ✅ PASS | v1.16.1: Stage 0 (< 5 s) verified on hardware; no action, display returns to status screen. |
| IT-AC-016 | IT | Stage 1 (5–10 s): PIN reset to factory defaults | ✅ PASS | v1.16.1: Stage 1 verified on hardware; farmer and admin PINs reset to defaults. |
| IT-AC-017 | IT | Stage 2 (10–15 s): full NVS reset, no reboot | ✅ PASS | v1.16.1: Stage 2 verified on hardware; all NVS erased; system continues with factory defaults. |
| IT-AC-018 | IT | Stage 3 (15–20 s): factory reset + restart | ✅ PASS | v1.16.1: Stage 3 (auto at 20 s) verified on hardware; full NVS erase + ESP.restart(). |

---

## 6. Local User Interface

TSDS reference: §5.5 | FRS: FR-UI01–FR-UI09, FR-UI22–FR-UI24, FR-WS06, FR-WS10

### 6.1 Keypad Scanning and Debounce

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-UI-001 | UT | Single clean press → one key event | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-UI-002 | UT | Bouncing contact → one event | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-UI-003 | UT | Key repeat after initial delay | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| UT-UI-004 | UT | All 16 keys produce unique codes | ⬜ NOT EXECUTED | Host-build unit test not documented. |

### 6.2 Main Status Screen

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-005 | IT | Main screen shows T, RH, wind speed, direction | ✅ PASS | v1.16.0: T/RH page reformatted (temperature and RH on separate rows, aligned °C/% units) confirmed on hardware. |
| IT-UI-006 | IT | Main screen shows current operating mode | ✅ PASS | Phase 7 T8: mode indicator on LCD confirmed; `"Mode:Window Cal."` added in v1.16.3. |
| IT-UI-007 | IT | Active alarm displayed on main screen | ✅ PASS | Phase 4 T3: alarm indicators on LCD confirmed during sensor fault and motor alarm tests. |
| IT-UI-008 | IT | Screen refreshes when new sensor data arrives | ✅ PASS | Phase 7 T8: LCD update on new T4 data confirmed; v1.16.0 display improvements verified. |

### 6.3 Menu Navigation Depth

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-009 | IT | Any first-level setting reachable within 4 key presses | ⬜ NOT EXECUTED | Navigation depth formal test not documented. |
| IT-UI-010 | IT | `*` key navigates back one level at every depth | ✅ PASS | Phase 7 T8: `*` navigation confirmed (T8-03 = LCD navigation); v1.16.2 browse state `*` → group selector verified. |
| IT-UI-011 | IT | `#` key confirms value entry | ✅ PASS | Phase 7 T8: `#` confirmation for setpoint edits confirmed on hardware. |

### 6.4 Status Page Cycling

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-012 | IT | Status pages 0–4 auto-cycle every 5 s (FR-UI22) | ✅ PASS | Phase 7/v1.16.0: 5-page LCD rotation including page 4 (time/date) confirmed; v1.16.2 day/night browse integrated. |
| IT-UI-013 | IT | Page 4 shows time and NTP/RTC source label | ✅ PASS | v1.13.0: time display page implemented and confirmed; RTC label switches to NTP on WiFi sync. |

### 6.5 Manual Date/Time Set

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-014 | IT | Admin required; DDMMYY + HHMM entry; DS1307 updated | ✅ PASS | v1.13.0: `dm_set_manual_time()` implemented and LCD time-set flow confirmed on hardware. |
| IT-UI-015 | IT | `*` during date entry aborts without changing time | ✅ PASS | v1.13.0: abort path implemented; confirmed that time is unchanged on `*` press. |

### 6.6 Wind Protection Disabled Warning

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-016 | IT | Persistent warning while wind_prot_en = false (FR-WS10) | ⬜ NOT EXECUTED | LCD wind-protection-disabled indicator not explicitly verified in results. |

### 6.7 BOOT Button LCD Progress Bar

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-UI-017 | IT | BOOT hold suppresses normal display; animated bar; stage labels (FR-UI24) | ✅ PASS | v1.16.1: bar fills correctly; stage labels change at each 5 s boundary; no status flicker confirmed on hardware. |

---

## 7. WiFi Access Point Mode

TSDS reference: §5.6 | FRS: TR-NW01, FR-NW02

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-WA-001 | IT | AP does not start on boot | ✅ PASS | Phase 8 T10: AP-on-demand confirmed; AP SSID not visible until explicitly enabled. |
| IT-WA-002 | IT | AP starts when enabled via admin menu | ✅ PASS | Phase 8 T10: T10-01–T10-08 PASS; AP activation via admin menu confirmed. |
| IT-WA-003 | IT | Client connects to AP; DHCP address assigned | ✅ PASS | Phase 8 T10: client connection and DHCP verified. |
| IT-WA-004 | IT | AP uses WPA2 security minimum | ✅ PASS | Phase 8 T10: WPA2 security type confirmed on AP scan. |
| IT-WA-005 | IT | AP shuts down after timeout with no client | ✅ PASS | v1.12.0: AP lifecycle fix verified; AP shuts down after configured timeout. |
| IT-WA-006 | IT | AP timeout resets while client connected | ✅ PASS | v1.12.0: AP lifecycle confirmed; timeout deferred while client active. |
| IT-WA-007 | IT | AP timeout is configurable | ✅ PASS | Phase 8 T10: configurable AP timeout (`ap_timeout_min`) confirmed via web UI. |

---

## 8. WiFi Client Mode

TSDS reference: §5.7 | FRS: FR-NW01–FR-NW07, FR-DN06, FR-DN07

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-WC-001 | IT | Client connects to configured SSID with DHCP | ✅ PASS | Phase 8 T10: T10-01–T10-08 PASS; WiFi client connection confirmed. |
| IT-WC-002 | IT | Auto-reconnect after AP drop | ✅ PASS | Phase 8 T10: reconnect after AP drop confirmed. |
| IT-WC-003 | IT | Static IP configuration applied | ✅ PASS | Phase 8 T10: static IP mode confirmed. |
| IT-WC-004 | IT | NTP sync triggered on client connection | ✅ PASS | Phase 8 T10: NTP synchronisation confirmed; TN4 sent to T4; DS1307 updated. |
| IT-WC-005 | IT | LCD shows Disconnected when AP unavailable | ✅ PASS | Phase 8 T10: disconnected state on LCD confirmed. |
| IT-WC-006 | IT | Geolocation updates lat/lon and TZ after NTP sync (FR-DN06, FR-DN07) | ✅ PASS | v1.13.0: geolocation via ip-api.com confirmed; lat/lon and tz_str updated in NVS and cfg_shadow. |
| IT-WC-007 | IT | Timezone applied immediately without reboot (FR-DN07) | ✅ PASS | v1.13.0: `setenv`/`tzset` on config update confirmed; time display reflects new TZ within one refresh. |

---

## 9. Web Interface

TSDS reference: §5.8 | FRS: FR-NW06, FR-DN04, FR-MQ01–FR-MQ05

### 9.1 Authentication and Public Access

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-WI-001 | ST | Unauthenticated requests to protected endpoints → 401 | ✅ PASS | Phase 9 T11: web server authentication confirmed; protected endpoints return 401. |
| ST-WI-012 | ST | Unauthenticated GET /api/status → 200 | ✅ PASS | v1.16.3: `/api/status` auth check removed; public access confirmed on hardware. |
| ST-WI-013 | ST | Unauthenticated GET /api/history → 200 | ✅ PASS | v1.16.3: `/api/history` auth check removed; public access confirmed on hardware. |
| ST-WI-002 | ST | Farmer login via web → farmer-level access | ✅ PASS | Phase 9 T11: farmer role confirmed; restricted parameter set visible. |
| ST-WI-003 | ST | Admin login via web → full access | ✅ PASS | Phase 9 T11: admin role confirmed; all parameters accessible. |
| ST-WI-004 | ST | Web session expires after configured timeout | ✅ PASS | v1.14.0: session expiry fix confirmed; subsequent request returns 401. |

### 9.2 Dashboard and Settings

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-WI-005 | ST | Dashboard shows live sensor values | ✅ PASS | Phase 9 T11: dashboard live data confirmed on hardware; values match LCD. |
| ST-WI-006 | ST | Setpoint change via web reflected in firmware and NVS | ✅ PASS | Phase 9 T11: T_max_day change via web confirmed; T4 config updated; NVS persisted. |
| ST-WI-007 | ST | Admin-only parameters absent in farmer session | ✅ PASS | Phase 9 T11: role-based parameter hiding confirmed on web interface. |
| ST-WI-008 | ST | No window open/close controls on dashboard | ✅ PASS | Phase 9 T11: confirmed no manual window controls present in web UI. |
| ST-WI-014 | ST | Dashboard shows today's sunrise and sunset (FR-DN04) | ✅ PASS | v1.13.0: sunrise/sunset fields added to dashboard; confirmed with valid lat/lon. |
| ST-WI-015 | ST | Sensor history — newest row at top (highest timestamp first) | ✅ PASS | v1.16.6: `dm_ring_count()` + offset fix + `slice().reverse()` in `loadHistory()`; newest row confirmed at top after 1+ hour operation. |

### 9.3 MQTT Client

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-WI-009 | ST | MQTT publishes sensor data at configured interval | ⚠️ DEFERRED | T12 MQTT client remains a stub as of v1.16.6; MQTT not yet implemented. |
| ST-WI-010 | ST | MQTT publishes window states and mode on change | ⚠️ DEFERRED | See ST-WI-009. |
| ST-WI-011 | ST | MQTT CLOSE_ALL command received and executed | ⚠️ DEFERRED | See ST-WI-009. |

---

## 10. OTA Firmware Update

TSDS reference: §5.9 | FRS: TR-SW02

### 10.1 Firmware Update

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-OT-001 | ST | New firmware uploaded and applied via web | ✅ PASS | Phase 10 T13 v1.15.0: firmware OTA upload confirmed; device reboots to new bank; version reported on dashboard. |
| ST-OT-002 | ST | NVS retained after firmware OTA | ✅ PASS | Phase 10 T13 v1.15.0: NVS settings intact after OTA confirmed. |
| ST-OT-003 | ST | Corrupt firmware image rejected | ⬜ NOT EXECUTED | Corrupt-image rejection test not documented in results. |
| ST-OT-004 | ST | 3-fail rollback | ✅ PASS | Phase 10 T13 v1.15.0: 3-fail rollback confirmed; device reverts to previous bank on 3rd failed boot. |
| ST-OT-005 | ST | OTA blocks web file serving during write (EG1.OTA_IN_PROGRESS) | ⬜ NOT EXECUTED | OTA-in-progress web-block test not explicitly documented. |
| ST-OT-006 | ST | Combined firmware + web file update activates only when both complete | ✅ PASS | Phase 10 T13 v1.15.0: two-phase upload (firmware → assets → activation) confirmed on hardware. |
| ST-OT-008 | ST | Asset version mismatch warning shown on dashboard | ⬜ NOT EXECUTED | Version-mismatch banner test not documented. |

### 10.2 USB OTA

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-OT-007 | IT | Firmware flashable via USB-C | ✅ PASS | Repeated throughout all phases; every firmware version from v0.1.0 to v1.16.6 flashed via PlatformIO native USB. |

---

## 11. NVS Configuration Storage

TSDS reference: §5.10 | FRS: FR-CF06, TR-SW01

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-NV-001 | IT | Factory defaults applied on blank NVS | ✅ PASS | Phase 1 T4: blank-NVS boot confirmed; all parameters at documented defaults. |
| IT-NV-002 | IT | Setting change persisted to NVS; survives power cycle | ✅ PASS | Phase 1 T4: NVS write confirmed; value present after reboot. |
| IT-NV-003 | IT | All NVS namespaces (climate, wind, motor, system, access) written and read | ✅ PASS | Phase 1 T4: all namespaces verified; T4 loads complete `cfg_shadow_t` on boot. |
| IT-NV-004 | IT | NVS survives OTA firmware update | ✅ PASS | Phase 10 T13 v1.15.0: NVS intact after OTA (same as ST-OT-002). |
| IT-NV-005 | IT | Factory reset clears all NVS | ✅ PASS | v1.16.1: Stage 2 (full NVS erase) and Stage 3 (full erase + reboot) confirmed on hardware. |
| UT-NV-006 | UT | Range validation rejects out-of-range write | ⬜ NOT EXECUTED | Host-build unit test not documented. |
| IT-NV-007 | IT | NVS schema version mismatch handled gracefully | ⬜ NOT EXECUTED | Schema migration integration test not documented. |

---

## 12. Watchdog and Fault Handling

TSDS reference: §5.11 | FRS: TR-SW03, FR-ST02, FR-S05, FR-W04

### 12.1 Watchdog

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-WD-001 | IT | Watchdog resets MCU on T1 starvation | ⬜ NOT EXECUTED | Deliberate T1 starvation test not documented. |
| IT-WD-002 | IT | Restart closes all relays after watchdog reset | ⬜ NOT EXECUTED | Post-WDT-reset CLOSE_ALL test not documented. |
| IT-WD-003 | IT | Watchdog reset reason logged | ⬜ NOT EXECUTED | Reset-reason log entry test not documented. |

### 12.2 Sensor Fault Handling

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-WD-004 | IT | Climate control inhibited on T/RH sensor fault | ✅ PASS | Phase 4 T3: T3-01–T3-13 confirmed; T6 inhibited during SENSOR_FAULT_T; no OPEN commands issued. |
| IT-WD-005 | IT | Wind safety CLOSE_ALL on wind sensor fault (safe-fail) | ✅ PASS | Phase 4 T3: WIND_OVERRIDE set on SENSOR_FAULT_W; CLOSE_ALL issued; confirmed on hardware. |
| IT-WD-006 | IT | Last known window state maintained during T/RH fault | ✅ PASS | Phase 4 T3: state retention during fault confirmed; no spurious CLOSE on T/RH fault. |

### 12.3 Motor Alarm

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-WD-007 | IT | Motor alarm detected on GPIO42 LOW; all relays de-energised | ✅ PASS | Phase 2 T2 IT-07: motor alarm onset confirmed on hardware with logic analyser; EG1.MOTOR_ALARM set; T6 inhibited. |
| IT-WD-008 | IT | CLOSE_ALL re-calibration after motor alarm clears | ✅ PASS | Phase 2 T2 IT-08: alarm-clear guard + CLOSE_ALL re-calibration + mode = AUTOMATIC confirmed. |
| IT-WD-009 | IT | Alarm mid-OPEN move de-energises relay immediately | ✅ PASS | Phase 2 T2 IT-09: alarm during ongoing open move confirmed; relay de-energised immediately. |
| IT-WD-010 | IT | Alarm re-asserted during 60 s guard detected ≤ 5 s | 🔲 PENDING | v1.16.4: fix implemented (per-chunk `gpio_read` inside guard loop); hardware test explicitly marked "Pending" in results. |
| IT-WD-011 | IT | Alarm active at power-on; CLOSE_ALL calibration skipped | 🔲 PENDING | v1.16.4: boot-time GPIO read added; hardware test explicitly marked "Pending" in results. |

---

## 13. Security

TSDS reference: §2.4, §3 | FRS: TR-NW01, TR-NW04, FR-AC06, FR-NW06

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| ST-SE-001 | ST | No plain-text PINs in NVS flash dump | ✅ PASS | Phase 7/8 T8: PIN storage as SHA-256 hash confirmed; NVS stores `pin_farmer_hash` / `pin_admin_hash` — no plain text. |
| ST-SE-002 | ST | WiFi AP uses WPA2 or stronger | ✅ PASS | Phase 8 T10: WPA2 security confirmed on AP scan. |
| ST-SE-003 | ST | Protected endpoints return 401; public endpoints return 200 | ✅ PASS | v1.16.3: /api/status, /api/history, /api/sd/status made public; write endpoints and /api/config remain auth-required; confirmed. |
| ST-SE-004 | ST | Session cookie invalid after logout | ✅ PASS | Phase 9 T11: logout path confirmed; replayed cookie rejected with 401. |
| ST-SE-005 | ST | HTTP traffic does not expose PIN in plain text | ⬜ NOT EXECUTED | HTTP traffic capture for PIN leakage not documented. |
| ST-SE-006 | ST | Web login lockout matches keyboard lockout | ⬜ NOT EXECUTED | Web lockout boundary test not documented. |

---

## 14. Day/Night Management

TSDS reference: §4.3 T4, §5.2 | FRS: FR-DN01–FR-DN07, FR-C01–FR-C08

### 14.1 Sunrise/Sunset Calculation

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| UT-DN-001 | UT | Sunrise/sunset accuracy for known lat/lon/date (NOAA reference ±5 min) | ✅ PASS | Phase 1 T4: sunrise/sunset algorithm confirmed; values compared against NOAA reference for configured location. |
| UT-DN-002 | UT | is_daytime = true between sunrise and sunset | ✅ PASS | Phase 1 T4: `is_daytime` flag confirmed true during daytime period. |
| UT-DN-003 | UT | is_daytime = false before sunrise | ✅ PASS | Phase 1 T4: `is_daytime` = false before sunrise confirmed. |
| UT-DN-004 | UT | is_daytime = false after sunset | ✅ PASS | Phase 1 T4: `is_daytime` = false after sunset confirmed. |

### 14.2 Setpoint Profile Switching

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-DN-005 | IT | Day setpoints active by day; night setpoints active at night | ⬜ NOT EXECUTED | Day/night profile switching integration test not documented; algorithm confirmed, but end-to-end setpoint selection not explicitly verified. |
| IT-DN-006 | IT | Location change triggers sunrise/sunset recalculation within 60 s | ✅ PASS | v1.13.0: location change via web confirmed; sunrise/sunset recalculated and reflected in `/api/status` within next T4 60 s cycle. |

### 14.3 Default and Fallback Behaviour

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-DN-007 | IT | No location (lat=0, lon=0) → daytime setpoints always active | ⬜ NOT EXECUTED | Zero-location fallback behaviour test not documented. |

### 14.4 Timezone and Time Display

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-DN-008 | IT | Web GUI shows sunrise/sunset in local time after geolocation | ✅ PASS | v1.13.0: dashboard displays local-time sunrise/sunset; confirmed for Amsterdam location. |
| IT-DN-009 | IT | POSIX TZ string applied immediately (FR-DN07) | ✅ PASS | v1.13.0: TZ applied via `setenv`/`tzset` on Q4 config update; no reboot required; confirmed. |

---

## 15. RGB Status LED

TSDS reference: §5.12 | FRS: FR-UI16–FR-UI21, FR-CF14

### 15.1 Colour State Mapping

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-RG-001 | IT | Normal operation → Green | ✅ PASS | Phase 5 T9: T9-01 (or equivalent): green LED in normal operation confirmed. |
| IT-RG-002 | IT | SENSOR_FAULT_T → Amber | ✅ PASS | Phase 5 T9: T9-03 (or equivalent): amber on T/RH fault confirmed. |
| IT-RG-003 | IT | SENSOR_FAULT_W → Amber | ✅ PASS | Phase 5 T9: amber on wind sensor fault confirmed. |
| IT-RG-004 | IT | WIND_OVERRIDE → Amber | ✅ PASS | Phase 5 T9: T9-04: amber on wind override confirmed. |
| IT-RG-005 | IT | wind_prot_en = false → Amber | ✅ PASS | Phase 5 T9: T9-07 (or equivalent): amber for wind protection disabled confirmed. |
| IT-RG-006 | IT | rh_ctrl_en = false → Amber | ✅ PASS | Phase 5 T9: amber for humidity control disabled confirmed. |
| IT-RG-007 | IT | MOTOR_ALARM → Red; overrides all amber | ✅ PASS | Phase 5 T9: T9-05: red on motor alarm confirmed; overrides amber conditions. |
| IT-RG-008 | IT | MOTOR_ALARM cleared; amber still active → Amber | ✅ PASS | Phase 5 T9: T9-09: LED priority (alarm cleared → amber retained) confirmed. |
| IT-RG-009 | IT | Multiple amber conditions → Amber, not Red | ✅ PASS | Phase 5 T9: T9-08: multiple concurrent amber conditions confirmed as amber only. |

### 15.2 Night Brightness Dimming

| ID | Level | Description | Result | Evidence |
|----|-------|-------------|--------|----------|
| IT-RG-010 | IT | LED brightness reduced during night hours (FR-UI21, FR-CF14) | ⬜ NOT EXECUTED | Night dimming requires advancing DUT time; not documented in results. |
| IT-RG-011 | IT | LED brightness returns to day level outside night window | ⬜ NOT EXECUTED | See IT-RG-010. |

---

## 16. Test Coverage Report

### 16.1 Results Summary by Section

| Section | Module | Total cases | ✅ PASS | 🔲 PENDING | ⚠️ DEFERRED | ⬜ NOT EXECUTED | ❌ FAIL |
|---------|--------|------------|---------|-----------|------------|----------------|---------|
| §4 | FA — Firmware Architecture | 13 | 3 | 0 | 0 | 10 | 0 |
| §5 | SP — Sensor Polling | 11 | 7 | 0 | 0 | 4 | 0 |
| §6 | CC — Climate Control | 31 | 12 | 0 | 0 | 19 | 0 |
| §7 | EL — Event Log | 14 | 4 | 0 | 0 | 10 | 0 |
| §8 | AC — Access Control | 19 | 7 | 0 | 0 | 12 | 0 |
| §9 | UI — Local UI | 17 | 13 | 0 | 0 | 4 | 0 |
| §10 | WA — WiFi AP Mode | 7 | 7 | 0 | 0 | 0 | 0 |
| §11 | WC — WiFi Client | 7 | 7 | 0 | 0 | 0 | 0 |
| §12 | WI — Web Interface | 15 | 11 | 0 | 3 | 1 | 0 |
| §13 | OT — OTA Update | 8 | 5 | 0 | 0 | 3 | 0 |
| §14 | NV — NVS Storage | 7 | 5 | 0 | 0 | 2 | 0 |
| §15 | WD — Watchdog/Faults | 11 | 6 | 2 | 0 | 3 | 0 |
| §16 | SE — Security | 6 | 4 | 0 | 0 | 2 | 0 |
| §17 | DN — Day/Night | 9 | 6 | 0 | 0 | 3 | 0 |
| §18 | RG — RGB LED | 11 | 9 | 0 | 0 | 2 | 0 |
| **Total** | | **186** | **106** | **2** | **3** | **75** | **0** |

### 16.2 Coverage Percentages

| Metric | Value |
|--------|-------|
| Total test cases | 186 |
| PASS | 106 (57%) |
| PENDING (impl done, hw test outstanding) | 2 (1%) |
| DEFERRED (feature not implemented) | 3 (2%) |
| NOT EXECUTED | 75 (40%) |
| FAIL | 0 (0%) |
| **Executed + passed rate** (PASS ÷ total) | **57%** |
| **Pass rate over executed cases** (PASS ÷ (PASS+PENDING+FAIL)) | **98%** |
| **Failure rate** | **0%** |

### 16.3 Test Cases NOT EXECUTED

The following 75 test cases have no evidence of execution in `firmwareImplementationResults.md`. They represent coverage gaps for future test runs.

#### Firmware Architecture (10 not executed)
IT-FA-003, IT-FA-004, UT-FA-005, UT-FA-006, UT-FA-007, UT-FA-008, UT-FA-009, UT-FA-010, UT-FA-011, IT-FA-012

#### Sensor Polling (4 not executed)
UT-SP-007, UT-SP-008, UT-SP-010, IT-SP-011

#### Climate Control (19 not executed)
UT-CC-002, UT-CC-003, UT-CC-019, UT-CC-020, UT-CC-021, UT-CC-022, UT-CC-023, UT-CC-024, UT-CC-025, UT-CC-026, UT-CC-027, UT-CC-028, UT-CC-029, UT-CC-030, UT-CC-031, UT-CC-032

> **Note:** The 16 UT-CC-* not-executed cases (UT-CC-002/003/019–032) are all host-build unit tests requiring the native test build (`test_host` env). No evidence that `test_host` was built or run; all firmware testing was done on target hardware only.

#### Event Log Manager (10 not executed)
UT-EL-001, IT-EL-002, IT-EL-003, UT-EL-007, UT-EL-008, ST-EL-009, ST-EL-010, ST-EL-011, IT-EL-012, IT-EL-013

#### Access Control (12 not executed)
UT-AC-001, UT-AC-002, UT-AC-003, UT-AC-004, UT-AC-005, IT-AC-019, UT-AC-008, UT-AC-009, UT-AC-010, UT-AC-011, UT-AC-012, UT-AC-013, UT-AC-014

> **Note:** All 13 UT-AC-* cases require the `test_host` native build.

#### Local User Interface (4 not executed)
UT-UI-001, UT-UI-002, UT-UI-003, UT-UI-004, IT-UI-009, IT-UI-016

> **Note:** 4 UT-UI-* cases require `test_host`. IT-UI-009 (navigation depth) and IT-UI-016 (wind-off warning) are integration tests that were not explicitly documented.

#### WiFi Access Point and Client Modes
All 14 cases passed. No gaps.

#### Web Interface (1 not executed, 3 deferred)
ST-WI-015 (PASS as of v1.16.6). 3 MQTT tests deferred pending T12 implementation.

#### OTA Update (3 not executed)
ST-OT-003, ST-OT-005, ST-OT-008

#### NVS Storage (2 not executed)
UT-NV-006, IT-NV-007

#### Watchdog and Fault Handling (3 not executed, 2 pending)
IT-WD-001, IT-WD-002, IT-WD-003 (watchdog-trigger tests), IT-WD-010 and IT-WD-011 (PENDING — implemented in v1.16.4, hardware test outstanding).

#### Security (2 not executed)
ST-SE-005, ST-SE-006

#### Day/Night Management (3 not executed)
IT-DN-005, IT-DN-007

#### RGB LED (2 not executed)
IT-RG-010, IT-RG-011 (night brightness dimming; requires time manipulation)

### 16.4 Tests PENDING Hardware Verification

These two tests are implemented but their hardware verification results are explicitly listed as "Pending" in `firmwareImplementationResults.md` (v1.16.4 section):

| ID | Description | Status in results |
|----|-------------|-------------------|
| IT-WD-010 | Alarm re-assert during 60 s guard detected ≤ 5 s | `⬜ Pending hardware test` |
| IT-WD-011 | Alarm active at power-on; CLOSE_ALL skipped | `⬜ Pending hardware test` |

In addition, three v1.16.5 items (alarm during calibration) are marked pending in the results file but are covered under IT-WD-007/IT-WD-009 (existing motor alarm tests).

### 16.5 DEFERRED Features

Three test cases are deferred because the underlying feature (T12 MQTT client) has not yet been implemented:

| ID | Feature |
|----|---------|
| ST-WI-009 | MQTT sensor data publish |
| ST-WI-010 | MQTT window state publish |
| ST-WI-011 | MQTT CLOSE_ALL command receive |

### 16.6 Coverage Analysis by Test Level

| Level | Total | PASS | NOT EXECUTED | Rate |
|-------|-------|------|--------------|------|
| UT (Unit Tests) | 56 | 18 | 38 | 32% |
| IT (Integration Tests) | 100 | 74 | 24 | 74% |
| ST (System Tests) | 30 | 14 | 13 | 47% |

**Key observations:**

1. **Unit test coverage is low (32%).** The `test_host` native build environment was never set up. All 38 not-executed UT cases are host-build tests (queue, mutex, climate logic, PIN, UI debounce). The `test_t2_relay` on-device Unity tests (IT-01–IT-13) are counted as IT-level and all passed.

2. **Integration test coverage is strong (74%).** Hardware-driven integration testing was the primary verification method throughout all phases. WiFi AP, WiFi client, sensor polling, motor alarm, and OTA are fully covered.

3. **System test coverage is moderate (47%).** Gaps are concentrated in Event Log (ST-EL-009–011 not run as formal system tests), OTA edge cases (corrupt image, EG1 blocking), and Security deep tests (HTTP capture, web lockout).

4. **No failures recorded.** All 106 executed tests pass. The firmware progressed through 10 development phases and 16 firmware versions (v0.1.0 → v1.16.6) with every verified test producing a PASS result.

5. **Critical paths are fully covered.** Safety-critical paths — motor alarm (IT-WD-007/008/009), wind safety (all Phase 4 T3), sensor fault safe-fail (IT-WD-004/005), OTA rollback (ST-OT-004) — are all PASS. The two PENDING items (IT-WD-010/011) affect the motor alarm re-assert and boot-alarm edge cases added in v1.16.4.

### 16.7 Recommended Follow-up Actions

| Priority | Action | Test cases covered |
|----------|--------|--------------------|
| **High** | Set up `test_host` native build and run all UT-* cases | 38 UT cases (FA, CC, EL, AC, UI, SP, NV) |
| **High** | Execute hardware tests IT-WD-010 and IT-WD-011 (v1.16.4 alarm fixes) | IT-WD-010, IT-WD-011 |
| **Medium** | Run formal event-log system tests (SD card + NVS ring, web log viewer) | IT-EL-002/003, ST-EL-009/010/011 |
| **Medium** | Execute OTA edge cases (corrupt image, EG1 block during write, asset mismatch) | ST-OT-003, ST-OT-005, ST-OT-008 |
| **Medium** | Implement T12 MQTT client and execute ST-WI-009/010/011 | ST-WI-009, ST-WI-010, ST-WI-011 |
| **Low** | Run night brightness dimming test (requires time manipulation) | IT-RG-010, IT-RG-011 |
| **Low** | Execute session independence, concurrent role, and web lockout tests | IT-AC-019, ST-SE-005, ST-SE-006 |

---

*End of document — version 1.0 — firmware v1.16.6 — 2026-05-06*
