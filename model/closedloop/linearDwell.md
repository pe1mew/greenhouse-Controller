# `min_intv_m3`: what to ship as the linear dwell

| Field | Value |
|---|---|
| Question | The shipping default for `motor/min_intv_m3`, the linear dwell. In mode 2 it replaces **both** of M3's dwells (2.12.0), so at 0 M3 has no dwell at all and only whatever spacing the law gives itself. The key ships at 0; seconds, 0–1500 |
| Recommendation | **600 s (10 min)** |
| Evidence | The closed-loop simulator, 5C88's logged weather 2026-06-05 to 09-16, the adopted plants, `graded` as mode 2's law — and `graded` with its own hold removed, standing in for a law that does not space its moves |
| Owner | The model session (handoff, 2026-09-20). Firmware-side the key is settled; only the default was open |

## The recommendation, and why

**Ship 600 s.**

1. **It costs nothing today.** `graded` already holds 10 minutes between moves, so 0, 300 and 600 give exactly the same summer: swing 2.0 °C, 21.6 M3 drives a day, 21.5 motor minutes, and not one deferred target. The key only begins to bite above the law's own hold.
2. **It is the floor that protects whatever law runs next.** The interval is the caller's protection; a hold inside the law is the law's own, and the contract exists because the law will be replaced. Run `graded` with its hold removed — a law that re-decides every 30 s — and 0 gives **42.5 M3 drives a day and 30 motor minutes**, against mode 1's 7.7 and 22.5. At 600 that is back to 19.3 drives and 25.2 minutes, with a swing of 2.1 °C, still better than mode 1's 2.7. A default of 0 works only while the law happens to be well behaved, and fails silently when one is not.
3. **10 minutes is the loop's dead time, not a round number.** The controller's reading lags the air by 3.5–5.5 min (NS-10), T5's average adds to that, and a 25 % move takes 44 s in production. Below 10 minutes a law is answering something it cannot see yet.

**Do not ship above 900 s.** At 900 the swing is back to mode 1's 2.7 °C while M3 still drives twice as often (15.1 against 7.7): mode 2 has stopped paying for itself. At 1200 and 1500 it is worse than mode 1 (3.3 and 3.5 °C) and still makes more starts.

**0 stays the right setting for a deliberate test** — it is what the law alone does, and it is one `--set` away.

## The evidence

Primary plant, airflow proportional, the whole summer. `graded` as it ships (10-minute hold):

| `min_intv_m3` | Swing °C | North wind | Other wind | M3 drives a day | Motor minutes a day | Targets deferred a day |
|---|---|---|---|---|---|---|
| mode 1 (`stepped`) | 2.7 | 3.2 | 2.2 | 7.7 | 22.5 | — |
| **0 / 300 / 600** | **2.0** | **1.8** | **2.1** | **21.6** | **21.5** | **0** |
| 900 | 2.7 | 2.9 | 2.6 | 15.1 | 23.8 | 107 |
| 1200 | 3.3 | 3.4 | 3.2 | 12.6 | 20.9 | 176 |
| 1500 | 3.5 | 4.0 | 3.5 | 11.0 | 18.7 | 228 |

The same law with `M3_HOLD_MS = 0`, which is what a law that does not space its own moves would do:

| `min_intv_m3` | Swing °C | North wind | M3 drives a day | Motor minutes a day |
|---|---|---|---|---|
| 0 | 1.6 | 1.1 | **42.5** | 30.0 |
| 300 | 1.8 | 1.6 | 25.7 | 27.6 |
| **600** | **2.1** | **1.9** | **19.3** | **25.2** |
| 900 | 2.6 | 2.8 | 15.0 | 23.4 |
| 1200 | 3.3 | 3.3 | 12.6 | 21.0 |
| 1500 | 3.5 | 4.0 | 11.1 | 18.7 |

The pattern holds on the second plant and at another airflow curve: 0 and 600 are identical there too, and 900 is already at or past mode 1's swing.

| Run | Swing, mode 1 | Swing at 0 and at 600 | Swing at 900 | M3 drives a day, mode 1 / 600 / 900 |
|---|---|---|---|---|
| primary, airflow 1 | 2.7 | 2.0 | 2.7 | 7.7 / 21.6 / 15.1 |
| second, airflow 1 | 2.8 | 2.3 | 2.9 | 7.5 / 21.3 / 15.0 |
| primary, airflow 2 | 2.8 | 2.3 | 2.9 | 7.7 / 22.1 / 15.3 |

## What this does not settle

- **The simulator ranks settings; it does not predict the greenhouse.** The simulated swing runs below the logged swing on the same days, and both adopted plants under-state north-wind days, which is where mode 2 gains most.
- **The part-open airflow curve is unmeasured.** The ranking holds across the curves tried; the numbers move.
- **The minimum move is still unmeasured** — the shortest pulse that actually shifts the leaf, a rig measurement. The 20 mm deadband remains a guess, and an interval does not substitute for it.
- **Mode 2 has not been soaked.** Contract §5 wants motor starts per hour from the rig, which no simulation can supply.
- **The rig and production feel the same interval differently.** 600 s is about 3.4 M3 traverses in production (176 s) and about 46 on the rig (13 s), so a rig soak in mode 2 will look idle between moves. That is the key working, not a fault.

## Regenerate

```bash
python model/closedloop/law_compare.py --plants primary --airflow 1 --set min_intv_m3=900
python model/closedloop/closed_loop.py reproduce --start 2026-06-05 --end 2026-09-16 --plant2 model/campaign-summer-2026/plant2/plant2_summer2026_Ca2.9_tau120_tau90_ev5_dir.json --firmware current --model graded --set wpos_fitted_m3=1 --set ctrl_mode_m3=1 --set min_intv_m3=600
```

The "no hold" family is the same run with `--define M3_HOLD_MS=0u`, which builds a variant of the law under `build/variants/` and leaves the source as it is.
