# Closed-loop simulator

| Field | Value |
|---|---|
| Purpose | Verify control algorithms before they reach the greenhouse: the binary (stepped) law 5C88 runs today, and the linear M3 law (mode 2) once the new firmware and hardware can drive it |
| Status | **2026-09-18, evening.** Both gates pass. **A two-node plant fitted on correctly timed data reproduces 5C88's limit cycle in closed loop.** Across 102 days, including held-out days, it gets the number of M3 openings, the cycle period, the time above 31 degC and the day-to-day pattern right. It does not reach the full swing (2.0-2.8 against 3.1 degC). The adopted single-node plant does not reproduce the cycle |
| Branch | `modelWorkWireSensor` |
| Inputs | Files in the repo only: 5C88's SD logs, the LoRa exports and the plant artifacts in `../campaign-summer-2026/`, and `drivers/ventModel/` |

## Two errors, and what they had done

Everything below is on corrected data. **Two errors made every earlier summer-long result wrong**, and anyone reading older notes should know which ones.

1. **The LoRa database is stamped in UTC; the SD logs are local time.** Every merge that joined them unconverted paired each indoor sample with the outdoor weather and door state of two hours later. That covers every `calibration_input_*.csv`, and so the whole summer-2026 calibration. Proven two ways, and converted since: see [`../lora_time.py`](../lora_time.py). `dataset.py` and `prepare_calibration_input.py` both convert now.
   - **On correctly timed data the adopted single-node plant scores 4.07 degC** held out, against 2.24 on the shifted data it was fitted to.
   - **Campaign F3 reverses.** At the median daytime M3 opening the sun is still rising, so the drop after M3 opens is M3's own effect.
   - **The Jul 11 temperature flush does not hold,** while the humidity flush does. Errata are in `campaignResults_summer2026.md` and `thermalProfileCampaign.md` §7.3.
2. **This simulator's own clock wrapped.** `_ms()` wrapped at 2^32 ms like the firmware's, but `firmware.Channel` compares times plainly where T2 compares them wrap-safely. At the first wrap, **2026-07-18 20:09**, every deadline set just before looked 49 days away, and M3 froze shut for the rest of every summer-long run. Fixed by keeping the clock unbounded; a fail-first check reproduces the deferral on the old clock. **The 102-day closed-loop figures before this fix were void after that date.** Runs that ended earlier, such as the first Jun 5 - Jul 11 reproduction, were not affected.

Also masked: **2026-07-09 06:06-09:48**, where 5C88's DS1307 (gh#37) stamped a block of rows 68 minutes early (`dataset.BAD_SD_WINDOWS`).

## What it is

| Part | File | What it is |
|---|---|---|
| Data | `logdata.py`, `dataset.py`, `../lora_time.py` | 5C88's SD logs, plus the raw LoRa exports (outdoor `lht65-20`, doors `lds01-5`/`-6`) converted from UTC, aligned per SD sample. Window positions come from the RELAY rows' exact times; outdoor data is interpolated between uplinks |
| Plant, single node | `plant.py` | `calibrate_plant_dynamic.simulate()`, stepped 30 s at a time, with the adopted artifact `plant_calibrated_constrained_summer2026_freem3.json` |
| Plant, two nodes | `plant2.py`, `plant2_kernel.c` | A fast air node and a slow structure/soil node, plus humidity, with optional M3 wind-direction term. One C loop serves both the fit (a whole summer in about 20 ms) and the closed loop (one step at a time), so the simulated plant is exactly the fitted one |
| Fit | `refit.py` | Fits the two-node plant, with every 4th day held out; reports the M3 response test; `compare` sets artifacts side by side |
| Firmware chain | `firmware.py` | T5 `avg_push()`/`avg_get()` in float32 with `lroundf()`; T2's channel state machine (travel + 5 s, 2 s reversal gap, dwell deferring `SRC_T6` only, the gh#48 in-travel guard from 2.3.1); T6's caller (inhibit resets, day/night setpoints, narrowing before widening, a MODE row on change) |
| Control law | `ventmodel.py`, `ventmodel_ffi.cpp` | `drivers/ventModel/` itself, compiled from the firmware's own sources into `build/ventmodel.dll` and called through ctypes, with every struct field checked by name on load |
| Command line | `closed_loop.py` | The gates and the closed-loop reproduction |
| Campaign figures | `campaign_figures.py` | The figures `campaignResults_summer2026.md` and `thermalProfileCampaign.md` §9.12 quote that come from neither `refit.py` nor `closed_loop.py`: the forced tests, the event study, the hottest days, wind, windward M3-only minutes, the indoor LoRa sensors, a model-free wind check, and the plants' heat loss per ventilation step |

