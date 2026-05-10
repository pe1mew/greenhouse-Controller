# input_Daytime_Solar_Gain.csv

## Scenario overview

A 24-hour outdoor time series (86 340 s, 1 440 rows at 60 s resolution) captured on a clear
spring or early-summer day.  The greenhouse starts the night very cold and saturated, then
receives strong solar irradiance through the morning, reaching a peak indoor temperature of
**34.2 °C** around midday before cooling back towards dusk.

| Property | Value |
|---|---|
| Duration | ~24 h |
| T_in_C range | 5.1 – 34.2 °C |
| RH_in_pct range | 32.9 – 100 % |
| Start conditions | 7.6 °C / 100 % RH |
| End conditions | 10.8 °C / 93.5 % RH |

## Why this scenario is interesting

**1. Solar-driven temperature climb — graduated ventilation steps**
The outdoor temperature rises continuously from ~6 °C at dawn to a mid-day peak above 34 °C.
This exercises the full ventilation step ladder (steps 1, 2, 3) in sequence as T_in crosses
`t_max_day`, `t_max_day + step_width`, and `t_max_day + 2×step_width`.  The step-up and
step-down sequence — including the close-hysteresis guard — can be validated against the
expected thresholds.

**2. RH drops to below rh_min during peak heat**
As temperature peaks, RH falls to ~33 %.  With `rh_min_day = 50 %` (default), the RH
controller issues a full-close demand (step_rh = 0, no graduation) while the temperature
controller simultaneously demands maximum ventilation (step_t = 3).  This is a direct
conflict that exercises `cr_priority` (default: T wins), making this scenario the primary
test case for the T/RH conflict-resolution logic.

**3. Spike events — transient response**
Around t = 64 440 s (≈17.9 h) the temperature jumps from 22.9 °C to 32.4 °C and back within
two poll cycles, and RH spikes correspondingly.  These abrupt transitions test whether the
sliding-average filter (T5) attenuates the spikes before T6 reacts, and whether the
dwell timer prevents unnecessary window churn.

**4. Full RH swing (100 % → 33 % → 100 %)**
The full humidity sweep from saturated through critically dry and back covers every RH control
branch: neutral (within band), open demand (above rh_max), and full-close demand (below rh_min).

**Typical assertions to make**
- M1 opens first at T_avg ≥ `t_max_day`; M2/M3 follow at each additional step_width increment.
- No close command is issued while T_avg is within the hysteresis deadband.
- When RH < `rh_min_day` and T is above setpoint, `step_resolved` equals `step_t` (T wins).
- The peak temperature in the results CSV does not exceed what the fully-open ACH model predicts.
