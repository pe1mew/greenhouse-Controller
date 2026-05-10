# input_S2_High_Humidity_Mild_Day.csv

## Scenario overview

A 24-hour outdoor time series (86 340 s, 1 440 rows at 60 s resolution) representing a mild,
overcast or humid summer day.  Temperature stays moderate (peak 32 °C), while relative
humidity is persistently high — starting saturated at 100 % and rarely dropping below 48 %.

| Property | Value |
|---|---|
| Duration | ~24 h |
| T_in_C range | 8.4 – 32.0 °C |
| RH_in_pct range | 48.2 – 100 % |
| Start conditions | 10.0 °C / 100 % RH |
| End conditions | 16.2 °C / 91.7 % RH |

## Why this scenario is interesting

**1. Humidity-driven ventilation dominates**
For most of the daytime, indoor RH exceeds `rh_max_day = 75 %` while temperature remains
below or only slightly above `t_max_day = 28 °C`.  This is the primary scenario for
exercising RH-led graduated ventilation: step_rh reaches 1, 2, or 3 while step_t is 0
or 1.  The `cr_priority = 0` (T first) default returns the higher-deviation step when both
demand open, so both control channels are active simultaneously.

**2. Moderate T and high RH — conflict unlikely but possible**
Around t ≈ 43 200 s (12 h), temperature briefly dips below `t_max_day` while RH is still
above `rh_max_day`.  At that moment RH demands open while T demands nothing (step_t = 0).
With `cr_priority = 0` (T first), the RH demand should still win because T is neutral — this
is a boundary case in `vent_resolve_conflict()` that this scenario reliably exercises.

**3. RH never falls below rh_min — no dry-air conflict**
Unlike the solar-gain scenario, RH does not drop below `rh_min_day`.  This means the
full-close RH branch (step_rh = 0) is never triggered.  Running both scenarios back-to-back
shows the model switching between the two distinct RH branches cleanly.

**4. Extended high-humidity night**
From late afternoon onwards, RH climbs back to 91 % and above.  During night hours the active
setpoints shift to `rh_max_ngt = 80 %`, which is lower than the outdoor RH.  This exercises
the NOAA day/night switchover and demonstrates that the night RH setpoints — not the day ones
— determine whether windows open after sunset.

**Typical assertions to make**
- Windows open driven by RH alone during the period when T_avg < `t_max_day` and RH > `rh_max_day`.
- `step_resolved` equals `step_rh` when step_t = 0 and step_rh > 0.
- After the NOAA sunset transition, `is_daytime` changes from 1 to 0 and the active RH upper
  bound switches from `rh_max_day` to `rh_max_ngt`.
- Dwell timer prevents rapid repeated open/close cycles during the RH fluctuations around
  t ≈ 22 560 s and t ≈ 72 180 s.
