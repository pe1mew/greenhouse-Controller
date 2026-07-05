# Summer-2026 Calibration Campaign — Results

| Field | Value |
|---|---|
| Document | Campaign results summary (conclusions only; derivations live in [thermalProfileCampaign.md](thermalProfileCampaign.md) §9.5–9.9) |
| Campaign window | 2026-06-04 → 2026-07-04, unit 5C88 (production, Herenboeren Willemshoeve) |
| Status | **Calibration complete.** All six plant parameters identified, including `ach_m3` via the deliberate M3-only test of 2026-07-04 (NS-6). AC-9 open-loop accuracy target not met — closed-loop simulation is the remaining step. |
| Adopted artifact | [`campaign-summer-2026/plant_calibrated_constrained_summer2026_freem3.json`](campaign-summer-2026/plant_calibrated_constrained_summer2026_freem3.json) |
| Data volume | 87 301 indoor samples (30 s cadence), 47 866 calibration-valid (54 %); outdoor T/RH/lux from LoRa lht65-20 (10 min); door exclusion from LDS01-5/6 |

---

## 1. The calibrated model

Six-parameter lumped thermal model, `ach_m2 = ach_m1` by prior (identical windows). Fitted on Jun 4–18 + Jun 25–Jul 4; validated on the held-out week Jun 19–25 (never seen in training).

| Parameter | Value | Meaning | Confidence |
|---|---|---|---|
| `k_solar` | 0.0847 W/lux | solar gain per outdoor lux | High — 30 days of solar transients |
| `c_eff` | 2.894 MJ/°C | effective heat capacity | High |
| `transpiration` | 0.0003 kg/s | canopy latent-heat sink | High |
| `ach_inf` | 0.203 /h | infiltration, all closed | High — thousands of night rows |
| `ach_m1` = `ach_m2` | 0.166 /h | per roof window (8 m²) | High |
| `ach_m3` | **0.050 /h** | north-wall window (80 m²) | **Upper bound** — optimiser pins at any floor given; steady-state check gives 0.03–0.09 /h |

ACH by window state: all closed 0.20 → M1 0.37 → M1+M2 0.54 → **all open 0.59 /h**.

> Scale caveat: absolute ACH values assume greenhouse volume V = 2 900 m³ (derived, not measured). Conductances (W/K) and all *ratios* between states are volume-independent — the findings below do not change if V is revised.

### Validation (held-out week Jun 19–25)

| Model | T RMSE | within ±1 °C | RH RMSE |
|---|---|---|---|
| Spring-2026 (previous artifact) | 4.96 °C | 9 % | 16.1 % |
| Binary open/closed (first pass) | 1.36 °C | 52 % | 5.7 % |
| **Constrained 6-param, free-m3 (adopted)** | **1.19 °C** | 55.7 % | 6.3 % |

## 2. Key findings

**F1 — The model works, and is 4× better than its predecessor.** Val T RMSE 1.19 °C vs 4.96 °C for the spring artifact. It reproduced the Jun 26 heatwave peak (42.2 °C) and daily profiles across 30 days including a heatwave, cool spells, and rain days.

