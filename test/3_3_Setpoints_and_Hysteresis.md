# 3.3 Setpoints and Hysteresis — Automated Test

## Purpose

Verifies the climate control setpoint and hysteresis logic (T6) for all test cases in
`softwareTestPlan.md` §6.3 against the live device, using the sensor emulator to inject
controlled readings. The expectations are those of the law in force since firmware 2.15.0,
`stepped` v2 (gh#84); only the two humidity cases changed with it.

| ID | Description |
|----|-------------|
| UT-CC-014 | OPEN when T > T_max_day (is_daytime = true) |
| UT-CC-015 | Stay open when T above (T_max − hyst_t) — hysteresis holds |
| UT-CC-016 | CLOSE when T < T_max − hyst_t |
| UT-CC-017 | CLOSE when T < T_min_day |
| UT-CC-018 | OPEN when RH > RH_max_day (rh_ctrl_en = true, cr_priority 1): M1 only, from T_min + 2 °C |
| UT-CC-019 | No relay chatter at setpoint boundary |
| UT-CC-024 | Close when RH < RH_min_day: humidity's M1 opening ends |
| UT-CC-025 | Graduated ventilation step 1: M1 only |
| UT-CC-026 | Graduated ventilation step 2: M1 + M2 |
| UT-CC-027 | Graduated ventilation step 3: M1 + M2 + M3 |
| UT-CC-028 | Night setpoints used when is_daytime = false |
| UT-CC-029 | Day setpoints used when is_daytime = true |

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
python 3_3_Setpoints_and_Hysteresis.py
```

Results are written to `test/3_3_Setpoints_and_Hysteresis.log` and echoed to stdout. Afterwards
restart the Node-RED feed: until then the emulator serves the teardown's T 10 °C / RH 55 %, and
the rig's climate is frozen there.

---

## Test Parameters

The script temporarily writes the following values to NVS to keep test duration
manageable. All values are **restored in a `finally` block and read back**.

| NVS key | Namespace | Factory default | Test value | Purpose |
|---------|-----------|-----------------|------------|---------|
| `poll_interval` | `system` | 30 s | 30 s | Minimum wait between sensor reads |
| `travel_m1/m2` | `motor` | 21 s | 5 s | Minimum motor travel time |
| `travel_m3` | `motor` | 171 s (the rig: 13 s) | *unchanged* | It belongs to the rig: a shorter value stops M3 short of its end. The motor wait is computed from it (53 s on the rig) |
| `avg_win_t` | `climate` | 6 min | 1 min | Minimum allowed (v1.16.25 `cfg_clamp` enforces ≥1). 1 min × 60/30 = 2 samples; first poll after a push reads (prev+new)/2; second poll reads the new value (handled by `push_and_verify_sensor` retry). |
| `avg_win_rh` | `climate` | 10 min | 1 min | Same as `avg_win_t`. |
| `dwell_open_m1/2/3` | `motor` | 300/300/1500 s | 0 s | No hold at OPEN before CLOSE accepted |
| `dwell_close_m1/2/3` | `motor` | 0/0/600 s | 0 s | No hold at CLOSED before OPEN accepted |
| `ctrl_mode_m3` | `motor` | 1 (Lineair) | 0 | M3 on the timed law, so `stepped` decides all three windows (see below) |
| `wind_prot_en` | `wind` | 1 | 0 | Wind override must not interfere |
| `rh_ctrl_en` | `climate` | 1 | 1 / 0 | On for UT-CC-018/024, off for the temperature cases |
| `cr_priority` | `climate` | 0 | 1 | UT-CC-018/024: humidity may also open M1 |
| `lat_deg` … `lon_frac` | `system` | site location | see below | Force is_daytime for the day setpoints and for CC-028/029 |

### Setpoints written for the climate tests

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `t_max_day` | 25 °C | Clear step boundaries (40 °C in CC-018/024/028: the temperature never asks) |
| `hyst_t` | 6 °C | step_width = 6 ÷ 3 = 2 |
| `t_min_day` | 15 °C | CC-017: T = 14 °C is below it. CC-018/024: the humidity floor, 15 + 2 = 17 °C |
| `rh_max_day` | 70 % | RH control boundary |
| `rh_min_day` | 40 % | Over-dry threshold (inert since 2.15.0) |
| `hyst_rh` | 6 % | step_width = 2 for RH; the close guard holds a live vote down to 68 % |
| `t_max_ngt` | 18 °C | Night setpoint discriminator |

---

## Expected Duration

Measured in an offline dry run on the rig's timings (30 s poll, `travel_m3` 13 s); a real run adds
network time.

| Phase | Time |
|-------|------|
| Pre-check: the emulator holds a pushed value | ~130 s |
| Admin login + setup | ~50 s |
| UT-CC-014 (open on T) | ~125 s |
| UT-CC-015 (hyst hold, 2 polls) | ~70 s |
| UT-CC-016 (close on T) | ~125 s |
| UT-CC-017 (open then close below T_min) | ~250 s |
| UT-CC-018 (open on RH, M1 only) | ~180 s |
| UT-CC-019 (chatter — 3 poll cycles) | ~285 s |
| UT-CC-024 (open on RH, then close) | ~320 s |
| UT-CC-025/026/027 (graduated steps) | ~335 s |
| UT-CC-028 (night setpoints) | ~180 s |
| UT-CC-029 (day setpoints) | ~160 s |
| Teardown + read-back | ~20 s |
| **Total** | **~2 230 s (~37 min)** |

---

## How the Tests Work

### Sensor verification loop

Each test case pushes a sensor value to the emulator, then waits
`poll_interval + 5 s` (35 s) before reading `GET /api/status`. The controller's
`climate.temp_avg_c` and `climate.rh_avg_pct` are compared against the pushed values with a
tolerance of ±0.3 °C / ±1 %, tight enough that only a converged average passes. On mismatch the
push + wait cycle is repeated up to `MAX_SENSOR_RETRIES = 2` times; if it still mismatches the case
is recorded **INCONCLUSIVE** (it could not be judged), not FAIL. This handles transient Modbus or
network glitches without masking genuine firmware failures.

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
**object** keyed `M1`/`M2`/`M3` that also carries `M3_ctrl_mode`, `law`, `M3_ctrl_reason`,
`M3_pos_gate` and, while M3's position is trusted, `M3_percent_x10` and more. Only the three
states are read (`"UNKNOWN" | "CLOSED" | "MOVING_OPEN" | "OPEN" | "MOVING_CLOSE" | "PART_OPEN"`);
iterating the object would yield its key names. UT-CC-019 compares those three states, never the
whole object: M3's measured position moves by a few tenths with no drive at all.

### M3 on the timed law

Since 2.13.0 a unit with a position sensor defaults to Lineair (`ctrl_mode_m3 = 1`), where the law
is `graded`: M3 opens only from step 2, moves at most 25 % per drive and not within 10 min of its
last one. These cases pin `stepped`, so setup writes `ctrl_mode_m3 = 0` and waits until the status
shows `law "stepped v2"` and `M3_ctrl_mode "TIMED"`. The teardown restores the setting.

### Graduated ventilation algorithm

The firmware (`drivers/ventModel/src/vent_model_stepped.cpp`) uses:
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

### Humidity cases under stepped v2 (UT-CC-018, UT-CC-024)

With `t_max_day = 40 °C` the temperature asks for nothing, so humidity is the only voice, and since
2.15.0 that case is rule 5 of `vent_resolve_conflict()`:

- **It opens M1 only** — never M2 or M3, however far RH is above `rh_max` (RH=80 % gives
  step_rh 3).
- **Only under `cr_priority` 1 or 2.** Under 0 (temperature first, the default) humidity never
  opens a window on its own, which is why both cases set 1.
- **Only from `t_min + 2 °C`**: 17 °C here, so both cases push T = 20 °C. An opening already live
  holds down to `t_min + 1 °C`.

UT-CC-024 then pushes RH = 35 %, below `rh_min`, and M1 closes. Since 2.15.0 that close does not
depend on `rh_min`: a live humidity vote ends once the average is at or below
`rh_max − max(hyst_rh/3, 1)` = 68 % (the close guard), and a "too dry" vote changes no decision
(FRS FR-C08: `rh_min` is inert). M1 would close the same at RH = 55 %. The case stays as the check
that a humidity-only opening ends.

Before 2.15.0 both cases expected RH to open all three windows (priority 1 was "RH takes
priority").

### Day / night setpoint forcing (UT-CC-028/029)

`T4` recomputes `is_daytime` from the location as soon as `lat_deg`/`lon_deg` are written
(`update_sun_times()` inside `apply_config_update()`), and about once a minute from its clock. The
script writes a location and keeps the first one the status confirms:
- **Night:** 1° N at the longitude where it is solar midnight right now. T4 clamps sunrise and
  sunset to the UTC day rather than wrapping them, so this stays night for at least five hours at
  any date and hour. (Latitude 1, never 0: 0/0 means "no location" and reads as day, FR-DN05.)
- **Day:** the pole that is in polar day — 89° N from the March equinox to the September one,
  89° S the rest of the year — or, within about a day of an equinox when neither pole is, 1° N at
  the longitude where it is solar noon.

The old fixed 89° N (day) / 89° S (night) was right from March to September only: on 2026-09-27
89° N had a 2.7-hour day around 12:00 UTC.

The discriminator temperature for UT-CC-029 is 14 °C:
- Day  (`t_max_day = 25`): 14 < 25 → step 0 → windows stay **CLOSED**
- Night (`t_max_ngt = 12`): 14 > 12 → step 1 → windows would **OPEN**

Windows remaining CLOSED at T = 14 °C with `is_daytime = true` proves the
firmware selects the day setpoint, not the night setpoint.

### Note on t_min_day (UT-CC-017)

The temperature step does **not** evaluate `t_min_day`: it uses only `t_max` and `hyst_t`.
UT-CC-017 is satisfied because whenever `T < t_min_day`, `T` is also below the
hysteresis close threshold (`t_max_day − hyst_t = 19 °C` for the test
configuration), so windows will close via the normal hysteresis guard (FRS FR-C07 holds whenever
`t_max − hyst_t ≥ t_min`). The case pushes 14 °C against `t_min_day = 15 °C`, the test plan's
values; until 2026-09-27 it pushed 10 °C against 5 °C, so it never had T below `t_min` at all.

Since 2.15.0 `t_min` does have a job: it is the base of the humidity floor (see above), which is
why UT-CC-018/024 set it too.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All 12 tests passed |
| `1` | One or more tests failed |
| `2` | Could not run or could not judge: no unit or production found, firmware before 2.15.0, the emulator overwritten, setup preconditions not met, or a case INCONCLUSIVE |

---

## Log File Format

```
2026-09-27 12:00:00  INFO   Device   : http://192.168.20.160
2026-09-27 12:00:00  INFO   Unit     : 2344  fw 2.15.0  law graded v2  M3 LINEAR
...
2026-09-27 12:03:00  INFO   SETUP complete - AUTOMATIC, law stepped v2, M3 TIMED, M1=CLOSED M2=CLOSED M3=CLOSED
2026-09-27 12:03:00  INFO   --- UT-CC-014: OPEN when T > T_max_day ---
2026-09-27 12:03:00  INFO     Pushed T=26 C RH=55 % - waiting 35 s for a poll ...
2026-09-27 12:03:35  INFO     Unit reads T 18.0 C (pushed 26)
2026-09-27 12:03:35  INFO     Not converged yet (attempt 1); pushing again ...
2026-09-27 12:04:10  INFO     Sensor confirmed: T_avg=26.0 C RH_avg=55 %
2026-09-27 12:05:03  INFO     Windows: M1=OPEN M2=CLOSED M3=CLOSED
2026-09-27 12:05:03  INFO   [UT-CC-014] PASS - T=26 C > t_max=25 C -> M1 opened: M1=OPEN M2=CLOSED M3=CLOSED
...
2026-09-27 12:36:40  INFO   TEARDOWN: read-back matches the start for all 27 settings
2026-09-27 12:36:42  INFO   ============================================================
2026-09-27 12:36:42  INFO   SUMMARY: 12/12 passed, 0 failed, 0 inconclusive
```
