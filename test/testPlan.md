# Firmware Integration Test Plan

## Output files

```
test/
  testPlan.md                  ← this document
  conftest.py                  ← pytest fixtures and helpers
  lib/
    __init__.py
    serial_monitor.py          ← background serial reader + pattern matcher
    device_api.py              ← REST client for device (192.168.20.150)
    emulator_api.py            ← REST client for sensor emulator (192.168.20.226)
  test_01_boot.py
  test_02_climate_temp.py
  test_03_climate_rh.py
  test_04_wind_override.py
  test_05_sensor_fault.py
  test_06_config_api.py
  test_07_session.py
  test_08_motor_alarm.md       ← manual procedure only (TC-18, TC-19, TC-20)
  test_09_graduated_vent.py    ← TC-21, TC-22, TC-23
  test_10_dwell_timers.py      ← TC-24
  test_11_conflict_resolution.py ← TC-25, TC-26
  test_12_standby.py           ← TC-27, TC-28
  test_13_history.py           ← TC-29
```

---

## Context

The greenhouse controller firmware has no automated integration test suite. Unit tests exist
for individual drivers (NVS, relay FSM, etc.) but no end-to-end tests verify the interaction
between sensor input, the climate/safety control logic, relay actuation, and the REST API.

Two external interfaces make automation feasible:

- **Serial port (COM8, 115 200 baud)** — all firmware tasks emit structured `ESP_LOG` lines
  that identify module, state transitions, and commanded actions.
- **Sensor emulator REST API (192.168.20.226)** — `POST /api/data` pushes T, RH, wind speed,
  and wind direction into the firmware via the same Modbus path as the real sensors;
  `POST /config/sensor` selects REST mode.

The device REST API at `192.168.20.150` is used for configuration (setpoints, poll interval,
travel time) and for asserting final state via `GET /api/status`.

Motor alarm testing (GPIO42 physical pin) is included as a **manual** procedure only.

---

## Test environment

| Component | Value |
|---|---|
| Device IP | `192.168.20.150` |
| Sensor emulator IP | `192.168.20.226` |
| Serial port | `COM8`, 115 200 baud |
| Device admin PIN | configured on device |
| Python version | 3.10+ |
| Test runner | `pytest` |

### Python dependencies

```
pytest
pytest-timeout
pyserial
requests
```

Install: `python -m pip install pytest pytest-timeout pyserial requests`

---

## Test infrastructure design

### `lib/serial_monitor.py`

Background thread reads serial lines into a deque. Test assertions call `wait_for(pattern,
timeout, after)` which polls the deque for a line matching the regex, considering only lines
that arrived after the `after` monotonic timestamp.

```python
class SerialMonitor:
    def __init__(self, port='COM8', baud=115200): ...
    def wait_for(self, pattern, timeout=15, after=None) -> str | None: ...
    def mark(self) -> float:  # returns time.monotonic()
```

### `lib/emulator_api.py`

```python
BASE = 'http://192.168.20.226'

def set_rest_mode(): ...          # both sensors → REST (emulator-controlled)
def set_live_mode(sensor): ...    # named sensor → Live (Modbus pass-through)
def push(T=None, RH=None, Speed=None, Direction=None): ...
```

### `lib/device_api.py`

```python
BASE = 'http://192.168.20.150'

def login(pin, role='admin'): ...
def logout(): ...
def whoami() -> Response: ...
def set_config(ns, key, value) -> Response: ...
def set_config_ok(ns, key, value): ...   # retries on HTTP 503 (NVS queue full)
def get_status() -> dict: ...
def get_config() -> dict: ...
def get_ota_status() -> dict: ...
```

### `conftest.py` fixtures and helpers

| Name | Type | Scope | Purpose |
|---|---|---|---|
| `serial_mon` | fixture | session | Starts `SerialMonitor` on COM8 |
| `device` | fixture | session | Logs in as admin; provides `DeviceApi` instance |
| `emulator` | fixture | session | Calls `set_rest_mode()`; provides `EmulatorApi` |
| `fast_config` | fixture | function | Writes fast parameters; confirms via `wait_for_config`; restores on teardown |
| `neutral_sensors` | fixture | function | Pushes safe sensor values before and after each test |
| `log_step(msg)` | helper | — | Timestamped output to terminal and `results.log` |
| `wait_for_config(device, expected, timeout)` | helper | — | Polls `GET /api/config` until all keys match; raises on timeout |
| `wait_for_automatic_mode(device, timeout)` | helper | — | Polls `GET /api/status` until `mode == "automatic"` |

### NVS write confirmation protocol

Firmware NVS writes are **asynchronous** — `POST /api/config` returns HTTP 200 immediately but
the value is committed to flash and applied to the running firmware state after approximately
400 ms per key (queue depth permitting). Every fixture and test body that writes a config
value calls `wait_for_config` to poll `GET /api/config` until the returned value matches,
before proceeding to push sensor data or assert firmware behaviour.

### Result log

Every test run writes `test/results.log` — one line per test (`PASSED`, `FAILED`, or
`ERROR`), with failed-test details indented beneath. The file is written incrementally so
it is readable even if the run is interrupted.

---

## Fixture teardown and state restoration

`fast_config` restores the following after every test that uses it:

| Key | Restored to |
|---|---|
| `system / poll_interval_s` | value read from `GET /api/config` before the test |
| `motor / travel_m1` | value read from `GET /api/config` before the test (default 21 s) |
| `motor / travel_m2` | value read from `GET /api/config` before the test (default 21 s) |
| `motor / travel_m3` | value read from `GET /api/config` before the test (default 171 s) |
| `climate / avg_win_t` | value read from `GET /api/config` before the test (default 3) |
| `climate / avg_win_rh` | value read from `GET /api/config` before the test (default 3) |

`neutral_sensors` pushes `T=20.0, RH=60.0, Speed=0.5, Direction=180.0` both before the test
(setup) and after (teardown).

Variables **not** restored by fixtures are left at whatever NVS value the test wrote last. The
next test's fixture writes and confirms its own required values via `wait_for_config`, so a
residual value from a previous test cannot affect the outcome of the next.

---

## TC-01 — Boot sequence

**File:** `test_01_boot.py`  **Automated:** Yes

**What is tested:**  
The complete startup sequence from reset to fully operational state. Verifies that all
FreeRTOS tasks start, the initial CLOSE_ALL calibration completes, and the device enters
AUTOMATIC control mode before accepting REST API requests.

**State before test body runs:**

