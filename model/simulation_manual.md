# Greenhouse Controller Simulation — User Manual

**Firmware model:** v1.16.19  
**Script:** `simulation.py`  
**Settings:** `settings.json`

---

## Overview

`simulation.py` is a faithful software model of the implemented greenhouse controller firmware.  It runs the same climate control logic as the device using historical weather data as input and produces time-series CSV output and a four-panel PNG plot.

The following firmware tasks are modelled:

| Firmware task | Simulated behaviour |
|---|---|
| **T5** `sensor_poll` | Sliding-window averaging of indoor T and RH (configurable window sizes) |
| **T6** `climate_control` | Graduated ventilation step algorithm with close-hysteresis guard and T/RH conflict resolution |
| **T3** `safety_monitor` | Wind speed threshold override and direction exclusion zone |
| **T2** `relay_controller` | Motor travel timing and post-open / post-close dwell enforcement |
| **T4** `data_manager` | Day/night setpoint selection using the NOAA sunrise/sunset algorithm (same as firmware `sunrise.cpp`) |

A first-order **plant model** computes indoor temperature and humidity at each time step from outdoor conditions, window configuration, solar heat gain, crop transpiration, and the greenhouse's effective heat capacity (`c_eff_mj_per_c`). Indoor T relaxes toward the ventilation equilibrium with time constant `tau_T = c_eff / (ACH × V × ρ × cp)`; AH relaxes with `tau_AH = 1 / ACH`. Setting `c_eff_mj_per_c = 0` recovers the previous instant-equilibrium model.

---

## Requirements

```
python >= 3.8
matplotlib   (optional — required for PNG plots only)
```

No other third-party packages are needed.

---

## Usage

```
python simulation.py <weather_csv> [settings_json]
```

### Arguments

| Argument | Required | Description |
|---|---|---|
| `weather_csv` | Yes | CSV file with outdoor conditions (see format below) |
| `settings_json` | No | JSON settings file (defaults to `settings.json` next to the script) |

If `settings_json` is not supplied, or the file does not exist, the firmware factory defaults (v1.16.19) are used for every parameter.

### Example

```bash
# Run with the bundled weather file and default settings:
python simulation.py airTemperature_2025-05-01_to_2025-09-01.csv

# Run with a custom settings file:
python simulation.py airTemperature_2025-05-01_to_2025-09-01.csv my_settings.json
```

---

## Input file format

### Weather CSV (`weather_csv`)

The file must have the following columns.  The header row is mandatory.

| Column | Required | Unit | Description |
|---|---|---|---|
| `dateTime` | Yes | — | Timestamp in `YYYY-MM-DD HH:MM:SS` format (UTC assumed) |
| `airTemperature` | Yes | °C | Outdoor air temperature |
| `airHumidity` | Yes | % | Outdoor relative humidity (0–100) |
| `windSpeed` | No | m/s | Wind speed (used for wind override if `wind_prot_en = 1`) |
| `windDirection` | No | ° | Wind direction 0–359 ° (used for direction exclusion zone) |

Rows are sorted by timestamp automatically.  Missing wind columns disable wind override simulation.

**Example rows:**

```
dateTime,airTemperature,airHumidity
2025-05-01 00:12:22,7.6,92.1
2025-05-01 00:42:22,7.0,91.8
2025-05-01 06:15:00,12.3,78.4
```

Measurements at arbitrary irregular intervals are accepted.  The simulation interpolates linearly between consecutive rows at every `dt` step (default 10 s).

---

## Settings file (`settings.json`)

The settings file is a JSON document with five top-level sections.  Any key that is absent falls back to the firmware factory default.

### Complete structure with all defaults

