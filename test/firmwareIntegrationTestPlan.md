# Firmware Integration Test Plan

**Project:** Greenhouse Ventilation Controller  
**Document:** Firmware Integration Test Plan  
**Version:** 1.0  
**Date:** 2026-05-06  
**Firmware target:** v1.16.6+

---

## 1. Purpose

This document describes the automated integration test suite for the greenhouse controller firmware.  
Tests run against live hardware over two external interfaces:

| Interface | Address | Purpose |
|---|---|---|
| Device REST API | `http://192.168.20.150` | Configuration, status assertion |
| Sensor emulator REST API | `http://192.168.20.226` | Inject T, RH, wind speed/direction |
| Serial port | `COM8`, 115 200 baud | Log-line assertions (state transitions, commands) |

---

## 2. Test environment

| Component | Value |
|---|---|
| Device IP | `192.168.20.150` |
| Sensor emulator IP | `192.168.20.226` |
| Serial port | `COM8`, 115 200 baud |
| Python version | 3.10+ |
| Test runner | `pytest` |

### Python dependencies

```
pytest
pytest-timeout
pyserial
requests
```

Install: `pip install pytest pytest-timeout pyserial requests`

---

## 3. File layout

```
test/
  firmwareIntegrationTestPlan.md   ← this document
  conftest.py                       ← pytest fixtures
  lib/
    __init__.py
    serial_monitor.py               ← background serial reader + pattern matcher
    device_api.py                   ← REST client for device (192.168.20.150)
    emulator_api.py                 ← REST client for sensor emulator (192.168.20.226)
  test_01_boot.py                   ← TC-01: boot sequence
  test_02_climate_temp.py           ← TC-02 to TC-04: temperature control
  test_03_climate_rh.py             ← TC-05 to TC-06: RH control
  test_04_wind_override.py          ← TC-07 to TC-10: wind override
  test_05_sensor_fault.py           ← TC-11 to TC-13: sensor faults
  test_06_config_api.py             ← TC-14 to TC-15: config REST API
  test_07_session.py                ← TC-16 to TC-17: session management
  test_08_motor_alarm.md            ← TC-20: manual procedure (GPIO42)
  test_09_ota.md                    ← TC-18 to TC-19: manual OTA procedure
```

---

## 4. Test infrastructure

### 4.1 `lib/serial_monitor.py`

Background thread reads serial lines into a deque.  
`wait_for(pattern, timeout, after)` polls for a matching regex line.

### 4.2 `lib/emulator_api.py`

Pushes sensor readings to `POST /api/data` on the emulator.  
`set_rest_mode()` switches both sensors (fg6485a, s200) to REST mode so emulator data is consumed by the firmware instead of live Modbus.

### 4.3 `lib/device_api.py`

Thin wrapper around `requests.Session`.  
`login(pin, role)`, `set_config(ns, key, value)`, `get_status()`, `get_config()`.

### 4.4 `conftest.py` fixtures

| Fixture | Scope | Purpose |
|---|---|---|
| `serial_mon` | session | Starts `SerialMonitor` on COM8 |
| `device` | session | Logs in as admin; provides `DeviceApi` instance |
| `emulator` | session | Calls `set_rest_mode()`; provides `EmulatorApi` |
| `fast_config` | function | Sets reduced poll_interval + travel times; restores on teardown |
| `neutral_sensors` | function | Pushes safe T/RH/Wind values at start and teardown |

**Timing note:** All sensor-driven tests wait `POLL_INTERVAL + 5` seconds (35 s with fast_config) for the firmware to react. `wait_for()` uses a 40 s timeout.

---

## 5. Test cases

### TC-01 — Boot sequence
**File:** `test_01_boot.py` | **Automated:** Yes

| Step | Action | Expected |
|---|---|---|
| 1 | Power-cycle or hardware reset device | Serial: `=== Greenhouse Controller v` |
| 2 | Wait for scheduler | Serial: `All tasks spawned` |
| 3 | Wait for CLOSE_ALL calibration | Serial: `CLOSE_ALL calibration complete` |
| 4 | `GET /api/whoami` (no auth) | HTTP 200 |
| 5 | `GET /api/status` | `"mode"` present; value not `null` |

**Assert:** All 5 checks pass within 240 s of reset trigger.

---

### TC-02 — Temperature: windows open above t_max
**File:** `test_02_climate_temp.py` | **Automated:** Yes

**Setup:** `t_max_day=25`, `hyst_t=2`, `avg_win_t=1`, `avg_win_rh=1`, `rh_ctrl_en=0`

| Step | Action | Expected |
|---|---|---|
| 1 | Push `T=28.0` (3 °C above t_max) | — |
| 2 | Wait 35 s | Serial: `CMD_OPEN` or `MOVING_OPEN` |
| 3 | `GET /api/status` | At least one window not `closed` |