Before displaying the operator prompt, `conftest.py` writes the following values to NVS so
that the CLOSE_ALL calibration after reset completes within `BOOT_TIMEOUT = 60 s`:

| Variable | Value written to NVS | Confirmed |
|---|---|---|
| `motor / travel_m1` | 5 s | Written via `set_config_ok`; no read-back (device is about to reset) |
| `motor / travel_m2` | 5 s | As above |
| `motor / travel_m3` | 5 s | As above |

After the operator presses ENTER, `conftest._reset_mark` is set to `time.monotonic()`.
All three serial `wait_for` calls in TC-01 pass `after=_reset_mark` so they look back
into the serial deque from this anchor point, preventing missed events if calibration
completes before an individual test method calls `wait_for`.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Monitor serial after reset | Line matching `=== Greenhouse Controller v` appears within 60 s of `_reset_mark` |
| 2 | Wait for task scheduler | Line matching `All tasks spawned` appears within 60 s of `_reset_mark` |
| 3 | `GET /api/status` within 5 s of step 2 | Response JSON contains `"mode": "WINDOW_CAL"` (CLOSE_ALL calibration in progress; `eg1` bit 6 = `0x40` is set) |
| 4 | Wait for calibration | Line matching `CLOSE_ALL calibration complete` appears within 60 s of `_reset_mark` |
| 5 | `GET /api/whoami` | HTTP 200 or 401 (server responding; not 500 or timeout) |
| 6 | `GET /api/status` | Response JSON contains `"mode": "AUTOMATIC"` |

**Overall PASS:** All six criteria are met.

---

## TC-02 — Temperature: windows open above t_max

**File:** `test_02_climate_temp.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `climate_setup`

**What is tested:**  
When the measured temperature exceeds `t_max_day`, the climate control task commands one or
more vents to open. Verifies the primary temperature-triggered ventilation response.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | Runtime already 30 — write is no-op; not polled |
| `motor / travel_m1` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m2` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `climate_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `climate_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 (disabled) | `climate_setup` | `wait_for_config` |
| `wind / wind_prot_en` | prior NVS value | not set | n/a — Speed=0.5 (emulator) keeps override inactive |
| Emulator T | 20.0 °C | `neutral_sensors` | `emulator.push()` |
| Emulator RH | 60.0 % | `neutral_sensors` | `emulator.push()` |
| Emulator Speed | 0.5 m/s | `neutral_sensors` | `emulator.push()` |
| Emulator Direction | 180.0 ° | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C` (3 °C above t_max) | Emulator accepts (HTTP 200) |
| 2 | Wait up to 45 s | Serial line matching `MOVING_OPEN` or `CMD_OPEN` appears |
| 3 | `GET /api/status` | At least one entry in `windows[]` is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** Serial confirms an open command AND the status endpoint confirms at least
one window is no longer `CLOSED`.

---

## TC-03 — Temperature: windows close below t_max − hyst_t

**File:** `test_02_climate_temp.py`  **Automated:** Yes

**What is tested:**  
When temperature drops below the close threshold (`t_max_day − hyst_t`), the climate
control task commands all open vents to close.

**State before test body runs:**

Same as TC-02 (same `climate_setup` fixture). In addition, the test body first replicates
the TC-02 open-window precondition by pushing `T = 28.0 °C` and sleeping `WAIT_AFTER_PUSH`
(35 s) to confirm windows are open.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m2` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `climate_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `climate_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 (disabled) | `climate_setup` | `wait_for_config` |
| `wind / wind_prot_en` | prior NVS value | not set | n/a — Speed=0.5 keeps override inactive |
| Emulator T | 20.0 °C → 28.0 °C (in-test) | `neutral_sensors` → test body | `emulator.push()` |
| Emulator RH | 60.0 % | `neutral_sensors` | `emulator.push()` |
| Emulator Speed | 0.5 m/s | `neutral_sensors` | `emulator.push()` |
| Emulator Direction | 180.0 ° | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |
| Windows state | open | in-test precondition push | 35 s sleep after T=28 push |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C`; wait 35 s | Windows open (serial / status) |
| 2 | Push `T = 22.0 °C` (1 °C below close threshold 23 °C) | Emulator accepts |
| 3 | Wait up to 45 s | Serial matching `CMD_CLOSE`, `CMD_CLOSE_ALL`, or `MOVING_CLOSE` |
| 4 | `GET /api/status` | No entry in `windows[]` is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** Serial confirms a close command AND no window remains open.

---

## TC-04 — Temperature: hysteresis prevents premature close

**File:** `test_02_climate_temp.py`  **Automated:** Yes

**What is tested:**  
The hysteresis band between `t_max_day − hyst_t` and `t_max_day` must suppress close
commands while temperature is still above the close threshold.

**State before test body runs:**

Same as TC-02. Test body also opens windows via T=28 + 35 s sleep first.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / t_max_day` | 25 °C | `climate_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `climate_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `climate_setup` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |
| Windows state | open | in-test precondition push | 35 s sleep after T=28 push |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C`; wait 35 s | Windows open |
| 2 | Push `T = 24.0 °C` (inside hysteresis band: above 23 °C, below 25 °C) | Emulator accepts |
| 3 | Monitor serial for 40 s | **No** line matching `CMD_CLOSE`, `CMD_CLOSE_ALL`, or `MOVING_CLOSE` |

**Overall PASS:** Zero close events are observed during the 40 s observation window.

---

## TC-05 — RH: windows open above rh_max

**File:** `test_03_climate_rh.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `rh_setup`

