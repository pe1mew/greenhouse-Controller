# `graded`: a first candidate for mode 2's law

| Field | Value |
|---|---|
| Law | [`drivers/ventModel/src/vent_model_graded.cpp`](../../drivers/ventModel/src/vent_model_graded.cpp), `graded` v1, with 15 host tests (`pio test -e native`) |
| Status | **A candidate, 2026-09-19.** Which law mode 2 runs is the plan's open decision 9 ([`integrateWindowPositionSensor.md`](../../design/integrateWindowPositionSensor.md) §10). This is the simplest candidate the plan names, a proportional map with a rate limit. Its constants are provisional |
| Evidence | The closed-loop simulator only: 5C88's logged weather, 2026-06-05 to 09-16, on both adopted plants, with today's firmware (2.12.0's T2, T17 and T6 as built, 045a39c and 9c53be7; re-run 2026-09-21), across a range of M3 airflow curves. No rig run, no greenhouse run |
| Regenerate | `python model/closedloop/law_compare.py` (about 10 minutes); the sweep with `--define`, below; the walkthrough with `graded_walkthrough.py` (seconds) |

## Short answer

In the simulator, over the summer, on both adopted plants:
- **North-wind days: the swing drops from 3.0–3.2 to 1.5–2.2 °C, at every airflow curve tried.** That is the robust result. It is also where the plants are weakest: they under-state the logged north-wind swing, 3.9 °C. So it is a lead to confirm on the greenhouse, not yet a finding.
- **Other days: no clear change,** between −0.5 and +0.4 °C depending on the plant and the airflow curve.
- **Heat: no gain.** Hours at or above 31 °C stay at 2.3–2.4 a day with proportional airflow. If a part-open M3 lets through less air (exponent 2), they rise to 2.6. Where the stepped law throws M3 wide — a reading of 30.5 °C, which the ladder reads as 31 — this law has it half open.
- **The motor: about 2.8 times the starts, the same running time.** M3 makes about 22 drives a day against the stepped law's 7.7, yet runs 21.9 minutes a day against 22.6. The contract asks that a continuous law not multiply the starts. No setting tried meets that; see the sweep below. Whether starts or running time wear the motor is unmeasured (contract §7), so that is a decision to make.

## How it behaves over time

At 5C88's daytime settings — `t_max` 28 °C, `hyst_t` 5, `deadzone_m3` 20 mm, `min_intv_m3` 600 s. Both tables below are printed by `python model/closedloop/graded_walkthrough.py`, which drives this law through the emulated firmware, so they follow any change to either.

**The law reads T5's average, never the sensor directly.** The step ladder compares that average **rounded to whole degrees**; M3's map uses it at 0.1 °C. A reading of 30.5 °C is therefore 31 to the ladder, and that is where the two halves meet.

### M1 and M2: the stepped ladder, unchanged

The step is `ceil(deviation / step_width)` capped at 3, where `deviation` is the rounded average less `t_max`, and `step_width` is `max(hyst_t / 3, 1)` — 1 °C at these settings.

| T_avg, °C | as the ladder reads it | Step | M1 | M2 | M3's demand |
|---|---|---|---|---|---|
| 28.0 | 28 | 0 | hold | hold | 0 % |
| 28.5 | 29 | 1 | open | hold | 0 % |
| 29.0 | 29 | 1 | open | hold | 0 % |
| 29.5 | 30 | 2 | open | open | 0 % |
| 30.0 | 30 | 2 | open | open | 25 % |
| 30.5 | 31 | 3 | open | open | 50 % |
| 31.0 | 31 | 3 | open | open | 75 % |
| 31.5 | 32 | 3 | open | open | 100 % |
| 32.0 | 32 | 3 | open | open | 100 % |
| 32.5 | 33 | 3 | open | open | 100 % |

**Closing is not the mirror of opening.** Once any step is live, the law does not return to step 0 until the average is a whole `hyst_t` below `t_max` — 23 °C here. M2 shuts again at 29, but M1 stays open long after the greenhouse has cooled. That close guard is mode 1's, the conflation F8 quantified as a bad trade, and this candidate inherits it untouched.

### M3: proportional, and gated behind step 2

Aperture = (T_avg − `t_max` − 1.5 °C) ÷ 2.0 °C, clamped to 0–100 %: 50 % per °C, shut at 29.5, a quarter at 30.0, half at 30.5, fully open from 31.5. **Half open is exactly where mode 1 throws it wide**, because a reading of 30.5 rounds to 31, which is mode 1's step 3. Humidity that demands step 3 on its own opens M3 fully instead.

The gate means M3 never leads: both roof windows are open before the 80 m² wall flap moves at all.

Five things then slow it down — the law's three (§9 item 3 is the table of record) and the caller's two:

- **at least 10 %,** or the correction is not worth a motor start;
- **at most 25 % per move,** about 44 s of travel in production;
- **no move within 10 minutes** of M3's last drive, which covers the loop's dead time;
- **T6 drops a target** within the deadband (20 mm, 1.3 %) of where M3 rests;
- **T6 holds a target** until `min_intv_m3` has passed since the last drive.

T2 then cuts the relay a learned lead early, so the leaf coasts onto the target ([`README.md`](README.md), "A linear M3").

### A morning, minute by minute

A synthetic ramp: 27 °C, then 1 °C per 20 min to 32, eighty minutes there, and back down at the same rate. T6 wakes every 30 s, so every time below sits on that grid, and a window's two rows are its travel.

| Time, min:s | T_avg, °C | Step | What happens |
|---|---|---|---|
| 0:00 | 27.0 | 0 | M1 shut; M2 shut; M3 shut |
| 50:00 | 28.5 | 1 | M1 moving |
| 50:30 | 28.5 | 1 | M1 open |
| 70:00 | 29.5 | 2 | M2 moving |
| 70:30 | 29.5 | 2 | M2 open |
| 73:30 | 29.7 | 2 | T6 asks M3 for 10 %; M3 moving |
| 74:00 | 29.7 | 2 | M3 part-open at 10 % |
| 84:00 | 30.2 | 2 | T6 asks M3 for 35 %; M3 moving |
| 85:00 | 30.2 | 2 | M3 part-open at 35 % |
| 95:00 | 30.8 | 3 | T6 asks M3 for 60 %; M3 moving |
| 96:00 | 30.8 | 3 | M3 part-open at 60 % |
| 106:00 | 31.3 | 3 | T6 asks M3 for 85 %; M3 moving |
| 107:00 | 31.4 | 3 | M3 part-open at 85 % |
| 117:00 | 31.9 | 3 | T6 asks M3 for 100 %; M3 moving |
| 120:00 | 32.0 | 3 | M3 open |
| 213:30 | 31.3 | 3 | T6 asks M3 for 90 %; M3 moving |
| 214:00 | 31.3 | 3 | M3 part-open at 90 % |
| 224:00 | 30.8 | 3 | T6 asks M3 for 65 %; M3 moving |
| 225:00 | 30.8 | 3 | M3 part-open at 65 % |
| 235:00 | 30.2 | 2 | T6 asks M3 for 40 %; M3 moving |
| 236:00 | 30.2 | 2 | M3 part-open at 40 % |
| 246:00 | 29.7 | 2 | T6 asks M3 for 15 %; M3 moving |
| 247:00 | 29.6 | 2 | M3 part-open at 15 % |
| 250:30 | 29.5 | 1 | M2 moving |
| 251:00 | 29.4 | 1 | M2 shut |
| 257:00 | 29.1 | 1 | T6 asks M3 for 0 %; M3 moving |
| 260:00 | 29.0 | 1 | M3 shut |

Reading it: M1 opens when the average first rounds to 29 and M2 at 30. M3 starts three and a half minutes after M2, at 10 %, the first correction that clears the smallest move. From there it climbs in 25 % steps, one per ten minutes — the rate limit and the hold in series — and reaches the open end at 117 min. **It then stands still for an hour and a half**, because the demand is met and nothing asks it to move. Coming down it unwinds the same way, shuts when the step falls below 2, and M1 is still open at the end, waiting for 23 °C.

**Mode 1 on the same ramp opens M3 fully at 93:00 and shuts it at 233:30: two drives against ten,** with the same single M1 drive and two M2 drives. That is the trade this candidate makes — the ventilation follows the temperature instead of stepping to the end of its travel, and the motor pays for it. It is also why `min_intv_m3` exists as a floor under whatever law runs next ([`linearDwell.md`](linearDwell.md)).

## Results

`law_compare.py` closes the loop over 2026-06-05 to 09-16 twice per plant: the stepped law with a binary M3, and `graded` with a linear one. Both use today's firmware and 5C88's settings. The swing is the campaign's measure, the median over days of the temperature range between successive M3 openings. The fluctuation is the daytime spread of the temperature around its own 60-min average. The script's docstring defines each row. North-wind days are those with most of the logged M3-open time in wind from 315–45°: 25 days, against 57 with other wind.

### The summer, proportional airflow

| | Logged | Primary, stepped | Primary, graded | Second, stepped | Second, graded |
|---|---|---|---|---|---|
| Hours at or above 31 °C a day | 2.3 | 2.3 | 2.4 | 2.3 | 2.4 |
| Daytime mean, °C | 27.7 | 27.4 | 27.5 | 27.4 | 27.5 |
| **Swing, °C** | 3.1 | 2.7 | **2.0** | 2.8 | **2.4** |
| … north-wind days | 3.9 | 3.2 | **1.8** | 3.0 | **2.2** |
| … other-wind days | 2.2 | 2.2 | 2.2 | 2.4 | 2.5 |
| Fluctuation, °C | 0.7 | 0.7 | 0.6 | 0.7 | 0.6 |
| **M3 drives a day** | 8.1 | 7.7 | **21.6** | 7.5 | **21.3** |
| M3 motor minutes a day | 19.9 | 22.6 | 21.9 | 22.0 | 21.7 |
| M3 openings a day, from shut | 3.9 | 3.8 | 3.1 | 3.7 | 3.2 |
| M3 open hours a day | 4.8 | 5.3 | 6.7 | 5.2 | 6.6 |
| M3 mean opening, % | 19.8 | 21.7 | 19.6 | 21.3 | 19.2 |
| M1+M2 drives a day | 15.2 | 12.2 | 12.1 | 12.3 | 11.9 |

T6 dropped or deferred no target: `graded` does not ask for a move within the deadband, and its own 10-minute hold is as long as the shipped minimum interval, `min_intv_m3` = 600 s ([`linearDwell.md`](linearDwell.md)), so the interval never binds it.

**Re-run 2026-09-21 against 2.12.0 as built (045a39c and 9c53be7).** What changed in the emulation:
- T2 now cuts a targeted stop one learned lead early, and the leaf coasts onto its target.
- T17 reads where the leaf rests.
- In mode 2 both of M3's dwells are `min_intv_m3`.
- T6 tells the law ABORTED when a drive it did not command took M3.
- The CLOSE_ALL sweep now runs at reboots and at STANDBY's end, with T6 paused meanwhile, as today's firmware does.

Against the first run, 2026-09-19:
- **Primary plant:** other-wind days moved from 2.1 to 2.2 °C, and no other swing cell by more than 0.1.
- **Second plant:** the swing moved from 2.3 to 2.4 °C, and north-wind days from 1.8 to 2.2 °C.
- **Where it came from:** part came with the stop rule and part with the sweep. The second plant's north-wind days moved from 1.8 to 2.0 °C with the first and from 2.0 to 2.2 °C with the second. Switched back to the old reboot handling, the new code reproduces the 045a39c run exactly (checked on the primary plant), so everything since then is the sweep. It matters most on the forced-test days: today's firmware would close every window at each STANDBY exit, and `graded` then starts afresh behind its hold.
- **The motor** runs about a second longer per targeted drive, because the stop now comes a lead short of the target instead of up to a deadband short.
- **The settled stops** land within 0.1 % of their target on average (Jul 13–29: 316 stops, the worst 0.6 %). The earlier emulation left them up to the deadband (1.3 %) short.

### Across the airflow range

How much air a part-open M3 lets through is unmeasured, so the comparison runs three curves: M3's airflow as its opening to the power 0.5 (more air early), 1 (proportional, as fitted) and 2 (less air early). Stepped → graded:

| Airflow exponent | Swing, primary / second | North wind, primary / second | Other wind, primary / second | Hours ≥ 31 °C a day, primary / second | M3 drives a day |
|---|---|---|---|---|---|
| 0.5 | 2.7 → 1.8 / 2.7 → 1.8 | 3.1 → 1.8 / 3.0 → 1.6 | 2.2 → 1.7 / 2.4 → 1.9 | 2.3 → 2.2 / 2.3 → 2.2 | 7.7 → 21.3 |
| 1 | 2.7 → 2.0 / 2.8 → 2.4 | 3.2 → 1.8 / 3.0 → 2.2 | 2.2 → 2.2 / 2.4 → 2.5 | 2.3 → 2.4 / 2.3 → 2.4 | 7.7 → 21.6 |
| 2 | 2.8 → 2.3 / 2.8 → 2.7 | 3.2 → 1.6 / 3.1 → 1.5 | 2.3 → 2.3 / 2.5 → 2.9 | 2.3 → 2.6 / 2.4 → 2.6 | 7.7 → 22.1 |

The north-wind gain holds at every curve and on both plants. The other-wind result and the heat hours turn with the curve, so they stay open until the curve is measured.

### The rate limit: fewer starts cost swing

The primary plant, proportional airflow, `graded` with its hold time and smallest correction varied (`M3_HOLD_MS`, `M3_MOVE_MIN_X10`); everything else as v1. `min_intv_m3` is 0 here, so the law's own hold is what spaces the moves:

| Hold | Smallest correction | Swing | North wind | Other wind | Hours ≥ 31 °C | M3 drives a day | Motor minutes a day |
|---|---|---|---|---|---|---|---|
| stepped | — | 2.7 | 3.2 | 2.2 | 2.3 | 7.7 | 22.6 |
| 5 min | 10 % | 1.7 | 1.4 | 1.8 | 2.3 | 29.3 | 26.7 |
| **10 min (v1)** | **10 %** | **2.0** | **1.8** | **2.2** | **2.4** | **21.6** | **21.9** |
| 15 min | 10 % | 2.3 | 1.9 | 2.4 | 2.4 | 17.2 | 18.7 |
| 5 min | 20 % | 2.0 | 1.8 | 2.1 | 2.3 | 20.9 | 24.3 |
| 10 min | 20 % | 2.3 | 2.1 | 2.4 | 2.3 | 17.2 | 20.3 |
| 15 min | 20 % | 2.2 | 1.9 | 2.7 | 2.4 | 14.8 | 17.9 |

- **v1's 10 min** covers the loop's dead time: the reading's lag, T5's average and the move itself come to about 6–8 min. It keeps about 70 % of 5 min's damping (0.7 of the 1.0 °C it takes off the stepped swing) with a quarter fewer starts.
- **Gentler settings do worse than the stepped law on other-wind days** (2.4–2.7 against 2.2 °C), and still make about twice its starts.
- **Opening fully and closing again in 25 % steps takes eight moves, where the stepped law takes two.** A proportional M3 with a rate limit cannot match the stepped law's starts without giving up what it is for.
- **The north-wind column is noisy.** It is a median over 25 days, and neighbouring settings move it by 0.3–0.4 °C either way. Read it as a direction, not a ranking.

Each row is one run of `law_compare.py --plants primary --airflow 1 --set min_intv_m3=0 --define M3_HOLD_MS=900000u --define M3_MOVE_MIN_X10=200` and so on. `--define` builds a variant under `build/variants/` from a copy of the source, which stays as it is. Against the first run (2026-09-19):
- **The 5-minute rows improved:** at 10 % moves the swing went from 1.9 to 1.7 °C, and at both move sizes the other-wind swing fell by 0.2 °C. The earlier emulation kept M3's 10-minute close dwell in mode 2, and that bound only a law holding for less than 10 minutes.
- **The other rows** moved by up to 0.2 °C in swing, partly with the stop rule and partly with the CLOSE_ALL sweep now modelled at reboots and at STANDBY's end.
- **The north-wind column** moved by up to 0.5 °C, which is the noise described above.

## The law, as the contract asks it declared (§9)

1. **The law.** The stepped law is embedded and decides the step exactly as in mode 1: the same temperature and humidity branches, hysteresis, conflict rule and reason. **M1 and M2 follow its first two steps**, window for window. **M3 opens only from step 2.** Its aperture is proportional to the temperature excess above `t_max`: shut at +1.5 °C, fully open at +3.5 °C. It is half open where mode 1 would open it fully: a reading of +2.5 °C over `t_max`, which the ladder reads as its step 3 ("How it behaves over time"). Humidity that demands step 3 on its own opens M3 fully, as in mode 1. **It suits a slow actuator with a late reading because it does not chase.** M3 moves only for a correction of at least 10 %, by at most 25 % per move, and not within 10 min of its last drive. That covers the loop's dead time, how long the controller's reading takes to show what a move did: the sensor lags the air by 3.5–5.5 min (NS-10), T5 averages on top of that, and a 25 % move takes 44 s in production.
2. **Wind:** not read. Nothing in it depends on direction, so an invalid wind reading changes nothing.
3. **Tunables.** Constants in the file until they are keys (contract §3, "Adding a tunable of your own"):

   | Name | Unit | Value | What it trades |
   |---|---|---|---|
   | `M3_START_C10` | 0.1 °C above `t_max` | 15 | where M3 starts to open: lower cools earlier and uses M3 more |
   | `M3_FULL_C10` | 0.1 °C above `t_max` | 35 | where it is fully open: a narrower band is stiffer and nearer to a binary M3 |
   | `M3_MOVE_MIN_X10` | 0.1 % | 100 | the smallest correction made: larger means fewer motor starts, a coarser M3 |
   | `M3_STEP_MAX_X10` | 0.1 % | 250 | the rate limit: at most this per move |
   | `M3_HOLD_MS` | ms | 600 000 | no move within this long of the last drive: longer means fewer starts, a slower answer |
   | `M3_POS_STALE_MS` | ms | 90 000 | a position older than this is not acted on (T17 reads every 30 s at rest) |

   M1 and M2 keep the operator's `hyst_t` and humidity settings, through the stepped law.
4. **Temperature against humidity:** the stepped law's rule, for every `cr_priority`, because it is the stepped law's code. The conflict decides the step, and so whether M3 may open at all (step 2). Within that, M3 takes the larger of the two demands.
5. **An unknown or stale position:** M3 is held, and reason bit `0x20` is set. **A `FAIL_TIMEOUT` or an `ABORTED` last command:** the target is re-based on the position (`0x80`), so nothing is re-issued blind, and the next move is decided afresh from where M3 is, after the hold time. **A digital M3:** the stepped law's M3, as in mode 1 (`0x10`).
6. **Motor starts:** about 22 M3 drives a day in the simulator, against the stepped law's 7.7, with the same running time: 21.9 against 22.6 motor minutes a day. That is 2.8 times the starts, which the contract asks a continuous law not to do; see "The rate limit" above.
7. **Name** `graded`, **version** 1. **Reason:** the stepped law's code in the low bits (0 no data, 1 temperature only, 2 both wanted open, 3 they agreed, 4 a conflict decided by `cr_priority`), plus M3's bits: `0x10` M3 digital, `0x20` position unknown or stale, `0x40` a move due but within the hold time, `0x80` re-based after a timeout or a takeover.
8. **The row.** `step`, `step_t` and `step_rh` are the stepped law's, which carry M1 and M2 and the M3 gate. **Mode 2's row:** `demand_t_x10` is M3's temperature aperture in 0.1 %, 0..1000. `demand_rh_x10` is 1000 when humidity alone demands step 3, 0 when it votes lower or to close, and −1 when it abstains. The caller adds M3's commanded target.
9. **Two contrasting weeks:** the results split every day by wind, north (M3's side) against the rest, over the whole summer rather than two weeks.

## What this does not show

- **How much air a part-open M3 lets through.** It is unmeasured (plan §5c), so the comparison is run at three curves; see the results for whether the verdict holds across them.
- **North-wind days are the plants' weak spot.** The adopted plants under-state the logged swing on those days (3.0–3.2 against 3.9 °C, [`README.md`](README.md), "The swing gap"), which is where this candidate gains most. Treat that gain as a lead to confirm on the greenhouse (contract §6).
- **The thermal claim needs the greenhouse** (contract §5 item 5). A rig soak can prove that apertures are reached; only a production summer can prove the limit cycle is damped.
- **`graded` has not run on the rig or in the greenhouse.** It is in T6's model table for testing only. The simulator emulates 2.12.0's T2, T17 and T6 as built (045a39c, 9c53be7): the stop a learned lead early, the leaf's run-on, the settle read, one dwell in mode 2, T6's feedback including ABORTED, and the CLOSE_ALL sweep (`README.md`, "A linear M3"). The run-on is the rig's, 420 ms; production's is unmeasured, and T2 learns it whatever it is.
