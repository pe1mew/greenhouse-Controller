# Summer-2026 Calibration Campaign — Results

| Field | Value |
|---|---|
| Document | Campaign results summary (conclusions only). Derivations: [thermalProfileCampaign.md](thermalProfileCampaign.md) §9.12, claim by claim, with the original analysis kept in §9.5-9.11 under banners; [closedloop/README.md](closedloop/README.md) for the revised model and its tests |
| Revision | **2026-09-18: revised on correctly timed data** (§0). The first version (2026-07-04, extended 2026-08-31) was built on outdoor and door data that were two hours late |
| Data | 5C88 SD logs 2026-06-04 → 2026-09-17: 58 files, 105 logged days, 296 678 samples at 30 s. Outdoor T/RH/lux from LoRa `lht65-20` (10 min) and doors from `lds01-5`/`-6`, both converted from UTC. Indoor LoRa `lht65-02`/`-03` are used for cross-checks |
| Status | **A two-node plant reproduces the binary law's limit cycle in closed loop, on held-out days too.** It gets the number of M3 openings, the cycle period and the time above 31 °C right. The swing is still short (§1, F1). AC-9 and AC-10, which compare every sample, still fail (§3) |
| Adopted artifacts | [`plant2/plant2_summer2026_Ca2.9.json`](campaign-summer-2026/plant2/plant2_summer2026_Ca2.9.json) (primary) and [`plant2/plant2_summer2026_dir.json`](campaign-summer-2026/plant2/plant2_summer2026_dir.json) (second, with a wind-direction term). **Superseded:** `plant_calibrated_constrained_summer2026_freem3.json` |

---

## 0. What changed on 2026-09-18, and why

**The LoRa database stamps its rows in UTC. The controller's SD logs are local time.** Every calibration input joined the two unconverted, so each indoor sample was paired with the outdoor weather and door state of two hours later. Proven two ways over the summer ([`lora_time.py`](lora_time.py)):

- `lht65-20`'s daylight is centred 124 min before the day the controller's logged sunrise and sunset define. That is the median of 91 days, IQR -130..-118; after conversion, -4 min.
- The indoor `lht65-02`/`-03` temperature changes match the controller's best at a 2-hour lag: r = 0.86 and 0.88, against 0.16 and 0.19 at zero lag.

`prepare_calibration_input.py` and `closedloop/dataset.py` convert since then, and `thermalProfileCampaign.md` §7.3 carries an erratum.

**Also corrected:**
- The morning of 2026-07-09 (06:06-09:48) is masked. 5C88's DS1307 clock (gh#37) stamped a block of rows 68 minutes early.
- Before firmware 2.1.3 (OTA on 2026-07-11) every SD stamp may be off by up to about 2 minutes.
- The first closed-loop runs had an error of their own: the simulator's clock wrapped on 2026-07-18. The figures below are after that fix (closedloop/README.md).

**Withdrawn from the first version:**
- The §1 single-node parameters and their 1.19 °C validation, and F1.
- From F2: "30-100×", "≤ 0.05 /h leeward", "T_in converged to T_out" and "direction alone decides".
- F3's "trigger artifact", F4's "~0.6 /h ceiling", and F6's "treat M3 as near-dead when ~SW".

**Standing:**
- Everything taken from the SD logs alone: F7-F9, F11's minutes, the M3 opening counts and the wind statistics.
- F5, F10, and the door states during the forced tests: door 1 was open for hours either side of them.

## 1. The model

**Two nodes plus humidity.**
- **A fast air node**, which is what the controller's sensor sees.
- **A slow structure/soil node.**
- **One absolute-humidity node.**
- **Heat flows** between the two nodes, from both to outside, and out through the windows and doors by ventilation.
- **Sun** heats both nodes in proportion to outdoor lux.
- **Transpiration** is a constant plus a lux-dependent term.