**What is tested:**  
When relative humidity exceeds `rh_max_day`, the climate control task opens vents for
dehumidification. Verifies the RH-triggered ventilation path.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m2` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / rh_max_day` | 70 % | `rh_setup` | `wait_for_config` |
| `climate / rh_min_day` | 40 % | `rh_setup` | `wait_for_config` |
| `climate / hyst_rh` | 5 % | `rh_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 1 (enabled) | `rh_setup` | `wait_for_config` |
| `climate / t_max_day` | 40 °C | `rh_setup` | `wait_for_config` |
| `wind / wind_prot_en` | prior NVS value | not set | n/a — Speed=0.5 keeps override inactive |
| Emulator T | 20.0 °C | `neutral_sensors` | `emulator.push()` |
| Emulator RH | 60.0 % | `neutral_sensors` | `emulator.push()` |
| Emulator Speed | 0.5 m/s | `neutral_sensors` | `emulator.push()` |
| Emulator Direction | 180.0 ° | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `RH = 80.0 %` (10% above rh_max) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `CMD_OPEN` or `MOVING_OPEN` |
| 3 | `GET /api/status` | At least one entry in `windows[]` is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** Serial confirms an open command AND at least one window is confirmed open.

---

## TC-06 — RH: windows close below rh_min (over-dry)

**File:** `test_03_climate_rh.py`  **Automated:** Yes

**What is tested:**  
When humidity drops below `rh_min_day`, all vents close fully (CMD_CLOSE_ALL) to
retain moisture. Verifies the over-dry protection path.

**State before test body runs:**

Same as TC-05. Test body opens windows via RH=80 + 35 s sleep first.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / rh_max_day` | 70 % | `rh_setup` | `wait_for_config` |
| `climate / rh_min_day` | 40 % | `rh_setup` | `wait_for_config` |
| `climate / hyst_rh` | 5 % | `rh_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 1 | `rh_setup` | `wait_for_config` |
| `climate / t_max_day` | 40 °C | `rh_setup` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |
| Windows state | open | in-test precondition push | 35 s sleep after RH=80 push |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `RH = 80.0 %`; wait 35 s | Windows open |
| 2 | Push `RH = 35.0 %` (5% below rh_min) | Emulator accepts |
| 3 | Wait up to 45 s | Serial matching `CMD_CLOSE_ALL`, `CMD_CLOSE`, or `MOVING_CLOSE` |
| 4 | `GET /api/status` | No entry in `windows[]` is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** Serial confirms a close command AND no window remains open.

---

## TC-07 — Wind override: speed threshold

**File:** `test_04_wind_override.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `wind_setup`

**What is tested:**  
When wind speed exceeds `v_max` and wind protection is enabled, the firmware overrides
climate control, closes all windows, and enters `WIND_OVERRIDE` mode.

**State before test body runs:**

`wind_setup` writes climate variables. The test body then writes wind variables and
confirms them before pushing sensor data.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m2` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 40 °C | `wind_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 (disabled) | `wind_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 1 (enabled) | **test body** | `wait_for_config` |
| `wind / v_max` | 5 m/s | **test body** | `wait_for_config` |
| Emulator T | 20.0 °C | `neutral_sensors` | `emulator.push()` |
| Emulator RH | 60.0 % | `neutral_sensors` | `emulator.push()` |
| Emulator Speed | 0.5 m/s | `neutral_sensors` | `emulator.push()` |
| Emulator Direction | 180.0 ° | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 45.0 °C`; wait 35 s | Windows open |
| 2 | Push `Speed = 8.0 m/s` (3 m/s above v_max) | Emulator accepts |
| 3 | Wait up to 45 s | Serial matching `WIND_OVERRIDE set` |
| 4 | `GET /api/status` | `"mode"` is `"WIND_OVERRIDE"` AND no window is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** Mode transitions to WIND_OVERRIDE AND all windows are closed or closing.

---

## TC-08 — Wind override: clears when speed drops

**File:** `test_04_wind_override.py`  **Automated:** Yes

**What is tested:**  
When wind speed falls back below `v_max`, WIND_OVERRIDE clears and the device returns
to AUTOMATIC mode.

**State before test body runs:**

Same fixture state as TC-07. The test body independently sets and confirms wind variables
(does not depend on TC-07 having run first).

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / t_max_day` | 40 °C | `wind_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `wind_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 1 | **test body** | `wait_for_config` |
| `wind / v_max` | 5 m/s | **test body** | `wait_for_config` |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |
| WIND_OVERRIDE active | yes | in-test precondition push | 35 s sleep after Speed=8 push |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `Speed = 8.0 m/s`; wait 35 s | WIND_OVERRIDE active |
| 2 | Push `Speed = 2.0 m/s` (below v_max) | Emulator accepts |
| 3 | Wait up to 45 s | Serial matching `WIND_OVERRIDE cleared` |
| 4 | `GET /api/status` | `"mode"` is `"AUTOMATIC"` |

**Overall PASS:** Mode returns to AUTOMATIC after wind speed drops.

---

## TC-09 — Wind override: direction exclusion zone

**File:** `test_04_wind_override.py`  **Automated:** Yes

**What is tested:**  
A configured direction exclusion zone triggers WIND_OVERRIDE when wind falls within it,
even at low speeds. Leaving the zone clears the override.

**State before test body runs:**

`wind_setup` provides climate baseline. All four wind variables are written and confirmed
in the test body before the first sensor push.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / t_max_day` | 40 °C | `wind_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `wind_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 1 | **test body** | `wait_for_config` |
| `wind / v_max` | 20 m/s | **test body** | `wait_for_config` |
| `wind / dir_excl_low` | 270 ° | **test body** | `wait_for_config` |
| `wind / dir_excl_high` | 350 ° | **test body** | `wait_for_config` |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `Direction = 310°`, `Speed = 3.0 m/s` (inside NW zone 270–350°) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `WIND_OVERRIDE set` |
| 3 | Push `Direction = 180°` (outside zone) | Emulator accepts |
| 4 | Wait up to 45 s | Serial matching `WIND_OVERRIDE cleared` |

**Overall PASS:** Override activates on excluded direction AND clears when direction leaves
the zone.

---

## TC-10 — Wind override: disabled by config

**File:** `test_04_wind_override.py`  **Automated:** Yes

**What is tested:**  
When `wind_prot_en = 0`, no wind condition should trigger WIND_OVERRIDE. Verifies that
wind protection can be fully disabled.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / t_max_day` | 40 °C | `wind_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `wind_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 0 (disabled) | **test body** | `wait_for_config` |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `Speed = 20.0 m/s` | Emulator accepts |
| 2 | Monitor serial for 45 s | **No** line matching `WIND_OVERRIDE set` |
| 3 | `GET /api/status` | `"mode"` is `"AUTOMATIC"` |

**Overall PASS:** No override event is observed AND mode remains AUTOMATIC.

---

## TC-11 — Sensor fault: T/RH sensor

**File:** `test_05_sensor_fault.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `fault_setup`

