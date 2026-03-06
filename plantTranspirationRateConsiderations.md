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

## Typical Values for Dutch Greenhouse Crops

For a floor area of approximately 250 m² (consistent with V ≈ 500 m³):

| Crop | Daily loss [mm/m²] | m_transp day [kg/s] | m_transp night [kg/s] |
|---|---|---|---|
| Tomato (mature, summer) | 2.5 – 4.0 | 0.007 – 0.012 | ~0.001 |
| Cucumber | 3.0 – 5.0 | 0.009 – 0.014 | ~0.001 |
| Pepper | 1.5 – 2.5 | 0.004 – 0.007 | ~0.001 |
| Lettuce / leafy crops | 1.0 – 1.5 | 0.003 – 0.004 | ~0.0005 |

The current simulation placeholder of **0.003 kg/s** corresponds to roughly 1 mm/m²/day
over 250 m², which is realistic for a lightly planted crop or cool conditions, but low
for a full canopy in midsummer.

---

## Steady-State Impact on Humidity

Setting d(AH)/dt = 0 in the ODE gives the steady-state indoor/outdoor absolute humidity
difference:

```
ΔAH = m_transp / (ACH_total · V)
```

With one window open (ACH_total = 10 h⁻¹ ≈ 0.00278 s⁻¹) and V = 500 m³:

| m_transp [kg/s] | ΔAH [kg/m³] | ≈ ΔRH at 20°C |
|---|---|---|
| 0.003 (placeholder) | 0.0022 | ~30% |
| 0.007 (tomato, day) | 0.0050 | ~70% |
| 0.012 (cucumber, peak) | 0.0086 | >100% → condensation risk |

This shows that `m_transp` is a **critical parameter**: it directly determines how many
windows must be open to keep RH in range. An underestimate leads to a controller that
opens too few windows; an overestimate causes unnecessary cooling.

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

Once the crop is known, replace the constant placeholder with a simple two-value
day/night profile:

```python
def m_transp(t_seconds: float) -> float:
    hour = (t_seconds / 3600) % 24
    if 6.0 <= hour <= 20.0:
        return 0.007   # kg/s — daytime (e.g. tomato, 250 m²)
    return 0.001       # kg/s — night (stomata mostly closed)
```

This is still a simplification but captures the most important diurnal variation
without requiring solar radiation data.
