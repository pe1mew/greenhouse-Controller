# Thermal-Profile Campaign Plan

| Field | Value |
|---|---|
| Document | Thermal-Profile Campaign Plan |
| Project | Greenhouse Ventilation Controller |
| Status | **Calibration complete (2026-07-04), all parameters identified.** Campaign data Jun 4 – Jul 4 (extended for NS-6 + heatwave). Adopted artifact: constrained 6-param free-m3 model, val T RMSE 1.19 °C. The NS-6 M3-only test resolved `ach_m3` ≤ 0.05 /h — area scaling refuted (§9.9). AC-9 requires the closed-loop `simulation.py` step. **Results summary: [campaignResults_summer2026.md](campaignResults_summer2026.md)** — this document is the working audit trail. |
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
| LoRaWAN — LHT65-20 | Outdoor T, RH, luminosity | **10 min** (600 s, verified) | 0.1 °C / 1 % RH / 1 lux | LHT65-20 → TTN → MariaDB → SQL → CSV |

All controller-side data lands as CSV rows on the SD card under `/sdcard/log/`. The daily upload to the status website continues unchanged.

The outdoor sensor data lands in a separate CSV via the SQL export documented in `model/srcData/sql.md`, joined post-hoc by timestamp during analysis.

> **Data-source authority for the model.** The SD card log files are the **authoritative measurement source** for the campaign — indoor T/RH, window bitmask, wind, and operating mode. The only MySQL source that feeds the model is `lht65-20` (outdoor T, RH, lux). See `model/fetch_lora_data.py` for the export script.

## 5. Firmware changes — SD-card log

> **Deployed.** All changes in this section shipped in firmware `rc.1.5.2` and are operational in 2.1.1 (verified 2026-06-27 against source and summer-2026 campaign logs). `LOG_SENSOR` is sunset; `LOG_SENSOR_HR` and `LOG_SUN` are live. Rotation defaults (1 MB × 30 files) are in effect. See `model/logUpdatePlan.md` for the companion implementation record.

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

Send once from the TTN console; the LHT65 receives the downlink in the RX window immediately after its next uplink (worst-case wait = one current uplink interval). The change is persistent across battery replacement. **Already active** — campaign data confirms 10 min cadence from 2026-06-04 (see NS-2 in Appendix A).

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

### 7.4 Door-open exclusion mask (Option A)

The greenhouse has two sliding harvest doors whose open/closed state is recorded by LDS01 sensors (`lds01-5` = door 1, `lds01-6` = door 2). When a harvest door is open it creates an unmodelled ventilation path (~9 m² aperture, ACH contribution 2–5 h⁻¹) that completely dominates the bitmask-driven window ACH. Including those hours in the plant-model fit biases `ach_open[bitmask]` upward and `c_eff` downward.

**Option A (adopted for this campaign):** exclude all SD-log rows where either door was open at the time of the row's timestamp, using a forward-filled binary flag from the last known LDS uplink.

Campaign impact (summer-2026, Jun 4–25):
- `lds01-5` (door 1): 57 h open (10.8%), 8 open blocks, longest 19 h
- `lds01-6` (door 2): 134 h open (25.3%), 74 open blocks, longest 99 h (Jun 6–10)
- After exclusion: **40 718 / 61 714 rows valid (65%)** — sufficient for a robust fit

Export the door-state CSVs with `fetch_lora_data.py` (same tool as §7.2):

```
python model/fetch_lora_data.py --sensor lds01-5 \
    --start 2026-06-04 --end 2026-06-26 \
    --output model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv

python model/fetch_lora_data.py --sensor lds01-6 \
    --start 2026-06-04 --end 2026-06-26 \
    --output model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv
```

### 7.5 Merged calibration input

`model/prepare_calibration_input.py` joins the SD log files, outdoor CSV, and door CSVs into a single flat CSV at the 30 s SENSOR_HR cadence. It adds `door1_open`, `door2_open`, and `calibration_valid` columns.

```
python model/prepare_calibration_input.py \
    --logs    model/campaign-summer-2026/ \
    --outdoor model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv \
    --door1   model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv \
    --door2   model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv \
    --output  model/campaign-summer-2026/calibration_input_2026-06-04_2026-06-25.csv
```

Output schema:

| Column | Source | Notes |
|---|---|---|
| `timestamp` | SD log | Local time, naive (Europe/Amsterdam) |
| `T_in_C` | SENSOR_HR_0 ch=0 | `value_a / 10.0` |
| `RH_in_pct` | SENSOR_HR_0 ch=0 | `value_b` |
| `wind_ms` | SENSOR_HR_1 ch=1 | `value_a / 10.0`, forward-filled |
| `wind_dir_deg` | SENSOR_HR_1 ch=1 | `value_b`, forward-filled |
| `bitmask` | SENSOR_HR_2 ch=2 | forward-filled |
| `T_out_C` | lht65-20 | forward-filled, stale after 1 800 s |
| `RH_out_pct` | lht65-20 | forward-filled |
| `lux` | lht65-20 | forward-filled |
| `lux_stale_s` | computed | seconds since last outdoor uplink |
| `door1_open` | lds01-5 | 0/1, forward-filled, 0 if no prior uplink |
| `door2_open` | lds01-6 | 0/1, forward-filled, 0 if no prior uplink |
| `calibration_valid` | computed | 1 when both doors closed AND outdoor data fresh |

Pass `--keep-door-rows` to suppress the door exclusion (for Option B covariate modelling).

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

The primary input to both stages is the merged calibration CSV produced by `model/prepare_calibration_input.py` (§7.5). This single file bundles all SD log rows with the outdoor and door-state columns already joined and the `calibration_valid` flag pre-computed.

For the summer-2026 campaign the file is:
`model/campaign-summer-2026/calibration_input_2026-06-04_2026-06-25.csv`

Component sources (produced by the §7 pipeline):

1. SD log files in `model/campaign-summer-2026/*.log` — indoor T/RH, wind, bitmask.
2. `model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv` — outdoor T, RH, lux.
3. `model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv` — door 1 state.
4. `model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv` — door 2 state.
5. The `settings.json` snapshot in force on the controller at day 0 (captured from `/api/config` before flashing the campaign firmware).

### 9.2 Stage 1 — calibrated dynamic plant model (primary)

Extend `calibrate_plant.py` to a new script `model/calibrate_plant_dynamic.py`:

- **Drops the "windows always closed" assumption.** The current calibrator fits only `ach_inf`, `k_solar`, `c_eff_mj_per_c`, `transpiration_kg_s`. The new one additionally fits a vector `ach_open[bitmask]` (7 free parameters, one per non-zero bitmask) plus an optional wind-modulation coefficient `k_wind_ach`.
- **Drives the fit with the recorded window-state trace** from `LOG_SENSOR_HR,channel=2` rows joined to the outdoor weather and the indoor measurements.
- **Door exclusion (Option A).** Filter the merged CSV to `calibration_valid == 1` before fitting. This drops 32% of rows (20 036 / 61 714) where either harvest door was open. The remaining 40 718 rows (65%) are the fit dataset. See §7.4 for the impact analysis.
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

### 9.5 First-pass calibration test run (2026-06-27)

Script: `model/calibrate_plant_campaign.py` — a simplified version of the planned `calibrate_plant_dynamic.py`. Key differences from the §9.2 plan: uses a **binary open/closed flag** (`bitmask > 0`) rather than per-bitmask `ach_open[]`, and fits only 5 parameters (`k_solar`, `c_eff_mj_per_c`, `transpiration_kg_s`, `ach_closed`, `ach_open`). No train/validation split — all `calibration_valid == 1` rows are used for fitting.

**Setup:** global DE (popsize=20, maxiter=300) converged at 143 iterations; confirmed global minimum via Nelder-Mead polish.

**Data:** 62 058 grid points (30 s, Jun 4–25); 41 158 in fit mask (66%); fit excludes door-open and stale-outdoor rows per Option A.

**Parameter comparison:**

| Parameter | Spring-2026 | Summer-2026 | Change |
|---|---|---|---|
| `k_solar` (W/lux) | 0.44289 | 0.09405 | −79% |
| `c_eff_mj_per_c` | 2.890 | 3.572 | +24% |
| `transpiration_kg_s` | 0.00477 | 0.00031 | −94% |
| `ach_closed` (/h) | 0.500 | 0.182 | −64% |
| `ach_open` (/h) | 1.650 | 0.526 | −68% |

**Residuals (evaluated on `calibration_valid == 1` rows, same set used for fitting):**

| Metric | Spring-2026 model | Summer-2026 fit | Improvement |
|---|---|---|---|
| T RMSE | 5.10 °C | 2.69 °C | −47% |
| RH RMSE | 16.74 % | 6.73 % | −60% |

Output files: `model/campaign-summer-2026/plant_calibrated_summer2026.json`, `model/campaign-summer-2026/calibration_compare_summer2026.png`.

**What the plot shows.** The spring model (red dashed) overestimates daytime T by 5–15 °C on clear days and underestimates RH by 20–40 %; both effects are systematic, not random. The summer fit (green) tracks measured T and RH closely across the full 22-day series, with the largest residuals on the Jun 6–10 door-open block (excluded from fit, visible as pink shading). Visual inspection confirms no systematic bias in the summer-fit residuals.

**Model-structure limitations.** The fitted `k_solar = 0.094 W/lux` gives an implied peak solar load of ~4 700 W (vs 21 900 W spring), and `transpiration ≈ 0`. These are not physically implausible but are smaller than expected, because the open-loop first-order model cannot represent the controller feedback loop: in practice, solar gain raises T → controller opens windows → T is partially vented; the model compensates by reducing `k_solar` rather than by simulating the control action. Consequently, the fitted parameters are best viewed as **effective** values for reproducing the measured T trace under actual controlled operation, not as independent physical measurements.

`c_eff = 3.57 MJ/°C` is close to the air-only floor (V·ρ·cp = 2.89 MJ/°C), suggesting the 30 s SD backbone data still drives the model toward fast thermal response to match the sharp diurnal T cycles.

**Gap to AC-9.** The AC-9 target is ±1 °C for ≥ 95 % of samples on a held-out validation week. At T RMSE = 2.69 °C (in-sample), this is not met. To close the gap: (a) implement `calibrate_plant_dynamic.py` with per-bitmask `ach_open[]` — the dominant missing degree of freedom is that the binary open/closed model averages across M1-only, M1+M2, and M1+M2+M3 states; (b) introduce a proper train/validation split so AC-9 can be evaluated on out-of-sample data. The simplified binary model is adequate for directional comparisons (see §9.4 worked example) but not for the ±1 °C fidelity claim.

### 9.6 Per-bitmask calibration and staged M3 identification (2026-06-27)

**Motivation.** The binary model (§9.5) uses a single `ach_open` for any window-open state. The §9.2 plan requires per-bitmask `ach_open[bitmask]` to resolve M1, M2, M3 contributions individually. This section documents the two approaches tried and the conclusion that per-bitmask identification is not achievable from this dataset open-loop.

**Approach A — joint 7-parameter fit (`calibrate_plant_dynamic.py`).**
Adds `ach_inf`, `ach_m1`, `ach_m2`, `ach_m3` as independent parameters; ACH is additive: `ach_total = ach_inf + ach_m1·m1 + ach_m2·m2 + ach_m3·m3`. Train/val split Jun 4–18 / Jun 19–25.

Training-set bitmask coverage:

| Bitmask | State | Training rows |
|---|---|---|
| 0b000 | All closed | 18 903 (157.5 h) |
| 0b001 | M1 only | 4 326 (36.0 h) |
| 0b011 | M1+M2 | 905 (7.5 h) |
| 0b101 | M1+M3 | 362 (3.0 h) |
| 0b111 | M1+M2+M3 | 1 775 (14.8 h) |
| others | M2-only, M3-only, M2+M3 | < 25 rows each |

Fast run (popsize=10, maxiter=100): `ach_m3 → 0` (degenerate). Full run (popsize=25, maxiter=400): `ach_m2 → 0` (different degenerate solution). Nelder-Mead polish drove `c_eff` below the physical floor (1.55 MJ/°C vs floor 2.89 MJ/°C) because scipy Nelder-Mead does not strictly enforce bounds. Binary and per-bitmask models are visually indistinguishable on the plot; validation RMSE improvement is negligible (1.34 °C vs 1.37 °C binary).

**Approach B — two-stage fit (`calibrate_plant_staged.py`).**
Stage 1: fit `[k_solar, c_eff, transp, ach_inf, ach_m1]` using loss restricted to 0b000+0b001 rows only (23 229 training rows, cleanest M1 signal). Stage 2: lock Stage 1 params, fix `ach_m2 = ach_m1` (identical window geometry), fit `ach_m3` from 0b101 rows alone (362 training rows — rapid cool-down events where M2 has closed but M3 dwell has not expired).

