# `graded`: a first candidate for mode 2's law

| Field | Value |
|---|---|
| Law | [`drivers/ventModel/src/vent_model_graded.cpp`](../../drivers/ventModel/src/vent_model_graded.cpp), `graded` v1, with 15 host tests (`pio test -e native`) |
| Status | **A candidate, 2026-09-19.** Which law mode 2 runs is the plan's open decision 9 ([`integrateWindowPositionSensor.md`](../../design/integrateWindowPositionSensor.md) §10). This is the simplest candidate the plan names, a proportional map with a rate limit. Its constants are provisional |
| Evidence | The closed-loop simulator only: 5C88's logged weather, 2026-06-05 to 09-16, on both adopted plants, with today's firmware, across a range of M3 airflow curves. No rig run, no greenhouse run |
| Regenerate | `python model/closedloop/law_compare.py` (about 10 minutes); the sweep with `--define`, below |

## Short answer

In the simulator, over the summer, on both adopted plants:
- **North-wind days: the swing drops from 3.0–3.2 to 1.5–1.9 °C, at every airflow curve tried.** That is the robust result. It is also where the plants are weakest: they under-state the logged north-wind swing, 3.9 °C. So it is a lead to confirm on the greenhouse, not yet a finding.
- **Other days: no clear change,** between −0.5 and +0.3 °C depending on the plant and the airflow curve.
- **Heat: no gain.** Hours at or above 31 °C stay at 2.3–2.4 a day with proportional airflow. If a part-open M3 lets through less air (exponent 2), they rise to 2.6. At 31 °C M3 is half open, where the stepped law has it fully open.
- **The motor: about 2.8 times the starts, the same running time.** M3 makes about 21 drives a day against the stepped law's 7.7, yet runs 21 minutes a day against 22. The contract asks that a continuous law not multiply the starts. No setting tried meets that; see the sweep below. Whether starts or running time wear the motor is unmeasured (contract §7), so that is a decision to make.

## Results

`law_compare.py` closes the loop over 2026-06-05 to 09-16 twice per plant: the stepped law with a binary M3, and `graded` with a linear one. Both use today's firmware and 5C88's settings. The swing is the campaign's measure, the median over days of the temperature range between successive M3 openings. The fluctuation is the daytime spread of the temperature around its own 60-min average. The script's docstring defines each row. North-wind days are those with most of the logged M3-open time in wind from 315–45°: 25 days, against 57 with other wind.

### The summer, proportional airflow

| | Logged | Primary, stepped | Primary, graded | Second, stepped | Second, graded |
|---|---|---|---|---|---|
| Hours at or above 31 °C a day | 2.3 | 2.3 | 2.4 | 2.3 | 2.4 |
| Daytime mean, °C | 27.7 | 27.4 | 27.5 | 27.4 | 27.5 |
| **Swing, °C** | 3.1 | 2.7 | **2.0** | 2.8 | **2.3** |
| … north-wind days | 3.9 | 3.2 | **1.8** | 3.0 | **1.8** |
| … other-wind days | 2.2 | 2.2 | 2.1 | 2.4 | 2.5 |
| Fluctuation, °C | 0.7 | 0.7 | 0.6 | 0.7 | 0.6 |
| **M3 drives a day** | 8.1 | 7.7 | **21.6** | 7.5 | **21.3** |
| M3 motor minutes a day | 19.9 | 22.5 | 21.5 | 21.8 | 21.2 |
| M3 openings a day, from shut | 3.9 | 3.8 | 3.1 | 3.7 | 3.1 |
| M3 open hours a day | 4.8 | 5.3 | 6.7 | 5.2 | 6.6 |
| M3 mean opening, % | 19.8 | 21.7 | 19.7 | 21.3 | 19.2 |
| M1+M2 drives a day | 15.2 | 12.1 | 12.0 | 12.2 | 11.8 |

T6 dropped or deferred no target: `graded` does not ask for a move within the deadband, and the minimum interval is 0.

### Across the airflow range

How much air a part-open M3 lets through is unmeasured, so the comparison runs three curves: M3's airflow as its opening to the power 0.5 (more air early), 1 (proportional, as fitted) and 2 (less air early). Stepped → graded:

| Airflow exponent | Swing, primary / second | North wind, primary / second | Other wind, primary / second | Hours ≥ 31 °C a day, primary / second | M3 drives a day |
|---|---|---|---|---|---|
| 0.5 | 2.7 → 1.8 / 2.7 → 1.9 | 3.1 → 1.9 / 3.0 → 1.7 | 2.2 → 1.7 / 2.4 → 1.9 | 2.3 → 2.2 / 2.3 → 2.2 | 7.6 → 21.4 |
| 1 | 2.7 → 2.0 / 2.8 → 2.3 | 3.2 → 1.8 / 3.0 → 1.8 | 2.2 → 2.1 / 2.4 → 2.5 | 2.3 → 2.4 / 2.3 → 2.4 | 7.7 → 21.6 |
| 2 | 2.8 → 2.3 / 2.8 → 2.6 | 3.2 → 1.8 / 3.1 → 1.5 | 2.3 → 2.3 / 2.5 → 2.8 | 2.3 → 2.6 / 2.4 → 2.6 | 7.7 → 22.1 |

