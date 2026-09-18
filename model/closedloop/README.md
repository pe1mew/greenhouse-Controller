# Closed-loop simulator

| Field | Value |
|---|---|
| Purpose | Verify control algorithms before they reach the greenhouse: the binary (stepped) law 5C88 runs today, and the linear M3 law (mode 2) once the new firmware and hardware can drive it |
| Status | **2026-09-18.** Both gates pass. The adopted single-node plant reproduces 20 % of the logged M3 openings in closed loop. **A two-node refit on the whole summer raises that to 48-57 %, and still does not reproduce the limit cycle.** The simulator is not yet fit to judge a new law (see "The two-node refit") |
| Branch | `modelWorkWireSensor` |
| Inputs | Files in the repo only: 5C88's SD logs, the LoRa exports and the plant artifacts in `../campaign-summer-2026/`, and `drivers/ventModel/` |

## What it is

| Part | File | What it is |
|---|---|---|
| Data | `logdata.py`, `dataset.py` | 5C88's SD logs, plus the raw LoRa exports (outdoor `lht65-20`, doors `lds01-5`/`-6`), aligned per SD sample. Window positions come from the RELAY rows' exact times, and outdoor data is interpolated between uplinks |
| Plant, single node | `plant.py` | `calibrate_plant_dynamic.simulate()`, stepped 30 s at a time, with the adopted artifact `plant_calibrated_constrained_summer2026_freem3.json` |
| Plant, two nodes | `plant2.py`, `plant2_kernel.c` | A fast air node and a slow structure/soil node, plus humidity. One C loop serves both the fit (a whole summer in about 20 ms) and the closed loop (one step at a time), so the simulated plant is exactly the fitted one |
| Fit | `refit.py` | Fits the two-node plant; every 4th day held out |
| Firmware chain | `firmware.py` | T5 `avg_push()`/`avg_get()` in float32 with `lroundf()`; T2's channel state machine (travel + 5 s, 2 s reversal gap, dwell deferring `SRC_T6` only, the gh#48 in-travel guard from 2.3.1); T6's caller (inhibit resets, day/night setpoints, narrowing before widening, a MODE row on change) |
| Control law | `ventmodel.py`, `ventmodel_ffi.cpp` | `drivers/ventModel/` itself, compiled from the firmware's own sources into `build/ventmodel.dll` and called through ctypes, with every struct field checked by name on load |
| Command line | `closed_loop.py` | The gates and the closed-loop reproduction |

The law is **not** re-implemented in Python. `simulation.py` carries its own port of the stepped law, and a port drifts. The contract makes the library host-compilable so that one set of sources serves the firmware, the host tests and this simulator (`design/ventModelContract.md` §4). When `vent_model_graded.cpp` exists, it becomes available here by adding one row to `k_models` in the shim.

## Running it

```bash
python model/closedloop/closed_loop.py gate-plant
python model/closedloop/closed_loop.py gate-control
python model/closedloop/closed_loop.py reproduce --start 2026-06-05 --end 2026-09-16 --plant2 model/campaign-summer-2026/plant2/plant2_summer2026.json
python model/closedloop/refit.py fit
python model/closedloop/refit.py compare model/campaign-summer-2026/plant2/*.json
```

Needs Python 3.11 with numpy, scipy and matplotlib, and the Code::Blocks MinGW `g++` that `drivers/ventModel` already uses (or `VENTMODEL_CXX`). Both DLLs are rebuilt automatically when a source changes. `build/` holds only the DLLs, which `.gitignore` excludes. **A DLL loaded by a running process cannot be rebuilt on Windows**, so let a running fit finish before changing `plant2_kernel.c`.

`reproduce` without `--plant2` runs the single-node artifact. Other options: `--openness position` (single node: drive it by the leaf's travelled fraction), `--rh-from-log` (feed the controller the logged humidity), `--calibrator-hold`, `--csv`, `--plot`. `refit.py fit` options: `--direction` (M3's windward term), `--door1 mask`, `--horizon-min N`, `--event-weight L`, `--fix NAME=VALUE`.

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
- The remaining whole-row misses are the humidity step at its boundaries. The log records RH in whole percent, but the firmware averages the sensor's 0.1 % values. The thresholds themselves are confirmed by the logged decisions: RH votes to close below 60 % by day, and to open above 75 % by day and 80 % at night, with a 4 % step width (`hyst_rh` 12).
- Settings, with the evidence for each, are in `firmware.py` (`SETTINGS_5C88`). The firmware timeline is from 5C88's OTA and boot rows: the gh#48 guard arrived with the 2.3.1 BOOT on 2026-07-29 01:05:29.