The kernel is [`closedloop/plant2_kernel.c`](closedloop/plant2_kernel.c), a backward-Euler step shared by the fit and the simulator. `closedloop/refit.py` fits it on 2026-06-04 to 2026-09-17, with **every 4th calendar day held out**. The loss is the air node against T_in plus absolute humidity in g/m³. Window openness comes from the relay log's exact times, including M3's 176-s travel. Doors are inputs.

Nine variants were fitted: different objectives, a wind-direction term, and fixed air capacities. Two are adopted:

| Parameter | `_Ca2.9` (primary) | `_dir` (second) | Meaning |
|---|---|---|---|
| `Ca` | 2.9 MJ/K (fixed: the air alone) | 16.0 MJ/K | air node's heat capacity |
| `Cs`, `Gas`, `Gso` | 1 689 MJ/K, 1 116 W/K, 774 W/K | 1 875 MJ/K, 1 621 W/K, 83 W/K | structure/soil node: capacity, coupling to air, loss to outside |
| `UA0` | 3 329 W/K: cover 2 507 + air exchange 1.0 /h | 5 565 W/K: cover 3 283 + 2.8 /h | heat loss with everything shut |
| `ach_m1` = `ach_m2` | 1.3 /h | 2.6 /h | per roof window |
| `ach_m3` | 4.8 /h | 9.5 /h + 16.3 /h × cos(wind angle off north), when positive | the north-wall flap |
| `ach_door` | 0.4 /h | 1.3 /h | per open door |
| sun at 30 klux | 48 kW to air, 17 kW to structure | 97 kW to air, 3 kW to structure | |
| transpiration | 7.5 kg/h + 8.6 kg/h per 10 klux | 22 kg/h + 20 kg/h per 10 klux | |

**How firm these are.** The nine variants fit about equally well, yet their absolute values differ a lot: `ach_m3` ranges from 4.8 to 19.5 /h and `Ca` from 2.9 to 22 MJ/K. So the individual numbers are not identified. What holds in every variant is the ranking. **M3 ventilates about 4-8× as much as a roof window:** 3.8× in `_Ca2.9`, 8.1× in `_ev20`. In `_dir` it is 3.7× with the wind along or off its wall, and 10× with the wind straight onto it. The closed-loop behaviour below is the firm result.

**Volume.** Every fit uses V = 2 400 m³, the first version's included; its text said 2 900 m³. Heat flows in W/K do not depend on the volume, but the ach figures scale with it.

### Validation, open loop (held-out days, correctly timed data)

| Model | T RMSE | bias | 95th pct | within ±1 °C | AH RMSE | RH RMSE |
|---|---|---|---|---|---|---|
| **Two-node `_dir`** | **1.58 °C** | -0.16 | 3.39 | 52.2 % | 1.22 g/m³ | 8.2 % |
| **Two-node `_Ca2.9`** | 1.72 °C | +0.02 | 3.68 | 51.7 % | 1.24 g/m³ | 7.9 % |
| Single node (superseded) | 4.07 °C | -0.52 | 7.99 | 24.9 % | 2.46 g/m³ | 12.4 % |

The single node's first-version score, 1.19 °C on the Jun 19-25 week, was measured on the shifted data it was fitted to.

### The test that matters: the binary law in closed loop

The model exists to verify control laws. So it is judged on whether the firmware's own law, run against it through the logged weather, behaves as 5C88 did. The law is `drivers/ventModel`'s stepped law, run through an emulated T5/T4/T6 chain; see `closedloop/README.md`.

| 2026-06-05 → 2026-09-16 | M3 openings | Day-by-day r | Days with cycling | Cycle | Hours ≥ 31 °C | Swing |
|---|---|---|---|---|---|---|
| **Logged, all 102 days** | **406** | | **74** | **46 min** | **237** | **3.1 °C** |
| `_Ca2.9` | 363 | 0.78 | 62 | 42 min | 252 | 2.7 °C |
| `_dir` | 422 | 0.75 | 67 | 42 min | 220 | 2.0 °C |
| Single node (superseded) | 184 | 0.52 | 13 | 113 min | 234 | 3.9 °C |
| **Logged, 25 held-out days** | **103** | | **17** | **45 min** | **72** | **3.6 °C** |
| `_Ca2.9` | 101 | 0.75 | 16 | 44 min | 80 | 2.8 °C |
| `_dir` | 128 | 0.76 | 19 | 41 min | 72 | 2.5 °C |
| Single node | 43 | 0.35 | 1 | 154 min | 74 | 5.1 °C |

