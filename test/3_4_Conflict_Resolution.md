# 3.4 Conflict Resolution — Automated Test

## Purpose

Verifies the `vent_resolve_conflict()` logic (T6) for all test cases in
`softwareTestPlan.md` §3.4 against the live device, using the sensor emulator to inject
controlled readings.

| ID | Description |
|----|-------------|
| UT-CC-020 | CR_TEMP_FIRST: T demands OPEN (step 1), RH demands CLOSE (step 0) → T wins → M1 opens |
| UT-CC-021 | CR_TEMP_FIRST: T demands CLOSE (step 0), RH demands OPEN (step 3) → T wins → stays CLOSED |
| UT-CC-022a | Rule 2 — both demand OPEN: max(step_t=1, step_rh=3) = 3 → all open |
| UT-CC-022b | Rule 3 — both demand CLOSE: step_t=0 == step_rh=0 → CLOSE_ALL |
| UT-CC-030 | CR_RH_FIRST: T demands OPEN (step 1), RH demands CLOSE (step 0) → RH wins → stays CLOSED |
| UT-CC-031a | CR_DEVIATION: T step=0 vs RH step=3 → max(0, 3) = 3 → all open |
| UT-CC-031b | CR_DEVIATION: T step=2 vs RH step=0 → max(2, 0) = 2 → M1+M2 open, M3 closed |

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
python 3_4_Conflict_Resolution.py
```

Results are written to `test/3_4_Conflict_Resolution.log` and echoed to stdout.

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
| `rh_ctrl_en` | `climate` | varies | 1 | RH control must be active for all tests |
| `lat_deg` / `lat_frac` | `system` | site location | 89 | Force is_daytime = true (day setpoints) |

### Setpoints written for the conflict tests

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `t_max_day` | 25 °C | step_width = hyst_t ÷ 3 = 2; T=26 → step 1, T=28 → step 2 |
| `hyst_t` | 6 °C | close threshold = 25 − 6 = 19 °C; T=10 < 19 → step 0 |
| `rh_max_day` | 70 % | RH=80 → step 3 (open demand) |
| `rh_min_day` | 40 % | RH=35 < 40 → step 0 (over-dry CLOSE demand) |
| `hyst_rh` | 6 % | — |

---

## Expected Duration

| Phase | Time |
|-------|------|
| Admin login + setup | ~55 s |
| UT-CC-020 (CR_TEMP_FIRST, T open wins) | ~130 s |
| UT-CC-021 (CR_TEMP_FIRST, T close wins, 2 polls) | ~120 s |
| UT-CC-022a+b (Rule 2 + Rule 3) | ~210 s |
| UT-CC-030 (CR_RH_FIRST, RH close wins, 2 polls) | ~120 s |
| UT-CC-031a+b (CR_DEVIATION, 2 sub-cases) | ~260 s |
| Teardown | ~25 s |
| **Total** | **~920 s (~15 min)** |

---

## How the Tests Work

### The `vent_resolve_conflict()` algorithm

`climate_control.cpp` resolves the T and RH ventilation demands via:

```
Rule 1: step_rh == VENT_STEP_NEUTRAL(−1)  → return step_t   (RH has no vote)
Rule 2: step_t > 0 AND step_rh > 0        → return max(step_t, step_rh)  (both open)
Rule 3: step_t == step_rh                 → return step_t   (same; no conflict)
Rule 4: genuine conflict                  → use cr_priority:
         0 = CR_TEMP_FIRST  → return step_t
         1 = CR_RH_FIRST    → return step_rh
         2 = CR_DEVIATION   → return max(step_t, step_rh)