**What is tested:**  
If the T/RH sensor stops responding to Modbus polls, the firmware must declare a fault,
set the fault flag in the event group, and inhibit the climate control task. When the
sensor recovers, the fault clears automatically.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m2` | 5 s | `fast_config` | `wait_for_config` |
| `motor / travel_m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| fg6485a sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| S200 sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| Emulator T | 20.0 °C | `neutral_sensors` | `emulator.push()` |
| Emulator RH | 60.0 % | `neutral_sensors` | `emulator.push()` |
| Emulator Speed | 0.5 m/s | `neutral_sensors` | `emulator.push()` |
| Emulator Direction | 180.0 ° | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Switch fg6485a to Live mode | Emulator confirms mode change |
| 2 | Wait ≤ 80 s (2 × poll_interval + 10 s) | Serial matching `T/RH sensor FAULT` |
| 3 | `GET /api/status` | Bit 2 (`sensor_fault_t`, mask `0x04`) is set in `eg1` field |
| 4 | Restore fg6485a to REST mode; push `T = 20.0, RH = 60.0` | Emulator accepts |
| 5 | Wait up to 45 s | Serial matching `T/RH sensor fault cleared` |
| 6 | `GET /api/status` | Bit 2 (`sensor_fault_t`) is clear in `eg1` field |

**Overall PASS:** Fault declared with correct bit, and clears automatically on sensor recovery.

---

## TC-12 — Sensor fault: wind sensor

**File:** `test_05_sensor_fault.py`  **Automated:** Yes

**What is tested:**  
Same fault detection and recovery mechanism as TC-11 but for the wind sensor (S200).
Verifies that `eg1` bit 3 (`sensor_fault_w`) is set and cleared independently.

**State before test body runs:**

Same as TC-11 (same `fault_setup`).

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| fg6485a sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| S200 sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| Emulator sensors | T=20, RH=60, Speed=0.5, Dir=180 | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Switch S200 to Live mode | Emulator confirms mode change |
| 2 | Wait ≤ 80 s | Serial matching `Wind sensor FAULT` |
| 3 | `GET /api/status` | Bit 3 (`sensor_fault_w`, mask `0x08`) is set in `eg1` |
| 4 | Restore S200 to REST mode; push `Speed = 0.5, Direction = 180.0` | Emulator accepts |
| 5 | Wait up to 45 s | Serial matching `Wind sensor fault cleared` |
| 6 | `GET /api/status` | Bit 3 (`sensor_fault_w`) is clear in `eg1` |

**Overall PASS:** Fault declared with correct bit, and clears on sensor recovery.

---

## TC-13 — Sensor fault: wind fault triggers safe-fail override

**File:** `test_05_sensor_fault.py`  **Automated:** Yes

**What is tested:**  
When `wind_prot_en = 1`, losing the wind sensor must activate WIND_OVERRIDE as a safe-fail
measure. Restoring the sensor clears the override.

**State before test body runs:**

Same fixture state as TC-11/TC-12. The test body writes and confirms `wind_prot_en = 1`
before switching the sensor to Live mode.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| fg6485a sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| S200 sensor mode | REST | `fault_setup` | `emulator.set_rest_mode()` |
| Emulator sensors | T=20, RH=60, Speed=0.5, Dir=180 | `neutral_sensors` | `emulator.push()` |
| Device mode | `automatic` | `wait_for_automatic_mode` | polls `GET /api/status` |
| `wind / wind_prot_en` | 1 (enabled) | **test body** | `wait_for_config` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Switch S200 to Live mode | Emulator confirms |
| 2 | Wait ≤ 80 s | Serial matching `WIND_OVERRIDE set` |
| 3 | Restore S200 to REST mode; push `Speed = 1.0, Direction = 180.0` | Emulator accepts |
| 4 | Wait up to 45 s | Serial matching `WIND_OVERRIDE cleared` |

**Overall PASS:** Safe-fail override activates on wind sensor loss AND clears on recovery.

---

## TC-14 — Config REST API: write and read back

**File:** `test_06_config_api.py`  **Automated:** Yes  
**Fixtures:** `save_and_restore` (snapshots and restores touched keys)

**What is tested:**  
Values written via `POST /api/config` are persisted to NVS and returned correctly by
`GET /api/config`. Verifies the full round-trip for five representative keys.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| Session | admin logged in | `device` fixture (session-scoped) | login HTTP 200 |
| Original values | read from `GET /api/config` | `save_and_restore` fixture | `device.get_config()` |

No `fast_config` is used — this test exercises raw config writes, not sensor behaviour.

**Keys tested and write values:**

| Key | Namespace | Test value | xfail? |
|---|---|---|---|
| `t_max_day` | `climate` | 28 | No |
| `rh_max_day` | `climate` | 75 | No |
| `v_max` | `wind` | 6 | No |
| `hyst_t` | `climate` | 3 | No |
| `poll_interval_s` | `system` | 60 | **Yes** — runtime FreeRTOS timer cannot be updated; GET returns running value (30) |

**Steps and pass criteria (per key):**

| Step | Action | PASS if |
|---|---|---|
| 1 | `POST /api/config {ns, key, value}` | HTTP 200 |
| 2 | Poll `GET /api/config` up to 8 s (400 ms per retry) | Returned value equals written value |
| 3 | `save_and_restore` teardown restores original value | HTTP 200 |

**Overall PASS:** For every non-xfail key, the value written is returned by read-back
within 8 s.

---

## TC-15 — Config REST API: farmer role restrictions

**File:** `test_06_config_api.py`  **Automated:** Yes

**What is tested:**  
The farmer role may write permitted climate keys but must be blocked from motor
parameters, wifi settings, and system settings.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| Session | farmer logged in | `farmer_api` fixture | login HTTP 200 |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Login as farmer | HTTP 200 with `ok: true` |
| 2 | `POST /api/config {ns:climate, key:t_max_day, value:30}` | HTTP 200 |
| 3 | `POST /api/config {ns:motor, key:travel_m1, value:10}` | HTTP 403 |
| 4 | `POST /api/wifi {ssid:test}` | HTTP 403 |

**Overall PASS:** Permitted key returns 200; both blocked operations return 403.

---

## TC-16 — Session: activity resets expiry timer

**File:** `test_07_session.py`  **Automated:** Yes

**What is tested:**  
Sessions use a sliding expiry window. Every authenticated API call extends the session
lifetime from the moment of the call. A session must expire if the user is genuinely
inactive for longer than the timeout.

**State before test body runs:**

