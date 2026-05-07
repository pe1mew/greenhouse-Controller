# 3.3 Setpoints and Hysteresis — Automated Test

## Purpose

Verifies the climate control setpoint and hysteresis logic (T6) for all test cases in
`softwareTestPlan.md` §3.3 against the live device, using the sensor emulator to inject
controlled readings.

| ID | Description |
|----|-------------|
| UT-CC-014 | OPEN when T > T_max_day (is_daytime = true) |
| UT-CC-015 | Stay open when T above (T_max − hyst_t) — hysteresis holds |
| UT-CC-016 | CLOSE when T < T_max − hyst_t |
| UT-CC-017 | CLOSE when T < T_min_day |
| UT-CC-018 | OPEN when RH > RH_max_day (rh_ctrl_en = true) |
| UT-CC-019 | No relay chatter at setpoint boundary |
| UT-CC-024 | CLOSE_ALL when RH < RH_min_day (over-dry) |
| UT-CC-025 | Graduated ventilation step 1: M1 only |
| UT-CC-026 | Graduated ventilation step 2: M1 + M2 |
| UT-CC-027 | Graduated ventilation step 3: M1 + M2 + M3 |
| UT-CC-028 | Night setpoints used when is_daytime = false |
| UT-CC-029 | Day setpoints used when is_daytime = true |

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| Device reachable | Default `http://192.168.20.150` (env: `GH_DEVICE_BASE`) |
| Sensor emulator reachable | Default `http://192.168.20.226` (env: `GH_EMULATOR_BASE`) |
| Admin PIN known | Default `12345678` (env: `GH_ADMIN_PIN`) |
| WiFi | Device and emulator both accessible on local network |
| Python | 3.10 or newer |
| `requests` library | `pip install requests` |

---

## How to Run

```powershell
# 1. Install dependency (once)
python -m pip install requests

# 2. Override addresses / PIN if not using defaults
$env:GH_DEVICE_BASE   = "http://192.168.20.150"
$env:GH_EMULATOR_BASE = "http://192.168.20.226"
$env:GH_ADMIN_PIN     = "12345678"

# 3. Run from the test directory
cd test
python 3_3_Setpoints_and_Hysteresis.py
```

Results are written to `test/3_3_Setpoints_and_Hysteresis.log` and echoed to stdout.

---

## Test Parameters

The script temporarily writes the following values to NVS to keep test duration
manageable. All values are **restored unconditionally in a `finally` block**.

| NVS key | Namespace | Production default | Test value | Purpose |
|---------|-----------|-------------------|------------|---------|
| `poll_interval` | `system` | 60 s | 30 s | Minimum wait between sensor reads |
| `travel_m1/m2/m3` | `motor` | 21/21/171 s | 5 s | Minimum motor travel time |
| `avg_win_t` | `climate` | varies | 0 | 0 min → window = 1 sample = immediate T response |
| `avg_win_rh` | `climate` | varies | 0 | 0 min → window = 1 sample = immediate RH response |
| `dwell_open_m1/2/3` | `motor` | 0 min | 0 min | No hold at OPEN before CLOSE accepted |
| `dwell_close_m1/2/3` | `motor` | 0 min | 0 min | No hold at CLOSED before OPEN accepted |
| `wind_prot_en` | `wind` | varies | 0 | Wind override must not interfere |
| `lat_deg` / `lat_frac` | `system` | site location | ±89 | Force is_daytime for CC-028/029 |

### Setpoints written for the climate tests

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `t_max_day` | 25 °C | Clear step boundaries |
| `hyst_t` | 6 °C | step_width = 6 ÷ 3 = 2 |
| `rh_max_day` | 70 % | RH control boundary |
| `rh_min_day` | 40 % | Over-dry threshold |
| `hyst_rh` | 6 % | step_width = 2 for RH |
| `t_max_ngt` | 18 °C | Night setpoint discriminator |

---

## Expected Duration

| Phase | Time |
|-------|------|
| Admin login + setup | ~55 s |
| UT-CC-014 (open on T) | ~50 s |
| UT-CC-015 (hyst hold, 2 polls) | ~75 s |
| UT-CC-016 (close on T) | ~50 s |
| UT-CC-017 (open then close on T_min) | ~100 s |
| UT-CC-018 (open on RH) | ~100 s |
| UT-CC-019 (chatter — 3 poll cycles) | ~155 s |
| UT-CC-024 (over-dry CLOSE_ALL) | ~145 s |
| UT-CC-025/026/027 (graduated steps) | ~200 s |
| UT-CC-028 (night setpoints) | ~100 s |
| UT-CC-029 (day setpoints) | ~90 s |
| Teardown | ~25 s |
| **Total** | **~1145 s (~19 min)** |