```json
{
  "climate": {
    "t_max_day":   28,    // Upper temperature threshold daytime (°C) — open above this
    "t_max_ngt":   20,    // Upper temperature threshold night (°C)
    "rh_min_day":  50,    // Lower RH bound daytime (%) — close if below
    "rh_max_day":  75,    // Upper RH bound daytime (%) — open if above
    "rh_min_ngt":  55,    // Lower RH bound night (%)
    "rh_max_ngt":  80,    // Upper RH bound night (%)
    "hyst_t":       3,    // Temperature hysteresis band (°C); step_width = hyst_t / 3
    "hyst_rh":      5,    // RH hysteresis band (%)
    "rh_ctrl_en":   1,    // 1 = RH control enabled, 0 = disabled (T only)
    "cr_priority":  0,    // 0 = T first, 1 = RH first, 2 = higher deviation wins
    "avg_win_t":    3,    // T averaging window (minutes)
    "avg_win_rh":   5     // RH averaging window (minutes)
  },
  "wind": {
    "wind_prot_en":  1,   // 1 = wind protection active
    "v_max":         6,   // Wind speed threshold (m/s) — override triggers at or above
    "dir_excl_low":  0,   // Direction exclusion zone low bound (°); 0 = disabled
    "dir_excl_high": 0    // Direction exclusion zone high bound (°); same = disabled
  },
  "motor": {
    "travel_m1":      21, // M1 full-travel time (s)
    "travel_m2":      21, // M2 full-travel time (s)
    "travel_m3":     171, // M3 full-travel time (s) — ridge/wall vent
    "dwell_open_m1": 120, // Post-open dwell M1 (s) — min time before closing again
    "dwell_open_m2": 120,
    "dwell_open_m3": 120,
    "dwell_close_m1":  0, // Post-close dwell M1 (s) — min time before opening again
    "dwell_close_m2":  0,
    "dwell_close_m3":  0
  },
  "system": {
    "poll_interval": 60,  // Sensor poll and control evaluation interval (s)
    "lat_deg":       52,  // Location latitude integer part (°N)
    "lat_frac":       0,  // Location latitude fractional part (×0.001 °)
    "lon_deg":        5,  // Location longitude integer part (°E)
    "lon_frac":       0   // Location longitude fractional part (×0.001 °)
  },
  "plant_file": "plant_general_crops.json"  // Path to the plant-model JSON (relative to the settings file)
}
```

The `plant_file` field references a separate JSON describing the physical greenhouse — air volume, vent ACHs, solar peak, transpiration, and effective heat capacity. This keeps the controller config (climate / wind / motor / system) cleanly decoupled from the building/crop description so each can be versioned and reused independently.

```json
// plant_general_crops.json
{
  "volume_m3":           2400.0, // Greenhouse air volume (m³)
  "ach_roof":               8.0, // ACH of each roof vent when fully open (h⁻¹)
  "ach_wall":              40.0, // ACH of wall vent when fully open (h⁻¹)
  "transpiration_kg_s":   0.010, // Crop transpiration moisture load (kg/s)
  "solar_peak_w":      20000.0, // Peak solar heat gain at noon, clear sky (W)
  "c_eff_mj_per_c":        30.0  // Air-coupled effective heat capacity (MJ/°C) — sets thermal-lag time constant. ~10 for empty greenhouse, ~30 for general crops, ~60 for dense canopy
}
```

Three plant files ship with the simulation:

| File | Use case |
|---|---|
| `plant_empty_greenhouse.json` | Empty greenhouse, bare floor, no crops (`c_eff = 10`) |
| `plant_general_crops.json` | General crops — moderate canopy + active soil (`c_eff = 30`) |
| `plant_calibrated.json` | Fit to live indoor sensor(s) by `calibrate_plant.py`; the file's `_comment` records which sensors were used |

Inline `"plant": { ... }` sections in a settings JSON are still honoured for backward compatibility; if both `plant` and `plant_file` are present, `plant` wins.

> **Note:** JSON does not support comments.  Remove `//` lines before use, or use `settings.json` / one of the plant files as the template (they have no comments).