How the columns are defined:
- **Day-by-day r** correlates the logged and simulated M3 openings per day.
- **Cycle** is the median gap between successive M3 openings, on the days that have one.
- **Swing** is the median temperature range inside such a gap.
- **Hours ≥ 31 °C** are hours at or above M3's entry temperature at 5C88's settings.

## 2. Key findings

**F1 — On correctly timed data, a two-node plant reproduces how the binary law behaves in closed loop; the single node does not** *(revised 2026-09-18)*.
- **It reproduces** how often M3 opens, the ~45-minute cycle, the time above the 31 °C M3 threshold and which days cycle, on held-out days as well.
- **It follows the 2.3.1 firmware change** (the gh#48 guard, 2026-07-29). M3 openings, logged against `_Ca2.9` / `_dir`:
  - before the change: 239 against 226 / 264;
  - from the change until door 1's sensor went silent on 2026-08-16: 87 against 87 / 90;
  - after that, where the door is assumed shut, both fall short: 80 against 50 / 68.
- **Still short: the swing,** 2.0-2.8 against 3.1-3.6 °C. That gap matters most for a law whose purpose is to damp the swing.

**F2 — M3 is the house's strongest ventilator; how much wind direction matters is open again** *(revised 2026-09-18; replaces "30-100×")*.
- **The fits** put M3 at about 4-8× a roof window in every variant (§1). `_dir` adds a direction term worth 2.7× with the wind straight onto the wall. `_Ca2.9` has no such term and reproduces the closed loop about as well (F1).
- **The forced M3-only tests, re-read on the correct clock.** Door 1 was open throughout every window and door 2 mostly shut. "Excess" is inside minus outside; the value in brackets is the lowest during the window.

| Window | Wind (median) | Sun (median) | T excess: before → end (lowest) | AH excess: before → end (lowest) |
|---|---|---|---|---|
| 2026-07-11 10:06-10:36 | 65°, 1.6 m/s | 8 klux | +6.6 → +3.0 °C (+2.9) | +5.0 → +0.7 g/m³ (+0.3) |
| 2026-07-11 11:14-11:44 | 26°, 1.6 m/s | 10 klux | +7.2 → +2.9 °C (+2.8) | +6.6 → +0.5 g/m³ (+0.3) |
| 2026-07-11 12:18-12:48 | 22°, 2.2 m/s | 34 klux | +8.0 → +4.7 °C (+4.6) | +6.5 → +0.8 g/m³ (+0.4) |
| 2026-07-04 09:52-10:54 | 261°, 1.5 m/s | 29 klux | +6.0 → +6.0 °C (+4.8) | +4.2 → +1.2 g/m³ (+0.6) |
| 2026-07-04 11:54-12:56 | 268°, 2.3 m/s | 41 klux | +5.8 → +6.9 °C (+5.8) | +2.2 → +2.2 g/m³ (+0.7) |

- **Jul 11 flushed the humidity:** within 0.5 g/m³ of outside in 13-16 minutes, in every window. The temperature fell 3.3-4.3 °C but stayed 2.9-4.7 °C above outside.
- **On Jul 4 the humidity dropped too**, to within 0.6-0.7 g/m³ at its lowest. The temperature did not drop.
- **The two days differ in sun as well as in wind.** Jul 4 ran in 29-41 klux, while Jul 11's first two windows ran in 8-10 klux. Jul 11's one sunny window ended at +4.7 °C, against +2.9-3.0 for the overcast ones. So the first version's "direction alone decides" does not hold. Separating direction from sun needs a model fit, and the two adopted fits disagree about it (NS-9).
- **A crude model-free check points elsewhere again.** With all three windows open, the doors shut and more than 20 klux, the house runs warmest per unit of sun in west wind (1.87 °C per 10 klux) and coolest in east wind (1.32), with north wind in between (1.51). The figure is flat across wind speeds (1.44-1.65). Different days sit in each sector, so this is a lead, not a measurement (`thermalProfileCampaign.md` §9.12.6).
- **Withdrawn:** "≤ 0.05 /h leeward" and "T_in converged to T_out". Jul 11's humidity flush stands.

**F3 — The drop when M3 opens is ventilation, not a trigger artifact** *(reversed 2026-09-18)*. Daytime window openings on the correct clock, median slopes 15 min before → 3-18 min after:

| Event | n | T | AH | lux |
|---|---|---|---|---|
| M3 opens on M1+M2 | 282 | +8.4 → -8.8 °C/h | +7.0 → -3.9 g/m³/h | +4 174 → +543 /h |
| M2 opens, M3 shut | 389 | +6.6 → -4.4 °C/h | +7.5 → -9.2 g/m³/h | +4 588 → -375 /h |
| M1 opens, all shut | 140 | +8.6 → -5.0 °C/h | +8.3 → -11.5 g/m³/h | +5 108 → +2 400 /h |

- **The sun is still rising when M3 opens.** The first version's "-2 860 lux/h" was the sun two hours later.
- **The temperature swing after M3 opens is the largest of the three moves:** -17.2 °C/h, against -11.0 for M2 and -13.5 for M1.
- **Absolute humidity falls after every opening.** The first version read a rising RH as "little exchange", but RH rises whenever air cools.
- **The open-loop M3 response test shows it from the other side.** 25 minutes after M3 opens, the logged house is 2.3 °C cooler (median of 290 daytime openings). A plant in which M3 barely ventilates warms instead, given the same weather: the single node, +0.5 °C. The adopted plants cool by 2.2 (`_Ca2.9`) and 1.9 °C (`_dir`).

**F4 — On the hottest days the house runs about 5 °C above outside with everything open** *(revised 2026-09-18; the "~0.6 /h ceiling" is withdrawn)*.
- **The logs:** on 25 of the 105 logged days the maximum reached 34 °C or more. At that peak all three windows were open on 22 of them, and a door on 15. The median peak was 36.3 °C against 30.4 °C outside, at 34 klux.
- **In closed loop every plant reproduces those days' hours above 31 °C,** the single node included: 175 logged, 170-182 simulated. So the hot days do not tell the plants apart; the cycling days do (F1).

**F5 — Window identity erratum (2026-07-05).** Earlier analysis mis-identified M3 as a roof ridge panel. Authoritative (FRS + boerHandleiding):
- **M1** is the south roof slope (8 m²), **M2** the north roof slope (8 m²), **M3** the **north side wall** (80 m²).
- 8.1× is the motor *travel* ratio (171 s/21 s); the *area* ratio is 10× (80/8 m²).
- **The prevailing-wind premise is wrong** *(added 2026-09-18)*. The first version called the north wall "the leeward side under prevailing SW winds". From 2026-06-19 to 09-17, wind of 1 m/s or more blew from 315-45° in 43 % of samples. By eight sectors: NW 23 %, N 21 %, W 18 %, S 11 %, and SW 6 %.

**F6 — Strategy consequences** *(revised 2026-09-18)*.
- **NS-7** (an independent M3 threshold, `t_thresh_m3`): the premises that shelved it are withdrawn. §9.9 found M3 ineffective, and §9.10 effective only windward. On the corrected data it is the strongest ventilator (F2). Whether a separate M3 threshold helps is now a closed-loop question, to be asked on the adopted plants together with F7's step size.
- **A direction-gated M3 strategy** needs a direction effect the data have not yet settled (F2, NS-9).
- **Still valid from §9.8,** unaffected by every revision: the -5 °C close hysteresis keeps M1 open all night, and `hyst_t` conflates trigger spacing with the close guard (quantified in F8).

## 2b. Operational findings (Jul–Aug 2026)

Added 2026-08-31. These come from the SD logs alone, so the clock correction does not touch them. They concern how the *controller* behaves.

**F7 — The climate loop limit-cycles, and the cause is M3's step size, not its tuning.** Measured on 5C88, 2026-07-20 (analysis: `model/vent_step_replay.py`): **~42 min period, ~4.9 °C swing** (peaks 30.5-33.2 °C, troughs 24.9-28.6 °C).
- **Bang-bang control with a minimum on-time predicts both figures.** Amplitude ≈ cooling rate × dwell = 0.2 °C/min × 25 min ≈ 5 °C; period ≈ dwell + reheat ≈ 42 min.
- **The driver is the ventilation ladder itself.** Step 2 (M1+M2, ~16 m²) to step 3 (+M3, ~80 m²) multiplies the aperture ~6× in one move, so the house overshoots ~5 °C below setpoint and then reheats.
- **The two-node plant reproduces this cycle in closed loop (F1),** so candidate laws can now be compared on it *(added 2026-09-18)*.

**F8 — `hyst_t` cannot fix F7, and the reason is arithmetic.** `step_width = hyst_t / NUM_VENT_STEPS` is **integer** division, so `hyst_t` is not a dial: 3/4/5 all give width 1, 6/7/8 give width 2, 9/10/11 give width 3. Only three regimes exist, and each moves M3's entry threshold:

| `hyst_t` | M3 opens at | M3 openings / 9 days | M3 open time |
|---|---|---|---|
| 5 (current) | 31 °C | ~31 | 6.7 h |
| 6–8 | 33 °C | ~6 | 3.0 h |
| 9–11 | 35 °C | ~3 | 2.8 h |

- **The trade is bad.** Raising `hyst_t` cuts cycling by ~80 %, but costs **~55 % of M3's ventilation time** in a greenhouse reaching 39 °C.
- **This refines F6's "not independently tunable".** The conflation is not merely inconvenient: it is now quantified.
- **The replay refuses to project unless it first reproduces the logged T-demand** at the unit's real settings. The command in §5 scores **96.8 %** over 378 decisions, and 97.8 % over the narrower Jul 19-28 window the table was computed on. The closed-loop simulator's emulated firmware chain scores 97.1 % on the same logs (gate-control).

**F9 — A step-down dead band (`vent_hyst`) was proposed and rejected on the data.** The hypothesis was that M3 cycles because temperature *hovers* at the 31 °C boundary. It does not: it makes **full ~5 °C excursions** past the threshold and back. Replaying 2026-07-20 with a 1 °C and a 2 °C dead band leaves the opening count **unchanged at 7** and only lengthens each opening (60 → 78 → 107 min). A dead band can merge openings only if the trough stays inside it; these troughs fall 3-6 °C below. Filed and closed as gh#47 (`not planned`).

**F10 — M3's mechanism is documented.** Confirmed by the operator 2026-07-28:
- **M3 is a 40 m hanging flap** suspended on ropes and wound by a *single* line shaft.
- **Raised is closed, lowered is open,** with ~2 m of vertical travel over 171 s (~11.7 mm/s). It is not a hinged or sliding leaf.
- **A flap hanging in front of a 40 m aperture is exposed to the flow across it,** a plausible route for any direction effect (F2).
- **It also constrains any position sensor** (see [`design/windowPositionSensorRequirements.MD`](../design/windowPositionSensorRequirements.MD)).

**F11 — NS-9's organic evidence base.**
- **It has roughly doubled.** When F2 was first written, the three forced Jul 11 windows were "the entire windward evidence base" (~60 min).
- **The scan:** M3-only-open samples under windward flow (315-45°) in the wind-valid era, from 2026-06-19 12:00. That yields **~114 min across eight days to 2026-08-31, and ~118 min across nine to 2026-09-17**:

| date | windward M3-only | note |
|---|---|---|
| 2026-06-25 | ~16 min | organic |
| 2026-07-04 | ~4 min | NS-6 test day |
| **2026-07-11** | **~60 min** | the three forced windows (F2) |
| 2026-07-15 | ~8 min | organic |
| 2026-08-16 / 21 / 23 | ~8 min total | organic, short |
| 2026-08-27 | ~16 min | organic |
| 2026-09-05 | ~4 min | organic *(added 2026-09-18)* |

- **Caveat: these are organic, not controlled.** They are short and unforced, and none has been checked for the confounders the Jul 11 tests were designed to exclude: doors, a preceding M1/M2 state, solar transients.
- **They are a *lead* for NS-9, not a substitute for forced tests.** The Jul 11 tests themselves ran with door 1 open (F2).

**F12 — The limit cycle is house-wide** *(added 2026-09-18)*. `lht65-02` and `-03` hang mid-width at 1/4 and 3/4 of the length; the controller's FG6485A is in the exact centre (operator).
- **By day they agree with it:** median +0.4 and 0.0 °C between 10 and 17 h.
- **At night they read a steady 0.8-1.0 °C lower.**
- **They show the M3 swing too,** at about half its amplitude and about 10 minutes later. After M3 opens they fall by up to 0.7-0.8 °C, where the centre falls 1.3 °C. After it closes they rise by 0.6 °C, where the centre rises 1.3 °C. Their slower housing and 10-minute sampling may account for part of that.
- **Their soil probes** run near the outside temperature at midday and well above the air at night.

---

## 3. Acceptance criteria

| Criterion | Target | Result (`_Ca2.9` / `_dir`, held-out days) | Status |
|---|---|---|---|
| AC-9 (T fidelity, point by point) | 95th-pct \|err\| ≤ 1.0 °C, ≥ 95 % within ±1 °C | 3.68 / 3.39 °C; 51.7 / 52.2 % | **FAIL** |
| AC-10 (RH fidelity) | RH RMSE ≤ 5 % | 7.9 / 8.2 % | **FAIL** |
| Closed-loop behaviour (proposed; the operator sets the pass mark) | the binary law, run against the plant, behaves as logged | M3 openings 101 / 128 against 103; cycle 44 / 41 against 45 min; hours ≥ 31 °C 80 / 72 against 72; swing 2.8 / 2.5 against 3.6 °C | swing short; the rest close |

AC-9 and AC-10 compare every 30-s sample. A plant that is right about the loop but a few minutes out of phase in a 45-minute cycle fails them. The closed-loop row tests what the model is for, and its pass mark is the operator's to set.

## 4. Open items

| Item | What | Why |
|---|---|---|
| ~~Audit trail~~ | ✅ Done 2026-09-18: `thermalProfileCampaign.md` §9.12 re-derives the findings and grades every earlier claim; §9.5-9.11 carry banners, Appendix A is updated (NS-10 added) | |
| Old scripts and inputs | Regenerate `calibration_input_*.csv` with the corrected merge, under new names, keeping the old ones for traceability. Move `m3_event_study.py` and `ns9_direction_stratified.py` onto the new inputs, or retire them in favour of `closedloop/campaign_figures.py` | They read the shifted inputs, and `m3_event_study.py` reads them from the original checkout's path. Rerun as they are, they reproduce the withdrawn numbers |
| The swing (NS-10) | 2.0-2.8 against 3.1-3.6 °C. Candidates to test before any structural change: the sensor's own lag, the pre-2.1.3 timestamp noise, and the soil probes as a measured input | The one closed-loop property not yet reproduced |
| NS-9 | Does direction matter, how much, and in what shape? `_dir` says 2.7× with north wind, `_Ca2.9` fits about as well without a term, and a crude check finds west wind worst (F2). Settle it with forced M3 tests **with the doors shut**, at matched or low sun, in north, west and south wind, and on top of M1+M2 as T6 uses M3 | Every forced test so far had door 1 open, and the two days differed in sun (F2) |
| ~~NS-8~~ | ✅ Resolved 2026-07-12. **Qualified 2026-09-18:** M3 flushes the house in north wind, but the leeward "~nothing" is withdrawn (F2) | |
| Door 1 | `lds01-5` last reported on 2026-08-16 at 09:57 local time (07:56 in the UTC export) | After that the door is assumed shut, and both plants fall short on M3 openings (F1) |
| Part-open M3 | The aperture-to-ventilation curve | Needs the new firmware and position hardware. Until then, vary it in the simulator |
| Contract | `design/ventModelContract.md` §6 still quotes the withdrawn claims: "30–100", "0.05 /h ... south-west", "M3 also cycles when it barely ventilates", "the closed-loop simulation has never been run" | Change it on `main`, where Phase 5 works |
| Volume | 2 400 m³ in the fits, 2 900 m³ in documents | Only matters for absolute ach |

## 5. Reproducibility

```
# 1. fetch outdoor + door data from the Wenumseveld MySQL DB (192.168.20.232).
#    Rows are UTC; dataset.py and prepare_calibration_input.py convert. --end is exclusive.
python model/fetch_lora_data.py --sensor lht65-20 --start 2026-06-04 --end 2026-09-18
python model/fetch_lora_data.py --sensor lds01-5  --start 2026-06-01 --end 2026-09-18
#    ... likewise lds01-6, lht65-02, lht65-03; move the files into model/campaign-summer-2026/

# 2. the simulator's gates: the plant equations and the firmware chain
python model/closedloop/closed_loop.py gate-plant
python model/closedloop/closed_loop.py gate-control

# 3. fit the two-node plant (every 4th day held out) and compare the variants
python model/closedloop/refit.py fit --fix Ca_MJ=2.9
python model/closedloop/refit.py fit --direction
python model/closedloop/refit.py compare model/campaign-summer-2026/plant2/*.json

# 4. the binary law in closed loop against a plant (F1, §3)
python model/closedloop/closed_loop.py reproduce --start 2026-06-05 --end 2026-09-16 \
    --plant2 model/campaign-summer-2026/plant2/plant2_summer2026_Ca2.9.json

# 5. the figures behind F2-F5, F11, F12 and thermalProfileCampaign.md §9.12
python model/closedloop/campaign_figures.py

# 6. controller-behaviour replay (F7-F9) -- validates before it projects
python model/vent_step_replay.py model/campaign-summer-2026/2026-07-2*.log     --hyst-t 5 --avg-win-t 3
```

Requires system Python 3.11 with numpy, scipy, matplotlib and mysql-connector-python (see gotcha log 2026-07-05), plus the Code::Blocks MinGW `g++` that `drivers/ventModel` uses. F4's hours above 31 °C are summed from step 4's per-day table, over the days with a logged maximum of 34 °C or more.

The first version's pipeline (`prepare_calibration_input.py`, then `calibrate_plant_constrained.py --free-m3`) still runs. `prepare_calibration_input.py` now merges on the correct clock, so a fresh merge will no longer reproduce the superseded artifact; the committed `calibration_input_*.csv` files are the old, shifted merges.

## 6. Where the derivations live

| Topic | Where |
|---|---|
| The UTC evidence and the conversion | [`lora_time.py`](lora_time.py) |
| Two-node plant, fits, M3 response test, closed-loop tests, the gates | [`closedloop/README.md`](closedloop/README.md) |
| The figures behind F2-F5, F11, F12 | [`closedloop/campaign_figures.py`](closedloop/campaign_figures.py) |
| The revision, claim by claim: what stands, what is withdrawn, the re-read tests, the ladder, wind | [`thermalProfileCampaign.md`](thermalProfileCampaign.md) §9.12 |
| First-version identification, the NS-6 procedure, the Jul 4 and Jul 11 tests (**on shifted data**, kept as the record under banners) | `thermalProfileCampaign.md` §9.5-9.11 |
| Window strategy analysis (partially superseded) | `thermalProfileCampaign.md` §9.8 |
| Step-by-step status NS-1 … NS-10 | `thermalProfileCampaign.md` Appendix A |
