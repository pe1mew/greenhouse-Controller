# Thermal-Profile Campaign Plan

| Field | Value |
|---|---|
| Document | Thermal-Profile Campaign Plan |
| Project | Greenhouse Ventilation Controller |
| Status | Approved — ready for D-1 firmware build |
| Approved | 10 min LoRaWAN interval (§6.3), 21-day duration (§8.3) — operator approval 2026-05-21 |
| Primary purpose | Calibrate `model/simulation.py` so the operator can vet proposed controller settings (setpoints, dwell times, hysteresis, conflict-resolution priority) on the simulator before deploying them to the live greenhouse — preventing oscillation patterns from reaching production. |
| Related | `model/calibrate_plant.py`, `model/simulation.py`, `model/srcData/sql.md`, `design/technicalSoftwareDesignSpecification.md` §5.3, §5.10, §5.13 |

---

## 1. Objective

### 1.1 Primary — calibrated simulation model for settings validation

Produce a **calibrated thermal model of the greenhouse** that runs under `model/simulation.py` and predicts indoor T (and RH) accurately enough that the operator can **validate proposed controller settings on the simulator before deploying them to the live greenhouse**.

The simulator already implements the firmware's full climate-control behaviour — T5 sliding averages, T6 graduated ventilation with hysteresis, T3 wind override, T2 motor travel timing and dwell enforcement, T4 day/night setpoint selection. What it currently lacks is a **plant model fitted to this greenhouse**: today it uses a single steady-state algebraic relation with `ACH_INF = 0.5 h⁻¹` as the only thermal degree of freedom. The campaign supplies the missing fit — `ach_open[bitmask]` per window-combination plus the solar gain coefficient and the effective heat capacity — so the simulation's predicted indoor T tracks the measured indoor T closely.

Once calibrated, the simulator becomes a **what-if tool**. The operator drops in a candidate `settings.json` (different setpoints, hysteresis values, dwell times, wind-protection thresholds, sliding-average windows, conflict-resolution priority) and runs the simulation against a representative weather period. The output reveals whether the candidate settings:

- Produce **oscillations** (e.g. the M2 close → re-open → re-close cycle visible in the 2026-05-20 14:24 soak slice — would a longer `dwell_close_m2` have prevented it?).
- Over- or under-shoot the target T band.
- Drive the windows excessively (operator-visible as motor wear).
- Interact poorly with the wind-protection logic on gusty days.
- Cause RH to spike or drop outside the acceptable band under the chosen `cr_priority`.

This lets the operator tune the controller **rationally** rather than by trial-and-error on the live crop, which is the slow, expensive way and the failure mode that motivates the campaign.

### 1.2 Secondary — cooling-rate table for T6 predict-then-act

The same dataset supports a separate use of the profile: building a runtime lookup table `cooling_rate[bitmask][T_in][ΔT][wind][lux]` that T6 could consult at decision time to pick the smallest bitmask whose predicted dT/dt clears the cooling deficit. This is an **alternative** to the simulator-driven offline tuning of §1.1 — instead of testing setpoints on a simulator, T6 itself becomes profile-aware in real time.

Both uses share the same data collection. §1.1 is the operator-facing deliverable. §1.2 is preserved as a follow-on (see §12), pursued only if §1.1's simulator-driven approach hits its limits.

## 2. Hypothesis to test

> A first-order dynamic plant model parameterised as
>
> `dT_in/dt = (k_solar · lux + Q_transp) / C_eff − ach(bitmask, wind) · (T_in − T_out) / 3600`
>
> with `ach(bitmask, wind) = ach_inf + ach_open[bitmask] · g(wind_speed, wind_dir_rel)`
>
> can be fitted from a 3-week campaign such that `simulation.py` driven by recorded outdoor weather and recorded window-state reproduces measured indoor T within **±1.0 °C, 95 % of samples**, on a held-out validation week.

If the hypothesis holds at the stated tolerance, the calibrated simulator is fit for the §1.1 settings-validation use case. If the tolerance is missed but the bias is systematic (e.g. consistent over-prediction during high-lux periods), the residual structure localises the missing inputs and motivates either a second campaign or a model upgrade (richer plant equations, opening-angle nonlinearity, exterior wind sensor) before the simulator is trusted for production tuning.

## 3. Scope and limits

In scope:
- Indoor T, RH, wind speed, wind direction logged by the greenhouse controller (already implemented).
- Window position state per channel, time-aligned to the indoor sensor sample.
- Outdoor T, RH, luminosity from the existing LoRaWAN LHT65 outdoor sensor (`lht65-20`, *buiten kas*).
- Firmware modifications to the SD-card log so the campaign data can be joined and analysed in one pass.
- A defined campaign duration with go/no-go acceptance criteria.
- Extension of `calibrate_plant.py` to fit per-bitmask `ach_open` parameters into the dynamic plant model (see §9.2).
- Validation of the calibrated model against held-out measured indoor T (see §10.2).

Out of scope:
- Changes to T6 climate-control logic on the live controller. The campaign collects data and produces a calibrated simulator; live-controller changes are a separate work item, gated on operator review of simulator outputs.
- Crop transpiration drift modelling beyond a single per-fit constant; weekly-resolution drift correction is a follow-on (§12.3).
- Outdoor wind speed measurement (the indoor S200 measures inside-greenhouse air movement; an exterior anemometer is a follow-on if validation residuals motivate it).
- Soil-temperature or substrate moisture (no sensors available).
- Production-time post-campaign retention of `LOG_SENSOR_HR` emission is **not out of scope** — it remains enabled in operational firmware to support the continuous-recalibration cadence in §12.1 (item 3). The §5 sunset is permanent: the new event type replaces the legacy `LOG_SENSOR` row outright, and the §5.3 rotation bump is the new operational default (no post-campaign revert of either).