Stage 1 result: `ach_inf = 0.165 /h`, `ach_m1 = 0.237 /h`, `c_eff = 2.89 MJ/°C` (clipped to physical floor). Stage 1 residuals on 0b000+0b001 rows: train RMSE 2.83 °C (bias −1.42 °C), val RMSE 1.19 °C.

Stage 2 result: `ach_m3 = 0.010 /h` — **hit the lower bound**. The 1-D grid search showed the minimum loss at the smallest allowed value; adding M3 ventilation increases the Stage 2 loss (75.4) vs the Stage 1 baseline (34.3). Validation: T RMSE 1.30 °C, 95th-pct 2.23 °C, within ±1 °C 50 % → AC-9 FAIL.

**Root cause — why 0b101 rows cannot identify M3.**
The 362 training rows of state 0b101 (M1+M3) arise exclusively from rapid cool-down events: outdoor temperature drops fast, the controller closes M2 (300 s dwell satisfied) but M3 remains open (1 500 s dwell still running). During these events T_in is already dropping due to falling solar gain — the `k_solar·lux` term explains the observed temperature trajectory without any M3 contribution. Adding `ach_m3 > 0` predicts additional convective cooling that is not present in the data, increasing the fit error. The Stage 2 loss minimum lies at `ach_m3 → 0` regardless of whether M1 is pre-fixed or fit jointly. This is a structural observability limitation: M3's ventilation effect and the falling-lux cooling are confounded in the 0b101 data, and the open-loop model has no way to separate them.

**Performance summary:**

| Model | Val T RMSE | Val 95th-pct | Val within ±1 °C |
|---|---|---|---|
| Spring-2026 baseline | 5.43 °C | 10.90 °C | 9 % |
| Binary summer-2026 (§9.5) | 1.37 °C | 2.62 °C | 51 % |
| Joint dynamic (calibrate_plant_dynamic.py) | 1.34 °C | 2.53 °C | 54 % |
| Two-stage (calibrate_plant_staged.py) | 1.30 °C | 2.23 °C | 50 % |

**Conclusion.** The binary model (`plant_calibrated_summer2026.json`) is the adopted calibration artifact. Per-bitmask `ach_m3` is not identifiable from this 21-day campaign open-loop. The ~0.07 °C validation RMSE improvement from additional parameterisation is within noise. AC-9 (≤ 1.0 °C 95th-pct, ≥ 95 % within ±1 °C) cannot be achieved with any open-loop plant model on controlled-greenhouse data — every controller-driven window state change creates a discontinuity the open-loop model cannot bridge. The proper path to AC-9 is the closed-loop `simulation.py` approach: drive the firmware's full control logic with the calibrated plant parameters and the recorded outdoor weather, allowing the simulated controller to make the same decisions as the real controller.

Scripts: `model/calibrate_plant_dynamic.py`, `model/calibrate_plant_staged.py`. Output JSON: `campaign-summer-2026/plant_calibrated_dynamic_summer2026.json`, `campaign-summer-2026/plant_calibrated_staged_summer2026.json` (retained for reference; binary model is authoritative).

### 9.7 Constrained 6-parameter model — physical priors and firmware limitations (2026-06-27)

#### Priors accepted

After the full identification study in §9.6, two physical priors were accepted and encoded as constraints:

**Prior 1 — M1 = M2 (ach_m2 = ach_m1).**
M1 and M2 are identical 21-step roof windows with the same geometry, travel, and installation. The 21-day campaign produced only 15 rows of M2-alone data (7.5 minutes) — statistically insufficient to identify `ach_m2` independently. Equality by construction is the correct prior; the data has nothing to say against it.

**Prior 2 — M3 dominates when open.** *(Refuted by NS-6 measurement — see §9.9.)*
M3 is the north side-wall window (FRS: *Zijwandbeluchting*, ~80 m² vs ~8 m² per roof window — 10× area; 171 s travel vs 21 s — 8.1× travel). When M3 is open, M1 and M2's combined contribution is nominally small relative to the M3-driven flow. This was accepted as a physical prior: M1 and M2 are still modelled explicitly, but M3 was expected to dominate any open state that includes it. The implication was `ach_m3 >> ach_m1`, with the lower bound set to `ach_m1` (M3 must contribute at least as much as one small window). *Note (2026-07-04): an earlier revision of this document mis-identified M3 as a roof ridge panel; the FRS and boer manual place it in the north side wall.*

#### Constrained 6-parameter model (`calibrate_plant_constrained.py`)

Free parameters: `k_solar`, `c_eff_mj_per_c`, `transpiration_kg_s`, `ach_inf`, `ach_m1`, `ach_m3`.
Constraint: `ach_m2 = ach_m1` (not fitted).

ACH formula: `ach(vm) = ach_inf + ach_m1·(m1 + m2) + ach_m3·m3`

Two-stage fit:
- **Stage 1** — M3-closed states (`vent_mask & 0b100 == 0`, 24 149 training rows including 905 rows of M1+M2). The 0b011 rows now contribute independent signal for `ach_m1` via `ach(0b011) = ach_inf + 2·ach_m1`.
- **Stage 2** — M3-open states (`vent_mask & 0b100 != 0`, 2 161 training rows; primary signal: 1 775 rows of 0b111 at full-ventilation during peak solar events).

Stage 1 result: `ach_inf = 0.178 /h`, `ach_m1 = 0.163 /h`, `c_eff = 2.89 MJ/°C`.

Implied ACH table:

| Bitmask | State | ACH (/h) | Train rows |
|---|---|---|---|
| 0b000 | All closed | 0.178 | 18 903 |
| 0b001 | M1 only | 0.340 | 4 326 |
| 0b010 | M2 only | 0.340 | 15 |
| 0b011 | M1+M2 | 0.503 | 905 |
| 0b100 | M3 only | 0.340 | 4 |
| 0b101 | M1+M3 | 0.503 | 362 |
| 0b110 | M2+M3 | 0.503 | 20 |
| 0b111 | M1+M2+M3 | 0.665 | 1 775 |

Stage 2 result: **`ach_m3 = ach_m1 = 0.163 /h` (lower bound)**. The grid scan shows loss monotonically increasing above the lower bound — the 0b111 data does not support `ach_m3 > ach_m1`. See the firmware limitation below for why.

Performance vs prior models:

| Model | Val T RMSE | Val 95th-pct | Val within ±1 °C |
|---|---|---|---|
| Spring-2026 baseline | 5.43 °C | 10.90 °C | 9 % |
| Binary summer-2026 | 1.37 °C | 2.62 °C | 51 % |
| Joint dynamic (7p) | 1.34 °C | 2.53 °C | 54 % |
| Two-stage staged (§9.6) | 1.30 °C | 2.23 °C | 50 % |
| **Constrained 6p (adopted)** | **1.20 °C** | **2.19 °C** | **57 %** |