---

## Output files

Both output files are written to the same directory as the input weather CSV.

### `results_<name>.csv`

Time-series CSV with one row per ~60 s of simulation time.

| Column | Unit | Description |
|---|---|---|
| `datetime` | UTC | Timestamp of this record |
| `elapsed_s` | s | Seconds from simulation start |
| `T_in_C` | °C | Indoor temperature (plant model) |
| `RH_in_pct` | % | Indoor relative humidity |
| `T_out_C` | °C | Outdoor temperature (from input CSV) |
| `RH_out_pct` | % | Outdoor relative humidity |
| `t_avg_C` | °C | T5 sliding-average temperature (integer, used by T6) |
| `rh_avg_pct` | % | T5 sliding-average RH (integer, used by T6) |
| `M1_state` | enum | M1 window state (`CLOSED`, `MOVING_OPEN`, `OPEN`, `MOVING_CLOSE`) |
| `M2_state` | enum | M2 window state (same enum as `M1_state`) |
| `M3_state` | enum | M3 window state (same enum as `M1_state`) |
| `step_t` | 0–3 | T6 temperature step |
| `step_rh` | −1–3 | T6 RH step (−1 = NEUTRAL = no RH demand) |
| `step_resolved` | 0–3 | T6 resolved ventilation step (drives window commands) |
| `is_daytime` | 0/1 | 1 = sunrise–sunset window active (NOAA algorithm) |
| `wind_override` | 0/1 | 1 = T3 wind override active (all windows commanded closed) |

### `results_<name>.png`

Four-panel plot (requires `matplotlib`):

1. **Temperature** — indoor T, outdoor T, `t_max_day` / `t_max_ngt` setpoints.  Night periods shaded.
2. **Humidity** — indoor RH, outdoor RH, `rh_max_day`, `rh_min_day`, `rh_max_ngt` setpoints.
3. **Window states** — M1, M2, M3 four-state trace (`CLOSED` / `MOVING_OPEN` / `OPEN` / `MOVING_CLOSE`) mirroring firmware `t2_get_window_states()`. Each motor's track has y-ticks `C ↑ ↓ O` at `CLOSED`, `MOVING_OPEN`, `MOVING_CLOSE`, `OPEN` respectively.
4. **Ventilation steps** — `step_t`, `step_resolved`; wind override period shaded in orange.

---

## Firmware model details

### Ventilation step algorithm (T6)

The step algorithm is a direct integer port of `climate_control.cpp`:

```
step_width = max(1, hyst_t // 3)         # integer floor division
deviation  = t_avg_int - t_max            # integer °C

if deviation <= 0:
    raw_step = 0
else:
    raw_step = (deviation + step_width - 1) // step_width   # integer ceiling

raw_step = clamp(raw_step, 0, 3)

# Close-hysteresis guard:
# Once step > 0, do not reduce to 0 until deviation <= -hyst_t.
# Hold at step 1 while in the deadband zone.
if current_step > 0 and raw_step == 0 and deviation > -hyst_t:
    return 1
```

With `hyst_t = 3` and `NUM_VENT_STEPS = 3`:
- `step_width = 1 °C`
- Step 1 (M1) opens at T > `t_max`
- Step 2 (M1+M2) opens at T > `t_max + 1`
- Step 3 (M1+M2+M3) opens at T > `t_max + 2`
- All windows close when T < `t_max - 3` (close-guard clears)

### RH step algorithm (T6)

| Condition | step_rh |
|---|---|
| `rh_ctrl_en = 0` | NEUTRAL (−1) — RH has no vote |
| `rh_avg > rh_max` | Graduated 1–3 (same algorithm as temperature) |
| `rh_avg < rh_min` | 0 — immediate full close (no graduation) |
| `rh_min ≤ rh_avg ≤ rh_max` | NEUTRAL (−1) |

### Conflict resolution (`cr_priority`)