The test body writes `session_timeout_min = 1` via the shared admin session and polls
`GET /api/config` until the value is confirmed live (≤ 10 s) before creating a new session.
This ensures the new session is created with `timeout_s = 60 s`.

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / session_timeout_min` | 1 min (60 s) | **test body** | poll `GET /api/config` until value = 1 |
| Session (dedicated) | fresh login with `timeout_s = 60 s` | **test body** | `api.login()` after config confirmed |

**Timeline and pass criteria:**

| Time | Action | PASS if |
|---|---|---|
| t = 0 s | Login | HTTP 200 |
| t = 45 s | `GET /api/whoami` (slides expiry to t = 105 s) | HTTP 200 |
| t = 90 s | `GET /api/whoami` (slides expiry to t = 150 s) | HTTP 200 |
| t = 165 s | `GET /api/whoami` (75 s after last call; past 60 s window) | HTTP 401 |

**Overall PASS:** Both keep-alive calls succeed with 200, AND the final call returns 401.

Teardown restores `session_timeout_min = 30` via the shared admin session.

---

## TC-17 — Session: logout

**File:** `test_07_session.py`  **Automated:** Yes

**What is tested:**  
`POST /api/logout` must immediately invalidate the session token server-side.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| Session | fresh admin login | **test body** | `api.login()` HTTP 200 |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Login | HTTP 200 with `ok: true` |
| 2 | `GET /api/whoami` | HTTP 200 |
| 3 | `POST /api/logout` | HTTP 200 |
| 4 | `GET /api/whoami` (same cookie) | HTTP 401 |

**Overall PASS:** The same cookie that was valid before logout returns 401 immediately after.

---

## TC-18 — OTA: firmware + assets via web GUI

**File:** `test_08_motor_alarm.md`  **Automated:** No (manual)

**What is tested:**  
The complete OTA update flow: firmware binary upload, asset ZIP upload, reboot, and the
two-phase commit that auto-accepts the new firmware after a healthy 35 s boot.

**Preconditions:** New `.bin` and `.zip` built with `.\bin\build_release.ps1`.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Upload `.bin` via web UI OTA section | Progress bar completes; status shows `fw_done` |
| 2 | Upload `.zip` via web UI OTA section | Status cycles `assets_buffering → assets_writing → rebooting` |
| 3 | Reload page after reboot | Footer shows the new firmware version string |
| 4 | `GET /api/ota/status` immediately after reboot | `state = "idle"`, `bank` flipped, `accepted = false` |
| 5 | Wait 35 s; `GET /api/ota/status` | `accepted = true`; serial shows `fail counter reset to 0` |

**Overall PASS:** All five steps succeed and the new firmware is committed.

---

## TC-19 — OTA: 3-fail rollback

**File:** `test_08_motor_alarm.md`  **Automated:** No (manual)

**What is tested:**  
If the new firmware crashes before the health-check timer fires, the bootloader rolls back
to the previous partition after three consecutive failures.

**Preconditions:** A deliberately crashing firmware binary (add `abort()` in `setup()`).

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Upload crashing binary via OTA | Upload accepted (HTTP 200) |
| 2 | Observe serial during boot loop | Crash banner × 3; rollback log line on 3rd reboot |
| 3 | Device recovers | Footer shows the previous firmware version |
| 4 | `GET /api/status` | HTTP 200; device in AUTOMATIC mode |

**Overall PASS:** Device recovers to previous firmware without manual intervention.

---

## TC-20 — Motor alarm: emergency stop

**File:** `test_08_motor_alarm.md`  **Automated:** No (manual — requires GPIO42 trigger)

**What is tested:**  
An RRK-3 alarm signal on GPIO42 must de-energise all relay outputs, enter MOTOR_ALARM
mode, and stay safe until the signal clears. After clearing, the device re-calibrates and
returns to AUTOMATIC.

**State before test:** Device in AUTOMATIC mode with windows open (use TC-02 to open).

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Confirm windows open | `GET /api/status` shows at least one window not `CLOSED` |
| 2 | Short GPIO42 to 3.3 V for ≥ 100 ms | Serial: `MOTOR_ALARM asserted` immediately |
| 3 | `GET /api/status` | `"mode"` is `"MOTOR_ALARM"` |
| 4 | Release GPIO42 | Serial: `MOTOR_ALARM cleared`, then `CLOSE_ALL calibration complete`, then `resuming AUTOMATIC` |
| 5 | `GET /api/status` | `"mode"` is `"AUTOMATIC"` AND all `windows[]` are `"CLOSED"` |

**Overall PASS:** Alarm detected on GPIO, all windows close safely, device returns to
automatic operation after alarm clears.

---

## TC-21 — Graduated ventilation: step 1 (M1 only)

**File:** `test_09_graduated_vent.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `graduated_setup`

**What is tested:**  
When temperature exceeds `t_max_day` by exactly one step-width, only M1 opens. M2 and M3
remain closed. Verifies step 1 of the three-step graduated ventilation algorithm
(FR-C09, TSDS §5.2).

**Algorithm:** `step_width = max(hyst_t / 3, 1)`. With `hyst_t = 3` → `step_width = 1`.
Push `T = t_max + 1 = 26 °C` → `deviation = 1` → `required_step = ceil(1/1) = 1` → open M1 only.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `graduated_setup` | `wait_for_config` |
| `climate / hyst_t` | 3 °C | `graduated_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 (disabled) | `graduated_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 0 (disabled) | `graduated_setup` | `wait_for_config` |
| `motor / dwell_open_min` (all channels) | 0 min | `graduated_setup` | `wait_for_config` |
| Emulator sensors | T=20, RH=60, Speed=0, Dir=0 | `neutral_sensors` | `emulator.push()` |
| Device mode | `AUTOMATIC` | `wait_for_automatic_mode` | polls `GET /api/status` |
| All windows | `CLOSED` | neutral push cool-down | `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 26.0 °C` (1 °C above t_max) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `MOVING_OPEN\|CMD_OPEN` |
| 3 | `GET /api/status` | `windows[0]` is `"OPEN"` or `"MOVING_OPEN"` |
| 4 | `GET /api/status` | `windows[1]` is `"CLOSED"` |
| 5 | `GET /api/status` | `windows[2]` is `"CLOSED"` |

**Overall PASS:** M1 opens AND M2 and M3 remain closed (step 1 only).

---

## TC-22 — Graduated ventilation: step 2 (M1 + M2)