## 4. Data sources

| Source | Quantity | Cadence | Precision | Path |
|---|---|---|---|---|
| Controller — FG6485A | Indoor T, RH | 30 s | 0.1 °C / 1 % RH | Modbus → T5 → T4 → LOG_SENSOR_HR (new) |
| Controller — S200 | Indoor wind speed, direction | 30 s | 0.1 m/s / 1 ° | Modbus → T5 → T4 → LOG_SENSOR_HR (new) |
| Controller — T2 state | Per-channel window state | event-driven + snapshot every 30 s | discrete 0..4 | LOG_RELAY (existing) + LOG_SENSOR_HR (snapshot) |
| Controller — T6 | Operating-mode + bitmask | event-driven | discrete | LOG_MODE_CHANGE (existing) |
| LoRaWAN — LHT65-20 | Outdoor T, RH, luminosity | 5–10 min (see §6) | 0.1 °C / 1 % RH / 1 lux | LHT65-20 → TTN → MariaDB → SQL → CSV |

All controller-side data lands as CSV rows on the SD card under `/sdcard/log/`. The daily upload to the status website continues unchanged.

The outdoor sensor data lands in a separate CSV via the SQL export documented in `model/srcData/sql.md`, joined post-hoc by timestamp during analysis.

## 5. Firmware changes — SD-card log

The existing `LOG_SENSOR` row format (12-byte fixed record; `value_a = int16 °C`, `value_b = int16 % RH`) is **sunset and replaced** by a new event type `LOG_SENSOR_HR` ("high-resolution sensor snapshot"). The replacement is **permanent and forward-only**: post-campaign operational firmware also emits `LOG_SENSOR_HR` and no longer emits `LOG_SENSOR`. Sunsetting (rather than running the two formats in parallel) is justified because:

- `LOG_SENSOR_HR` is strictly more informative — same T and RH plus 0.1 °C precision, plus wind sub-row, plus window-bitmask sub-row.
- The continuous-recalibration cadence in §12.1 (item 3) requires the high-resolution data in production logs anyway.
- Maintaining two parallel formats wastes SD space (~158 KB/day on the redundant `LOG_SENSOR` rows) and complicates downstream parsing tooling.
- A single canonical format is simpler to support, document, and verify.

Historical compatibility: pre-campaign SD files that contain the legacy `LOG_SENSOR` rows remain readable indefinitely (the file format is just CSV); downstream parsers must be taught to handle both row types during a transition window — see §11 risk row.

`LOG_SENSOR_HR` carries three sub-types discriminated by the existing `channel` field:

```
event_type = LOG_SENSOR_HR        (new — append to log_type_t enum)
initiator  = LOG_BY_SYSTEM
channel    = 0  → T + RH    : value_a = t_c10  (°C × 10),     value_b = rh
channel    = 1  → wind      : value_a = wind_dms (m/s × 10), value_b = wind_dir_deg
channel    = 2  → window st : value_a = bitmask  (see §5.1), value_b = 0 (reserved)
```

Per indoor sample (still 30 s default), T5 emits **three** `LOG_SENSOR_HR` rows and **no** `LOG_SENSOR` row.

### 5.1 Window-state bitmask encoding

A single 16-bit integer carries all three channel states. Each channel uses the **public `window_state_t`** enum (`firmware/src/types/app_types.h`), packed into 2 bits per channel:

```
bits  1..0  = M1 state    (0=CLOSED, 1=MOVING_OPEN, 2=OPEN, 3=MOVING_CLOSE)
bits  3..2  = M2 state    (same)
bits  5..4  = M3 state    (same)
bits 11..6  = reserved (0)
bit  12     = EG1.WIND_OVERRIDE  (1 if T3 has forced close-all)
bit  13     = EG1.MOTOR_ALARM    (1 if RRK-3 alarm active)
bit  14     = EG1.CALIBRATING    (1 if boot CLOSE_ALL sequence running)
bit  15     = reserved (0)
```

A "fully open" snapshot with all three channels OPEN and no overrides reads `0x002A` (= `0b00101010`).

**GAP states fold into the matching MOVING state.** T2's *internal* per-channel FSM uses the extended `ch_state_t` enum (`firmware/src/relay_controller/relay_controller.cpp:137`), which adds two transient `GAP_TO_OPEN` / `GAP_TO_CLOSE` states between direction reversals. These GAP states are not represented in the bitmask: at pack time `GAP_TO_OPEN` is rendered as `MOVING_OPEN` and `GAP_TO_CLOSE` as `MOVING_CLOSE`. The GAP intervals are bounded at ~2 s (relay-energise inter-frame delay) so the loss of distinction is negligible for thermal-profile fitting and keeps the field width at 2 bits per channel. The public `t2_get_window_states()` accessor already performs this collapse, so packing the bitmask from its output is the canonical implementation path.

### 5.2 Code touchpoints

