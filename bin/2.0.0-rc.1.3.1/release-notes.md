# 2.0.0-rc.1.3.1 — temperature 0.1 °C precision fix

Patch release on top of rc.1.3. **Four-file C/C++ change** (struct definition + producer + consumer + history emitter) plus the version bump. Fixes the operator-reported "temperature always ends in .0" bug in the web GUI and `/api/history`.

Supersedes rc.1.3 as the Phase 7 soak candidate; the 14-day clock restarts at day 0.

## The operator's report

> *"in webgui, temperature is presented with 1 digit in the fraction but the measurement and the average value never presents other than .0. this is also the case in the sensor history. what is reason. I want to have correct values in the webgui."*

## Root cause

The web GUI's tile renderer uses `c.temp_c.toFixed(1)` — which would normally produce `21.4`, `22.7` etc. But the value coming from `/api/status` (and `/api/history`) was always an integer °C value with a `.0` suffix. The decimal was being discarded earlier in the pipeline:

```
FG6485A sensor  →  tm.temperature_c (float, 0.1 °C resolution)
                       ↓
T5 sensor_poll  →  lroundf(...) → reading.temperature_c (int16, whole °C)  ← LOSS
                       ↓
T4 data_manager →  out->t_c10 = meas.temperature_c * 10  (× 10 too late)
                       ↓
status_json     →  "temp_c": 21.0      ← always ".0"
                       ↓
app.js          →  c.temp_c.toFixed(1) → "21.0"
```

The FG6485A driver header (`drivers/FG6485A/src/fg6485a.h:160`) is explicit:
```c
float temperature_c;  /**< Temperature in °C (resolution 0.1 °C, range -40…120). */
```

The sensor IS providing 0.1 °C resolution. T5 was throwing it away.

The web_server `/api/history` code carried a self-aware comment perpetuating the bug:
> *"sensor_reading_t stores temperature as whole-°C integers — the sensor delivers integer °C and we don't upsample. Emit `%d.0` so the dashboard's .toFixed(1) renders '21.0' rather than '21'."*

The first claim ("sensor delivers integer") is wrong. The `%d.0` workaround was the bug's tombstone.

## The fix

Additive — added tenths-precision fields to `sensor_reading_t` alongside the existing integer ones, same pattern as `wind_speed_ms10` (which has worked correctly since 1.20.x):

```c
/* firmware/src/types/app_types.h — added rc.1.3.1 */
typedef struct {
    int16_t  temperature_c;     /* whole °C — kept for climate_control,
                                 * LCD render, LOG_SENSOR value_a */
    /* ... existing fields preserved ... */
    int16_t  temperature_c10;   /* × 10 (e.g. 234 = 23.4 °C) — NEW */
    int16_t  t_avg_c10;         /* × 10 sliding-average — NEW */
    /* ... */
} sensor_reading_t;
```

T5 populates both fields from the same float source. T4 reads the c10 fields verbatim into `status_snapshot_t.t_c10` / `t_avg_c10` (instead of the previous `* 10` lie). `/api/history` emits `%d.%d` from the c10 fields.

**Result**: the canonical JSON now carries genuine 0.1 °C precision through the GUI + WS + history pipeline. `21.4 °C` reads as `21.4 °C`. The decimal is real.

## Zero-blast-radius for the rest of the system

The integer `temperature_c` / `t_avg_c` fields remain in `sensor_reading_t`. Anywhere that doesn't benefit from fractional precision keeps using them:

| Consumer | Field used | Why integer is fine |
|---|---|---|
| `climate_control.cpp` (T6 setpoint compare) | `meas.t_avg_c` | Setpoints are whole °C (T_max_day, T_max_ngt); fractional compare adds no decision value |
| `ui_display.cpp` (LCD render) | `meas.temperature_c` | LCD has 16 chars per row; `Temp:23 °C` is operator-readable. Adding `.4` would clutter without benefit |
| `data_manager.cpp:792` (LOG_SENSOR Q3 row) | `r->t_avg_c` | **SD log format preserved** — the LOG_SENSOR row's `value_a` is whole °C as documented in the log-parser, so no log-format break, no operator-side parser update needed |

## What did NOT change

- LOG_SENSOR rows in the SD log — byte-identical format. Log parser continues to work without changes.
- LCD render — still "Temp: 23 °C" in whole degrees.
- Climate-control setpoint comparison — still operates on whole °C.
- Wind / humidity formatting — unchanged (humidity GUI uses `.toFixed(0)` so the integer storage is already correct; wind has been `_ms10` since 1.20.x).
- All rc.1.1 / rc.1.2 / rc.1.2.1 / rc.1.3 fixes — preserved verbatim.

## Build delta vs rc.1.3

| Metric | rc.1.3 | rc.1.3.1 | Delta |
|---|---:|---:|---:|
| Firmware bin | 1 351 881 B | (build-pending) | small uptick from 2 new int16 struct fields + a few snprintf format-char bytes |
| RAM static | 60 568 B | (build-pending) | +4 B expected (two int16_t entries added to the singleton `sensor_reading_t` snapshot) |

Full flash usage still well under 65 % of the 2 MB OTA bank.

## Verification

After deploy, pull `/api/status` and the GUI should show non-`.0` temperature values matching the FG6485A's actual reading. The bench's prior runs at `T=17 degC` will now appear as `17.X` where X reflects the real fraction. The same change ripples into `/api/history`.

## Phase 7 soak — clock reset

Day 0 restarts against rc.1.3.1. Same acceptance criteria as rc.1; all prior fixes preserved.