**File:** `test_09_graduated_vent.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `graduated_setup`

**What is tested:**  
When temperature deviation reaches two step-widths, M2 opens in addition to M1 already
open. M3 remains closed. Verifies step-up escalation to step 2 (FR-C09, TSDS §5.2).

**Algorithm:** Same setup as TC-21 → `T = 27 °C` → `deviation = 2` → `required_step = 2`
→ open M1 and M2.

**State before test body runs:** Same as TC-21 (same `graduated_setup`). All windows start closed.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 27.0 °C` (2 °C above t_max) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `MOVING_OPEN\|CMD_OPEN` |
| 3 | `GET /api/status` | `windows[0]` is `"OPEN"` or `"MOVING_OPEN"` |
| 4 | `GET /api/status` | `windows[1]` is `"OPEN"` or `"MOVING_OPEN"` |
| 5 | `GET /api/status` | `windows[2]` is `"CLOSED"` |

**Overall PASS:** M1 and M2 open AND M3 remains closed (step 2).

---

## TC-23 — Graduated ventilation: step 3 (M1 + M2 + M3)

**File:** `test_09_graduated_vent.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `graduated_setup`

**What is tested:**  
When temperature deviation reaches three or more step-widths, all three windows open.
Verifies maximum ventilation (step 3) under the graduated algorithm (FR-C09, TSDS §5.2).

**Algorithm:** Same setup → `T = 28 °C` → `deviation = 3` → `required_step = 3`
→ open M1, M2, and M3.

**State before test body runs:** Same as TC-21 (same `graduated_setup`). All windows start closed.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C` (3 °C above t_max) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `MOVING_OPEN\|CMD_OPEN` |
| 3 | `GET /api/status` | `windows[0]` is `"OPEN"` or `"MOVING_OPEN"` |
| 4 | `GET /api/status` | `windows[1]` is `"OPEN"` or `"MOVING_OPEN"` |
| 5 | `GET /api/status` | `windows[2]` is `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** All three windows open (step 3 — full ventilation).

---

## TC-24 — Dwell timer: blocks rapid reversal after OPEN

**File:** `test_10_dwell_timers.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `dwell_setup`

**What is tested:**  
Once a window reaches the OPEN position, `dwell_open_min` must prevent the climate
control task from issuing a CLOSE command until the dwell period has elapsed. After the
dwell expires the close command fires normally. Verifies FR-A09 / FR-A11.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `dwell_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `dwell_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `dwell_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 0 | `dwell_setup` | `wait_for_config` |
| `motor / dwell_open_min` (all channels) | 1 min | `dwell_setup` | `wait_for_config` |
| Emulator sensors | T=20, RH=60, Speed=0, Dir=0 | `neutral_sensors` | `emulator.push()` |
| Device mode | `AUTOMATIC` | `wait_for_automatic_mode` | polls `GET /api/status` |
| All windows | `CLOSED` | neutral push cool-down | `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C`; wait 35 s | At least one window `"OPEN"` in `GET /api/status` |
| 2 | Record `open_ts = time.monotonic()`. Push `T = 20.0 °C` (below close threshold 23 °C) | Emulator accepts |
| 3 | Monitor serial for 45 s from step 2 | **No** line matching `CMD_CLOSE`, `CMD_CLOSE_ALL`, or `MOVING_CLOSE` — dwell blocks close |
| 4 | Sleep until `open_ts + 75 s` (dwell has expired) | — |
| 5 | Push `T = 20.0 °C` again | Emulator accepts |
| 6 | Wait up to 45 s | Serial matching `CMD_CLOSE`, `CMD_CLOSE_ALL`, or `MOVING_CLOSE` |
| 7 | `GET /api/status` | No window `"OPEN"` or `"MOVING_OPEN"` |

**Overall PASS:** No close command during dwell (step 3) AND close fires after dwell expires (step 6).

**Timing note:** Total ≈ 35 + 45 + margin + 45 ≈ 160 s. Ensure `--timeout=300`.

---

## TC-25 — Conflict resolution: CR_TEMP_FIRST (T wins)

**File:** `test_11_conflict_resolution.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `conflict_setup`

**What is tested:**  
When temperature demands ventilation (T > t_max) and humidity simultaneously demands
closure (RH < rh_min), `cr_priority = 0` (T-first) must let the temperature demand
prevail. Verifies FR-CR01 / FR-CR02.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / avg_win_rh` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `conflict_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `conflict_setup` | `wait_for_config` |
| `climate / rh_min_day` | 40 % | `conflict_setup` | `wait_for_config` |
| `climate / hyst_rh` | 5 % | `conflict_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 1 (enabled) | `conflict_setup` | `wait_for_config` |
| `climate / cr_priority` | 0 (T-first) | `conflict_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 0 | `conflict_setup` | `wait_for_config` |
| `motor / dwell_open_min` (all channels) | 0 | `conflict_setup` | `wait_for_config` |
| Emulator sensors | T=20, RH=60, Speed=0, Dir=0 | `neutral_sensors` | `emulator.push()` |
| Device mode | `AUTOMATIC` | `wait_for_automatic_mode` | polls `GET /api/status` |
| All windows | `CLOSED` | neutral push cool-down | `GET /api/status` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C` (T demands open), `RH = 35.0 %` (RH demands close — below rh_min) | Emulator accepts |
| 2 | Wait up to 45 s | Serial matching `MOVING_OPEN\|CMD_OPEN` |
| 3 | `GET /api/status` | At least one window `"OPEN"` or `"MOVING_OPEN"` (temperature wins) |

**Overall PASS:** Windows open — temperature demand prevails over the RH close demand.

---

## TC-26 — Conflict resolution: CR_RH_FIRST (RH wins)

**File:** `test_11_conflict_resolution.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `conflict_setup`

**What is tested:**  
Same conflict scenario as TC-25 but with `cr_priority = 1` (RH-first). The RH close
demand (RH below rh_min) must prevail — windows must not open despite high temperature.
Verifies FR-CR01 / FR-CR03.

**State before test body runs:**

Same as TC-25 except:

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `climate / cr_priority` | 1 (RH-first) | **test body** | `wait_for_config` |

All other variables identical to the TC-25 state table.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 28.0 °C`, `RH = 35.0 %` (conflicting demands) | Emulator accepts |
| 2 | Monitor serial for 45 s | **No** line matching `CMD_OPEN` or `MOVING_OPEN` |
| 3 | `GET /api/status` | All `windows[]` entries are `"CLOSED"` (RH close demand wins) |

**Overall PASS:** No ventilation command issued; all windows remain closed.

---

## TC-27 — Standby mode: climate control suppressed

**File:** `test_12_standby.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `standby_setup`

**What is tested:**  
When the device is in Standby mode, T6 (climate control) must not issue any ventilation
commands regardless of temperature or humidity conditions. Verifies FR-M03.