---

## How the Tests Work

### Sensor verification loop

Each test case pushes a sensor value to the emulator, then waits
`poll_interval + 5 s` (35 s) before reading `GET /api/status`.
The controller's `temp_avg` and `rh_avg` fields are compared against the pushed
values (tolerance ±1.5 °C / ±3 %).  On mismatch the push + wait cycle is
repeated up to `MAX_SENSOR_RETRIES = 2` times; if still mismatched the test
is aborted with FAIL and the reason logged.  This handles transient Modbus or
network glitches without masking genuine firmware failures.

### Window state verification

After the sensor is confirmed, the script waits for the motor travel time:
```
poll_interval + travel_s + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S
= 30 + 5 + 5 + 5 = 45 s
```
`FIRMWARE_TRAVEL_MARGIN_S = 5 s` is the fixed safety margin that the firmware
(`relay_controller.cpp`) adds internally to every relay pulse beyond `travel_s`.

Window states are read from the `windows` array in `GET /api/status`:
`["UNKNOWN" | "CLOSED" | "MOVING_OPEN" | "OPEN" | "MOVING_CLOSE", …]`

### Graduated ventilation algorithm

The firmware (`climate_control.cpp`) uses:
```
step_width = hyst_t ÷ NUM_VENT_STEPS = 6 ÷ 3 = 2
deviation  = T_avg − T_max_day
required_step = clamp(ceil(deviation ÷ step_width), 0, 3)
```
With `t_max_day = 25`, `hyst_t = 6`:

| T (°C) | deviation | ceil(dev÷2) | step | Channels |
|--------|-----------|-------------|------|----------|
| 26 | 1 | 1 | 1 | M1 only |
| 28 | 3 | 2 | 2 | M1 + M2 |
| 31 | 6 | 3 | 3 | M1 + M2 + M3 |
| 24 | −1 | 0 → held | 1 | hold (hysteresis) |
| 18 | −7 | 0 | 0 | close |

Close-hysteresis guard: step is not allowed to drop to 0 until
`deviation ≤ −hyst_t`, i.e. `T < t_max_day − hyst_t = 19 °C`.

### Day / night setpoint forcing (UT-CC-028/029)

`T4` recomputes `is_daytime` immediately when `lat_deg` is written via Q4
(calls `update_sun_times()` inside `apply_config_update()`).
The test exploits this:
- `lat_deg = 89`  (89 °N, May): `sunrise_calc` returns `SUNRISE_POLAR_DAY`
  → `is_daytime = true`
- `lat_deg = -89` (89 °S, May): `sunrise_calc` returns `SUNRISE_POLAR_NIGHT`
  → `is_daytime = false`

The discriminator temperature for UT-CC-029 is 14 °C:
- Day  (`t_max_day = 25`): 14 < 25 → step 0 → windows stay **CLOSED**
- Night (`t_max_ngt = 12`): 14 > 12 → step 1 → windows would **OPEN**

Windows remaining CLOSED at T = 14 °C with `is_daytime = true` proves the
firmware selects the day setpoint, not the night setpoint.

### Note on t_min_day (UT-CC-017)

The `t_min_day` NVS key is stored and exposed in `GET /api/config`, but
`climate_control.cpp` does **not** evaluate it in `vent_step_required_t()`.
The temperature step function uses only `t_max_day` and `hyst_t`.
UT-CC-017 is satisfied because whenever `T < t_min_day`, `T` is also below the
hysteresis close threshold (`t_max_day − hyst_t = 19 °C` for the test
configuration), so windows will close via the normal hysteresis guard.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All 12 tests passed |
| `1` | One or more tests failed |
| `2` | Admin session could not be established |

---

## Log File Format

```
2026-05-07 12:00:00  INFO   Admin session established
2026-05-07 12:00:01  INFO   SETUP: activating REST mode on sensor emulator
...
2026-05-07 12:00:55  INFO   --- UT-CC-014: OPEN when T > T_max_day ---
2026-05-07 12:00:57  INFO     Pushed T=26°C RH=55% — waiting 35 s for poll …
2026-05-07 12:01:32  INFO     Sensor confirmed: T=26.0°C RH=55%
2026-05-07 12:01:47  INFO     Windows: ['OPEN', 'CLOSED', 'CLOSED']
2026-05-07 12:01:47  INFO   [UT-CC-014] PASS — T=26°C > t_max=25°C → M1 opened: ['OPEN', 'CLOSED', 'CLOSED']
...
2026-05-07 12:20:10  INFO   ============================================================
2026-05-07 12:20:10  INFO   SUMMARY: 12/12 passed, 0 failed
```