---

### TC-03 — Temperature: windows close below t_max − hyst_t
**File:** `test_02_climate_temp.py` | **Automated:** Yes | **Depends on:** TC-02 (windows open)

| Step | Action | Expected |
|---|---|---|
| 1 | Push `T=22.0` (below 25−2=23 °C) | — |
| 2 | Wait 35 s | Serial: `CMD_CLOSE` or `CMD_CLOSE_ALL` |
| 3 | `GET /api/status` | All windows `closed` or `moving_close` |

---

### TC-04 — Temperature: hysteresis prevents premature close
**File:** `test_02_climate_temp.py` | **Automated:** Yes

**Precondition:** Windows open. Push `T=24.0` (above t_max−hyst_t=23, below t_max=25).

**Assert:** No `CMD_CLOSE` appears on serial for 40 s — windows remain open.

---

### TC-05 — RH: windows open above rh_max
**File:** `test_03_climate_rh.py` | **Automated:** Yes

**Setup:** `rh_max_day=70`, `rh_min_day=40`, `hyst_rh=5`, `rh_ctrl_en=1`, `t_max_day=40`

| Step | Action | Expected |
|---|---|---|
| 1 | Push `RH=80.0` | — |
| 2 | Wait 35 s | Serial: `CMD_OPEN` |
| 3 | `GET /api/status` | At least one window open |

---

### TC-06 — RH: windows close below rh_min (over-dry)
**File:** `test_03_climate_rh.py` | **Automated:** Yes | **Depends on:** TC-05 (windows open)

| Step | Action | Expected |
|---|---|---|
| 1 | Push `RH=35.0` | — |
| 2 | Wait 35 s | Serial: `CMD_CLOSE_ALL` |
| 3 | `GET /api/status` | All windows closed |

---

### TC-07 — Wind override: speed threshold
**File:** `test_04_wind_override.py` | **Automated:** Yes

**Setup:** `v_max=5`, `wind_prot_en=1`, `t_max_day=40`

| Step | Action | Expected |
|---|---|---|
| 1 | Push `T=45.0` to open windows; wait 35 s | Serial: `CMD_OPEN` |
| 2 | Push `Speed=8.0` | — |
| 3 | Wait 35 s | Serial: `WIND_OVERRIDE set` |
| 4 | `GET /api/status` | `mode` = wind_override; all windows closed |

---

### TC-08 — Wind override: clears when speed drops
**File:** `test_04_wind_override.py` | **Automated:** Yes | **Depends on:** TC-07

| Step | Action | Expected |
|---|---|---|
| 1 | Push `Speed=2.0` | — |
| 2 | Wait 35 s | Serial: `WIND_OVERRIDE cleared` |
| 3 | `GET /api/status` | `mode` = automatic |

---

### TC-09 — Wind override: direction exclusion zone
**File:** `test_04_wind_override.py` | **Automated:** Yes

**Setup:** `dir_excl_low=270`, `dir_excl_high=350`, `v_max=20` (speed never triggers alone), `wind_prot_en=1`

| Step | Action | Expected |
|---|---|---|
| 1 | Push `Direction=310.0`, `Speed=3.0` | — |
| 2 | Wait 35 s | Serial: `WIND_OVERRIDE set` |
| 3 | Push `Direction=180.0` | — |
| 4 | Wait 35 s | Serial: `WIND_OVERRIDE cleared` |

---

### TC-10 — Wind override: disabled by config
**File:** `test_04_wind_override.py` | **Automated:** Yes

**Setup:** `wind_prot_en=0`

| Step | Action | Expected |
|---|---|---|
| 1 | Push `Speed=20.0` | — |
| 2 | Wait 35 s | No `WIND_OVERRIDE set` on serial |
| 3 | `GET /api/status` | `mode` = automatic |

---

### TC-11 — Sensor fault: T/RH sensor
**File:** `test_05_sensor_fault.py` | **Automated:** Yes

| Step | Action | Expected |
|---|---|---|
| 1 | Set fg6485a to Live mode (stops responding) | — |
| 2 | Wait 70 s (2 × poll_interval) | Serial: `T/RH sensor FAULT` |
| 3 | `GET /api/status` | `sensor_fault_t: true` |
| 4 | Restore REST mode; push T=20.0, RH=60.0 | — |
| 5 | Wait 35 s | Serial: `T/RH sensor fault cleared` |
| 6 | `GET /api/status` | `sensor_fault_t: false` |

---

### TC-12 — Sensor fault: wind sensor
**File:** `test_05_sensor_fault.py` | **Automated:** Yes (mirrors TC-11 for S200 / `sensor_fault_w`)

---

### TC-13 — Sensor fault: wind fault triggers safe-fail wind override
**File:** `test_05_sensor_fault.py` | **Automated:** Yes

