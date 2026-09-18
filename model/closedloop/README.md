# Closed-loop simulator

| Field | Value |
|---|---|
| Purpose | Verify control algorithms before they reach the greenhouse: the binary (stepped) law 5C88 runs today, and the linear M3 law (mode 2) once the new firmware and hardware can drive it |
| Status | **Built 2026-09-18.** Both gates pass. The first closed-loop reproduction shows the calibrated plant **cannot yet reproduce the binary law's limit cycle** (below), so it is not yet fit to judge a new law |
| Branch | `modelWorkWireSensor` |
| Inputs | Files already in the repo only: 5C88's SD logs and the merged calibration input in `../campaign-summer-2026/`, the adopted plant artifact, and `drivers/ventModel/` |

## What it is

Three parts, each a faithful copy of something that already exists, joined into a loop:

| Part | File | Mirrors |
|---|---|---|
| Plant | `plant.py` | `calibrate_plant_dynamic.simulate()`, stepped 30 s at a time, with the adopted artifact `plant_calibrated_constrained_summer2026_freem3.json` |
| Firmware chain | `firmware.py` | T5 `avg_push()`/`avg_get()` in float32 with `lroundf()`; T2's channel state machine (travel + 5 s, 2 s reversal gap, dwell deferring `SRC_T6` only, the gh#48 in-travel guard from 2.3.1); T6's caller (inhibit resets, day/night setpoints, narrowing before widening, a MODE row on change) |
| Control law | `ventmodel.py`, `ventmodel_ffi.cpp` | `drivers/ventModel/` itself, compiled from the firmware's own sources into `build/ventmodel.dll` and called through ctypes |

The law is **not** re-implemented in Python. `simulation.py` carries its own port of the stepped law, and a port drifts; the contract makes the library host-compilable so that one set of sources serves the firmware, the host tests and this simulator (`design/ventModelContract.md` §4). When `vent_model_graded.cpp` exists, it becomes available here by adding one row to `k_models` in the shim.

`logdata.py` reads the logs. `closed_loop.py` is the command line.

## Running it

```bash
python model/closedloop/closed_loop.py gate-plant
python model/closedloop/closed_loop.py gate-control
python model/closedloop/closed_loop.py reproduce --start 2026-06-05 --end 2026-07-11 --plot run.png
```

Needs Python 3.11 with numpy, scipy and matplotlib, and the Code::Blocks MinGW `g++` that `drivers/ventModel` already uses (or `VENTMODEL_CXX`). The DLL is rebuilt automatically when a library source changes. `build/` holds only the DLL, which `.gitignore` excludes.