The law is **not** re-implemented in Python. `simulation.py` carries its own port of the stepped law, and a port drifts. The contract makes the library host-compilable so that one set of sources serves the firmware, the host tests and this simulator (`design/ventModelContract.md` §4). When `vent_model_graded.cpp` exists, it becomes available here by adding one row to `k_models` in the shim.

## Running it

```bash
python model/closedloop/closed_loop.py gate-plant
python model/closedloop/closed_loop.py gate-control
python model/closedloop/closed_loop.py reproduce --start 2026-06-05 --end 2026-09-16 --plant2 model/campaign-summer-2026/plant2/plant2_summer2026_Ca2.9.json
python model/closedloop/refit.py fit --fix Ca_MJ=2.9
python model/closedloop/refit.py compare model/campaign-summer-2026/plant2/*.json
python model/closedloop/campaign_figures.py
```

Needs Python 3.11 with numpy, scipy and matplotlib, and the Code::Blocks MinGW `g++` that `drivers/ventModel` already uses (or `VENTMODEL_CXX`). Both DLLs are rebuilt automatically when a source changes. `build/` holds only the DLLs, which `.gitignore` excludes. **A DLL loaded by a running process cannot be rebuilt on Windows**, so let a running fit finish before changing `plant2_kernel.c`, and build once before starting fits in parallel.

`reproduce` without `--plant2` runs the single-node artifact. Other options: `--rh-from-log`, `--csv`, `--plot`; for the single node, `--openness position` and `--calibrator-hold`. `refit.py fit` options: `--direction`, `--door1 mask`, `--horizon-min N`, `--event-weight L`, `--fix NAME=VALUE`.

## The gates

### gate-plant: the plant is the calibrator's

Stepping the adopted artifact over `calibration_input_2026-06-04_2026-07-04.csv` reproduces `calibrate_plant_constrained.simulate_c6()` to 1.5e-13 degC. It also reproduces the published validation figures exactly: T RMSE 1.19 degC, 95th pct 2.11, 55.7 % within +-1 degC, RH RMSE 6.3 %. **PASS.** This proves the equations are the calibrator's. It does not make the calibration right; see "Two errors" above.

### gate-control: the law in the firmware chain makes 5C88's decisions

Fed the logged sensor readings, the stepped law from the DLL, run through the emulated T5, T4 and T6:

| Logs | MODE rows | T-demand (first / window) | Whole row (first / window) |
|---|---|---|---|
| Jul 13-29, the baseline set | 381 | **97.1 %** / 97.6 % | 88.7 % / 92.4 % |
| All, Jun 4 - Sep 17 | 2 276 | - / 98.8 % | - / 92.0 % |

The baseline is `vent_step_replay.py`'s **96.8 % of 378** on the same logs (contract §5 item 3). **PASS.**

- "first" pairs a MODE row with the first cycle at or after its stamp, as the old replay does. "window" accepts any cycle in the minute after it. MODE rows are stamped with `dm_get_unix_time()`, a cache T4 refreshes about once a minute; against the RELAY clock, 944 rows belong to the first sample after the stamp and 792 to the second.
- The humidity misses sit at step boundaries: the log keeps whole percent, and the firmware averages 0.1 %. The thresholds are confirmed by the logged decisions (60 / 75 % by day, 80 % at night, 4 % steps).
- Settings, with their evidence, are in `firmware.py` (`SETTINGS_5C88`). The gh#48 guard arrived with the 2.3.1 BOOT on 2026-07-29 01:05:29.