```

Rules are evaluated in order; the first matching rule returns immediately.

`VENT_STEP_NEUTRAL = −1` is returned by `vent_step_required_rh()` only when
RH is inside `[rh_min, rh_max]` or `rh_ctrl_en = 0`.  The temperature function
never returns NEUTRAL.

### How genuine conflicts are created

A genuine conflict (Rule 4) requires one sensor to demand OPEN (`step > 0`) and
the other to demand CLOSE (`step = 0`), or vice-versa.

| Test | step_t | step_rh | Conflict type |
|------|--------|---------|---------------|
| CC-020, CC-030 | 1 (T=26 > t_max=25) | 0 (RH=35 < rh_min=40) | T open vs RH close |
| CC-021, CC-031a | 0 (T=10 < 19) | 3 (RH=80 > rh_max=70) | T close vs RH open |
| CC-031b | 2 (T=28) | 0 (RH=35 < rh_min=40) | T open (step 2) vs RH close |

Mirror tests:

| Pair | Same inputs | Different cr_priority | Opposite outcome |
|------|------------|----------------------|-----------------|
| CC-020 ↔ CC-030 | T=26, RH=35 | 0 (T wins) ↔ 1 (RH wins) | M1 opens ↔ stays CLOSED |
| CC-021 ↔ CC-031a | T=10, RH=80 | 0 (T wins) ↔ 2 (max wins) | stays CLOSED ↔ all open |

### Rule 2 and Rule 3 (CC-022)

`step_t=1, step_rh=3` — both positive → Rule 2 fires before Rule 4; `cr_priority`
is not consulted.  `max(1, 3) = 3` → all three windows open.

`step_t=0, step_rh=0` — equal → Rule 3 fires.  Since Rule 2 only fires when
both are *positive*, equal-zero falls through to Rule 3 (not Rule 2).
Result: `step = 0` → CLOSE_ALL.

### Negative test confirmation (CC-021, CC-030)

For tests that assert windows stay closed, two consecutive poll cycles are
observed:
1. `push_and_verify_sensor` — confirms the controller read the pushed values
2. Second `push_sensors` + `sleep(WAIT_FOR_SENSOR_S)` — second evaluation cycle

If windows have not opened after 2 complete evaluations, the inhibition is real.

### Sensor verification loop

Each test case pushes a sensor value to the emulator, then waits
`poll_interval + 5 s` (35 s) before reading `GET /api/status`.
The controller's `temp_avg` and `rh_avg` fields are compared against the pushed
values (tolerance ±1.5 °C / ±3 %).  On mismatch the push + wait cycle is
repeated up to `MAX_SENSOR_RETRIES = 2` times; if still mismatched the test
is aborted with FAIL and the reason logged.

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

Where a CLOSE is expected, both `CLOSED` and `MOVING_CLOSE` are accepted as
correct.  M3 has a production travel time of 171 s; at `TEST_TRAVEL_S = 5` the
relay is only energised for 10 s, so the motor may still be travelling when
the status is polled.

### `force_windows_closed` inter-test isolation

Between test cases the script pushes `T = 10 °C, RH = 55 %` and waits
`WAIT_FOR_MOTOR_S`.  `RH = 55 %` is inside `[rh_min=40, rh_max=70]` →
`step_rh = VENT_STEP_NEUTRAL` → Rule 1 fires before `cr_priority` is
consulted → `step = step_t = 0` regardless of `cr_priority`.
This ensures `force_windows_closed` is safe even when a test left
`cr_priority = CR_RH_FIRST` or `CR_DEVIATION`.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All 7 test cases passed |
| `1` | One or more test cases failed |
| `2` | Admin session could not be established |

---

## Log File Format

```
2026-05-07 12:00:00  INFO   Admin session established
2026-05-07 12:00:01  INFO   SETUP: activating REST mode on sensor emulator
...
2026-05-07 12:00:55  INFO   --- UT-CC-020: CR_TEMP_FIRST — T open (step=1) wins over RH close (step=0) ---
2026-05-07 12:00:57  INFO     Pushed T=26°C RH=35% — waiting 35 s for poll …
2026-05-07 12:01:32  INFO     Sensor confirmed: T=26.0°C RH=35%
2026-05-07 12:01:47  INFO     Windows: ['OPEN', 'CLOSED', 'CLOSED']
2026-05-07 12:01:47  INFO   [UT-CC-020] PASS — CR_TEMP_FIRST: T=step_t=1 vs RH=step_rh=0 → T wins → M1 open, M2+M3 closed: ['OPEN', 'CLOSED', 'CLOSED']
...
2026-05-07 12:16:10  INFO   ============================================================
2026-05-07 12:16:10  INFO   SUMMARY: 7/7 passed, 0 failed
```