| Value | Name | Rule |
|---|---|---|
| 0 | `CR_TEMP_FIRST` | Temperature step wins (default) |
| 1 | `CR_RH_FIRST` | RH step wins |
| 2 | `CR_DEVIATION` | Higher step wins (more ventilation) |

If both demand OPEN (step > 0), the higher step is always used regardless of `cr_priority`.  If one demands OPEN and the other CLOSE, `cr_priority` decides.

### Sliding average (T5)

```
win_samples = max(1, min(360, avg_win_min × 60 // poll_interval_s))
```

The averaging window is specified in **minutes** and converted to a sample count based on `poll_interval`.  Reducing `poll_interval` increases the sample rate; the window duration in minutes stays the same.

### Motor dwell (`dwell_open_m*`, `dwell_close_m*`)

After a window fully opens, the controller waits `dwell_open_s` seconds before accepting a CLOSE command.  This prevents immediate re-closing when a brief temperature or humidity exceedance triggers an open cycle.  Wind override bypasses the dwell and closes immediately.

### Day/night selection

Sunrise and sunset times are computed using the NOAA General Solar Position algorithm (same C implementation as `firmware/src/data_manager/sunrise.cpp`).  Set `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` to match your installation location.

---

## Typical simulation scenarios

### Scenario 1 — Validate temperature control
Run with default settings over the summer CSV.  Observe that M1 opens first when T exceeds `t_max_day`, M2 follows at `t_max_day + 1`, M3 at `t_max_day + 2`.

### Scenario 2 — Explore hyst_t sensitivity
Set `hyst_t = 1` (narrow band) vs `hyst_t = 5` (wide band) and compare actuations.  Narrower bands increase actuation count; wider bands tolerate more temperature exceedance before acting.

### Scenario 3 — RH-only control
Set `t_max_day = 50` (temperature never triggers) and `rh_ctrl_en = 1`.  All window activity is driven by humidity alone.  Observe how the graduated open responds to RH > `rh_max_day` and how full-close triggers at RH < `rh_min_day`.

### Scenario 4 — Conflict resolution
Set `t_max_day = 25` and `rh_min_day = 65`.  On a warm dry day, T demands OPEN while RH demands CLOSE.  Toggle `cr_priority` between 0 (T wins) and 1 (RH wins) and compare window behaviour.

### Scenario 5 — Wind override
Add `windSpeed` and `windDirection` columns to the CSV or set a low `v_max` (e.g. `3`) to force frequent overrides.  Observe that windows close immediately when override activates and the dwell is bypassed.

### Scenario 6 — Location-specific day/night
Change `lat_deg` / `lon_deg` to a Mediterranean location (e.g. lat 38, lon 15) and compare sunrise/sunset times and the resulting shift in which setpoint (`t_max_day` vs `t_max_ngt`) is active.

---

## Limitations

| Aspect | Current behaviour | Future improvement |
|---|---|---|
| Sensor placement | Indoor T/RH is the plant-model equilibrium value; no sensor lag or position offset | Add first-order sensor lag |
| Wind speed | Only from CSV columns; no turbulence model | Integrate measured wind gusts |
| Heating control | `t_min_day` / `t_min_ngt` are stored but heating actuators are not implemented | Add heating step algorithm when firmware supports it |
| Multi-compartment | Single-zone model | Extend to zoned greenhouses |
| Transpiration | Fixed `transpiration_kg_s`; not crop-stage dependent | Couple to a crop growth model |
| Motor alarm | Not simulated | Add fault-injection mode |

---

## File layout

```
simulation/
  simulation.py                              ← this script
  simulation_manual.md                       ← this document
  settings.json                              ← default settings (firmware v1.16.19)
  airTemperature_2025-05-01_to_2025-09-01.csv  ← example weather input
  results_<name>.csv                         ← generated output (time-series)
  results_<name>.png                         ← generated output (plot)
```