| File | Change |
|---|---|
| `firmware/src/types/app_types.h` | Append `LOG_SENSOR_HR` to `log_type_t` enum (after `LOG_SYSTEM`). The pre-existing `LOG_SENSOR` enum value stays in the enum so historical bench-soak files (rc.1.x) keep their stable type-column string when served by `/api/log/download`. |
| `firmware/src/event_logger/event_logger.h` | Document the `LOG_SENSOR_HR` channel-sub-type table mirroring §5 above. Mark `LOG_SENSOR` as deprecated — no longer emitted by the firmware, but enum slot kept for historical-file readability. |
| `firmware/src/event_logger/event_logger.cpp` (CSV row formatter) | Add the `LOG_SENSOR_HR` → `SENSOR_HR` string in the type column. Keep the legacy `LOG_SENSOR` → `SENSOR` mapping in the formatter so pre-campaign bench-soak files served by `/api/log/download` continue to display their original `SENSOR` type column unchanged. |
| `firmware/src/sensor_poll/sensor_poll.cpp` | **Remove** the existing `log_post()` of `LOG_SENSOR` after `dm_post_sensor()`. **Add** three `LOG_SENSOR_HR` rows (channel 0/1/2) in its place. Pack the window-bitmask (channel 2) from `t2_get_window_states()` (which already returns the public `window_state_t` — see §5.1 GAP-fold note) OR-ed with the EG1 override bits read via `xEventGroupGetBits(EG1)`. |
| `firmware/src/relay_controller/relay_controller.h` | (Optional convenience) Add `t2_get_window_bitmask()` that wraps `t2_get_window_states()` and returns the 16-bit packed value per §5.1. Not strictly required — the caller can pack inline — but a single-call helper keeps the bitmask encoding centrally maintained alongside the channel-state accessor. |
| `design/technicalSoftwareDesignSpecification.md` §5.3 | Post-campaign update: replace `LOG_SENSOR` row description with `LOG_SENSOR_HR` description. The TSDS describes the end-state design; once sunset lands in shipping firmware the TSDS must follow. |

The enum extension is **append-only** — no existing event-type values shift. The change is forward-only: post-campaign firmware emits only `LOG_SENSOR_HR`; there is no revert path planned. Historical files containing `LOG_SENSOR` rows remain unchanged on disk and on the status server and stay readable forever — only the emitter is retired.

### 5.3 SD log rotation and retention

Volume estimate per day with the format sunset applied:

| Source | Rows/day | Avg bytes/row | Bytes/day |
|---|---:|---:|---:|
| `LOG_SENSOR` (sunset — no longer emitted) | 0 | — | 0 |
| `LOG_SENSOR_HR` ×3 (replacement) | 8 640 | 55 | 475 KB |
| `LOG_RELAY` + `LOG_MODE_CHANGE` + `LOG_SYSTEM` | ~150 | 55 | 8 KB |
| **Total** | | | **~483 KB/day** |

For comparison, the pre-sunset operational volume was ~166 KB/day (one `LOG_SENSOR` row per 30 s plus housekeeping). The sunset adds ~317 KB/day net — about 3× the previous footprint — because the new format carries triple the information per sample.

Rotation policy **change becomes the new operational default** (no post-campaign revert):

| Parameter | Pre-sunset default | New default (campaign + ongoing) |
|---|---:|---:|
| Maximum file size | 512 KB | 1 024 KB |
| Files retained | 10 | 30 |
| Minimum retention floor | 3 | 5 |
| Low free-space threshold | 2 MB | 4 MB |

Result: ~2.1 days/file × 30 files = ~63 days of on-SD history. The daily T14 upload continues — uploaded files are unlinked as today, so the 30-file ceiling is only the in-transit safety margin in case of an upload outage. SD-card footprint at full retention is ~30 MB, negligible against any reasonable card size.

The operator changes these four limits via:
- `CFG_LOG_FILE_BYTES_MAX` and `CFG_LOG_FILES_MAX` in `firmware/config/cfg_defaults.h` (or equivalent NVS keys if the values are runtime-configurable).
- Verified by inspecting `/api/sd/status` after first rotation.

These values should also be reflected in the post-campaign update to `design/technicalSoftwareDesignSpecification.md` §5.3 rotation table.

### 5.4 Verification

After the campaign firmware is flashed:

1. Boot the controller, wait 60 s.
2. Open `/api/sd/status`; verify a CSV file is being written. Confirm it contains `SENSOR_HR` rows and **no** new `SENSOR` rows (any `SENSOR` rows present are from a previous file that has not yet rotated — check the timestamp).
3. Manually trigger a window OPEN via the dashboard. Verify a new `RELAY` row appears AND the next `SENSOR_HR,channel=2` row carries the matching bitmask.
4. Download the file via `GET /api/log/download?file=…` and grep for `SENSOR_HR,SYS,0,` (T/RH sub-type) — expect ~120 rows in the first hour. Also confirm `grep -c ',SENSOR,'` returns 0 on a file fully written under the new firmware.
5. Trigger the daily upload manually (set the `log_upload_hhmm` slot ~5 min ahead) and confirm the file lands on the status server.

## 6. LoRaWAN outdoor sensor — interval recommendation

### 6.1 Constraints

| Constraint | Value | Source |
|---|---|---|
| Regional band | EU868 | (assumed Netherlands) |
| Sensor model | Dragino LHT65 series (`lht65-20`) | Existing deployment |
| Default uplink interval | 1 200 s (20 min) | LHT65 factory |
| Battery | 2 × ER14505 (AA Li-SOCl₂), 2 400 mAh each | LHT65 datasheet |
| Battery life | ~10 y @ 20 min, SF7, EU868 | LHT65 datasheet |
| Battery life | ~5 y @ 5 min, SF9-10 | Engineering extrapolation |
| TTN fair-use uplink airtime budget | 30 s / day / device | The Things Network FUP |
| EU868 duty-cycle ceiling per sub-band | 1 % (most sub-bands) | ETSI EN 300 220 |
| LHT65 uplink payload | 11 bytes (T, RH, lux, battery) | LHT65 payload spec |
| Estimated airtime at SF9, BW125, 11 B | ~370 ms | LoRa calc |