`reproduce` options worth knowing: `--openness position` (drive the plant with the leaf's travelled fraction instead of the calibrator's open-unless-closed convention), `--rh-from-log` (feed the controller the logged humidity, to separate the temperature loop from the plant's weak humidity model), `--calibrator-hold`, `--csv`.

## The gates

A simulator's verdict is only evidence if it first reproduces what the real system did, so each layer is checked before the loop is closed.

### gate-plant: the plant is the calibrator's

Stepping the adopted artifact over `calibration_input_2026-06-04_2026-07-04.csv`:

| | Result |
|---|---|
| Largest difference from `calibrate_plant_constrained.simulate_c6()` | 1.5e-13 degC, 4.2e-13 % RH |
| Validation week (the calibrator's own mask) | T RMSE **1.19** degC, 95th pct 2.11, 55.7 % within +-1 degC, RH RMSE 6.3 % |

These are exactly the published figures. **PASS.** Without the calibrator's hold on each window change (the closed loop's setting): 1.21 degC, 54.1 %, RH 6.4 %.

### gate-control: the law in the firmware chain makes 5C88's decisions

Fed the logged sensor readings, the stepped law from the DLL, run through the emulated T5, T4 and T6:

| Logs | MODE rows | T-demand (first / window) | Whole row (first / window) |
|---|---|---|---|
| Jul 13-29, the baseline set | 381 | **97.1 %** / 97.6 % | 88.7 % / 92.4 % |
| All, Jun 4 - Sep 17 | 2 276 | - / 98.8 % | - / 92.0 % |

The baseline for comparison is `vent_step_replay.py`'s **96.8 % of 378** on the same logs (`campaignResults_summer2026.md` F8, and the reference check in `design/ventModelContract.md` §5 item 3). **PASS.**

- "first" pairs a MODE row with the first cycle at or after its timestamp, as the old replay does. "window" accepts any cycle in the minute after it. MODE rows are stamped with `dm_get_unix_time()`, a cache T4 refreshes from the RTC about once a minute. Measured against the accurate RELAY clock over all logs, 944 rows belong to the first sample after the stamp and 792 to the second.
- The remaining whole-row misses are the humidity step at its boundaries. The log records RH in whole percent, but the firmware averages the sensor's 0.1 % values, so a reconstruction from the log cannot resolve the boundary. The thresholds themselves are confirmed by the logged decisions: RH votes to close below 60 % by day, and to open above 75 % by day and 80 % at night, with a 4 % step width (`hyst_rh` 12).
- Settings used, with the evidence for each, are in `firmware.py` (`SETTINGS_5C88`). The firmware timeline is from 5C88's OTA and boot rows: the gh#48 guard arrived with the 2.3.1 BOOT on 2026-07-29 01:05:29.

## First closed-loop result, 2026-09-18

`reproduce --start 2026-06-05 --end 2026-07-11`, the whole window the outdoor data covers:

| | Logged | Simulated |
|---|---|---|
| M3 openings | 159 | 65 |
| M3 open hours | 210 | 229 |
| Typical M3 cycle on cycling days | 41-48 min | rarely cycles |

**The calibrated plant does not reproduce the limit cycle.** The simulated greenhouse opens M3 and holds it open. The real one opens it, drops 4-5 degC within about 15 minutes, closes it, and climbs back as fast. On 2026-07-09 the logs show M3 opening 10 times between 11:15 and 18:27, swinging between about 26.5 and 32 degC. The simulation never opens M3 that day: it peaks at 30.4 degC at 15:03, below the 31 degC entry temperature, and its one opening is at 23:21, on humidity. The morning warm-up differs the same way: 5.2 degC/h logged against 2.0 degC/h simulated between 06:00 and 08:00.

The cause is the plant's time constant, not the controller: what the sensor sees moves roughly ten times faster than the fitted single-node model. This matches two earlier signs:

- the unconstrained fit wanted `c_eff` below the air-only floor, and the constraint held it there;
- an open-loop fit scored by RMSE rewards a smooth prediction when its input (outdoor lux every 10 min, doors not modelled) cannot place fast changes exactly.

Hot-day peaks and the hours above the M3 entry temperature match well, so the saturated regime is fine. Early June's cool days run up to 2 degC too cold.

A lead, and a noisy one. The swing per cycle is roughly the cooling rate with M3 open times the 25 min dwell (F7), so it measures M3's effect **in the configuration it actually runs in** (M1+M2+M3, doors as they were). The four days with the largest logged swings (3.9-4.7 degC) all had 54-82 % north wind while M3 was open. The two clearly westerly cycling days (9-11 %) swung about 2.2 degC. But 2026-07-10 (61 % north) swung 2.8 and 2026-06-23 (31 %) 3.8. If direction mattered in this configuration as much as it does with M3 alone and door 1 open (30-100x), these swings would differ far more than they do.

## What this means, and what is next

The model cannot yet verify a law whose purpose is to damp that cycle. The limit cycle is itself data, though: its period, swing and number of openings are set by how fast the plant responds to M3. It can be fitted in closed loop where the open-loop fit could not see it.

1. **Refit the dynamics against closed-loop behaviour.** Use a two-node plant (air plus structure; NS-9 step 3) or a re-parameterised single node without the air-only floor. Score on openings, cycle period, swing and time above the threshold. Hold out days to validate.
2. **Make M3's effect depend on wind direction** in the operating configuration, fitted the same way.
3. **Extend past 2026-07-12.** The outdoor and door data end there. A fresh export (`fetch_lora_data.py`, `prepare_calibration_input.py`) adds 2.3.1 days with the gh#48 guard, a second firmware profile to validate against.
4. **Then mode 2:** `--openness position`, an assumed M3 aperture curve varied across a range (see `plant.py`), and `vent_model_graded` once written.

## Known limits

- T3 is not simulated: a reproduction takes the logged wind-override bit, which is exact because the plant cannot change the wind. A run on invented weather needs a T3 model.
- Doors are not in the plant. They are reported per day in the `door` column.
- A boot inside the loop is simplified: T5 and T6 reset, and T2 closes all windows.
- The plant's humidity side is weak (AC-10 fails), so the humidity branch in closed loop is too. `--rh-from-log` isolates it.
- Windows are driven by the simulated controller except during LCD admin sessions, when the operator had them and the logged RELAY rows are replayed.
