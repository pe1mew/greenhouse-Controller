# input_S3_Full_24h_Day-Night_Cycle.csv

## Scenario overview

A 24-hour outdoor time series (86 340 s, 1 440 rows at 60 s resolution) that forms a clean,
nearly symmetric day/night cycle.  The greenhouse begins and ends the day at a similar cool
temperature (~11 °C / 95–100 % RH), passes through a sharp midday peak of 33.6 °C, and then
returns to night-time conditions.  The start and end states are nearly identical, making this
the best baseline for steady-state or repeated-cycle testing.

| Property | Value |
|---|---|
| Duration | ~24 h |
| T_in_C range | 9.8 – 33.6 °C |
| RH_in_pct range | 36.0 – 100 % |
| Start conditions | 11.3 °C / 100 % RH |
| End conditions | 11.6 °C / 95.1 % RH |

## Why this scenario is interesting

**1. Complete day/night setpoint switchover — NOAA algorithm exercise**
Because the cycle begins cold and humid (night), warms through day, and cools back to night,
both the day setpoints (`t_max_day`, `rh_max_day`, `rh_min_day`) and the night setpoints
(`t_max_ngt`, `rh_max_ngt`, `rh_min_ngt`) are active at distinct, well-separated periods.
The `is_daytime` flag transitions twice during the run (sunrise and sunset).  This is the
definitive test for the NOAA sunrise/sunset algorithm and for correct setpoint selection
throughout the full diurnal cycle.

**2. Night-time over-humidity requiring window action at low temperature**
Before sunrise and after sunset, RH is 95–100 %, well above `rh_max_ngt = 80 %`.  The
controller must open windows in darkness to reduce humidity — but the outdoor temperature
at that time is only ~10 °C.  This tests that the night-time RH upper bound correctly triggers
ventilation independently of the temperature state.

**3. Symmetric profile enables quantitative energy bookkeeping**
Because start ≈ end (11.3 °C / 100 % vs 11.6 °C / 95 %), integrating the ACH over 24 h gives
a closed heat and moisture balance.  Deviations from symmetry in the results CSV indicate
model drift or numerical error in the plant model.

**4. Temperature spike at dusk (t ≈ 68 280 s)**
A brief spike of 22.9 → 25.3 → 22.6 °C appears near sunset (≈19 h).  At that moment the
algorithm is either transitioning from day to night setpoints or is in the hysteresis deadband.
This edge case tests whether the close-hysteresis guard correctly holds windows open during a
transient exceedance immediately before the day/night setpoint change.

**Typical assertions to make**
- `is_daytime = 1` only between the NOAA-computed sunrise and sunset for the configured
  latitude/longitude and simulation date.
- Night ventilation (steps > 0) occurs when RH > `rh_max_ngt` and T is below `t_max_ngt`.
- The daytime temperature peak drives a higher resolved step than the night-time RH demand
  alone would produce.
- Repeating the simulation back-to-back (concatenating the file with itself) should produce
  nearly identical window-actuation counts per cycle, confirming no accumulating state drift.
