# Setpoint Considerations

Recommended control setpoints for a greenhouse in the middle of the Netherlands,
based on Dutch horticultural practice (WUR / Wageningen standards) for the
growing season (May–September).

---

## Temperature

| Condition | Value | Reasoning |
|---|---|---|
| **T_sp (day)** | **25°C** | Standard for broad-spectrum crops (tomato, pepper, cucumber). Below is comfortable; above increases respiration stress. |
| T_sp (night) | Not separately controllable | The system has no heating — a night setpoint is irrelevant; windows stay closed to retain warmth. |
| Absolute maximum | ~32°C | Above this, photosynthesis efficiency drops sharply for most crops. |

The historical dataset shows outdoor peaks up to **40.6°C**. On hot afternoons the
controller will frequently be unable to hold 25°C indoors — that is a known
limitation of ventilation-only control, not a reason to raise the setpoint.

---

## Relative Humidity

| Condition | Value | Reasoning |
|---|---|---|
| **RH_sp** | **75%** | Centre of the healthy range for most crops; sufficient margin below the disease threshold. |
| Upper disease threshold | ~85% | Above this, risk of *Botrytis* (grey mould) and powdery mildew increases significantly — corresponds to `RH_critical`. |
| Lower stress threshold | ~60% | Below this, stomata close, transpiration stops, tip-burn risk in leafy crops. |

Night-time RH in the Netherlands regularly reaches **90–95%** (confirmed by the
historical data). This is normal and acceptable for short periods, but sustained
periods above 85% should trigger ventilation — consistent with the `RH_critical`
parameter in the control design.

---

## Suggested Initial Hysteresis Parameters

Consistent with design.md §3.6 and sensor tolerances (±0.3°C, ±2% RH).
Hysteresis bands are intentionally wider than sensor accuracy to prevent
excessive window chatter.

| Parameter | Value | Unit | Notes |
|---|---|---|---|
| T_sp | 25 | °C | Day setpoint |
| dT_low | 2 | °C above T_sp | 1 window opens |
| dT_mid | 4 | °C above T_sp | 2 windows open |
| dT_high | 7 | °C above T_sp | All 3 windows open |
| dT_hyst | 1 | °C below T_sp | Windows close |
| RH_sp | 75 | % | Humidity setpoint |
| dRH_low | 5 | % above RH_sp | 1 window opens |
| dRH_mid | 10 | % above RH_sp | 2 windows open |
| dRH_high | 15 | % above RH_sp | All 3 windows open |
| dRH_hyst | 5 | % below RH_sp | Windows close |
| RH_critical | 87 | % | Forces ≥1 window open even when T < T_sp |

These are starting values. They should be tuned once greenhouse dimensions and
motor run-time are known and initial simulation runs have been reviewed.
