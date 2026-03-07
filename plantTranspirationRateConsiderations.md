# Plant Transpiration Rate Considerations

Plant transpiration is the dominant source of moisture inside the greenhouse.
In the model it appears as `m_transp` [kg/s] in the humidity ODE (design.md §2.4):

```
V · d(AH)/dt = m_transp - ACH · V · (AH - AH_out)
```

---

## What Drives Transpiration

Transpiration is the evaporation of water through leaf stomata. The primary drivers are:

| Driver | Effect |
|---|---|
| **Solar radiation** | Opens stomata; the dominant daytime driver — transpiration roughly tracks irradiance |
| **Vapour pressure deficit (VPD)** | VPD = p_sat(T) − p_actual; higher VPD pulls water out of the leaf faster |
| **Temperature** | Raises p_sat and therefore VPD; secondary driver |
| **Leaf area index (LAI)** | More leaf surface = more evaporating area; scales with crop density and growth stage |
| **Night / darkness** | Stomata largely close; transpiration drops to 10–20% of daytime values |

The consequence is that `m_transp` is **not constant** — it follows a diurnal cycle closely
correlated with Q_solar. Treating it as constant (as the current simulation does) is a
known simplification.

---

## Typical Values for This Greenhouse (floor area 640 m²)

Daily loss values below are scaled to the actual floor area of 640 m².
1 mm/m²/day = 640 L/day = 640 kg/day = 0.0074 kg/s (constant).

| Crop | Daily loss [mm/m²] | m_transp day [kg/s] | m_transp night [kg/s] |
|---|---|---|---|
| Tomato (mature, summer) | 2.5 – 4.0 | 0.018 – 0.030 | ~0.003 |
| Cucumber | 3.0 – 5.0 | 0.022 – 0.037 | ~0.003 |
| Pepper | 1.5 – 2.5 | 0.011 – 0.018 | ~0.002 |
| Lettuce / leafy crops | 1.0 – 1.5 | 0.007 – 0.011 | ~0.001 |

---

## Approach: Configurable General Parameter

Because various crops are planted throughout the year, `m_transp` is treated as a
**farmer-configurable parameter** rather than a crop-specific measured value.

The farmer sets a single value (or day/night pair) appropriate for the current planting.
A conservative general default covers a wide range of crops without over-ventilating:

| Parameter | Recommended general default | Rationale |
|---|---|---|
| `m_transp` (constant, if day/night split not used) | **0.010 kg/s** | ≈ 1.35 mm/m²/day; conservative mid-range; suitable as a starting point for most crops |
| `m_transp_day` (if day/night profile is used) | **0.015 kg/s** | Daytime (06:00–20:00) — stomata open, solar-driven; ≈ 1.3 mm/m²/day averaged over full day |
| `m_transp_night` | **0.002 kg/s** | Night — stomata mostly closed |

The constant value of **0.010 kg/s** is the recommended default for simplicity. It under-estimates
peak summer transpiration for high-demand crops (cucumber, tomato) but errs on the side of
less ventilation, which avoids unnecessary cooling. The farmer should increase this value
for high-demand crops in summer.

---

## Steady-State Impact on Humidity

Setting d(AH)/dt = 0 in the ODE gives the steady-state indoor/outdoor absolute humidity
difference:

```
ΔAH = m_transp / (ACH_total · V)
```

With V = 2400 m³ and ACH values TBD, exact numbers cannot yet be given. The formula
shows that for a greenhouse 4.8× larger than the original reference (2400 vs 500 m³),
the humidity build-up from the same `m_transp` is 4.8× smaller — but the actual crop
load is also proportionally larger because the floor area is 2.56× larger. The net effect
depends on the ACH values, which are still to be determined.

---

## How to Measure It for This Greenhouse

| Method | Practicality | Accuracy |
|---|---|---|
| **Water balance** — measure water supplied minus drainage over 24 h, divide by floor area | Easy; requires only a flow meter and a day of observation | ±10–20% |
| **Weighing lysimeter** — weigh a representative plant pot over time | Accurate but requires a scale and isolation from irrigation | ±5% |
| **Energy balance** — latent heat ≈ 60–80% of net solar radiation absorbed by the crop | Requires a pyranometer; useful for a diurnal model | ±15–25% |
| **WUR crop coefficients** — use Penman-Monteith with published k_c values for the specific crop | No measurement needed; good first estimate | ±20–30% |

---

## Recommendation for the Model

Use the constant default of 0.010 kg/s as the initial simulation value. The farmer
adjusts this when the crop changes. For higher accuracy, replace with a day/night profile:

```python
def m_transp(t_seconds: float) -> float:
    hour = (t_seconds / 3600) % 24
    if 6.0 <= hour <= 20.0:
        return 0.015   # kg/s — daytime; adjust upward for tomato/cucumber in peak summer
    return 0.002       # kg/s — night (stomata mostly closed)
```

This captures the most important diurnal variation without requiring solar radiation data.