## Data through 2026-09-17

Fetched 2026-09-18 with `fetch_lora_data.py` (read-only, the script's own defaults) into `../campaign-summer-2026/`:

| File | Rows | Note |
|---|---|---|
| `lht65_20_2026-06-04_2026-09-17.csv` | 14 670 | 34 gaps over 30 min, the longest 11 h on 2026-08-27. Samples with the bracketing uplinks over 40 min apart are marked stale and not fitted |
| `lds01_5_2026-06-01_2026-09-17.csv` | 100 | **Door 1's sensor last reported on 2026-08-16 07:56 ("closed", battery 2.82 V)** and is event-driven, so 31 % of the samples rely on the assumption that the door stayed shut. `refit.py fit --door1 mask` drops them instead |
| `lds01_6_2026-06-01_2026-09-17.csv` | 1 081 | |

`dataset.py` checks itself against the logs:
- **Window positions:** rebuilt from the RELAY rows, they disagree with the logged bitmask on 0.125 % (M1), 0.003 % (M2) and 0.000 % (M3) of settled samples.
- **Outdoor data:** interpolated between uplinks, it agrees with the old forward-filled merge on average (T_out +0.003 degC, lux +4).

The dataset holds 297 122 samples from 2026-06-04 to 2026-09-17, which includes the three September SD logs.

## First closed-loop result: the adopted single-node plant

**It cannot make the limit cycle.** Over 2026-06-05 to 2026-09-16 it opens M3 80 times against 406 logged, and cycles on 7 days against 74. The single node moves about ten times slower than what the sensor sees. The M3 response test below shows why: **it does not respond to M3 at all.** On 2026-07-09 the logs show M3 opening 10 times between 11:15 and 18:27, with swings between about 26.5 and 32 degC. The simulation opens it once in the daytime, at 14:27, when it peaks at 30.5 degC.

## The two-node refit

`refit.py` fits the two-node plant (`plant2_kernel.c`) to the whole summer, with the logged windows, doors and interpolated weather as inputs. Every 4th calendar day is held out. The loss is MSE of the air node against T_in plus MSE of absolute humidity in g/m3. Differential evolution is seeded, and a bounded L-BFGS-B polish follows.

Two tests besides the open-loop error:

- **The M3 response.** The median change of T in the 5/10/15/25 minutes after every daytime M3 command, open loop, compared with the logged T_in over the same 290 openings and 276 closings. Model and log see the same weather, so M3's habit of opening as the sun backs off (campaign F3) is in both sides.
- **The closed loop** over 2026-06-05 to 2026-09-16, 102 days, 25 of them held out.

| Plant (`plant2/…`) | Held-out T RMSE, free run | M3 opens: dT at 25 min | M3 closes: dT at 15 min | M3 openings | Days with cycling | Swing | h at or above 31 degC |
|---|---|---|---|---|---|---|---|
| **Logged** | | **-2.3** | **+1.5** | **406** | **74** | **3.1 degC** | **237** |
| Single node (adopted) | 2.24 | 0.0 | 0.0 | 80 | 7 | 2.1 | 325 |
| `plant2_summer2026` (free run) | 2.00 | -1.2 | +0.8 | 194 | 28 | 1.3 | 386 |
| `_door1mask` | 2.00 | -1.0 | +0.8 | 218 | 34 | 1.2 | 365 |
| `_dir` (M3 windward term) | 2.00 | -1.3 | +1.0 | 233 | 33 | 1.4 | 364 |
| `_h30` (30-min predictions) | 2.02 | -0.8 | +0.5 | 167 | 25 | 1.6 | 413 |
| `_h60` (60-min predictions) | 2.07 | -0.5 | +0.3 | 142 | 21 | 1.2 | 411 |
| `_ev5` (M3 responses weighted x6) | 2.00 | -1.2 | +0.7 | 209 | 28 | 1.3 | 363 |
| `_ev20` (weighted x21) | 2.07 | -1.3 | +0.8 | 220 | 31 | 1.5 | 346 |
| `_Ca6` (air node fixed 6 MJ/K) | 2.01 | -1.2 | +1.0 | 220 | 32 | 1.3 | 357 |
| `_Ca2.9` (air alone) | 2.03 | -1.2 | +0.9 | 202 | 31 | 1.2 | 367 |

Held-out days behave like fitted days in every row (see `reproduce`'s summary), so none of this is overfitting.

**What it shows:**

1. **Two nodes are a real improvement:** 2.5-3x more of the logged cycling, a lower error (2.00 against 2.24 degC held out) and a smaller bias. Humidity improves most (AH error 1.6 against 2.4 g/m3), and transpiration comes out at tens of kg/h against the single node's 1 kg/h.
2. **No variant reproduces the limit cycle.** At best 57 % of the openings, swings of 1.2-1.6 against 3.1 degC, and far too long above 31 degC. The M3 response caps near -1.2 degC at 25 min against the logged -2.3, whatever the objective or constraint.
3. **The free-run error cannot choose the dynamics.** It sits at 2.00-2.07 degC for air capacities from 2.9 to 30 MJ/K. Short-horizon objectives do worse: they learn persistence (30-min predictions 0.80 degC against persistence's 0.96) rather than the M3 response. Only the response test and the closed loop tell the variants apart.
4. **The capped response is structural, not an optimiser artefact.** Heavy weighting of the M3 windows does not lift it, and fixing the air capacity makes the response fast or large but never both. At the moment M3 opens, the models' air temperature is close to the logged one (median bias -0.25 to -0.58 degC), so the shortfall is in the response itself.
5. **The best candidate explanation is local exposure of the sensor.** If the controller's sensor sits in M3's inflow, opening M3 cools the sensor quickly and far more than the bulk. Closing it lets the sensor snap back to the bulk temperature (+1.5 degC in 15 min). A plant with one air node can reproduce a large fast swing at the sensor only by making the whole house too cool during long openings, and every fit settled on that compromise. The same reading fits the Jul 11 forced tests (humidity at the outdoor level within 5 min) and the fast re-heat after them.

Two smaller results:
- **The windward term** (`_dir`) adds 7.1 /h to a base of 20.2 /h when the wind blows straight onto M3's wall: about +35 %, far from the 30-100x measured with M3 alone and door 1 open. Its fit converged to a worse loss than the fit without the term, which contains it as a special case, so the value is indicative only.
- **Humidity is not what limits the closed loop.** Feeding the controller the logged RH gives 166 openings against 194 with simulated RH. The shortfall is in the daytime, temperature-driven openings: 288 logged against 107 simulated between 08:00 and 20:00, and 118 against 87 at night.

## What this means, and what is next

The simulator's control chain is verified, but no plant reproduces the loop it has to judge. Using any of these plants to rank control laws would compare them against a greenhouse that cycles about half as much as the real one, with half the swing.

1. **Measure where the swing happens.** Two or three extra indoor T/RH loggers, one mid-house and one at the far end from M3, are now the highest-value next measurement. They tell a bulk swing from a local one, and with them a plant can have a sensor zone that is fitted rather than guessed. They can go in on 5C88 now, with no firmware change. Record where the FG6485A hangs relative to M3 and door 1.
2. **Then a three-node plant** (sensor zone, bulk air, structure) fitted to all the loggers. A sensor-zone model can be tried on the current data too, but with one sensor its split between zone and bulk is a guess.
3. **Better solar input** is the other lever: the plant takes outdoor lux every 10 minutes and knows nothing of the sun's angle on the cover.
4. **Linear M3 (mode 2)** needs all of the above plus a part-open aperture curve, which only the new firmware and hardware can measure.

## Known limits

- T3 is not simulated: a reproduction takes the logged wind-override bit, which is exact because the plant cannot change the wind. A run on invented weather needs a T3 model.
- Doors are inputs to the two-node plant and absent from the single node. Door 1 after 2026-08-16 is an assumption (above).
- A boot inside the loop is simplified: T5 and T6 reset, and T2 closes all windows.
- An SD gap longer than 5 minutes restarts the plant from the measurement.
- Windows are driven by the simulated controller except during LCD admin sessions, when the operator had them and the logged RELAY rows are replayed.