**F2 — M3 is an ineffective ventilator (the campaign's central surprise).** The 80 m² north-wall window (*Zijwandbeluchting*) contributes ≤ 0.05 /h — about **0.3× one 8 m² roof window**, where area scaling predicted ~10×. Three independent lines of evidence agree:
1. *Direct test (NS-6, 2026-07-04):* two 60-min M3-only openings; indoor temperature **rose** ~2.4 °C/h at 30–50 k lux with the wall wide open.
2. *Saturated-controller data:* heatwave days with all windows fully open and T_in far above setpoint (no feedback confounding) — only a small total ACH explains the observed peaks.
3. *Steady-state heat balance* on the cleanest M3-only stretch: total ACH 0.21–0.26 /h.

**F3 — The "fast drop when M3 opens" is a trigger artifact, not ventilation.** Event study over all daytime openings: the slope swing after M3 opens (−16.2 °C/h) is nearly identical to the swing after *M1 alone* opens (−15.4 °C/h, an 8 m² window). Windows open at the steepest temperature rise; lux is falling at the median M3-opening (−2 860 lux/h); and RH *rises* (+13 %/h) after M3 opens — cooling without much air exchange. M3's extra effect vs matched controls: ≈ −1 °C/h (area scaling would require −10 °C/h). See §9.9 robustness check + `campaign-summer-2026/m3_event_study.py`.

**F4 — Total passive ventilation capacity is the bottleneck: ~0.6 /h with everything open.** On hot days (T_out ≳ 25 °C at high lux) no window schedule reaches setpoint — the Jun 26–28 heatwave demonstrated this live. Strategy tuning cannot fix a capacity problem.

**F5 — Window identity erratum (2026-07-05).** Earlier analysis mis-identified M3 as a roof ridge panel. Authoritative (FRS + boerHandleiding): M1 = south roof slope 8 m², M2 = north roof slope 8 m², M3 = **north side wall** 80 m² — the leeward side under prevailing SW winds. Also: 8.1× is the motor *travel* ratio (171 s/21 s); the *area* ratio is 10× (80/8 m²).

**F6 — Strategy consequences.**
- NS-7 (independent earlier-opening threshold for M3, `t_thresh_m3`): **on hold — premise refuted.** Expected benefit collapsed from "8× ventilator opened too late" to ~9 % equilibrium improvement.
- Still valid from the §9.8 analysis, unaffected by the M3 result: the −5 °C close hysteresis keeps M1 open all night, and `hyst_t` conflates trigger spacing with the close guard (not independently tunable).

## 3. Acceptance criteria

| Criterion | Target | Result | Status |
|---|---|---|---|
| AC-9 (T fidelity) | 95th-pct \|err\| ≤ 1.0 °C, ≥ 95 % within ±1 °C | 2.11 °C / 55.7 % | **FAIL** (open-loop) |
| AC-10 (RH fidelity) | RH RMSE ≤ 5 % | 6.3 % | **FAIL** |

Both criteria were written for the model's end use: vetting controller settings in closed loop. Open-loop simulation accumulates drift over multi-day horizons that controller feedback absorbs. **Path to AC-9: wire the adopted parameters into `simulation.py` (closed loop) and re-evaluate.** The plant parameters themselves are final — NS-6 removed the last unknown.

## 4. Open items

| Item | What | Why |
|---|---|---|
| NS-8 | Why is M3 ineffective? (a) measure real open aperture at full 171 s travel; (b) note north = leeward, repeat test on a windy N/NE day with door discipline; (c) check controller sensor position relative to the north wall (local-wash hypothesis); (d) optional smoke test | Decides whether NS-7 is ever revived; possibly reveals a mechanical fix worth more than any firmware change |
| Closed-loop sim | Run `simulation.py` with the adopted artifact | Closes AC-9/AC-11; enables settings vetting (the campaign's primary purpose) |
| `ach_m3` precision | Value is a floor pin (≤ 0.05–0.09 /h) | Adequate for all current decisions; only NS-8 outcomes would justify refining it |
| Volume | V = 2 900 m³ assumed | Measure/estimate from drawings if absolute ACH ever matters |

## 5. Reproducibility

```
# 1. fetch outdoor + door data from the Wenumseveld MySQL DB (192.168.20.232)
python model/fetch_lora_data.py --sensor lht65-20 --start 2026-06-04 --end 2026-07-05 --output ...
python model/fetch_lora_data.py --sensor lds01-5  ... ; ... --sensor lds01-6 ...

# 2. merge with SD logs
python model/prepare_calibration_input.py --logs model/campaign-summer-2026/ \
    --outdoor ... --door1 ... --door2 ... --output calibration_input_2026-06-04_2026-07-04.csv

# 3. calibrate (adopted variant)
python model/calibrate_plant_constrained.py \
    --input model/campaign-summer-2026/calibration_input_2026-06-04_2026-07-04.csv \
    --val-start 2026-06-18T22:00 --val-end 2026-06-25T22:00 --free-m3 --plot

# 4. event study (F3)
python model/campaign-summer-2026/m3_event_study.py
```

Requires system Python 3.11 with numpy, scipy, matplotlib, mysql-connector-python (see gotcha log 2026-07-05).

## 6. Where the derivations live

| Topic | Section in [thermalProfileCampaign.md](thermalProfileCampaign.md) |
|---|---|
| First-pass + per-bitmask identification | §9.5, §9.6 |
| Constrained model, priors, confounding analysis, NS-6 procedure | §9.7 |
| Window strategy analysis (partially superseded — read with §9.9) | §9.8 |
| NS-6 execution, re-calibration, findings, robustness check | §9.9 |
| Step-by-step status NS-1 … NS-8 | Appendix A |