## Data

Fetched 2026-09-18 into `../campaign-summer-2026/` with `fetch_lora_data.py` (read-only), all stamped in UTC:

| File | Rows | Note |
|---|---|---|
| `lht65_20_2026-06-04_2026-09-17.csv` | 14 670 | Outdoor. The longest gap is 11 h on 2026-08-27; samples whose bracketing uplinks are over 40 min apart are masked |
| `lds01_5_2026-06-01_2026-09-17.csv` | 100 | Door 1. **Its sensor last reported on 2026-08-16 (closed)**; after that the door is assumed shut |
| `lds01_6_2026-06-01_2026-09-17.csv` | 1 081 | Door 2 |
| `lht65_02_2026-06-01_2026-09-17.csv` | 15 145 | **Indoor**, mid-width at 1/4 of the length (operator, 2026-09-18). Air T/RH plus a soil probe |
| `lht65_03_2026-06-01_2026-09-17.csv` | 14 631 | **Indoor**, mid-width at 3/4 of the length. Same channels |

The dataset holds 296 678 samples, 2026-06-04 to 2026-09-17, after the 2026-07-09 mask. Window positions rebuilt from RELAY rows disagree with the logged bitmask on at most 0.125 % of settled samples.

**What the indoor LoRa sensors show.** They are placed on the centre line either side of the controller's FG6485A, which is in the exact centre of the house.
- **By day** they agree with it (median +0.4 and 0.0 degC).
- **At night** they read a steady 0.8-1.0 degC lower.
- **They show the M3 swing too,** at about half its amplitude and about 10 minutes later. After M3 opens they fall by up to 0.7-0.8 degC, where the centre falls 1.3; after it closes they rise by 0.6 degC, where the centre rises 1.3. Part of that difference may be their slower housing and 10-minute sampling.

The limit cycle is a house-wide phenomenon. The soil probes run near the outside temperature at midday and well above the air at night.

## The two-node plant

Every 4th calendar day is held out. The loss is MSE of the air node against T_in plus MSE of absolute humidity in g/m3. Differential evolution is seeded, and a bounded L-BFGS-B polish follows. Each variant is judged three ways: the open-loop error, the **M3 response test**, and the **closed loop** over 2026-06-05 to 2026-09-16 (102 days, 25 held out).

The M3 response test is the median change of T in the 5-25 minutes after each daytime M3 command, open loop: 290 openings and 276 closings, where model and log see the same weather.

| Plant (`plant2/…`) | Held-out T RMSE | M3 opens, dT at 25 min | M3 closes, dT at 15 min | M3 openings | Day-by-day r | Days with cycling | Cycle | Hours >= 31 degC | Swing |
|---|---|---|---|---|---|---|---|---|---|
| **Logged** | | **-2.3** | **+1.5** | **406** | | **74** | **46 min** | **237** | **3.1 degC** |
| Single node (adopted) | 4.07 | +0.5 | +0.2 | 184 | 0.52 | 13 | 113 min | 234 | 3.9 |
| `plant2_summer2026` (free run) | 1.63 | -1.8 | +1.4 | 409 | 0.76 | 68 | 43 min | 228 | 2.0 |
| `_dir` (M3 windward term) | **1.58** | -1.9 | +1.5 | 422 | 0.75 | 67 | 42 min | 220 | 2.0 |
| `_Ca2.9` (air node = the air alone) | 1.72 | **-2.2** | +1.9 | 363 | **0.78** | 62 | 42 min | 252 | **2.7** |
| `_Ca6` (air node 6 MJ/K) | 1.64 | -1.8 | +1.5 | 387 | 0.77 | 63 | 42 min | 223 | 2.0 |
| `_ev5` (M3 responses weighted x6) | 1.70 | -1.8 | +1.4 | 437 | 0.76 | 70 | 43 min | 227 | 2.0 |
| `_ev20` (weighted x21) | 1.82 | -1.8 | +1.3 | 434 | 0.77 | 68 | 44 min | 227 | 1.9 |
| `_door1mask` | 1.70 | -1.8 | +1.4 | 384 | 0.76 | 67 | 43 min | 232 | 2.0 |
| `_h30` (30-min predictions) | 1.91 | -1.4 | +1.0 | 385 | 0.78 | 64 | 49 min | 224 | 1.8 |
| `_h60` (60-min predictions) | 1.85 | -1.3 | +0.9 | 354 | 0.79 | 63 | 46 min | 226 | 1.7 |

