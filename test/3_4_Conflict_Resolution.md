# 3.4 Conflict Resolution — Automated Test

## Purpose

Verifies the conflict resolution of the law in force since firmware 2.15.0, `stepped` v2
(gh#84), for all test cases in `softwareTestPlan.md` §6.4, against the live device, using the
sensor emulator to inject controlled readings. The law's rules are pinned by the host tests in
`drivers/ventModel` (`pio test -e native`); this script is the device-level view.

| ID | Description |
|----|-------------|
| UT-CC-020 | Priority 0: T demands OPEN (step 1), RH votes CLOSE (step 0) → T's step → M1 opens |
| UT-CC-021 | Priority 0: T demands nothing (step 0), RH demands OPEN (step 3) → stays CLOSED |
| UT-CC-022a | Rule 2 — both demand OPEN: max(step_t=1, step_rh=3) = 3 → all open |
| UT-CC-022b | Rule 3 — both demand CLOSE: step_t=0 == step_rh=0 → every window closes |
| UT-CC-030a | Dryness never closes against heat, priority 1: T=26 (step 1), RH=35 → M1 opens |
| UT-CC-030b | Dryness never closes against heat, priority 2: T=28 (step 2), RH=35 → M1+M2 open |
| UT-CC-031a | Humidity alone, priority 1, from closed at T_min + 1 → stays CLOSED (below the floor) |
| UT-CC-031b | Humidity alone, priority 1, at T_min + 2 → M1 opens, M2 and M3 stay CLOSED |
| UT-CC-031c | Humidity alone, priority 1, back to T_min + 1 → M1 stays open (the live opening holds) |
| UT-CC-031d | Humidity alone, priority 1, at T_min → M1 closes |
| UT-CC-031e | Humidity alone, priority 2, from closed at T_min + 2 → M1 only (priority 2 = priority 1) |
| UT-CC-031f | Humidity alone, priority 0, from closed at T_min + 2 → stays CLOSED |

Until 2.15.0, UT-CC-030 was "CR_RH_FIRST: humidity wins" (the dry vote closed a hot house) and
UT-CC-031 was "CR_DEVIATION: higher step wins" (T=10, RH=80 opened all three windows). Both are
restated as in `softwareTestPlan.md` §6.4. The old UT-CC-031b (T=28 against RH=35 under priority
2) is UT-CC-030b now, with the same outcome.

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **The operator's go** | Running this changes the rig: about 30 settings (restored and read back at the end), and the values the sensor emulator serves |
| **Node-RED feed stopped** | The emulator normally runs in REST, fed every 60 s by Node-RED on Shuttle2 with 5C88's live readings, which would overwrite every injected value. Stop the feed for the run and restart it afterwards. The script checks this first and stops with exit code 2, touching none of the unit's settings, if the emulator does not hold a pushed value |
| No soak or other harness running | The teardown restores the values it sampled at the start |
| Device reachable | The fitted dev-rig module: `2344` at `http://192.168.20.160` or `FDA4` at `http://192.168.20.169`. The script tries both and uses the one that answers (only the fitted one is powered), and logs its `unit_id`. `GH_DEVICE_BASE` overrides the search. It refuses to run against 5C88 |
| Firmware 2.15.0 or later | `GET /api/status` must carry `windows.law`; the script stops (exit 2) otherwise |
| Unit in AUTOMATIC | Setup stops (exit 2) on STANDBY, a motor alarm or a calibration |
| Sensor emulator reachable | Default `http://192.168.20.226` (env: `GH_EMULATOR_BASE`) |
| Admin PIN known | Default `12345678` (env: `GH_ADMIN_PIN`) |
| Python | 3.10 or newer |
| `requests` library | `pip install requests` |

---

## How to Run

```powershell
# 1. Install dependency (once)
python -m pip install requests

# 2. Optional overrides (without GH_DEVICE_BASE the fitted rig module is found)
$env:GH_DEVICE_BASE   = "http://192.168.20.160"
$env:GH_EMULATOR_BASE = "http://192.168.20.226"
$env:GH_ADMIN_PIN     = "12345678"

# 3. Run from the test directory
cd test
python 3_4_Conflict_Resolution.py
```

Results are written to `test/3_4_Conflict_Resolution.log` and echoed to stdout. Afterwards restart
the Node-RED feed: until then the emulator serves the teardown's T 10 °C / RH 55 %, and the rig's
climate is frozen there.

---

## Test Parameters

The script temporarily writes the following values to NVS to keep test duration
manageable. All values are **restored in a `finally` block and read back**.

| NVS key | Namespace | Factory default | Test value | Purpose |
|---------|-----------|-----------------|------------|---------|
| `poll_interval` | `system` | 30 s | 30 s | Minimum wait between sensor reads |
| `travel_m1/m2` | `motor` | 21 s | 5 s | Minimum motor travel time |
| `travel_m3` | `motor` | 171 s (the rig: 13 s) | *unchanged* | It belongs to the rig: a shorter value stops M3 short of its end. The motor wait is computed from it (53 s on the rig) |
| `avg_win_t` | `climate` | 6 min | 1 min | The minimum (0 is clamped to 1): 2 samples at a 30 s poll |
| `avg_win_rh` | `climate` | 10 min | 1 min | Same as `avg_win_t` |
| `dwell_open_m1/2/3` | `motor` | 300/300/1500 s | 0 s | No hold at OPEN before CLOSE accepted |
| `dwell_close_m1/2/3` | `motor` | 0/0/600 s | 0 s | No hold at CLOSED before OPEN accepted |
| `ctrl_mode_m3` | `motor` | 1 (Lineair) | 0 | M3 on the timed law, so `stepped` decides all three windows (see below) |
| `wind_prot_en` | `wind` | 1 | 0 | Wind override must not interfere |
| `rh_ctrl_en` | `climate` | 1 | 1 | RH control must be active for all tests |
| `cr_priority` | `climate` | 0 | 0, 1, 2 | Set by each case |
| `lat_deg` … `lon_frac` | `system` | site location | see below | Force is_daytime = true (day setpoints) |

### Setpoints written for the conflict tests

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `t_max_day` | 25 °C | step_width = hyst_t ÷ 3 = 2; T=26 → step 1, T=28 → step 2 |
| `hyst_t` | 6 °C | an open step holds down to 25 − 6 = 19 °C; T ≤ 25 from closed → step 0 |
| `t_min_day` | 12 °C | The humidity floor: humidity alone may open M1 from 14 °C, a live opening holds to 13 °C |
| `rh_max_day` | 70 % | RH=80 → step 3 (open demand) |
| `rh_min_day` | 40 % | RH=35 < 40 → step 0, a "too dry" vote that since v2 changes no decision |
| `hyst_rh` | 6 % | step_width = 2 for RH; the close guard holds a live vote down to 68 % |

---

## Expected Duration

Measured in an offline dry run on the rig's timings (30 s poll, `travel_m3` 13 s); a real run adds
network time.

| Phase | Time |
|-------|------|
| Pre-check: the emulator holds a pushed value | ~130 s |
| Admin login + setup | ~50 s |
| UT-CC-020 (priority 0, T open wins) | ~180 s |
| UT-CC-021 (priority 0, humidity alone refused, 2 polls) | ~160 s |
| UT-CC-022a+b (Rule 2 + Rule 3) | ~265 s |
| UT-CC-030a+b (dryness never closes against heat) | ~410 s |
| UT-CC-031a–f (the floor, its hold, the M1 cap, priorities 2 and 0) | ~810 s |
| Teardown + read-back | ~20 s |
| **Total** | **~2 030 s (~34 min)** |

---

## How the Tests Work

### The `vent_resolve_conflict()` algorithm (stepped v2)

`drivers/ventModel/src/vent_model_stepped.cpp` resolves the T and RH ventilation demands via:

```
Rule 1: step_rh == VENT_STEP_NONE(−1)   → return step_t   (RH has no vote)
Rule 2: step_t > 0 AND step_rh > 0      → return max(step_t, step_rh)  (both open)
Rule 3: step_t == step_rh               → return step_t   (same; no conflict)
Rule 4: step_t > 0 AND step_rh == 0     → return step_t   (dryness never closes against heat,
                                                           under every cr_priority)
Rule 5: what is left, RH alone wants OPEN (step_t == 0, step_rh > 0):
         cr_priority 0     → return 0                     (temperature first)
         cr_priority 1 / 2 → return min(step_rh, 1) — M1 only — when
                             T_avg ≥ t_min + 2, or T_avg ≥ t_min + 1 while the last
                             decision was a humidity-only opening; else 0
```

Rules are evaluated in order; the first matching rule returns immediately. T_avg is the average in
whole degrees, as T6 passes it (rounded half away from zero).

`VENT_STEP_NONE = −1` is returned by the RH branch when `rh_ctrl_en = 0`, or when RH is inside
`[rh_min, rh_max]` — except that a live humidity vote holds at step 1 until RH has fallen to
`rh_max − max(hyst_rh/3, 1)` (the close guard, new in v2). The temperature branch never returns
NONE. The close guard is pinned by the host tests and not exercised here.

### How the cases are built

| Test | cr_priority | step_t | step_rh | Rule | Outcome |
|------|-------------|--------|---------|------|---------|
| CC-020 | 0 | 1 (T=26 > t_max=25) | 0 (RH=35 < rh_min=40) | 4 | M1 open |
| CC-021 | 0 | 0 (T=10) | 3 (RH=80 > rh_max=70) | 5 | all closed |
| CC-022a | 0 | 1 (T=26) | 3 (RH=80) | 2 | all open |
| CC-022b | 0 | 0 (T=10) | 0 (RH=35) | 3 | all closed |
| CC-030a | 1 | 1 (T=26) | 0 (RH=35) | 4 | M1 open |
| CC-030b | 2 | 2 (T=28) | 0 (RH=35) | 4 | M1+M2 open |
| CC-031a | 1 | 0 (T=13) | 3 (RH=80) | 5, below the floor | all closed |
| CC-031b | 1 | 0 (T=14) | 3 (RH=80) | 5, at the floor | M1 only |
| CC-031c | 1 | 0 (T=13) | 3 (RH=80) | 5, live opening held | M1 stays open |
| CC-031d | 1 | 0 (T=12) | 3 (RH=80) | 5, below the hold | M1 closes |
| CC-031e | 2 | 0 (T=14) | 3 (RH=80) | 5, at the floor | M1 only |
| CC-031f | 0 | 0 (T=14) | 3 (RH=80) | 5, priority 0 | all closed |

Mirror tests:

| Pair | Same inputs | Different | Outcome |
|------|------------|-----------|---------|
| CC-020 ↔ CC-030a | T=26, RH=35 | priority 0 ↔ 1 | M1 opens in both: since v2 the dry vote loses under every priority (v1: stayed closed under 1) |
| CC-031a ↔ CC-031c | T=13, RH=80, priority 1 | from closed ↔ with M1 open on humidity | stays closed ↔ M1 stays open: the floor's hysteresis |
| CC-031b ↔ CC-031f | T=14, RH=80 | priority 1 ↔ 0 | M1 opens ↔ stays closed |

CC-021 (T=10) is refused twice over: by priority 0, and by the floor (10 < 14). CC-031f is the
priority-0 case above the floor.

### Rule 2 and Rule 3 (CC-022)

`step_t=1, step_rh=3` — both positive → Rule 2 fires before `cr_priority` is consulted.
`max(1, 3) = 3` → all three windows open. v2 keeps this: its M1 cap is for humidity *alone*, and
here the temperature is venting too.

`step_t=0, step_rh=0` — equal → Rule 3 fires. Since Rule 2 only fires when
both are *positive*, equal-zero falls through to Rule 3 (not Rule 2).
Result: `step = 0` → every open window closes.

### Negative test confirmation (CC-021, CC-031a, CC-031f)

For tests that assert windows stay closed, two consecutive poll cycles are
observed:
1. `push_and_verify_sensor` — confirms the controller read the pushed values
2. Second `push_sensors` + `sleep(WAIT_FOR_SENSOR_S)` — second evaluation cycle

If windows have not opened after 2 complete evaluations, the inhibition is real.

### Sensor verification loop

Each test case pushes a sensor value to the emulator, then waits
`poll_interval + 5 s` (35 s) before reading `GET /api/status`. The controller's
`climate.temp_avg_c` and `climate.rh_avg_pct` are compared against the pushed values with a
tolerance of ±0.3 °C / ±1 %, tight enough that only a converged average passes: with a 2-sample
window the first poll after a 1 °C step reads 0.5 °C off, and T6 would round a half-converged 13.5
to 14. On mismatch the push + wait cycle is repeated up to `MAX_SENSOR_RETRIES = 2` times; if it
still mismatches the case is recorded **INCONCLUSIVE** (it could not be judged), not FAIL.

Every push also carries a wind direction of 312° as a watermark, and the raw `wind.direction_deg`
must read it back. The Node-RED feed would replace it with 5C88's, so a mismatch means something
else is writing to the emulator.

### Window state verification

After the sensor is confirmed, the script waits for the motor travel time:
```
poll_interval + travel_s + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S
= 30 + 13 + 5 + 5 = 53 s        (travel_s: the longest in force, M3's 13 s on the rig)
```

`FIRMWARE_TRAVEL_MARGIN_S = 5 s` is the fixed safety margin that the firmware
(`relay_controller.cpp`) adds internally to every relay pulse beyond `travel_s`.

Window states are read from the canonical status (`status_json.cpp`), where `windows` is an
**object**, not a list:

```json
"windows": {"M1": "OPEN", "M2": "CLOSED", "M3": "CLOSED",
            "M3_ctrl_mode": "TIMED", "law": "stepped v2",
            "M3_ctrl_reason": "setting", "M3_pos_gate": "ok",
            "M3_percent_x10": 3, "M3_mm_x10": 2, "M3_at_end_sensor": true}
```

Only `M1`, `M2` and `M3` are read (`"UNKNOWN" | "CLOSED" | "MOVING_OPEN" | "OPEN" |
"MOVING_CLOSE" | "PART_OPEN"`); iterating the object would yield its key names. Where a close of
every window is expected, `CLOSED` and `MOVING_CLOSE` are both accepted; windows that must stay
shut must read `CLOSED`.

### M3 on the timed law

Since 2.13.0 a unit with a position sensor defaults to Lineair (`ctrl_mode_m3 = 1`), where the law
is `graded`: M3 opens only from step 2, moves at most 25 % per drive and not within 10 min of its
last one. These cases pin `stepped`, so setup writes `ctrl_mode_m3 = 0` and waits until the status
shows `law "stepped v2"` and `M3_ctrl_mode "TIMED"`. The teardown restores the setting.

### Forcing day setpoints

`T4` recomputes `is_daytime` from the location as soon as `lat_deg`/`lon_deg` are written, and
about once a minute from its clock. The script writes a location and keeps the first one the
status confirms: the pole that is in polar day (89° N from the March equinox to the September one, 89° S
the rest of the year), or, within about a day of an equinox when neither pole is, 1° N at the
longitude where it is solar noon. The old fixed 89° N was right from March to September only.

### `force_windows_closed` inter-test isolation

Between test cases the script pushes `T = 10 °C, RH = 55 %` and waits
`WAIT_FOR_MOTOR_S`. `RH = 55 %` is inside `[rh_min=40, rh_max=70]` →
`step_rh = VENT_STEP_NONE` → Rule 1 fires before `cr_priority` is
consulted → `step = step_t = 0` regardless of `cr_priority`.
It also ends a live humidity opening, so the next case starts with nothing for the floor to hold.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All 12 test cases passed |
| `1` | One or more test cases failed |
| `2` | Could not run or could not judge: no unit or production found, firmware before 2.15.0, the emulator overwritten, setup preconditions not met, or a case INCONCLUSIVE |

---

## Log File Format

```
2026-09-27 12:00:00  INFO   Device   : http://192.168.20.160
2026-09-27 12:00:00  INFO   Unit     : 2344  fw 2.15.0  law graded v2  M3 LINEAR
...
2026-09-27 12:02:10  INFO     The emulator held the pushed values
2026-09-27 12:03:00  INFO   SETUP complete - AUTOMATIC, law stepped v2, M3 TIMED, M1=CLOSED M2=CLOSED M3=CLOSED
2026-09-27 12:03:00  INFO   --- UT-CC-020: priority 0 - T open (step 1) against RH too dry (step 0) ---
...
2026-09-27 12:05:58  INFO   [UT-CC-020] PASS - prio 0, step_t 1 vs step_rh 0: T's step -> expected M1 open, the rest CLOSED; got M1=OPEN M2=CLOSED M3=CLOSED
...
2026-09-27 12:33:30  INFO   TEARDOWN: read-back matches the start for all 27 settings
2026-09-27 12:33:32  INFO   ============================================================
2026-09-27 12:33:32  INFO   SUMMARY: 12/12 passed, 0 failed, 0 inconclusive
```