> **API note:** `GET /api/status` `mode` field does not currently serialise Standby — it
> returns `"AUTOMATIC"` even when the device is in Standby (see Known Limitations). Test
> assertions therefore rely on serial patterns from T6 log output to confirm T6 is
> inhibited, rather than the REST mode field.

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `motor / travel_m1/m2/m3` | 5 s | `fast_config` | `wait_for_config` |
| `climate / avg_win_t` | 1 | `fast_config` | `wait_for_config` |
| `climate / t_max_day` | 25 °C | `standby_setup` | `wait_for_config` |
| `climate / hyst_t` | 2 °C | `standby_setup` | `wait_for_config` |
| `climate / rh_ctrl_en` | 0 | `standby_setup` | `wait_for_config` |
| `wind / wind_prot_en` | 0 | `standby_setup` | `wait_for_config` |
| Emulator sensors | T=20, RH=60, Speed=0, Dir=0 | `neutral_sensors` | `emulator.push()` |
| Device operating mode | Standby | `POST /api/mode {"mode":"standby"}` | HTTP 200 |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Enter Standby: `POST /api/mode {"mode":"standby"}` | HTTP 200 |
| 2 | Push `T = 28.0 °C` (3 °C above t_max) | Emulator accepts |
| 3 | Monitor serial for 45 s | **No** line matching `\[T6\].*CMD_OPEN\|\[T6\].*MOVING_OPEN` |
| 4 | `GET /api/status` | All `windows[]` entries are `"CLOSED"` |
| 5 | Exit Standby: `POST /api/mode {"mode":"automatic"}` | HTTP 200 |

**Overall PASS:** No climate-control open command is issued while in Standby mode.

**Teardown:** `standby_setup` restores AUTOMATIC mode; `neutral_sensors` restores emulator values.

---

## TC-28 — Standby mode: wind safety still active

**File:** `test_12_standby.py`  **Automated:** Yes  
**Fixtures:** `fast_config`, `neutral_sensors`, `standby_setup`

**What is tested:**  
Wind safety (WIND_OVERRIDE) must engage even while the device is in Standby mode.
Emergency wind closure must not be inhibited by the standby state. Verifies FR-M04.

**State before test body runs:**

Same as TC-27 except:

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `wind / wind_prot_en` | 1 (enabled) | `standby_setup` | `wait_for_config` |
| `wind / v_max` | 5 m/s | `standby_setup` | `wait_for_config` |

All other variables identical to the TC-27 state table.

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Enter Standby: `POST /api/mode {"mode":"standby"}` | HTTP 200 |
| 2 | Push `Speed = 8.0 m/s` (3 m/s above v_max) | Emulator accepts |
| 3 | Wait up to 45 s | Serial matching `WIND_OVERRIDE set` |
| 4 | `GET /api/status` | `"mode"` is `"WIND_OVERRIDE"` |
| 5 | Push `Speed = 1.0 m/s`; wait up to 45 s | Serial matching `WIND_OVERRIDE cleared` |

**Overall PASS:** WIND_OVERRIDE engages in Standby AND clears when wind drops.

---

## TC-29 — History: newest entry is first

**File:** `test_13_history.py`  **Automated:** Yes  
**Fixtures:** `fast_config`

**What is tested:**  
`GET /api/history` must return sensor records in newest-first order (descending timestamp).
Verifies the history sort order introduced in firmware v1.16.6
(dm_ring_read newest-at-head−1 fix).

**State before test body runs:**

| Variable | Required value | Set by | Confirmed by |
|---|---|---|---|
| `system / poll_interval_s` | 30 s (runtime) | `fast_config` | no-op |
| Emulator sensors | REST mode active | session `emulator` fixture | `emulator.set_rest_mode()` |

**Steps and pass criteria:**

| Step | Action | PASS if |
|---|---|---|
| 1 | Push `T = 20.0 °C`; wait 35 s | First reading captured by firmware |
| 2 | Push `T = 21.0 °C`; wait 35 s | Second reading captured |
| 3 | Push `T = 22.0 °C`; wait 35 s | Third reading captured |
| 4 | `GET /api/history?limit=5` | HTTP 200; `rows` array has ≥ 3 entries |
| 5 | Compare timestamps | `rows[0].ts > rows[N-1].ts` (newest entry is first) |
| 6 | Check most recent temperature | `rows[0].temperature` is within ±1 °C of 22 °C |

**Overall PASS:** Records are in descending timestamp order AND the most recent temperature
matches the last emulator push.

**Timing note:** Total wait ≈ 3 × 35 s = 105 s. Session-level `--timeout=300` covers this.

---

## Known limitations

| Limitation | Impact | Recommended action |
|---|---|---|
| `GET /api/status` does not expose `MODE_STANDBY` — returns `"AUTOMATIC"` when device is in Standby (web_server.cpp `mode_str` logic omits the Standby case) | TC-27 and TC-28 cannot assert Standby mode via REST; they rely on serial patterns instead | File firmware issue to add `"STANDBY"` to the mode serialisation in `web_server.cpp` lines 245–248 |
| `poll_interval_s` write via NVS does not update the running FreeRTOS timer (takes effect after reboot) | TC-14 marks `poll_interval_s` as `xfail`; effective interval during a test run is the pre-boot NVS value | Design limitation; noted in TC-14 state table |
| Graduated ventilation tests (TC-21–TC-23) require `dwell_open_min = 0` to allow immediate window reversals between sub-tests; if firmware enforces a minimum > 0 the step isolation may be imperfect | `graduated_setup` writes `dwell_open_min = 0` and verifies via `wait_for_config`; if rejected, increase inter-test sleep to the minimum dwell period | Verify the minimum accepted NVS value; adjust `graduated_setup` fixture accordingly |
| TC-27 and TC-28 assume a `POST /api/mode` endpoint exists for programmatic mode switching; if only LCD-triggered Standby is supported the tests cannot be automated | These TCs would need to be converted to manual procedures in `test_08_motor_alarm.md` | Confirm or add a REST endpoint for mode selection (Standby / Automatic toggle) |

---

## Timing reference

