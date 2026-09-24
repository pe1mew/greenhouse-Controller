# Humidity control: what the branch asks for, and what it gets

| Field | Value |
|---|---|
| Subject | The humidity branch of the stepped law — [`vent_model_stepped.cpp:184`](../../drivers/ventModel/src/vent_model_stepped.cpp) — and the conflict rule that decides what happens to its vote (`:213`). Mode 1 and mode 2 share both |
| Status | **An audit, 2026-09-24.** Nothing here is implemented. The one hazard is filed as [gh#84](https://github.com/pe1mew/greenhouse-Controller/issues/84); the rest is a proposal with costs attached, for the operator to accept or drop |
| Evidence | 5C88's SD logs, 2026-06-04 09:35 to 2026-09-23 07:30 — 61 files, 312 543 samples, 109.1 days — driven through the compiled law with 5C88's own settings. The Python mirror the variants vary from is checked against the DLL on **every** sample: 0 mismatches. No rig run, no greenhouse run |
| Scope | The demand layer only: the step the law asks for, before dwell, wind override, the EG1 inhibit mask or a sensor fault get a say |
| Regenerate | `python model/closedloop/humidity_audit.py` (about a minute) |

## Short answer

- **The humidity axis cannot open a window at all under the shipped defaults**, and that is deliberate: `cr_priority` 0 is standing in for a temperature floor that was never wired.
- Over 109 days it asked to vent while the temperature was idle for **1203.9 h — 46 % of the time, 11 h a day — and was refused every time.** 869 h of that was a demand for *every* window.
- What it does instead is raise a step the temperature already asked for: **285.3 h, 2.6 h a day.** That is the entire effect of humidity control on this greenhouse.
- **No setting changes this.** `rh_max`, `rh_min`, `hyst_rh` and `avg_win_rh` only shape a vote that gets discarded. Of the two settings that do change it, one (`cr_priority` 1) is a hazard — [gh#84](https://github.com/pe1mew/greenhouse-Controller/issues/84) — and the other (`cr_priority` 2) hands an unbounded, unfloored authority to a setpoint the house cannot meet.
- **The night ceiling is not reachable.** Night RH runs at a median of 89 % against an 80 % ceiling, p10 83 %. The humidity axis is pegged at step 3 most of every night.
- **Venting would have removed water, though — nearly always.** Against the outdoor sensor, the air outside held less water than the air inside in **99.6 % of the refused hours**, by a median of 2.20 g/m³. Venting offered a 3.03 K lower dewpoint for a 3.04 K lower air temperature. That is the argument for giving the axis authority, and the argument for putting a floor under it.

## Why the axis is inert

`cfg_defaults.h:53` says it in as many words:

```c
#define DEF_CR_PRIORITY  0  /**< 0 = CR_TEMP_FIRST (preserves T-floor protection on cool humid nights) */
```

There is no T-floor. `t_min` (16 °C day, 14 °C night) is stored, published over `/api/config`, editable on the LCD, logged as `LOG_PARAM_T_MIN_DAY` — and **read by no control path**. `cfg_defaults.h:38` calls it "informational; future heating" and `ui_display.cpp:179` keeps it out of the day/night parameter list with the comment `HEATING CONTROL NOT IMPLEMENTED`. What actually keeps the house from venting on a cold damp night is that temperature-first discards every humidity demand while the temperature is below its setpoint.

So the default is not a preference between two axes. It is a missing feature being covered by a priority setting, and the cost is that the other axis never acts.

A second claim in the same file does not hold either. `cfg_defaults.h:51` gives `hyst_rh` = 12 as a "wider RH dead band — suppresses small-signal step toggles". The humidity branch has **no dead band**: `vent_step_required_rh()` enters the ladder only when `rh_avg > rh_max`, so the close guard inside `step_from_deviation()` (`:151`, reached only at a deviation of zero or less) is unreachable from it. `hyst_rh` sets the step width — 4 % RH per step — and nothing else. The vote lapses the instant the average touches the ceiling.

## What the house actually does

RH as the law sees it, 10-minute average, whole percent:

| | p10 | median | p90 | at or above 90 % |
|---|---:|---:|---:|---:|
| day | 42 | 70 | 90 | 10.7 % |
| night | 83 | **89** | 92 | 43.6 % |

109.1 days, 2618 h analysed, 5C88's own settings (`t_max` 28/20, `rh_max` 75/80, `rh_min` 50/55, `hyst_t` 5, `hyst_rh` 12, `avg_win` 6/10 — the shipped defaults, unchanged since commissioning; 2 climate `SETPT` rows in the whole period):

| | hours | share | per day |
|---|---:|---:|---:|
| temperature asks to vent | 1319.6 | 50.4 % | 12.10 |
| — at step 1 | 859.8 | 32.8 % | 7.88 |
| — at step 2 | 140.9 | 5.4 % | 1.29 |
| — at step 3 | 318.9 | 12.2 % | 2.92 |
| RH above its ceiling | 1608.1 | 61.4 % | 14.74 |
| — by day | 715.4 | 27.3 % | 6.56 |
| — at night | 892.7 | 34.1 % | 8.18 |
| **humidity asks to vent, temperature idle (refused)** | **1203.9** | **46.0 %** | **11.04** |
| — by day | 676.7 | 25.8 % | 6.20 |
| — at night | 527.2 | 20.1 % | 4.83 |
| — asking step 1 (M1) | 86.1 | 3.3 % | 0.79 |
| — asking step 2 (M1+M2) | 248.3 | 9.5 % | 2.28 |
| — **asking step 3 (everything)** | **869.4** | **33.2 %** | **7.97** |
| — at or above `t_min` | 1037.6 | 39.6 % | 9.51 |
| — — by day (the narrowest useful authority) | 616.1 | 23.5 % | 5.65 |
| — — at night | 421.5 | 16.1 % | 3.86 |
| — below `t_min` (a floor would refuse these too) | 166.2 | 6.3 % | 1.52 |
| what humidity does today: raises a live step | 285.3 | 10.9 % | 2.62 |
| dry house while temperature wants to vent | 354.1 | 13.5 % | 3.25 |
| — by day | 353.7 | 13.5 % | 3.24 |
| — at night | 0.5 | 0.0 % | 0.00 |

Two things stand out. The refused demand is mostly **step 3** — 8 hours a day of "open everything", half of it at night. And it grows through the season: September asks for 13 h a day against June's 7.8, so whatever is decided here matters most in the months the campaign barely covers.

| month | days | refused, at or above `t_min` | per day |
|---|---:|---:|---:|
| 2026-06 | 26.6 | 206.9 h | 7.78 |
| 2026-07 | 29.2 | 243.3 h | 8.34 |
| 2026-08 | 31.0 | 297.2 h | 9.59 |
| 2026-09 | 22.3 | 290.2 h | 13.01 |

The temperature during the refused demand says how much of it a floor would catch:

| T_avg | hours | per day |
|---|---:|---:|
| below 10 °C | 0.6 | 0.01 |
| 10–13 | 126.5 | 1.16 |
| 14–15 | 227.1 | 2.08 |
| 16–19 | 397.2 | 3.64 |
| 20–23 | 279.3 | 2.56 |
| 24 and up | 173.0 | 1.59 |

A floor at `t_min` removes 166 h of 1204 — 14 %. In June–September that is nearly nothing, because the house is rarely cold. In November it is most of it. **The summer campaign is the wrong season to size a floor against**, and there is no winter data.

## What the settings can and cannot do

| setting | effect on "can humidity open a window" |
|---|---|
| `rh_max`, `rh_min` | none — they move the threshold of a vote that is then discarded |
| `hyst_rh` | none — it is the step width only (see above), not a dead band |
| `avg_win_rh` | none — it smooths the reading |
| `rh_ctrl_en` | turns the vote off entirely |
| `cr_priority` 1 | yes, and it also lets a **dry** house close against a heat demand: 354 h over 109 days, 220 h of it while the temperature was asking step 3. At the tomato row's own `rh_min` of 60 it is 568 h. Filed as [gh#84](https://github.com/pe1mew/greenhouse-Controller/issues/84) — the crop tables recommend this setting for tomato and strawberry |
| `cr_priority` 2 | yes: 11 h a day more venting, 8 of it at step 3, 4.8 of it at night, with no floor and no cap |

So the answer to "should the defaults change" is **no, not as the law stands**. Priority 2 is the only setting that unlocks the axis, and it unlocks it without either of the two brakes the audit says it needs.

## Would venting have dried the house?

The question the ceiling exists for, and it is answerable: `lht65-20` (*buiten kas*) logs outdoor T and RH through the campaign. Joined to the refused hours — UTC to local through `lora_time.utc_to_local()`, the conversion the 2026-09-18 erratum installed, forward-filled, stale after 1800 s — 1093.1 h of the 1203.9 h match (91 %; the rest is the fall period, which the export does not cover, and gaps).

| | matched hours |
|---|---|
| outside air holds **less** water than inside | **99.6 %** (day 99.7 %, night 99.4 %) |
| outside dewpoint is lower than inside | 99.6 % |
| median AH_in − AH_out | **2.20 g/m³** |
| median dewpoint drop on offer | 3.03 K |
| median T_in − T_out — the heat paid for it | 3.04 K |

So the intuition that an unheated house cannot vent its way dry is **wrong for this house, in this season**: opening a window during the refused hours would almost always have removed water, night included. What it also does is cost about 3 K of air temperature, which is why the floor in item 1 below is not optional — and why the axis asking for step 3 for 8 h a day is not a good way to spend it.

What this does **not** settle is the crop. Venting drops the dewpoint and the air temperature by about the same 3 K, and whether that leaves a leaf further from or closer to condensation depends on the leaf's own temperature, which nothing here measures — at night a leaf radiating to a clear sky sits below air temperature. The moisture balance is measured; the condensation verdict is not.

## What would have to change

All of it inside `drivers/ventModel`, behind [`ventModelContract.md`](../../design/ventModelContract.md), with `pio test -e native` and this audit re-run. Ranked cheapest first; each is independently useful.

1. **Wire `t_min` as the floor under a humidity-only open.** No new config key, already per-crop in both manuals, and it restores the protection `cr_priority` 0 is currently faking. Measured on this data it withholds 166 h of 1204; in winter it is the whole point.
2. **Cap a humidity-only demand at step 1, or 2.** Its refused demand is step 3 for 8 h a day. M3 wide open on a damp night is a heat dump, not a dehumidifier — and in mode 2 that demand is all-or-nothing on M3 (`vent_model_graded.cpp:175`), so it goes to 100 % or nowhere.
3. **Give the branch a real close guard**, so `hyst_rh` means what `cfg_defaults.h:51` already claims. The cost is the hours the vote is held open below the ceiling:

   | guard | vote flips | per day | vote open | held below the ceiling |
   |---|---:|---:|---:|---:|
   | shipped (none) | 416 | 3.81 | 1608.1 h | 0.0 h |
   | 3 % | 316 | 2.90 | 1640.1 h | 32.0 h |
   | **4 % (`hyst_rh`/3, the ladder's own width)** | **298** | **2.73** | 1655.3 h | **47.2 h** |
   | 5 % | 282 | 2.59 | 1666.9 h | 58.7 h |
   | 12 % (`hyst_rh` whole) | 232 | 2.13 | 1751.4 h | 143.3 h |

   4 % is the natural choice: it costs 47 h in 109 days and cuts a quarter of the flips. The full 12 % would hold the vote open down to 63 % RH, which in this house is nearly permanent. Today the flips are free because priority 0 discards them; the moment humidity can open a window, each one is a motor cycle.
4. **Only then** consider `cr_priority` 2 as a default, and fix priority 1 per gh#84.
5. **Decide what the 3 K is worth, and spend it deliberately.** Night authority is defensible on the moisture balance — the outside air is drier 99.4 % of those hours — but it costs about 3 K of house temperature and an unmeasured amount of leaf cooling. The conservative shape is floored, capped at M1, and day-only: **5.65 h a day** of extra M1 against 11 h a day of everything. The wider shape needs a leaf-temperature measurement, or a season of trying it on one unit.

## What this does not show

- **What venting does to the crop.** The moisture balance is measured (above); the leaf is not. No leaf or surface temperature is logged anywhere in this campaign, so whether a 3 K air-temperature drop bought with a 3 K dewpoint drop leaves the crop safer or wetter is an open question. Every recommendation above is bounded by this, not by the moisture balance.
- **The outdoor join stops at 2026-09-18.** The `lht65-20` export ends there, so the fall logs contribute to every table except the outdoor one.
- **No humidity event was ever fitted.** The only deliberate ventilation tests with humidity measured — three M3-only windows on 2026-07-11 — ran with door 1 open throughout, as did the two on 07-04 ([`thermalProfileCampaign.md`](../thermalProfileCampaign.md) §9.12.4), so the plants carry no humidity response and the closed loop cannot score any of this. The numbers above are counted from the logs, not simulated, which is why that does not block them.
- **One season, one house.** June to September, no winter, and 5C88's geometry.
- **The demand layer only.** Dwell, wind override, the inhibit mask and sensor faults all sit between these hours and any actual window movement, and none of them are in the count.