The constrained model outperforms binary by 0.17 °C RMSE and +6 percentage-point within-±1 °C. The improvement comes entirely from the M1=M2 prior: by treating 0b011 rows as `ach_inf + 2·ach_m1` rather than averaging them with M1-only rows, the model correctly assigns more ventilation to the multi-window states. The `ach_m3` lower-bound result means M3's effect is modelled conservatively (equal to M1), which will underestimate peak ventilation in 0b111 states but is the best the data supports.

**Adopted calibration artifact:** `campaign-summer-2026/plant_calibrated_constrained_summer2026.json` replaces the binary model as the authoritative output. It encodes both physical priors and is strictly better than binary on validation metrics. *(Superseded 2026-07-04: the NS-6 test resolved `ach_m3`; the adopted artifact is now `plant_calibrated_constrained_summer2026_freem3.json` — see §9.9.)*

#### Firmware limitations on M3 identification

The following firmware design constraints prevented direct identification of `ach_m3` from this campaign. These are correct design choices for normal operation; they are limitations only for open-loop plant calibration.

**1. Staged opening sequence — M3 never opens alone.**
The firmware always ventilates in the sequence M1 → M1+M2 → M1+M2+M3 (tasks T6/T2). M3 can only open after M1 and M2 have been open for their dwell periods. The result: the M3-alone state (0b100) has only 4 training rows in 21 days. There is no admin API or test mode to open M3 in isolation.

Calibration impact: `ach_m3` must be identified from M3-combined states (0b101, 0b110, 0b111). M1 and M2's confounding contribution cannot be removed experimentally.

**2. M3-open states are structurally confounded by controller feedback.**
M3 opens only when the controller has decided maximum ventilation is required. This means every M3-open period is either (a) a rapid cool-down event where lux drops independently explain the temperature fall (0b101/0b110), or (b) a sustained high-heat event where the controller holds T_in near setpoint (0b111). In case (a), the external driver (falling lux) explains the observed temperature without needing M3. In case (b), the controller feedback holds T_in flat, and the open-loop model interprets "T_in stays near setpoint despite high lux" as compatible with a small ach_m3. Neither state allows the plant's natural open-loop thermal response to M3 to be observed.

Calibration impact: adding `ach_m3 > ach_m1` increases the model's predicted cooling in M3-open states, which worsens the fit relative to the controlled/confounded observed T_in. The optimiser is therefore forced to the lower bound regardless of dataset size or fitting strategy.

**3. Dwell times create asymmetric identification windows.**
`dwell_open_m1 = dwell_open_m2 = 300 s` vs `dwell_open_m3 = 1 500 s`. The much longer M3 dwell means M3 is almost always co-open with M1 and M2 except during the brief rapid-close events (0b101: 362 rows, 3.0 h across 21 days). These events are the only M3-only-increment data, and they occur exclusively during rapid cool-down — see point 2.

**Path to ach_m3 identification:**

Option A — **deliberate M3-only test using the existing LCD manual override.** No firmware changes required.