The north-wind gain holds at every curve and on both plants. The other-wind result and the heat hours turn with the curve, so they stay open until the curve is measured.

### The rate limit: fewer starts cost swing

The primary plant, proportional airflow, `graded` with its hold time and smallest correction varied (`M3_HOLD_MS`, `M3_MOVE_MIN_X10`); everything else as v1:

| Hold | Smallest correction | Swing | North wind | Other wind | Hours ≥ 31 °C | M3 drives a day | Motor minutes a day |
|---|---|---|---|---|---|---|---|
| stepped | — | 2.7 | 3.2 | 2.2 | 2.3 | 7.7 | 22.5 |
| 5 min | 10 % | 1.9 | 1.5 | 2.0 | 2.3 | 28.6 | 27.0 |
| **10 min (v1)** | **10 %** | **2.0** | **1.8** | **2.1** | **2.4** | **21.6** | **21.5** |
| 15 min | 10 % | 2.1 | 1.4 | 2.4 | 2.4 | 17.1 | 18.2 |
| 5 min | 20 % | 2.1 | 1.5 | 2.3 | 2.3 | 20.5 | 24.2 |
| 10 min | 20 % | 2.3 | 2.2 | 2.4 | 2.3 | 17.2 | 19.8 |
| 15 min | 20 % | 2.3 | 1.9 | 2.7 | 2.4 | 14.7 | 17.5 |

- **v1's 10 min** covers the loop's dead time: the reading's lag, T5's average and the move itself come to about 6–8 min. It keeps nearly all of 5 min's damping with a quarter fewer starts.
- **Gentler settings do worse than the stepped law on other-wind days** (2.4–2.7 against 2.2 °C), and still make about twice its starts.
- **Opening fully and closing again in 25 % steps takes eight moves, where the stepped law takes two.** A proportional M3 with a rate limit cannot match the stepped law's starts without giving up what it is for.

Each row is one run of `law_compare.py --plants primary --airflow 1 --define M3_HOLD_MS=900000u --define M3_MOVE_MIN_X10=200` and so on. `--define` builds a variant under `build/variants/` from a copy of the source, which stays as it is.

## The law, as the contract asks it declared (§9)

1. **The law.** The stepped law is embedded and decides the step exactly as in mode 1: the same temperature and humidity branches, hysteresis, conflict rule and reason. **M1 and M2 follow its first two steps**, window for window. **M3 opens only from step 2.** Its aperture is proportional to the temperature excess above `t_max`: shut at +1.5 °C, fully open at +3.5 °C. It is half open where mode 1 would open it fully (+2.5 °C, 31 °C by day at 5C88's settings). Humidity that demands step 3 on its own opens M3 fully, as in mode 1. **It suits a slow actuator with a late reading because it does not chase.** M3 moves only for a correction of at least 10 %, by at most 25 % per move, and not within 10 min of its last drive. That covers the loop's dead time, how long the controller's reading takes to show what a move did: the sensor lags the air by 3.5–5.5 min (NS-10), T5 averages on top of that, and a 25 % move takes 44 s in production.
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
6. **Motor starts:** about 21 M3 drives a day in the simulator, against the stepped law's 7.7, with the same running time: 21 against 22 motor minutes a day. That is 2.8 times the starts, which the contract asks a continuous law not to do; see "The rate limit" above.
7. **Name** `graded`, **version** 1. **Reason:** the stepped law's code in the low bits (0 no data, 1 temperature only, 2 both wanted open, 3 they agreed, 4 a conflict decided by `cr_priority`), plus M3's bits: `0x10` M3 digital, `0x20` position unknown or stale, `0x40` a move due but within the hold time, `0x80` re-based after a timeout or a takeover.
8. **The row.** `step`, `step_t` and `step_rh` are the stepped law's, which carry M1 and M2 and the M3 gate. **Mode 2's row:** `demand_t_x10` is M3's temperature aperture in 0.1 %, 0..1000. `demand_rh_x10` is 1000 when humidity alone demands step 3, 0 when it votes lower or to close, and −1 when it abstains. The caller adds M3's commanded target.
9. **Two contrasting weeks:** the results split every day by wind, north (M3's side) against the rest, over the whole summer rather than two weeks.

## What this does not show

- **How much air a part-open M3 lets through.** It is unmeasured (plan §5c), so the comparison is run at three curves; see the results for whether the verdict holds across them.
- **North-wind days are the plants' weak spot.** The adopted plants under-state the logged swing on those days (3.0–3.2 against 3.9 °C, [`README.md`](README.md), "The swing gap"), which is where this candidate gains most. Treat that gain as a lead to confirm on the greenhouse (contract §6).
- **The thermal claim needs the greenhouse** (contract §5 item 5). A rig soak can prove that apertures are reached; only a production summer can prove the limit cycle is damped.
- **Nothing here is tested in firmware.** T2's target path is 2.11.0 and not built, and the simulator's M3 emulates its design (`README.md`, "A linear M3").