**Held out**, where the fit never saw the day:
- `_Ca2.9`: 101 M3 openings against 103 logged, 80 against 72 hours at or above 31 degC, cycling on 16 against 17 days, a 44 against 45 min cycle, a swing of 2.8 against 3.6 degC.
- `_dir`: 128 openings, 72 against 72 hours at or above 31 degC.

**By period** (`_dir`), M3 openings logged / simulated: 239 / 264 before the gh#48 firmware, 87 / 90 with it, and 80 / 68 after door 1's sensor went silent. So the firmware change is modelled right, and the weakest stretch is the one where the door is assumed.

**What it shows:**
- **The two-node plant, fitted on correctly timed data, reproduces how the binary law behaves:** how often M3 opens, the ~45-minute cycle, the time above the M3 threshold, and which days cycle.
- **The swing is still short.** 2.0 degC for most variants, 2.7 with the air node held at the air alone, against 3.1 logged.
- **Humidity is fitted much better than before:** AH error 1.2 against 2.4 g/m3.
- **The parameters are physically plausible.** For `_Ca2.9`: cover conduction about 2.5 kW/K; infiltration 1 /h; each roof window 1.3 /h; M3 4.8 /h. At 30 klux, 48 kW of sun into the air. Transpiration is 7.5 kg/h plus 8.6 kg/h per 10 klux.
- **The windward term** (`_dir`: 9.5 /h + 16.3 /h x cos of the windward angle) says M3 ventilates about 2.7x more with wind straight onto its wall. That is the direction effect in the configuration M3 actually runs in, and far from the 30-100x once claimed for M3 alone.
- **The free-run error alone cannot pick the dynamics.** Short-horizon objectives learn persistence (the 30-minute fit's 30-minute predictions: 0.75 against persistence's 0.96 degC) and give the weakest M3 response. The response test and the closed loop are what tell the variants apart.

## Which plant to verify a law against

- **`plant2_summer2026_Ca2.9.json` as the primary.** It is physically anchored (the air node is the air), responds to M3 by the logged amount, reproduces the held-out cycling closely, and has the largest swing.
- **`plant2_summer2026_dir.json` as the second.** It has the best open-loop fit and carries wind direction, which a direction-aware law needs.

Verify a new law against both, and treat a verdict that differs between them as unsettled. **Do not use the single-node artifact for this.**

## What is next

1. **The swing gap** (2.0-2.8 against 3.1 degC). The sensor's own lag and the ~2-minute timestamp noise before 2.1.3 are candidates to test before any structural change. The indoor soil probes could become a measured input for the slow node.
2. **The campaign documents are revised** (2026-09-18): `campaignResults_summer2026.md`, and `thermalProfileCampaign.md` §9.12 with banners on §9.5-9.11. NS-9 is open again: the fits disagree on the direction term, and a crude model-free check finds west wind, not south-west, the worst (§9.12.6).
3. **Linear M3 (mode 2)** still needs a part-open aperture curve, which only the new firmware and hardware can measure. Until then, vary it across a range (`plant.py`/`plant2.py` openness) and check the verdict holds across it.

## Known limits

- T3 is not simulated: a reproduction takes the logged wind-override bit, which is exact because the plant cannot change the wind. A run on invented weather needs a T3 model.
- Door 1 after 2026-08-16 is an assumption (above).
- A boot inside the loop is simplified: T5 and T6 reset, and T2 closes all windows.
- An SD gap longer than 5 minutes restarts the plant from the measurement.
- Windows are driven by the simulated controller except during LCD admin sessions, when the operator had them and the logged RELAY rows are replayed.