The firmware already provides a manual motor override (introduced gh#29, `rc.1.5.0`). When an admin enters the motor-control menu on LCD Screen 5, STANDBY is engaged automatically, pausing T6 (climate control). Motor commands are posted directly to T2 (relay controller) with `source = SRC_OPERATOR_MANUAL`, which bypasses dwell timers and the staged opening sequence. Any individual channel can be opened or closed independently of the others.

*Implementation detail (for the calibration pipeline):* when M3 is open and M1/M2 are closed, T2's bitmask accessor returns `M1=CLOSED(0b00) | M2=CLOSED(0b00) | M3=OPEN(0b10)` → raw `value_a = 0x20`. The calibration pipeline decodes this as `vent_mask = 0b100` — the M3-only state. STANDBY prevents T6 from re-opening M1 or M2 during the test window.

*Safety gate:* `CMD_OPEN` for any channel is blocked by `EG1_BIT_WIND_OVERRIDE` (T3 wind safety, active when wind > `v_max`). A `CMD_CLOSE_ALL` from T3 can also close M3 mid-test if wind rises. Run the test in calm conditions, well below the wind threshold.

**NS-6 M3 calibration test procedure** (see NS table below):

| Step | Action |
|---|---|
| Precondition | Clear sunny day, outdoor lux ≥ 30 000, wind < 50 % of `v_max`, stable for ≥ 30 min before test |
| 1 | Navigate to LCD Screen 5 (window states display) |
| 2 | Press `#` → enter admin PIN → STANDBY engages, T6 pauses |
| 3 | If M1 or M2 are open: select each (`1` / `2`), press `2` (Close) |
| 4 | Select M3 (`3`) → press `1` (Open) — T2 energises M3 relay immediately |
| 5 | Wait **45–60 minutes** in place; do not exit or touch LCD (admin session timeout = 5 min idle; navigate back to motor menu if needed to keep session alive) |
| 6 | Select M3 → press `2` (Close) |
| 7 | Exit manual menu (`*`) — STANDBY clears when admin session expires (~5 min) |
| Post | Download the SD log covering the test window; add to `campaign-summer-2026/` as `m3_calib_<date>.csv` |
| Analysis | Rerun `calibrate_plant_constrained.py` Stage 2 with the augmented dataset — the ~120 new `vent_mask=0b100` rows in stable lux directly constrain `ach_m3` without controller-feedback confounding |

*Why 45–60 min is sufficient:* the thermal time constant τ = C_eff / (ach_total × ρ·V·cp) ≈ 3 600 / ach_m3 seconds. If M3 contributes ach_m3 ≈ 2 /h (8× M1, area-scaling estimate), τ ≈ 27 min; one full time constant captures 63 % of the step response — a unique, unambiguous fit of ach_m3.

Option B — **closed-loop simulation**: `simulation.py` drives the firmware's full control logic (T6 decisions, dwell enforcement) with the calibrated plant parameters. The simulated controller makes the same decisions as the real controller, so the simulation's predicted T_in trajectory is already the controlled response — M3's effect is observable within the simulation's own feedback loop. The constrained 6p model provides the plant parameters; `ach_m3 = ach_m1` is conservative and will be corrected by the simulation dynamics. This is independent of the M3 test and should be pursued regardless (it closes AC-9 and AC-11).

Script: `model/calibrate_plant_constrained.py`. Output: `campaign-summer-2026/plant_calibrated_constrained_summer2026.json`.

### 9.8 Window strategy analysis: implications from calibration (2026-06-27)

> **⚠ Superseded in part by §9.9 (2026-07-04).** This section's quantitative case rests on the *area-scaled* estimate ach_m3 ≈ 1.32 /h ("physical M3"). The NS-6 direct measurement refutes that estimate: ach_m3 ≤ 0.05 /h. Reasons 2–3 (M3 as dominant ventilator opened too late) do not survive; reasons 4–5 (night-long −5 °C close hysteresis; non-orthogonal `hyst_t`) are unaffected. See §9.9 for the revised conclusions.

The summer-2026 calibration gives the first quantitative per-channel ACH estimates for this greenhouse. Comparing those numbers against the T6 ventilation algorithm reveals a structural mismatch between the implemented control strategy and the physical reality of the windows.

#### Current firmware strategy (T6 `step_from_deviation`)

T6 maps temperature deviation above setpoint to a step index 0–3, with equal step widths of `max(hyst_t / 3, 1)` °C. At the default `hyst_t = 5 °C`, each step triggers at +1 °C:

| Step | Bitmask | Windows open | Trigger (above T_set) |
|---|---|---|---|
| 0 | 0b000 | none | — |
| 1 | 0b001 | M1 | T_set + 1 °C |
| 2 | 0b011 | M1 + M2 | T_set + 2 °C |
| 3 | 0b111 | M1 + M2 + M3 | T_set + 3 °C |

The step widths are **equal in temperature** (1 °C each). The implicit assumption is that each step adds roughly equal ventilation capacity.

#### What the calibration actually shows

Adopted constrained model (`plant_calibrated_constrained_summer2026.json`):

| Step | Bitmask | Calibrated ACH | Incremental ACH | × per step |
|---|---|---|---|---|
| 0 | 0b000 | 0.178 /h | — | — |
| 1 | 0b001 | 0.341 /h | +0.163 /h (M1) | — |
| 2 | 0b011 | 0.504 /h | +0.163 /h (M2 = M1) | 1.0× step 1 |
| 3 | 0b111 (lower bound) | 0.667 /h | +0.163 /h (M3 = M1) | 1.0× — **artificial lower bound** |
| 3 | 0b111 (area-scaled) | 1.824 /h | +1.320 /h (M3 ≈ 8.1× M1) | **8.1× step 1** |

The lower-bound row is the model artefact (controller-feedback confounding, §9.7). The area-scaled row is the physical estimate from M3's 171 s travel vs M1's 21 s (ratio 8.1, applied to `ach_m1 = 0.163 /h`); the FRS area ratio (80 m² north-wall window vs 8 m² roof window) is 10× — either way the estimate was ~an order of magnitude, and NS-6 refuted it (§9.9).

The equal-step-width assumption is **physically incorrect**. Step 3 does not add the same ventilation increment as steps 1 or 2 — it adds 8× more.

#### Why this matters: thermal response time

The thermal time constant of the greenhouse plant model is:

```
τ = C_eff / (ACH_total × V_air × ρ_air × c_p_air)
```

Since all terms except ACH_total are fixed for a given greenhouse, τ scales as 1/ACH_total. The relative response times per step:

| Step | ACH | τ relative to step 1 |
|---|---|---|
| 0 (closed) | 0.178 /h | ~ very long |
| 1 (M1) | 0.341 /h | 1.00 × |
| 2 (M1+M2) | 0.504 /h | 0.68 × (only 32 % faster than step 1) |
| 3 (lower bound) | 0.667 /h | 0.51 × (2.0× faster than step 1) |
| 3 (area-scaled M3) | 1.824 /h | **0.19 × (5.3× faster than step 1)** |

Steps 1 and 2 provide essentially the same response time — adding M2 to M1 makes the greenhouse only 32 % faster to equilibrate. Step 3 with a physically correct M3 makes it **5× faster**. The jump from step 2 to step 3 is not a linear increment; it is a **regime change**.

#### Equilibrium temperature excess

At solar steady state `(dT_in/dt = 0)`, the indoor-outdoor temperature difference scales as:

```
T_in_eq - T_out = (P_solar + P_transp) / (ACH_total × V_air × ρ_air × c_p_air)
```

Again, `∝ 1/ACH_total`. On a fully sunny summer day with `k_solar × lux` fixed:

| Step | ACH | T_excess relative to step 3 (area-scaled) |
|---|---|---|
| 0 (closed) | 0.178 /h | 10.2 × |
| 1 (M1) | 0.341 /h | **5.3 ×** |
| 2 (M1+M2) | 0.504 /h | **3.6 ×** |
| 3 (area-scaled) | 1.824 /h | **1.0 ×** (reference) |

If M3 at full capacity can maintain `T_in = T_out + 3 °C` in full sun, then M1 alone maintains `T_in = T_out + 16 °C` and M1+M2 maintains `T_in = T_out + 11 °C`. The steps below M3 are not "partial cooling" — they are ineffective cooling on a hot summer day.

#### Five reasons to reconsider the strategy

**1. M3 opens too late.**
The current strategy requires T to exceed setpoint by 3 °C before M3 opens. By that point the plant has already been heat-stressed for potentially hours (steps 1 and 2 have τ ≈ several hours each). M3 should engage sooner.

**2. Equal step widths are physically wrong.**
The 1 °C spacing between each step was designed under an equal-ventilation-per-step assumption. The calibration proves M3 provides 8× more ACH than M1 or M2. A threshold structure that treats all three steps as equal ignores the most important physical fact about this greenhouse.

**3. Step 1 → step 2 is nearly useless on a hot summer day.**
M2 is identical to M1 (same geometry, same ACH contribution). Opening M1+M2 vs M1 alone changes the equilibrium temperature excess by only (5.3 - 3.6) / 5.3 = 32 %. On a day requiring M3, this 32 % improvement from M2 is irrelevant — the decisive action is M3. The step 1 → 2 transition wastes a +1 °C deviation band without materially improving the situation.

**4. The −5 °C close hysteresis keeps M1 open all night.**
The `step_from_deviation` close-guard holds step ≥ 1 until `T_in < T_set − hyst_t = T_set − 5 °C`. With `T_set = 28 °C`, M1 stays open until T_in < 23 °C. On many summer nights T_in never drops to 23 °C, so M1 stays energised all night. The calibration shows `ach_m1 = 0.163 /h` — this is sustained ventilation removing transpired moisture and heat throughout the night. Depending on outdoor humidity, this may or may not be desirable, but the current strategy gives no independent control of the night-close threshold.

**5. hyst_t conflates two independent tuning objectives.**
A single `hyst_t` parameter controls three things simultaneously: when M1 opens (+step_width), when M2 opens (+2×step_width), and when M3 opens (+3×step_width), as well as the full-close guard (−hyst_t). Tuning `hyst_t` to make M3 open sooner (say reducing to 3 °C → M3 at +1 °C deviation) also makes M1 open sooner (+0.33 °C, likely oscillation territory) and closes the full-close guard to −3 °C (possibly causes night oscillation). The parameters are not orthogonal.

#### Proposed alternative strategies

**Option A (minimum-invasive) — independent M3 threshold (`t_thresh_m3`)**

Add a single NVS parameter `t_thresh_m3` (°C above setpoint to open M3, namespace `"climate"`). M3 is opened whenever `T_in > T_set + t_thresh_m3`, regardless of which step M1/M2 are on. Default: `t_thresh_m3 = t_thresh_m1` (current step-1 threshold, so M3 opens at the same deviation as M1 on hot days). Operators can lower it to 0.5–1.0 °C for aggressive summer preemptive opening.

Impact on step table logic: none. T6 evaluates the M3 decision independently of the step index. T2's dwell enforcement and safety gates are unchanged.

This is the **recommended starting point** — one parameter, zero risk of regressing M1/M2 behaviour, directly testable after NS-6 confirms `ach_m3`.

**Option B — revised step table: skip M2 intermediate step**

Replace the step table with:

| Step | Bitmask | Windows |
|---|---|---|
| 0 | 0b000 | none |
| 1 | 0b001 | M1 |
| 2 | 0b101 | M1 + M3 |
| 3 | 0b111 | M1 + M2 + M3 |

Step 2 (M1+M3) provides ach ≈ 0.341 + 1.320 = 1.661 /h (area-scaled estimate) vs the current M1+M2 at 0.504 /h — more than 3× better cooling without adding M2 as an intermediate. Step 3 adds M2 for maximum flow but the marginal benefit of M2 when M3 is already open is small (+9 % ACH).

**Requires firmware change to `VENT_STEP_TABLE`.** Also changes the behaviour of the 25-min `dwell_open_m3` — at step 2 M3 would now be subject to that dwell on every step-down from 2 to 1, which may create instability. Needs simulation study before implementation.

**Option C — feedforward M3 on solar gain**

Open M3 preemptively when `lux > lux_thresh_m3` (configurable), before T even rises above setpoint. The calibration gives `k_solar = 0.087 W/lux`; at `lux = 50 000` that is 4 350 W of solar gain. Even with M1+M2 open the greenhouse cannot stay within 1 °C of setpoint. A solar-triggered M3 pre-open is physically justified.

Requires reading the `lux` value in T6, which currently reads only T and RH from the sensor shadow. Minor architecture change.

#### Summary

| Reason to change | Severity | Addressed by |
|---|---|---|
| M3 opens 2 °C too late | High — plants already heat-stressed | Option A / B |
| Step widths ignore 8× M3 area advantage | High — fundamentally wrong physics | Option A / B / C |
| Step 1→2 adds only 32 % improvement | Medium — wastes a 1 °C deviation band | Option B |
| hyst_t conflates M1/M2/M3 and close guard | Medium — no independent M3 tuning | Option A |
| Night-close always at T_set − 5 °C | Low — may over-ventilate mild nights | Separate t_close_night parameter |

The calibration alone does **not** give ACH values for alternative bitmask states (0b101, 0b100) — those require the NS-6 M3 test. However, even with the current conservative lower-bound `ach_m3 = ach_m1`, steps 1 and 2 have identical ventilation increments, which already justifies Option A: there is no penalty from opening M3 earlier. *(2026-07-04: NS-6 measured `ach_m3` ≈ 0.05 /h — "no penalty" still holds, but the expected benefit collapses too; see §9.9.)*

### 9.9 NS-6 M3-only test — execution and results (2026-07-04)

**The test was executed on 2026-07-04 and refutes the area-scaling hypothesis.** `ach_m3` is small — comparable to a *fraction* of `ach_m1`, not 8× it.

#### Execution

Two M3-only windows via the LCD manual override (procedure §9.7), M1 = M2 = CLOSED, `vent_mask=0b100` in the SD log:

| Window | Local time | Duration | Condition |
|---|---|---|---|
| 1 | 09:54:47 – 10:54:37 | 60 min | lux 20–53 k, wind 0.3–2.9 m/s |
| 2 | 11:57:27 – 12:57:17 | 60 min | lux 16–36 k, wind 1.5–4.1 m/s |

**Deviation from procedure:** greenhouse doors were open during much of both windows (Saturday farm activity) — door1 the whole of window 1; window 2 contaminated 11:57–12:23 (door1) and 12:40–12:57 (door2). The Option-A exclusion mask left a clean stretch **12:23–12:40** (33 rows, doors closed, fresh outdoor data). Across the extended dataset the 0b100 state now has **103 valid training rows** (was 4).

#### Steady-state cross-check (clean stretch)

By 12:23 the house had been in M3-only state for 26 min and T_in was nearly flat (quasi-steady). Mean values: T_in 30.6 °C, T_out 24.1 °C, ΔT 6.5 °C, lux 27 900. Heat balance with the Stage-1 parameters (k_solar 0.085 W/lux, P_transp ≈ 760 W):

```
UA_vent = (P_solar − P_transp − C_eff·dT/dt) / ΔT ≈ 214–257 W/K
→ ACH_total ≈ 0.21–0.26 /h  →  implied ach_m3 ≈ 0.03–0.09 /h
```

A large north-wall window (~80 m²) yielding less incremental ventilation than the small (~8 m²) M1 roof window.

#### Re-calibration (input extended Jun 4 → Jul 4)

Input: `calibration_input_2026-06-04_2026-07-04.csv` (87 301 rows, 47 866 valid, 54 %). Validation window held at Jun 19–25 for comparability; training = Jun 4–18 + Jun 25–Jul 4 (includes the heatwave Jun 26–28 and the NS-6 test). Two Stage-2 variants (`--free-m3` flag added to `calibrate_plant_constrained.py`):

| Variant | ach_m3 bound | ach_m3 result | Stage-2 loss | val T RMSE | val 95th | val ±1 °C |
|---|---|---|---|---|---|---|
| Bounded (area prior) | ≥ ach_m1 = 0.166 | **0.166 (pinned at bound)** | 38.83 | 1.32 °C | 2.49 °C | 54.4 % |
| Free (NS-6 mode) | ≥ 0.05 floor | **0.050 (pinned at floor)** | 33.56 | **1.19 °C** | **2.11 °C** | 55.7 % |

Stage-1 parameters (both variants, extended data): k_solar 0.0847 W/lux, c_eff 2.894 MJ/°C, transp 0.0003 kg/s, ach_inf 0.203 /h, ach_m1 = ach_m2 0.166 /h.

The optimiser pushes `ach_m3` to whatever floor it is given, and the free variant fits better on *both* training and validation. Two independent signals agree:

1. **The M3-only test rows** — direct, unconfounded (T6 paused, no feedback).
2. **The heatwave 0b111 rows (Jun 26–28)** — the controller was *saturated* (all windows fully open, T_in up to 42.2 °C, far above setpoint), so no feedback confounding: this is open-loop data too, and only a small total ACH explains the observed peaks.

**Adopted artifact (supersedes the 2026-06-27 adoption):** `campaign-summer-2026/plant_calibrated_constrained_summer2026_freem3.json` — val T RMSE 1.19 °C. The bounded variant is retained alongside for comparison.

#### Findings

| # | Finding |
|---|---|
| 1 | **Area scaling refuted.** ach_m3 ≤ 0.05 /h (boundary-pinned; steady-state check 0.03–0.09 /h) ≈ 0.3× ach_m1 — not the ~10× the FRS area ratio (80 m² wall window vs 8 m² roof window) predicted (§9.6, §9.8). |
| 2 | **Full-open ventilation is modest.** ACH(0b111) ≈ 0.59 /h vs ACH(0b011) ≈ 0.54 /h — opening the 80 m² north-wall window on top of both roof windows adds ~9 %. |
| 3 | **Heatwave behaviour is now explained.** With ~0.6 /h total ACH, Jun 26's T_max = 42.2 °C at 60–80 k lux is exactly what the model predicts; the earlier expectation that "physical M3" (1.8 /h) would hold setpoint up to T_out ≈ 26 °C was based on the refuted area scaling. |
| 4 | **Physical interpretation, partial.** M3 is the window in the **north side wall** (FRS: *Zijwandbeluchting*) — the leeward side under the prevailing Dutch SW winds, so wind-driven exchange through it is weak. For 0b100 (M3 alone) the exchange is single-sided through one wall opening — buoyancy-only, inherently weak. Harder to explain is finding 2: with M1/M2 open, the low north-wall inlet + high roof outlets should form a stack circuit, yet the measured increment stays small. Hypotheses: (a) M3's *effective open aperture* is much smaller than 80 m² — the 171 s travel says nothing about how wide the wall actually opens or where the gap is; (b) if the opening gap sits near the top of the wall (gutter height), the stack height difference to the roof windows is small and the circuit is weak; (c) leeward sheltering suppresses the wind-assist in the observed 1–4 m/s range. → NS-8. |

#### Robustness check — "but T drops fast when M3 opens" (2026-07-05)

Objection: daily plots show temperature falling fast right after M3 opens, which seems to contradict a small `ach_m3`. Event study over all daytime channel-opening transitions (`m3_event_study.py`; slopes are medians, 15 min before vs +3…+18 min after):

| Event (n) | T_in slope pre → post | RH_in slope pre → post | lux slope pre → post |
|---|---|---|---|
| M3 opens on M1+M2 (74) | +8.2 → −8.0 °C/h | +4.4 → **+13.0 %/h (rises)** | flat → −2 860 lux/h |
| M2 opens, M3 closed (95) | +5.5 → −3.9 °C/h | +1.4 → −5.1 %/h | +1 370 → −2 390 lux/h |
| M1 opens, all closed (42) | +10.2 → −5.3 °C/h | −9.3 → −12.0 %/h | flat → +3 240 lux/h |

Resolution: (1) the slope swing after M3 opens (−16.2 °C/h) is nearly identical to the swing after **M1 alone** opens (−15.4 °C/h) — the fast drop is a property of the *trigger moment* (windows open exactly at the steepest rise; what follows a peak is decline), not of window area; (2) lux flips from flat to falling at the median M3 event — step 3 is reached at the daily solar knee, so the sun backs off exactly when M3 opens (§9.7 confounding, now quantified); (3) RH *rises* after M3 opens — cooling without much air exchange; a real flush of drier outdoor air would pull RH down. Difference-in-differences vs the controls gives M3 an extra ≈ −1 °C/h, matching the measured model (−0.4 °C/h predicted) and excluding area scaling (−10.3 °C/h predicted).

Open question feeding NS-8: the **controller sensor's position relative to the north wall** — if it sits near M3, opening the wall washes the sensor locally (fast local relief, slow bulk exchange), which would explain both the visual impression and the controller's step-3 behaviour.

#### Caveats

- `ach_m3 = 0.05` is a floor pin, not an interior optimum — read it as "ach_m3 ≤ 0.05–0.09 /h", best available estimate, not a precise value.
- Door contamination cost most of window 1; the conclusion rests on the clean 17-min stretch + the saturated heatwave rows, which agree.
- Both test windows had light-to-moderate wind (≤ 4 m/s). A windy-day repeat would test the wind-driven hypothesis in finding 4.

#### Consequences for NS-7 (window strategy)

The §9.8 recommendation was premised on M3 being an 8× ventilator opened too late. Measurement inverts this: **opening M3 earlier (t_thresh_m3) buys almost nothing** — equilibrium temperature excess improves ~9 % — and no passive window strategy reaches setpoint on hot days, because total ventilation capacity (~0.6 /h) is the bottleneck, not the trigger schedule. NS-7 is re-scoped accordingly (see NS table): the actionable follow-up is NS-8 (understand *why* M3 is ineffective — mechanical aperture check first), after which a strategy revision can be re-evaluated on measured ground.

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

*§6.3 (10-minute LoRaWAN interval) and §8.3 (21-day campaign duration) approved by the operator on 2026-05-21. Firmware deployed (rc.1.5.2 / 2.1.1). Campaign data collected approx. 2026-06-04 to 2026-06-25 (21 days). Binary calibration complete 2026-06-27 — see §9.5. Per-bitmask M3 identification concluded non-identifiable open-loop 2026-06-27 — see §9.6. Binary model (`campaign-summer-2026/plant_calibrated_summer2026.json`) is the adopted calibration artifact.*

---

## Appendix A — Next steps before kick-off

| # | Item | Status |
|---|---|---|
| NS-1 | Implement §5 firmware changes (`LOG_SENSOR_HR`, `LOG_SUN`, rotation bump) | ✅ **COMPLETE** — shipped in `rc.1.5.2`; operational in 2.1.1. Verified 2026-06-27: source confirms `LOG_SENSOR_HR` and `LOG_SUN` enum + CSV mapping + emit sites; `SD_ROTATE_BYTES = 1 MB`, `SD_MAX_FILES = 30`, `SD_MIN_FILES = 5`, `SD_FREE_MIN_BYTES = 4 MB`. Note: emit site is in `data_manager.cpp` (Q6 handler) rather than `sensor_poll.cpp` as originally planned — architecturally equivalent. `t2_get_window_bitmask()` accessor implemented in `relay_controller.h`. |
| NS-1a | Update analysis-pipeline log parser for `SENSOR_HR` and `SUN` row types | ✅ **COMPLETE** — `model/campaign-summer-2026/plot_daily.py` dispatches on `SENSOR_HR_0` (T+RH), `SENSOR_HR_1` (wind), `SENSOR_HR_2` (bitmask) and `SUN`. Verified 2026-06-27 against 12 campaign log files: 61 714 `SENSOR_HR` rows decoded, 15 `SUN` rows decoded, 0 legacy `SENSOR` rows present. Legacy `SENSOR` row decoding preserved. |
| NS-2 | Configure LHT65-20 uplink interval to 600 s via TTN downlink (payload `01 00 02 58`, FPort 2) | ✅ **COMPLETE** — confirmed from campaign data (2026-06-27): 2 851 rows over 22 days; 92.6% of inter-uplink gaps are exactly 10 min (median 10.0 min, mean 11.1 min); longer gaps (20/30/40 min) are integer multiples consistent with LoRaWAN packet loss, not a longer base interval. TTN console screenshot not retained but data is conclusive. |
| NS-3 | Verify Phase 7 soak 14-day clean criterion (zero panics, zero WDT, zero coredump) | ✅ **COMPLETE** — firmware advanced through rc.1.5.x → 2.0.x → 2.1.1 without recorded panic or WDT resets. Soak gate passed before production deployment of 5C88. |
| NS-4 | First-pass calibration test run on summer-2026 campaign data | ✅ **COMPLETE** (2026-06-27) — `model/calibrate_plant_campaign.py` fit on 41 158 valid rows (Option A). T RMSE 2.69 °C vs 5.10 °C spring model; RH RMSE 6.73 % vs 16.74 %. DE converged at 143/300 iterations. Output: `campaign-summer-2026/plant_calibrated_summer2026.json` + `calibration_compare_summer2026.png`. **AC-9 not yet achieved** — per-bitmask `calibrate_plant_dynamic.py` still needed. |
| NS-5 | Implement `calibrate_plant_dynamic.py` with per-bitmask `ach_open[]` vector and train/validation split | ✅ **COMPLETE** (2026-06-27) — joint 7-param, two-stage, and constrained 6-param (`calibrate_plant_constrained.py`) fits all implemented. Physical priors accepted: M1=M2 (identical geometry); M3 dominates (171-step panel). `ach_m3` remains at lower bound due to controller-feedback confounding in all M3-open states — see §9.6 and §9.7. **Adopted artifact: constrained 6-param model** (`plant_calibrated_constrained_summer2026.json`, val RMSE 1.20 °C, 57 % within ±1 °C). *(Artifact superseded 2026-07-04 by the NS-6 re-calibration — see NS-6 row and §9.9.)* |
| NS-6 | M3 deliberate calibration test — 45–60 min M3-only open via LCD manual override | ✅ **COMPLETE** (2026-07-04) — two 60-min M3-only windows executed via LCD manual override (09:55–10:55 and 11:57–12:57 local). Doors open during much of both windows (Saturday farm activity) left one clean 17-min stretch + 103 valid `0b100` rows overall. Re-calibration with data extended to Jul 4 (val window Jun 19–25 unchanged): **`ach_m3` pins at any floor it is given — measured ≤ 0.05 /h, ≈ 0.3× ach_m1, refuting the 8.1× area-scaling estimate.** Adopted artifact: `plant_calibrated_constrained_summer2026_freem3.json` (val T RMSE 1.19 °C). Full analysis: §9.9. |
| NS-7 | Evaluate and implement independent M3 ventilation threshold (`t_thresh_m3`) in T6 | ⏸ **ON HOLD — premise refuted by NS-6** (2026-07-04). The case for opening M3 earlier assumed ach_m3 ≈ 8× ach_m1; measurement gives ≤ 0.3× — earlier M3 opening improves equilibrium temperature excess by only ~9 %. No passive strategy reaches setpoint on hot days at ~0.6 /h total ACH. Re-evaluate only after NS-8 explains M3's ineffectiveness (and if a mechanical fix raises its real aperture). The unaffected §9.8 findings (night-long −5 °C close hysteresis; non-orthogonal `hyst_t`) can be pursued separately. See §9.9. |
| NS-8 | Investigate why M3 (north-wall window, ~80 m²) is an ineffective ventilator | ⬜ **PENDING** (new, from NS-6) — ach_m3 ≤ 0.05 /h even with the roof windows open (wall-inlet → roof-outlet stack circuit) contradicts simple area scaling. (a) **Mechanical/geometric check first:** measure M3's actual open aperture at full 171 s travel — how wide does the wall really open, and where is the gap (top-hinged near the gutter = small stack height)? Photograph/measure on next site visit. (b) Note M3 faces **north = leeward** for prevailing SW winds; repeat the M3-only test on a windier day (>5 m/s, ideally N/NE wind) with door discipline to separate wind-driven from buoyancy exchange, §9.9 finding 4. (c) Check the controller T/RH sensor's position relative to the north wall — a sensor near M3 gets washed locally when the wall opens (fast local relief, slow bulk exchange), which would explain the visual fast-drop impression and the controller's step-3 behaviour (§9.9 robustness check). (d) Optional: smoke/tracer observation at the wall opening with M1+M2 open vs closed. Outcome decides whether NS-7 is revived. |

Campaign data collection complete (Jun 4 – Jul 4, 2026; originally Jun 4–25, extended for the NS-6 M3 test and heatwave coverage). Log data in `model/campaign-summer-2026/` shows continuous `SENSOR_HR` collection from 2026-06-04. Fill in dates at end of campaign:

| Kick-off | approx. 2026-06-04 (first `SENSOR_HR` log file) — confirm exact flash timestamp |
|---|---|
| Day-0 marker | (confirm from flash record) |
| Day-21 cutoff | approx. 2026-06-25 — confirm, or extend to day 28 if AC-6 bitmask-diversity check fails |
| SQL export start | = Day-0 marker |
| SQL export end | = Day-21 cutoff + 1 h |