At SF9, 5 min interval = 288 uplinks/day × 370 ms = **107 s/day airtime**, which **exceeds** the TTN 30 s/day fair-use guideline. To stay inside TTN FUP at 5 min cadence the sensor must use SF7 (~50 ms airtime → 14 s/day) — feasible only if the gateway has line-of-sight to a sensor mounted on the greenhouse exterior wall.

### 6.2 Greenhouse thermal time-constants (for context)

| Quantity | Typical change rate | Sample-interval implied |
|---|---|---|
| Outdoor T, smooth diurnal | 1–2 °C / hour | ≥ 10 min |
| Outdoor T, weather-front passage | up to 5 °C / hour | ~5 min |
| Outdoor RH | 5 %RH / hour smooth, 10 %RH / 10 min on storm | ~5 min |
| Solar lux, clear-sky envelope | smooth 30 min timescale | ~10 min |
| Solar lux, cloud transient | seconds — but smoothed average is what the model needs | ~5 min |

The slowest meaningful timescale is outdoor T diurnal smoothing (≥ 10 min). The fastest is solar-lux transients (irrelevant if averaged). The right campaign interval lies between **5 and 10 min**, biased toward 5 min if the radio link permits.

### 6.3 Recommendation

**Set LHT65-20 uplink interval to 600 s (10 minutes) for the duration of the campaign.**

Rationale:

