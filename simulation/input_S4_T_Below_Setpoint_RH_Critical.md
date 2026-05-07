# input_S4_T_Below_Setpoint_RH_Critical.csv

## Scenario overview

A short 4-hour outdoor time series (14 340 s, 240 rows at 60 s resolution) depicting a cold,
fog-saturated morning.  Temperature stays in the narrow band 5.1 – 7.6 °C throughout — far
below both `t_max_day = 28 °C` and `t_max_ngt = 20 °C` — while RH remains stubbornly at
95 – 100 %, well above both night and day upper RH setpoints.

| Property | Value |
|---|---|
| Duration | ~4 h |
| T_in_C range | 5.1 – 7.6 °C |
| RH_in_pct range | 95.4 – 100 % |
| Start conditions | 7.6 °C / 100 % RH |
| End conditions | 5.9 °C / 97.1 % RH |

## Why this scenario is interesting

**1. Pure RH-only ventilation — temperature never triggers**
Because indoor temperature is at most 7.6 °C, the temperature step is always 0 throughout
the run.  All window activity is driven exclusively by the RH controller.  With RH at 95–100 %
and `rh_max_ngt = 80 %` (or `rh_max_day = 75 %`), step_rh should be 1 – 3 continuously.
This isolates the RH branch of the control algorithm completely, making it the cleanest test
for verifying `rh_ctrl_en`, `rh_max`, and the graduated RH step logic in isolation.

**2. Cold-air ventilation conflict — comfort vs. crop protection**
Opening windows at 6 °C is agronomically undesirable even though humidity demands it.  In the
current firmware, heating control is not implemented, so the controller follows the RH demand
regardless.  Simulating this scenario makes the limitation concrete and quantifiable:
the results CSV will show windows opening at sub-optimal temperatures, which motivates
adding a `t_min` inhibit guard to the RH branch once heating control is available.

**3. Dwell timer behaviour under continuous demand**
Because the RH trigger condition never clears (humidity stays above setpoint for 4 h straight),
the dwell timer logic is exercised at every poll cycle.  After the initial open, `dwell_open_s`
prevents immediate re-close; since the RH demand persists, no close is ever issued.  This
confirms that the dwell guard correctly handles a steady-state open condition without chattering.

**4. Short scenario — suitable for rapid parameter sweeps**
At only 4 hours and 240 rows the file runs in a few seconds.  It is ideal for iterating over
`rh_max_ngt`, `rh_ctrl_en`, `cr_priority`, and `avg_win_rh` values to observe how each
parameter affects the onset and graduation of RH-driven ventilation, without waiting for a
full 24-hour simulation to complete.

**Typical assertions to make**
- `step_t = 0` for every record in the output CSV.
- `step_rh >= 1` for every record where RH_avg > `rh_max` (night or day, depending on `is_daytime`).
- `step_resolved = step_rh` throughout (no T contribution to resolve).
- Setting `rh_ctrl_en = 0` results in `step_rh = NEUTRAL` and zero window actuations.