| Parameter | Default (NVS) | `fast_config` value | Rationale |
|---|---|---|---|
| `poll_interval_s` | 60 s | 30 s (write; runtime stays 30 s) | Halves sensor reaction time |
| `travel_m1` | 21 s | 5 s | Window traverses in 5 s |
| `travel_m2` | 21 s | 5 s | Window traverses in 5 s |
| `travel_m3` | 171 s | 5 s | Window traverses in 5 s |
| `avg_win_t` | 3 | 1 | Single sample = immediate T response |
| `avg_win_rh` | 3 | 1 | Single sample = immediate RH response |
| `WAIT_AFTER_PUSH` | — | 35 s | poll_interval (30) + 5 s buffer |
| `FAULT_WAIT` | — | 80 s | 2 × poll_interval + 10 s buffer |
| `BOOT_TIMEOUT` | — | 60 s | Covers boot + CLOSE_ALL with travel=5 s |
| `DWELL_WAIT` | — | 75 s | dwell_open_min (60 s) + 15 s buffer; used by TC-24 |
| `HISTORY_FILL_WAIT` | — | 105 s | 3 × WAIT_AFTER_PUSH; used by TC-29 |

---

## Serial patterns reference

| Event | Regex |
|---|---|
| Boot banner | `=== Greenhouse Controller v` |
| All tasks started | `All tasks spawned` |
| CLOSE_ALL done | `CLOSE_ALL calibration complete` |
| Window opening | `MOVING_OPEN\|CMD_OPEN` |
| Window closing | `MOVING_CLOSE\|CMD_CLOSE\|CMD_CLOSE_ALL` |
| Wind override set | `WIND_OVERRIDE set` |
| Wind override cleared | `WIND_OVERRIDE cleared` |
| T/RH fault | `T/RH sensor FAULT` |
| T/RH fault cleared | `T/RH sensor fault cleared` |
| Wind fault | `Wind sensor FAULT` |
| Wind fault cleared | `Wind sensor fault cleared` |
| Motor alarm | `MOTOR_ALARM asserted` |
| Motor alarm cleared | `MOTOR_ALARM cleared` |
| Config written | `Q4 applied:` |
| OTA healthy | `fail counter reset to 0` |
| Calibration mode (EG1 bit 6) | `CLOSE_ALL calibration\|EG1_BIT_CALIBRATING\|WINDOW_CAL` |
| T6 inhibited in Standby | `\[T6\].*standby\|\[T6\].*inhibited\|MODE_STANDBY` |
| Conflict resolution outcome | `\[T6\].*conflict\|\[T6\].*CR_\|C22` |
| Graduated step change | `\[T6\].*step [123]\|\[T6\].*vent_step\|\[T6\].*required_step` |

---

## `eg1` event-group bit reference

The `eg1` field in `GET /api/status` is a raw bitmask of the FreeRTOS event group:

| Bit | Mask | Meaning |
|---|---|---|
| 0 | `0x01` | `WIND_OVERRIDE` active |
| 2 | `0x04` | `SENSOR_FAULT_T` — T/RH sensor fault |
| 3 | `0x08` | `SENSOR_FAULT_W` — wind sensor fault |
| 4 | `0x10` | `OTA_IN_PROGRESS` — OTA update in progress |
| 5 | `0x20` | `MOTOR_ALARM` active |
| 6 | `0x40` | `EG1_BIT_CALIBRATING` — CLOSE_ALL calibration active; `mode` = `"WINDOW_CAL"` |

---

## Running the automated tests

### 1. Prerequisites

| Requirement | Details |
|---|---|
| Python | 3.10 or newer (`python --version`) |
| Device reachable | Ping `192.168.20.150` — must respond before starting |
| Emulator reachable | Ping `192.168.20.226` — must respond before starting |
| Serial port connected | USB cable to ESP32-S3; device must be running firmware |
| Serial port name | Default is `COM8`; adjust via `GH_SERIAL_PORT` env var |

### 2. Install dependencies

```powershell
python -m pip install pytest pytest-timeout pyserial requests
```

### 3. Configure PINs and connection details

**Windows (PowerShell):**
```powershell
$env:GH_ADMIN_PIN  = "your-admin-pin"
$env:GH_FARMER_PIN = "your-farmer-pin"
```

Or edit the constants in `conftest.py`:
```python
ADMIN_PIN  = "your-admin-pin"
FARMER_PIN = "your-farmer-pin"
```

To override defaults:
```powershell
$env:GH_SERIAL_PORT  = "COM5"
$env:GH_DEVICE_BASE  = "http://192.168.1.100"
$env:GH_EMULATOR_BASE= "http://192.168.1.200"
```

### 4. Special requirement for TC-01

TC-01 looks for the firmware boot banner on serial. Before running TC-01 (or the full
suite), the conftest will write `travel_m1/m2/m3 = 5 s` to NVS and then prompt:

```
============================================================
  ACTION REQUIRED — TC-01 Boot test

  Reset or power-cycle the device NOW, then press ENTER
  as soon as the device starts booting so the serial
  monitor catches the boot banner.
============================================================
```

Press ENTER as the device starts booting. The `_reset_mark` timestamp is recorded at
that point; all serial assertions in TC-01 use it as their look-back anchor.

### 5. Run the full automated suite

```powershell
cd test
python -m pytest -v -s --timeout=300
```

- `-v` — verbose: prints each test name and PASS/FAIL as it runs.
- `-s` — no capture: real-time step output and the TC-01 device-reset prompt appear in the terminal immediately.
- `--timeout=300` — TC-16 waits 165 s; this covers it with margin.

### 6. Run a single test file

```powershell
python -m pytest test_02_climate_temp.py -v -s --timeout=120
```

### 7. Run a single test case

```powershell
python -m pytest "test_02_climate_temp.py::TestTemperatureControl::test_tc02_windows_open_above_t_max" -v -s --timeout=120
```

### 8. Skip TC-01 when device is already running

```powershell
python -m pytest --ignore=test_01_boot.py -v -s --timeout=300
```

### 9. Read the results

Pytest prints a summary to the terminal. A plain-text log is written to `test/results.log`:

```
Greenhouse Controller Integration Tests
Started : 2026-05-06 14:32:01
============================================================

PASSED   test_01_boot.py::TestBoot::test_boot_banner
PASSED   test_01_boot.py::TestBoot::test_all_tasks_spawned
FAILED   test_02_climate_temp.py::TestTemperatureControl::test_tc02_windows_open_above_t_max
         AssertionError: No window-open event on serial after 45 s (T=28.0, t_max=25)

============================================================
Finished: 2026-05-06 14:47:23
```

### 10. Verify fixture teardown

After any run, confirm `fast_config` restored the device:

```bash
curl http://192.168.20.150/api/config
```

Check `travel_m1/m2/m3` and `avg_win_t/rh` are back to their pre-test values. If a test
was interrupted before teardown completed, restore manually via the device web UI or
`POST /api/config`.