**Setup:** `wind_prot_en=1`. Set S200 to Live mode.

| Step | Action | Expected |
|---|---|---|
| 1 | Wait 70 s | Serial: `WIND_OVERRIDE set` (safe-fail) |
| 2 | Restore S200 to REST mode; push Speed=1.0 | — |
| 3 | Wait 35 s | Serial: `WIND_OVERRIDE cleared` |

---

### TC-14 — Config REST API: write and read back
**File:** `test_06_config_api.py` | **Automated:** Yes

| Step | Action | Expected |
|---|---|---|
| 1 | Login as admin | 200 OK |
| 2 | `POST /api/config {ns:"climate", key:"t_max_day", value:28}` | 200 OK |
| 3 | Wait 5 s for serial | `Q4 applied: climate/t_max_day = 28` |
| 4 | `GET /api/config` | `t_max_day == 28` |
| 5 | Restore original | — |

Repeated for: `rh_max_day`, `v_max`, `hyst_t`, `poll_interval`.

---

### TC-15 — Config REST API: farmer role restrictions
**File:** `test_06_config_api.py` | **Automated:** Yes

| Step | Action | Expected |
|---|---|---|
| 1 | Login as farmer | 200 OK |
| 2 | `POST /api/config {ns:"climate", key:"t_max_day", value:30}` | 200 OK |
| 3 | `POST /api/config {ns:"motor", key:"travel_m1", value:10}` | 403 Forbidden |
| 4 | `POST /api/wifi {ssid:"test"}` | 403 Forbidden |

---

### TC-16 — Session: activity resets expiry timer
**File:** `test_07_session.py` | **Automated:** Yes

**Setup:** `session_timeout_min=1`

| Step | Action | Expected |
|---|---|---|
| 1 | Login as admin | 200 OK |
| 2 | Wait 45 s; call `GET /api/whoami` | 200 OK (session extended) |
| 3 | Wait 45 s; call `GET /api/whoami` | 200 OK (session extended again) |
| 4 | Wait 75 s with NO calls | `GET /api/whoami` → 401 |

---

### TC-17 — Session: logout
**File:** `test_07_session.py` | **Automated:** Yes

| Step | Action | Expected |
|---|---|---|
| 1 | Login | 200 OK |
| 2 | `POST /api/logout` | 200 OK |
| 3 | `GET /api/whoami` | 401 |

---

### TC-18 — OTA: firmware + assets via web GUI (manual)
**File:** `test_09_ota.md` | **Automated:** No

Procedure: build release → upload `.bin` → upload `.zip` → verify version after reboot → verify `accepted:true` after 35 s.

---

### TC-19 — OTA: 3-fail rollback (manual)
**File:** `test_09_ota.md` | **Automated:** No

Procedure: upload crash binary → observe 3 reboots → verify rollback to previous bank/version.

---

### TC-20 — Motor alarm: emergency stop (manual, GPIO42)
**File:** `test_08_motor_alarm.md` | **Automated:** No

Procedure: short GPIO42 to 3.3 V → observe MOTOR_ALARM → release → observe guard + CLOSE_ALL + AUTOMATIC resume.

---

## 6. Timing reference

| Parameter | Default | fast_config value | Rationale |
|---|---|---|---|
| `poll_interval` | 60 s | 30 s | Minimum; halves wait time |
| `travel_m1/m2/m3` | 21/21/171 s | 5 s | Minimum travel |
| `avg_win_t` | 1 | 1 | Immediate response |
| `avg_win_rh` | 1 | 1 | Immediate response |
| Test wait after push | — | 35 s | poll_interval + 5 s buffer |

---

## 7. Serial patterns reference

| Event | Regex |
|---|---|
| Boot complete | `All tasks spawned` |
| CLOSE_ALL done | `CLOSE_ALL calibration complete` |
| Window opening | `MOVING_OPEN\|CMD_OPEN` |
| Window closing | `MOVING_CLOSE\|CMD_CLOSE` |
| Wind override set | `WIND_OVERRIDE set` |
| Wind override cleared | `WIND_OVERRIDE cleared` |
| T/RH fault | `T/RH sensor FAULT` |
| T/RH fault cleared | `T/RH sensor fault cleared` |
| Wind fault | `Wind sensor FAULT` |
| Motor alarm | `MOTOR_ALARM asserted` |
| Motor alarm cleared | `MOTOR_ALARM cleared` |
| Config written | `Q4 applied:` |

---

## 8. Running the automated suite

```bash
# All automated tests (requires device on network + COM8 connected)
pytest test/ -v --timeout=120

# Single module
pytest test/test_02_climate_temp.py -v --timeout=120
```

Expected total run time: ~15–20 minutes for TC-01 through TC-17 (dominated by sensor poll cycles).