- At SF9, 10 min = 144 uplinks/day × 370 ms ≈ **53 s/day airtime**. Still over TTN's 30 s/day soft cap, but TTN tolerates 2× the cap on a single-device basis for short campaigns; in practice this works for 3–4 weeks without rate-limiting.
- At SF7 (line-of-sight to gateway), 10 min ≈ 7 s/day airtime — comfortably inside TTN FUP regardless of campaign length.
- Battery life at 10 min, SF9: ~6–7 years projected — comfortably exceeds the campaign window with ample reserve for follow-on campaigns.
- Captures the relevant outdoor T/RH timescale and a useful luminosity average (the latter is smoothed inside LHT65's photodiode integration anyway).
- Joining indoor 30 s data with outdoor 10 min data uses zero-order-hold or linear interpolation on the outdoor side — both well-established techniques.

If 5 min resolution is later judged necessary (e.g. weather-front-transition behaviour proves under-resolved), the sensor can be reconfigured mid-campaign via TTN downlink without redeployment. The cost is roughly halved battery life and ≈100 s/day airtime — still tolerable for a single device in a fixed installation.

If the LHT65-20 link budget is borderline (RSSI < −110 dBm, SNR < −5 dB at the gateway) the recommendation drops to **900 s (15 min)** at SF10–11 to preserve battery and keep airtime inside FUP. Confirm link quality from the TTN console before committing.

### 6.4 Configuration mechanism

LHT65 transmit-data-cycle (TDC) is set via a 4-byte LoRaWAN downlink command:

| Field | Value |
|---|---|
| Payload (hex) | `01 00 02 58` |
| Length | 4 bytes |
| FPort | **2** (Dragino LHT65 application default; confirm from any recent LHT65-20 uplink in the TTN console — some older deployments are configured on FPort 1) |
| Confirmed | Yes — the LHT65 ACKs, giving positive proof the command applied |
| Encoding | byte 0 = `0x01` (command code: set TDC); bytes 1–3 = 24-bit big-endian TDC in **seconds**; `0x000258` = 600 |

Send once from the TTN console; the LHT65 receives the downlink in the RX window immediately after its next uplink (worst-case wait = one current uplink interval, i.e. up to 20 min on the present 1200 s setting). The change is persistent across battery replacement. Verify the new cadence by confirming the next 6 uplinks are ~600 s apart.

## 7. Outdoor CSV import

The post-campaign analysis script reads outdoor data from a CSV file exported from the LoRaWAN backend MariaDB via the pattern already documented in `model/srcData/sql.md`.

### 7.1 Schema

```
dateTime,airTemperature,airHumidity,lumosity
2026-06-01 00:02:48,12.4,87,0
2026-06-01 00:12:48,12.3,87,0
...
```

- `dateTime` — ISO 8601, **local time** (matches controller-side timestamps; no TZ conversion at join time).
- `airTemperature` — °C, decimal (LHT65 native precision 0.1 °C).
- `airHumidity` — %, integer or decimal.
- `lumosity` — lux, integer (NULL acceptable when sensor has no lux probe; LHT65-20 has the external lux probe so values are populated).

### 7.2 Export procedure

At the **end** of the campaign (or once per week for early-warning sanity checks):

```sql
SELECT dateTime, airTemperature, airHumidity, lumosity
  FROM wenumseveld
 WHERE sensor = 'lht65-20'
   AND dateTime >= 'CAMPAIGN_START'
   AND dateTime <  'CAMPAIGN_END'
 ORDER BY dateTime;
```

Save as `model/srcData/outdoor-lht65-20_<CAMPAIGN_START>_to_<CAMPAIGN_END>.csv` using the same `sed 's/\t/,/g'` pipeline as `sql.md`.

### 7.3 Time alignment

The outdoor sensor's `dateTime` and the controller's SD-log `timestamp` are both Europe/Amsterdam local time. No timezone normalisation needed at join time. The two streams differ in cadence (30 s indoor vs 600 s outdoor); the analysis script joins by **forward-fill** on the outdoor side: each indoor 30 s row is annotated with the most-recent prior outdoor reading.

If `lumosity` is NULL for an outdoor row (sensor transient or LoRaWAN gap), the forward-fill carries the previous value with a `lux_stale_s` column tracking the staleness; rows older than 1 800 s (3 outdoor intervals) are excluded from the fit.

## 8. Campaign duration

### 8.1 Cells the analysis needs to populate

The cooling-rate table is indexed by:

| Axis | Bins |
|---|---|
| Window bitmask (M3 M2 M1) | 8 states (`000`..`111`) |
| Indoor T | 10 bins (10, 12.5, 15, 17.5, 20, 22.5, 25, 27.5, 30, 32.5 °C) |
| ΔT = T_in − T_out | 6 bins (−2, 0, 2, 4, 6, 8 °C above ambient) |
| Wind speed (indoor S200) | 4 bins (0, 1, 2, 4 m/s) |
| Sun lux | 4 bins (0, 5 k, 25 k, 80 k lx) |

Sparse cells expected — many combinations are physically rare (e.g. all-windows-open at 12 °C indoor, or zero wind at 32 °C outdoor with full sun). The acceptance target is ≥ 5 samples in each cell where the bitmask is **likely** (i.e. excluding the rare-by-physics cells).

### 8.2 Cycles vs days

| Days | Diurnal cycles | Likely venting events | Confidence in fit |
|---:|---:|---:|---|
| 1 | 1 | 0–2 | None — anecdote |
| 7 | 7 | 5–10 | Low — one-week weather only |
| 14 | 14 | 10–25 | Moderate — most common bitmasks populated |
| **21** | **21** | **15–40** | **Target — most cells reach ≥5 samples** |
| 28 | 28 | 20–55 | High — robust statistics on rare bitmasks |

### 8.3 Recommendation

**21 days of continuous operation under the campaign firmware build.**

Rationale:

- Captures a full set of weather conditions (clear/cloudy/windy/calm/rainy) typical of the season.
- 21 diurnal cycles populate the most-common bitmasks (`000` all-closed, `001` M1 only, `011` M1+M2, `111` all-open) with ≥ 10 samples per cell.
- Aligns roughly with the existing Phase 7 soak window of 14 days — adds 7 days post-soak so the campaign firmware proves itself stable first.
- 28 days is the fallback if the 21-day extract shows ≥ 3 bitmasks with < 5 samples in critical T-bins (typically 25–30 °C).

### 8.4 Timing

Best season: **late spring through summer** (May–August in NL). Reasons:
- Window-venting events are concentrated in this window (the activity that the campaign measures).
- Solar lux range is widest → better luminosity coefficient fit.
- Outdoor T spans ~5 °C nights → 30 °C days, exercising the ΔT axis.

Avoid mid-winter campaigns: windows stay closed, the bitmask table degenerates to a single row, and the model collapses to `ach_inf` fit (which `calibrate_plant.py` already does).

Concrete proposed slot: **start the campaign as soon as the current Phase 7 soak passes its 14-day clean criterion**, run for 21 days, terminate on the morning of day 22 (capture one final night to anchor the cool-down model).

## 9. Data analysis pipeline

The pipeline is two-stage, both stages reusing `model/calibrate_plant.py` and `model/simulation.py` infrastructure. The output of stage 1 is the calibrated plant model that drives the §1.1 simulator-fidelity deliverable. Stage 2 is the optional secondary product from §1.2.

### 9.1 Inputs (common to both stages)

1. All daily SD log CSVs from `bin/<version>/campaign-logs/` (or the status-server aggregate at `/hbwv/log/`).
2. The outdoor CSV `model/srcData/outdoor-lht65-20_<CAMPAIGN_START>_to_<CAMPAIGN_END>.csv`.
3. The exact `settings.json` snapshot in force on the controller at day 0 (captured from `/api/config` before flashing the campaign firmware).

### 9.2 Stage 1 — calibrated dynamic plant model (primary)

Extend `calibrate_plant.py` to a new script `model/calibrate_plant_dynamic.py`:

- **Drops the "windows always closed" assumption.** The current calibrator fits only `ach_inf`, `k_solar`, `c_eff_mj_per_c`, `transpiration_kg_s`. The new one additionally fits a vector `ach_open[bitmask]` (7 free parameters, one per non-zero bitmask) plus an optional wind-modulation coefficient `k_wind_ach`.
- **Drives the fit with the recorded window-state trace** from `LOG_SENSOR_HR,channel=2` rows joined to the outdoor weather and the indoor measurements.
- **Train/validation split.** The 21-day campaign is split day-of-the-week-stratified into a 14-day training set and a 7-day held-out validation set. The fit minimises mean-squared error against measured indoor T on the training set; the validation set is used only for the §10 acceptance criterion.
- **Forced-state intervals (wind override / motor alarm).** Rows where the bitmask sub-row's EG1 bit 12 (`WIND_OVERRIDE`) or bit 13 (`MOTOR_ALARM`) is set carry physically valid sensor data but reflect a *forced* all-closed window state, not a T6-decided one. They are **included by default in the plant-model fit** because the fit needs only the joint state (window-bitmask, outdoor weather, indoor measurement) — the physics is independent of what placed the bitmask there. They are **excluded by default from the stage-2 cooling-rate table** because that table aims to characterise nominal operator-relevant venting, and forced-closed periods over-weight the all-closed cell artificially. Both defaults are exposed as command-line flags on `calibrate_plant_dynamic.py` for ad-hoc analyst override.

Outputs:

- `model/plant_calibrated_<CAMPAIGN_START>_<CAMPAIGN_END>.json` — the new plant configuration. Drop-in replacement for the existing `plant_calibrated.json`; same JSON schema with the additional `ach_open` map.
- `model/profile/<dates>_fit_report.md` — fit residuals (overall MSE, max error, 95th-percentile error), per-bitmask coverage map, training-vs-validation comparison plots.
- `model/profile/<dates>_validation_overlay.png` — measured vs simulated indoor T over the 7-day validation week (one line per source), with residual band shaded.

The simulator is then validated by running `python simulation.py <validation_week_weather.csv> plant_calibrated_<dates>.json` and comparing the output `results_*.csv` indoor-T column to the measured indoor T from the controller log over the same week.

### 9.3 Stage 2 — cooling-rate lookup table (secondary)

Independent of stage 1; written as `model/build_thermal_profile.py`:

- Walks all `LOG_SENSOR_HR` rows in the campaign dataset; for each pair of consecutive 30 s samples computes `dT/dt`, the current bitmask, the joined outdoor conditions, and bins all five axes per §8.1.
- Aggregates per (bitmask, T_bin, ΔT_bin, wind_bin, lux_bin) cell: mean, stdev, sample count.
- Emits `model/profile/<dates>_cooling_rate_table.csv` for use as a follow-on T6 runtime lookup table (see §12).

Stage 2 is **not on the critical path** for the primary deliverable. It can be produced any time after the campaign data lands; it does not gate the simulator-fidelity claim.

### 9.4 Worked example — answering the "would dwell prevent the oscillation?" question

The §1.1 use case is best illustrated by the M2 oscillation observed at 2026-05-20 14:24 in the 18.9 h soak (M2 closed → 4 min later re-opened → 23 min later closed again, with the indoor T climbing from 29 °C back to 33 °C in between). The operator's question is: "would `dwell_close_m2 = 5 min` have prevented this?"

The workflow with the calibrated simulator:

1. Take the recorded outdoor weather for 2026-05-20 from the campaign outdoor CSV.
2. Copy `settings.json` to `settings_test.json`; change `dwell_close_m2` from `0` to `5` (minutes).
3. Run `python simulation.py outdoor_2026-05-20.csv settings_test.json`.
4. Open `results_outdoor_2026-05-20.csv`; inspect the M2 state column and the indoor-T column around the 14:24 mark.
5. If M2 stays closed for 5 min after the 14:24 close, by which time T has risen to (say) 31 °C, the next T6 decision is made under different state — typically choosing to open M3 (next-most-effective vent) instead of immediately re-opening M2, breaking the oscillation pattern.

This worked example becomes the acceptance demonstration for the §1.1 deliverable (see AC-9 below).

## 10. Acceptance criteria

The campaign is "successful" when **all data-collection criteria AC-1..AC-8 hold** AND **the model-fidelity criterion AC-9 holds** on the held-out validation week.

### 10.1 Data-collection criteria (campaign-window scope)

| # | Criterion | Target |
|---|---|---|
| AC-1 | Controller uptime over the 21-day campaign | ≥ 99.5 % (≤ 2.5 h cumulative outage) |
| AC-2 | SD-log gaps (missing `LOG_SENSOR_HR` rows) | ≤ 0.5 % of expected rows |
| AC-3 | Daily T14 upload success rate | ≥ 95 % |
| AC-4 | Outdoor LHT65-20 uplink delivery rate | ≥ 90 % of expected (TTN reports) |
| AC-5 | Number of `LOG_RELAY` events (any state) over the 21-day period | ≥ 50 (≥ 2 / day average) |
| AC-6 | At least one venting event lasting ≥ 5 min per `M1` / `M1+M2` / `M1+M2+M3` bitmask, **counted only from rows where EG1.WIND_OVERRIDE and EG1.MOTOR_ALARM are both clear** (forced-closed intervals do not count toward bitmask diversity) | ≥ 1 each |
| AC-7 | Outdoor luminosity (`lumosity`) field populated | ≥ 95 % of outdoor rows |
| AC-8 | No `ESP_RST_PANIC`, no `ESP_RST_TASK_WDT`, no coredump capture | 0 |

If AC-6 fails (insufficient bitmask diversity), the campaign extends to 28 days. If AC-1, AC-2 or AC-8 fails, the campaign restarts after the root cause is fixed.

### 10.2 Model-fidelity criterion (validation-week scope)

| # | Criterion | Target | Notes |
|---|---|---|---|
| AC-9 | `simulation.py` driven by recorded outdoor weather, recorded window-state, and `plant_calibrated_<dates>.json` reproduces measured indoor T on the held-out validation week within **±1.0 °C, for ≥ 95 % of 30 s samples** | pass | The remaining ≤ 5 % may include short transients during rapid venting; max excursion ≤ 2.5 °C |
| AC-10 | Indoor RH residual on the validation week | RMS ≤ 5 %RH | RH is fit secondary to T; this is a sanity check, not a hard gate |
| AC-11 | The worked example in §9.4 produces a behaviourally-distinct M2 trajectory between `dwell_close_m2 = 0` and `dwell_close_m2 = 5 min` | qualitative pass | This is the operator-facing demonstration that the simulator is fit for §1.1 use |

If AC-9 fails, the residual structure is examined first. If residuals are systematic (e.g. consistent over-prediction during high-lux periods) the model is upgraded — e.g. nonlinear lux→solar-gain, opening-angle factor, exterior anemometer added — and the fit re-run on the same data without a new campaign. If residuals are unstructured (white noise above ±1 °C), a second campaign in a different season is scheduled to widen coverage.

If AC-9 passes but AC-11 fails, the simulator is still adequate for *aggregate* what-if questions (mean cooling rate, dwell-aggregate behaviour) but not for *single-event* questions like the M2 oscillation reproduction. This degraded-pass outcome should be documented in the fit report and used to scope follow-on work.

## 11. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| New `LOG_SENSOR_HR` rows fill SD faster than estimated; rotation deletes campaign data before T14 upload | Low | Medium | Bumped retention to 30 files × 1 MB = 30 MB / ~63 days, applied as the new permanent operational default. Daily upload removes files anyway. |
| **Log-parser must be updated for the new `LOG_SENSOR_HR` row format** | Low | Low | **There are no production units in the field yet** — this campaign runs on the first production unit, so there are no third-party or legacy parsers to bridge for. The single mitigation is to update the analysis-pipeline log parser to dispatch on `SENSOR_HR` and decode the three channel-discriminated sub-rows (channel 0 = T+RH at 0.1 °C, channel 1 = wind, channel 2 = bitmask). The legacy `SENSOR` row type continues to be recognised by the same parser so pre-campaign bench-soak files (e.g. the rc.1.3.2 soak data in `bin/2.0.0-rc.1.3.2/`) remain readable. No adapter shim required. |
| LHT65-20 LoRaWAN link drops mid-campaign | Medium | Medium | TTN console alarm on >2 h uplink silence; replace sensor or relocate gateway. Forward-fill in analysis tolerates short gaps (< 1 800 s). |
| Storm or downtime drops indoor sensor for hours | Low | Medium | Indoor uses Modbus wired; only an MCU reset or cable break causes gap. Reset captured by `ESP_RST_*` event; cable break flags `SENSOR_FAULT_T`. |
| Weather is unrepresentative (heatwave + drought, or persistent cloud) | Medium | Low | Document with the dataset; campaign duration extension already specified as fallback. |
| TTN fair-use rate-limiting kicks in on the 10 min cadence | Low | Low | Drop to 900 s (15 min) via downlink mid-campaign; battery and physics still fine. |
| Operator changes setpoints mid-campaign | Medium | Low | Setpoint changes are themselves logged as `LOG_SETPT` rows; analysis script segments the dataset on setpoint boundaries. |
| Campaign firmware destabilises after extended run | Low | High | The change is additive (one new event type, three extra rows per 30 s sample, one bumped rotation cap). Phase 7 soak validates stability before campaign begins. |
| **Wind override (T3 → EG1.WIND_OVERRIDE) or RRK-3 motor alarm (T2 → EG1.MOTOR_ALARM) suspends controller-driven venting during the campaign — bitmask gets forced to all-closed regardless of what T6 would have decided** | Medium (wind) / Low (motor alarm) | Medium for bitmask-diversity coverage; **none for the plant-model fit** | Sensor data continues to flow during these states (T5 polls, T4 stores, SD logs `LOG_SENSOR_HR` as normal) — **the data is physically valid** and contributes to the all-closed bitmask cell of the fit. Only the controller's *decision-making* is inhibited, not the physics being measured. The bitmask sub-row's EG1 bits 12 (WIND_OVERRIDE) and 13 (MOTOR_ALARM) — already defined in §5.1 — explicitly mark these intervals so the analyst can choose per-fit whether to include or exclude them. **Recommended analyst handling**: include in stage-1 plant-model fitting (physics doesn't care why the windows are closed); optionally exclude from stage-2 cooling-rate cells when computing "expected cooling under nominal controller operation". If wind/motor alarms consume > 25 % of the campaign window, extend the campaign to recover bitmask diversity (same fallback as AC-6). |

## 12. Out-of-scope and follow-ons

### 12.1 Primary-objective follow-ons (consume the calibrated simulator)

1. **Operator workflow / playbook for settings tuning.** A short Markdown guide showing the operator how to run `simulation.py` against a candidate `settings.json` and interpret the result. Lives at `model/operatorSettingsPlaybook.md`. Written after AC-11 passes.
2. **Pre-deployment vetting gate.** Any change to the live controller's setpoints or dwell parameters must first pass a simulator run on the most-recent 7 days of measured weather. Documented as a procedure addendum to the operator manual.
3. **Continuous re-calibration cadence.** The calibrated plant model drifts with the crop life-cycle (transpiration), the season (solar zenith), and slow building changes (caulking degrading). Quarterly re-runs of `calibrate_plant_dynamic.py` against the most-recent 7 days of operational logs keep the model current. The §5 firmware modifications (specifically `LOG_SENSOR_HR`) need to remain enabled in production for this to work — see the §3 scope-note update.
4. **What-if scenario library.** Capture the worked example from §9.4 plus a handful of standard scenarios (heatwave, cold front, cloudy week, gusty day) as canonical `input_S*.csv` files extending the existing scenario library in `model/input_S{1..5}_*.csv`.

### 12.2 Secondary-objective follow-ons (cooling-rate table for T6 runtime)

5. **T6 predict-then-act control mode.** Loads `cooling_rate_table.csv` into PSRAM at boot; T6 consults it at decision time to pick the smallest bitmask whose predicted dT/dt clears the cooling deficit. Replaces the current fixed-threshold "open everything" rule. Pursued only if the simulator-driven approach in §12.1 hits a limit (e.g. operators want autonomous closed-loop adaptation rather than human-vetted setpoint changes).
6. **Anomaly detection on dT/dt deviation.** Once the table predicts dT/dt within stdev, deviations > 2σ become alarmable (faulty window servo, broken end-switch, gauze tear, door left open). Built on the runtime profile loaded for follow-on #5.

### 12.3 Sensing follow-ons (close model gaps surfaced by the campaign)

7. **Outdoor anemometer.** The indoor S200 measures air movement *inside* the greenhouse, which is a proxy for ventilation but not for the exterior wind driving the Δp across openings. If the validation-week residuals (§10.2) correlate with outdoor wind events (which we cannot directly measure today), an exterior anemometer is the next sensor investment.
8. **Crop transpiration drift correction.** Current `calibrate_plant.py` treats `transpiration_kg_s` as a single fit per dataset. A weekly-resolution drift may be needed for crops with rapid growth phases; revisit after the validation residuals are seen.

## 13. Deliverables

At the end of the 21-day campaign window:

| # | Deliverable | Use | Owner | Path |
|---|---|---|---|---|
| D-1 | Campaign firmware build (e.g. `2.0.0-campaign.1`) | Pre-campaign | Firmware engineer | `bin/2.0.0-campaign.1/` |
| D-2 | 21 days of SD log CSVs, server-uploaded | During | Operator | Status server `/hbwv/log/` |
| D-3 | Outdoor CSV export | During / post | Operator | `model/srcData/outdoor-lht65-20_<dates>.csv` |
| D-4 | **Calibrated plant model (primary product)** | Post — feeds simulator | Analyst | `model/plant_calibrated_<dates>.json` |
| D-5 | **Fit/validation report** (residual stats, overlay plots, worked example output) | Post — proves AC-9/10/11 | Analyst | `model/profile/<dates>_fit_report.md` |
| D-6 | Per-bitmask cooling-rate table (secondary product) | Post — feeds future T6 mode | Analyst | `model/profile/<dates>_cooling_rate_table.csv` |
| D-7 | This plan, marked complete with post-campaign notes | Post | Analyst | `model/thermalProfileCampaign.md` (revision) |

D-1 and the rotation-config change in §5.3 are the only items required *before* the campaign starts; D-2 and D-3 accumulate *during*; D-4..D-7 are produced *after*.

**D-4 and D-5 are the primary deliverables.** D-6 is independent and can ship later without affecting D-4/D-5.

---

*End of plan. §6.3 (10-minute LoRaWAN interval) and §8.3 (21-day campaign duration) approved by the operator on 2026-05-21. Awaiting kick-off — see "Next steps before kick-off" below.*

---

## Appendix A — Next steps before kick-off

Three items must complete before day 0 of the campaign. Each is a separate work order; none has been started yet.

| # | Item | Owner | Output | Gate |
|---|---|---|---|---|
| NS-1 | Implement the §5 firmware changes on branch `dev/2.0.0-campaign.1`: add `LOG_SENSOR_HR` event type with three channel-discriminated sub-rows; **remove the existing `LOG_SENSOR` emit-site in `sensor_poll.cpp`** (sunset — replacement is permanent, not parallel); bump rotation defaults (1 MB × 30 files) as the new permanent operational values. | Firmware engineer | Build `2.0.0-campaign.1` in `bin/2.0.0-campaign.1/` with release notes + sha256. Note this is also the new operational baseline (post-campaign firmware does not revert §5 changes). | Bench-flash one unit, confirm `LOG_SENSOR_HR` rows appear at expected cadence, **no new `SENSOR` rows are emitted**, and the bitmask sub-row tracks a manual M1 OPEN/CLOSE cycle. |
| NS-1a | Update the analysis-pipeline log parser to dispatch on `SENSOR_HR` and decode the three channel-discriminated sub-rows (channel 0 = T+RH at 0.1 °C, channel 1 = wind, channel 2 = bitmask). Keep legacy `SENSOR` row decoding in place so pre-campaign bench-soak files remain readable. **No adapter shim needed** — this is the first production unit; there are no third-party parsers to bridge for. | Analyst | Updated parser in `model/` (or wherever the analysis scripts live); single round-trip test against a recent rc.1.3.2 bench-soak file (must still decode SENSOR rows) plus a synthetic SENSOR_HR file. | A combined file containing both row types parses cleanly with no warnings. |
| NS-2 | Configure LHT65-20 uplink interval to 600 s via TTN downlink — payload `01 00 02 58` (4 bytes), FPort 2 (confirm from live uplinks before sending; older deployments may be on FPort 1), confirmed downlink. See §6.4 for the byte-encoding details. | Operator | TTN console screenshot showing ACK + the next uplink at the new interval; one full hour confirmed. | TTN console shows ≥ 6 successive uplinks at 10-min spacing. |
| NS-3 | Verify current Phase 7 soak passes its 14-day clean criterion (zero `ESP_RST_PANIC`, zero `ESP_RST_TASK_WDT`, zero coredump captures). | Operator | Final soak summary appended to `bin/2.0.0-rc.1.3.2/release-notes.md`. | All three reset counters at 0 on day 14. |

Once all three gates pass, the campaign firmware (`2.0.0-campaign.1`) is flashed to the production unit, the wall-clock day-0 marker is logged, and the 21-day window begins. The campaign end-date and the SQL export window (§7.2) are computed at kick-off and recorded here.

| Kick-off | (TBD — fill at flash time) |
|---|---|
| Day-0 marker | (TBD) |
| Day-21 cutoff | (TBD) |
| SQL export start | (= day-0 marker) |
| SQL export end | (= day-21 cutoff + 1 h, to capture any late LoRaWAN uplinks) |

