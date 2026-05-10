# input_S5_Motor_Stall_M2.csv

## Scenario overview

An 8-hour outdoor time series (28 740 s, 480 rows at 60 s resolution) covering a morning
warm-up from cold and saturated conditions to a very hot afternoon peak of 35.4 °C.  The name
reflects the original design intent: this profile was constructed to drive the controller to
step 2 (M1 + M2 open) for an extended period, then simulate the effect of M2 stalling while
the temperature continues to climb.

| Property | Value |
|---|---|
| Duration | ~8 h |
| T_in_C range | 5.1 – 35.4 °C |
| RH_in_pct range | 57.1 – 100 % |
| Start conditions | 7.6 °C / 100 % RH |
| End conditions | 35.4 °C / 98.4 % RH |

## Why this scenario is interesting

**1. Rapid temperature ramp — all ventilation steps activated in sequence**
Temperature rises from 7.6 °C to 35.4 °C within 8 hours — a climb of nearly 28 °C.  This
drives the controller through all three ventilation steps (M1 → M1+M2 → M1+M2+M3) in a
relatively short window, making it easy to verify that each step threshold is crossed at the
correct T_avg value and that the step-up timing matches `poll_interval`.

**2. Extended operation at maximum ventilation**
The end of the series sits at 35.4 °C — 7 °C above `t_max_day = 28 °C` — which forces
step_resolved = 3 and all three motor channels commanded open.  Running the simulation to
this extreme tests the plant model's ceiling: without sufficient ACH the indoor temperature
diverges; with all vents open the model should reach a bounded steady state.

**3. Motor stall simulation (manual extension)**
The scenario is specifically designed to be modified: by editing `travel_m3` to a value
shorter than M3's actual full-travel time, M3 will report "open" before the ridge vent has
fully opened.  Observing the effect on indoor temperature in the results CSV quantifies
how much cooling capacity is lost when the largest vent (ACH_wall = 40 h⁻¹) does not
fully open — a direct sensitivity test of the `ach_wall` plant parameter.

Alternatively, setting `travel_m2 = 0` in settings.json effectively disables M2, simulating
a stalled motor at position 0.  The controller still commands M2 open but the plant model
sees M2_open = 0, showing the temperature response when step 2 is available only on paper.

**4. High-humidity fluctuations mid-morning**
Between t ≈ 19 500 s and t ≈ 22 000 s, RH oscillates between 72 % and 100 % in rapid
alternation while temperature is rising through the 20–25 °C range.  Both T and RH controllers
are simultaneously active during this window.  The rapid RH swings test whether the
`avg_win_rh` sliding average correctly damps short-duration spikes and prevents spurious
full-close (step_rh = 0) demands during an otherwise normal warm-up.

**5. Short enough for interactive exploration**
At 8 hours and 480 rows the simulation completes in seconds.  It is suitable for quick
sensitivity runs changing `hyst_t`, `avg_win_t`, `travel_m*`, or `dwell_open_s` to observe
how each parameter shifts the moment windows open or the step that is active during the
critical high-temperature period at the end of the series.

**Typical assertions to make**
- `step_t` transitions 0 → 1 → 2 → 3 as T_avg crosses successive step thresholds.
- With default `travel_m2 = 21 s`, M2_open = 1 for the entire duration that step_resolved >= 2.
- Setting `travel_m2 = 0` (stall): M2_open remains 0; indoor temperature in the results CSV
  is visibly higher during the step-2 and step-3 periods than with the default travel time.
- The dwell timer (120 s) prevents M1 from closing immediately if temperature briefly dips
  back below `t_max_day` during the mid-morning RH oscillations.
