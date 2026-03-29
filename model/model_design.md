# Climate Control Model Design
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Climate Control Model Design                   |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.1 (draft)                                   |
| Date         | 2026-03-29                                    |
| Status       | Draft                                         |
| Related docs | `technicalSoftwareDesignSpecification.md` §5.2 |
|              | `functionalRequirementsSpecification.md`       |
|              | `tasks.md` (T6 — Climate Control)              |

---

## Table of Contents

1. [Objective](#1-objective)
2. [Inputs and Outputs](#2-inputs-and-outputs)
3. [Schedule Resolution](#3-schedule-resolution)
4. [Graduated Ventilation Demand](#4-graduated-ventilation-demand)
5. [Conflict Resolution](#5-conflict-resolution)
6. [Anti-Resonance Mechanisms](#6-anti-resonance-mechanisms)
7. [Window Priority Order](#7-window-priority-order)
8. [Parameters Reference](#8-parameters-reference)
9. [Performance Assessment Criteria](#9-performance-assessment-criteria)
10. [Verification Approach](#10-verification-approach)
11. [Known Limitations](#11-known-limitations)

---

## 1. Objective

Maintain indoor greenhouse temperature and relative humidity within
farmer-configured bounds using the three window ventilators (M1, M2, M3) as
the sole control actuators.

The model is the algorithm executed by the T6 (Climate Control) FreeRTOS task
(TSDS §5.2). It runs periodically (default every 60 s) and issues open/close
commands to T2 (Relay Controller) via the Q1 actuation queue.

**Design goals, in priority order:**

1. Keep temperature within `[T_min, T_max]` for the active schedule.
2. Keep relative humidity within `[RH_min, RH_max]` for the active schedule.
3. Minimise window actuation count (no resonance — no rapid open/close cycling).
4. React to safety overrides from T3 (wind) and T2 (manual) without delay.

---

## 2. Inputs and Outputs

### Inputs (read from T4 Data Manager each evaluation cycle)

| Signal | Source | Description |
|--------|--------|-------------|
| `T_in` | T4 (from T5 Sensor Poll) | Indoor air temperature [°C] |
| `RH_in` | T4 (from T5 Sensor Poll) | Indoor relative humidity [%] |
| `t` | T4 (RTC / NTP) | Current timestamp (Unix epoch or elapsed seconds) |
| Active setpoints | T4 (NVS configuration) | T_min, T_max, RH_min, RH_max for the resolved schedule slot |
| Operating mode | T4 (EG1 event group) | Automatic / Standby / Wind-override / Manual-override |

### Outputs (posted to Q1 actuation queue → T2 Relay Controller)

| Signal | Description |
|--------|-------------|
| OPEN / CLOSE commands | Per window (M1, M2, M3), only when state change is needed |

The model never writes directly to relay GPIO. All commands pass through T2,
which enforces mutual exclusion and dwell timers at the hardware level
(TSDS §4.3, T2 task description).

---

## 3. Schedule Resolution

The active setpoints are resolved in two dimensions: **time-of-day** and
**season** (optional).

### 3.1 Time-of-day Schedule

| Period | Condition | Setpoints used |
|--------|-----------|----------------|
| Day | `day_start ≤ hour < day_end` | `day` schedule slot |
| Night | all other hours | `night` schedule slot |

Default boundaries: day = 06:00–20:00 (local time).

### 3.2 Seasonal Schedule (optional)

If the farmer enables seasonal schedules, they override the base day/night
setpoints for the months that fall within each season.

| Season | Months (default) | Condition |
|--------|-----------------|-----------|
| Summer | May–August | `summer_enabled = True` and month ∈ [summer_start, summer_end) |
| Winter | November–February | `winter_enabled = True` and month ∈ season window (wraps over year boundary) |
| Default (spring / autumn) | Remainder | No seasonal override active |

When a seasonal override is active, its day and night slots replace the base
day and night slots respectively. All four combinations are therefore
configurable independently:

```
Active slot = f(hour, month, config):
    season = resolve_season(month, config)   → DEFAULT / SUMMER / WINTER
    period = resolve_period(hour, config)    → DAY / NIGHT
    return config[season][period]            → ScheduleSetpoints
```

### 3.3 Schedule Transition Behaviour

At a day/night or season boundary, the active setpoints change immediately.
No ramping is applied. The hysteresis deadband (§4) prevents spurious window
commands at the moment of transition.

---

## 4. Graduated Ventilation Demand

### 4.1 Demand Levels

The model uses a discrete demand level `V` ∈ {0, 1, 2, 3} that maps directly
to the number of windows to open. Level 0 = all closed; level 3 = all open.

### 4.2 Temperature Demand

Windows are opened when indoor temperature rises above the upper limit `T_max`,
and closed when it falls below `T_max − dT_hyst` (hysteresis closing
threshold). The T_min lower limit serves as an additional guard (§5).

```
Opening thresholds (above T_max):
    T > T_max + 1×dT_step  →  demand_T = 1
    T > T_max + 2×dT_step  →  demand_T = 2
    T > T_max + 3×dT_step  →  demand_T = 3

Closing threshold (hysteresis band below T_max):
    T < T_max − dT_hyst    →  demand_T = 0

Deadband (T_max − dT_hyst ≤ T ≤ T_max + dT_step):
    demand_T = previous value (no change)
```

### 4.3 Humidity Demand

Windows are opened when RH rises above `RH_max`, and closed when it falls
below `RH_max − dRH_hyst`.

```
Opening thresholds (above RH_max):
    RH > RH_max + 1×dRH_step  →  demand_RH = 1
    RH > RH_max + 2×dRH_step  →  demand_RH = 2
    RH > RH_max + 3×dRH_step  →  demand_RH = 3

Closing threshold:
    RH < RH_max − dRH_hyst    →  demand_RH = 0

Deadband:
    demand_RH = previous value
```

---

## 5. Conflict Resolution

### 5.1 Combined Demand (no conflict)

When T and RH demand the same direction (both want more or less ventilation),
the higher demand level is used:

```
V_demand = max(demand_T, demand_RH)
```

### 5.2 T < T_min Guard (temperature floor)

When indoor temperature falls below the lower limit, windows are closed to
retain heat — regardless of humidity demand — because opening windows when it
is cold outside would cool the greenhouse further.

```
if T < T_min:
    V_demand = 0   # close all: temperature protection
```

**Exception — critical humidity override:**
If RH exceeds the critical threshold `RH_critical` even while T < T_min,
fungal disease risk overrides the temperature floor, and at least one window
is forced open:

```
if T < T_min AND RH > RH_critical:
    V_demand = max(1, demand_RH)   # minimum 1 window despite low T
```

### 5.3 T > T_max + RH below floor

When temperature is high (open windows) and humidity is low (close windows),
temperature takes priority and windows open. Opening for temperature is always
preferable to allowing overheating.

### 5.4 Conflict Log

Every resolved conflict (opposing T and RH demands) is logged to the Q3 event
queue with the active T, RH, setpoints, and the resolution applied
(TSDS §5.2).

---

## 6. Anti-Resonance Mechanisms

Rapid window cycling ("resonance") is prevented by two independent mechanisms:

### 6.1 Hysteresis Deadband

The deadband between the opening threshold and the closing threshold ensures
that a window command reversal requires a meaningful change in the measured
value — not just sensor noise or small fluctuations. See §4.2 and §4.3.

```
Effective deadband width:
    Temperature: dT_hyst + dT_step  (default 1 + 2 = 3°C)
    Humidity:    dRH_hyst + dRH_step (default 5 + 5 = 10%)
```

### 6.2 Dwell Timers

Each window has a minimum dwell time in the OPEN state (`dwell_open`) and a
minimum dwell time in the CLOSED state (`dwell_close`). A new command to
reverse the window state is suppressed until the appropriate dwell timer has
expired.

```
Command suppressed if:
    window.state == OPEN   AND (t − last_settled_t) < dwell_open
    window.state == CLOSED AND (t − last_settled_t) < dwell_close
```

Default dwell times: 600 s (10 minutes) for both open and close.

The combination of hysteresis and dwell timers means that:
- The measured value must cross a meaningful threshold before a command is issued.
- Once issued, the command cannot be reversed for at least `dwell_time` seconds.

This limits the maximum window actuation rate to:
```
max_actuations_per_hour = 3600 / dwell_time = 6  (at dwell = 600 s)
```
In practice, the hysteresis band reduces this significantly.

---

## 7. Window Priority Order

When the combined demand level is between 0 and 3, windows are opened and
closed in a fixed priority order to distribute wear evenly and optimise
airflow:

| Priority | Window | Type | Rationale |
|----------|--------|------|-----------|
| 1st | M2 — north roof vent | Roof (leeward) | Opened first; leeward position reduces wind load on open vent |
| 2nd | M1 — south roof vent | Roof (windward) | Opened second; additional airflow but higher wind exposure |
| 3rd | M3 — side wall vent | Side wall | Opened last; largest ACH contribution; used only when maximum ventilation needed |

Window closing reverses the priority (M3 closed first, M2 last).

```
V_demand = 0:  M1=CLOSE, M2=CLOSE, M3=CLOSE
V_demand = 1:  M2=OPEN,  M1=CLOSE, M3=CLOSE
V_demand = 2:  M2=OPEN,  M1=OPEN,  M3=CLOSE
V_demand = 3:  M2=OPEN,  M1=OPEN,  M3=OPEN
```

---

## 8. Parameters Reference

### 8.1 Farmer-configurable Parameters (FRS FR-C01–FR-C04, FR-CF01, FR-CF02)

Stored in NVS namespace `climate`. All four schedule slots (day, night, and
optionally summer and winter variants of each) have independent values.

| Parameter | Default (day) | Default (night) | Unit | FRS ref |
|-----------|--------------|-----------------|------|---------|
| `T_min` | 15.0 | 12.0 | °C | FR-C01 |
| `T_max` | 25.0 | 20.0 | °C | FR-C02 |
| `RH_min` | 50.0 | 55.0 | % | FR-C03 |
| `RH_max` | 80.0 | 85.0 | % | FR-C04 |

### 8.2 Administrator-configurable Parameters (TSDS §5.10)

| Parameter | Default | Unit | Description |
|-----------|---------|------|-------------|
| `dT_step` | 2.0 | °C | Spacing between graduated T opening levels |
| `dT_hyst` | 1.0 | °C | Hysteresis band below T_max for closing |
| `dRH_step` | 5.0 | % | Spacing between graduated RH opening levels |
| `dRH_hyst` | 5.0 | % | Hysteresis band below RH_max for closing |
| `RH_critical` | 88.0 | % | Critical RH threshold; overrides T_min guard |
| `dwell_open` | 600 | s | Minimum time in OPEN before close permitted |
| `dwell_close` | 600 | s | Minimum time in CLOSED before open permitted |
| `ctrl_interval` | 60 | s | Control evaluation period |
| `day_start` | 6.0 | h | Start of day schedule (local time) |
| `day_end` | 20.0 | h | End of day schedule (local time) |
| `summer_enabled` | False | bool | Enable summer setpoint override |
| `summer_start_month` | 5 | month | First month of summer (inclusive) |
| `summer_end_month` | 9 | month | First month after summer (exclusive) |
| `winter_enabled` | False | bool | Enable winter setpoint override |
| `winter_start_month` | 11 | month | First month of winter (inclusive) |
| `winter_end_month` | 3 | month | First month after winter (exclusive) |

### 8.3 Fixed Hardware Parameters (not configurable)

| Parameter | Value | Unit | Description |
|-----------|-------|------|-------------|
| `t_motor_M1` | 21 | s | M1 motor travel time |
| `t_motor_M2` | 21 | s | M2 motor travel time |
| `t_motor_M3` | 171 | s | M3 motor travel time |

---

## 9. Performance Assessment Criteria

The model is assessed on two primary objectives.

### 9.1 Climate Control Performance

A ventilation-only system can **reduce** temperature and humidity by opening
windows; it cannot **raise** them. Metrics are therefore split into
controllable events (where the controller could act) and uncontrollable
events (where outside conditions make the target physically unreachable).

#### 9.1.1 Temperature

The graduated demand function opens windows in steps driven by `dT_step`:

| Condition | Demand | Effect |
|-----------|--------|--------|
| T ≤ T_max + dT_step | 0 | No window commanded (lower deadband by design) |
| T > T_max + dT_step | 1 | M2 opens |
| T > T_max + 2×dT_step | 2 | M2 + M1 open |
| T > T_max + 3×dT_step | 3 | All 3 windows open |

The steady-state equilibrium at any intermediate demand level may itself lie
above the next trigger threshold under high solar load.  This creates stable
fixed points where demand stops escalating, which is a **design trade-off**
between anti-resonance and precise control, not a controller defect.

The "control defect" threshold is therefore `T_trig = T_max + 3×dT_step`
(demand = 3, all windows open). Additionally, even at demand = 3 the steady-state
temperature includes a residual solar gain:

    T_in_min_achievable = T_out + Q_solar / (ACH_max × V × ρ × c_p)

If `T_in_min_achievable > T_trig`, even full ventilation cannot reach the trigger;
such samples are classified as **uncontrollable** (hardware/solar limit).

| Metric | Acceptance threshold | Notes |
|--------|---------------------|-------|
| Controllable overtemp: % time T > T_trig AND T_in_min ≤ T_trig AND T_out ≤ T_max | ≤ 5% | All windows should be open (demand = 3) and max ventilation would reach T_trig; sustained exceedance is a controller defect |
| Uncontrollable overtemp: % time T > T_max AND (T_out > T_max OR T_in_min > T_trig) | Reported only | T_out too hot or solar gain alone exceeds max-vent capacity; not a controller defect |
| Graduated-demand deadband: % time T_max < T ≤ T_trig | Reported only | Expected behaviour of stepped demand; not a controller defect |

Where `T_trig = T_max + 3×dT_step` (demand=3 trigger, all windows open) and
`T_in_min = T_out + Q_solar / (ACH_max × V × ρ × c_p)` with all 3 windows open.
| % time T < T_min AND T_out ≥ T_min | ≤ 5% | Windows closed should retain warmth; failure here is a control defect |
| % time T < T_min AND T_out < T_min | Reported only | Heating not available; outside drives inside below T_min; not a control defect |

#### 9.1.2 Humidity

The graduated hysteresis demand function does not trigger the **first** window
open command for humidity until `RH > RH_max + dRH_step` (default: 85 % when
RH_max = 80 % and dRH_step = 5 %).  Time spent between `RH_max` and
`RH_max + dRH_step` is a designed deadband.  Additionally, a ventilation-only
system can reduce indoor humidity only when:

1. **T ≥ T_min** — the T_min guard is not blocking window opening.
2. **Minimum achievable indoor RH < RH_max** — even with all windows fully open,
   the steady-state indoor RH (outdoor AH + transpiration divided by max ACH)
   is below RH_max; if not, hardware limits are exceeded.

| Metric | Acceptance threshold | Notes |
|--------|---------------------|-------|
| Controllable RH overmax: % time RH > RH_max + dRH_step AND T ≥ T_min AND RH_min_achievable < RH_max | ≤ 10% | First humidity window should be open and it would help; sustained exceedance is a control defect |
| Uncontrollable — T_min guard: % time RH > RH_max AND T < T_min | Reported only | T_min guard prevents ventilation; not a control defect |
| Uncontrollable — outdoor saturated: % time RH > RH_max AND T ≥ T_min AND RH_min_achievable ≥ RH_max | Reported only | Even max ventilation cannot reach RH_max; hardware limit |
| Deadband RH: % time RH_max < RH ≤ RH_max + dRH_step | Reported only | Designed hysteresis deadband |
| % time RH < RH_min | Reported only | Ventilation cannot add moisture; informational |

### 9.2 Anti-Resonance

This is the primary quantitative pass/fail criterion.

| Metric | Acceptance threshold |
|--------|---------------------|
| Max window actuations per 10-minute window (any single window) | ≤ 1 |
| Max window actuations per hour (any single window) | ≤ 6 |
| Commands suppressed by dwell guard | > 0 (confirms guard is active) |

The 10-minute constraint is the critical resonance guard: with `dwell_open`
and `dwell_close` both set to 600 s, no window should change state more than
once in any 10-minute period. A violation indicates a dwell timer or
hysteresis misconfiguration.

---

## 10. Verification Approach

### 10.1 Dataset

**File:** `Archive/Iteration1/Environment/airTemperature_2025-05-01_to_2025-09-01.csv`

- 123-day record of outside temperature and relative humidity (30-minute intervals).
- Covers a full summer season including the hottest recorded day (33.8°C on May 1)
  and extended cool / rainy periods.
- Used as the disturbance input to the steady-state plant model.

### 10.2 Plant Model

The steady-state thermodynamic greenhouse model from
`Archive/Iteration1/Simulation/greenhouse_simulation.py` is used. It computes
indoor temperature and absolute humidity as algebraic equilibrium values given
outside conditions and window states (§2.3, §2.4 of the Archive design doc).

Key parameters used in verification:
- Greenhouse volume: 2400 m³
- ACH per roof vent (M1/M2): 8.0 h⁻¹
- ACH side wall vent (M3): 40.0 h⁻¹
- Background infiltration: 0.5 h⁻¹
- Transpiration rate: 0.010 kg/s
- Solar heat gain peak: 20 000 W

### 10.3 Verification Scenarios

| Scenario | Period | Purpose |
|----------|--------|---------|
| V1 — Full summer | May 1 – Sep 1, 2025 (123 days) | Overall season performance, monthly breakdown |
| V2 — Hot day | Day 0 (May 1, T_out up to 33.8°C) | Temperature control at extreme heat |
| V3 — High humidity | Day 30 (June 1, RH_out high) | Humidity control, conflict resolution |
| V4 — Cold night | Any night with T_out < 10°C | T_min guard effectiveness |
| V5 — Summer schedule | V1 repeated with summer override active | Seasonal setpoint comparison |

### 10.4 What Verification Covers

- Correct schedule slot selection (day/night, seasonal) over 123 days.
- Correct graduated demand calculation and hysteresis behaviour.
- Correct conflict resolution (T_min guard, RH_critical override).
- Anti-resonance: dwell timer enforcement and hysteresis deadband.
- Quantified performance metrics against the acceptance criteria in §9.

### 10.5 What Verification Does Not Cover

- Real measured inside conditions (no indoor sensor dataset available).
- Wind safety integration (T3 task; modelled separately).
- Manual override (T2 task; modelled separately).
- Exact thermodynamic accuracy of the plant model (parameters are estimates).

---

## 11. Known Limitations

1. **Ventilation is the only control mechanism.** Heating, shading, and
   irrigation are outside scope. The model cannot raise temperature or
   increase humidity — it can only reduce them by opening windows.

2. **Humidity floor (RH_min) is passive.** If RH drops below RH_min, the model
   closes all windows but cannot add moisture. The RH_min parameter is
   therefore an informational lower bound, not an actively controlled setpoint.

3. **Steady-state plant model.** The thermodynamic model assumes instant
   equilibrium at each time step. In reality, the greenhouse has thermal mass
   that delays temperature response. The steady-state model is conservative
   (overestimates how quickly the greenhouse responds to ventilation) but is
   sufficient for control algorithm validation.

4. **No wind dependency on ACH.** The plant model uses fixed ACH values
   regardless of wind speed or direction. In reality, wind significantly
   affects ventilation effectiveness. The wind safety override (T3) prevents
   window damage but the remaining ACH variation is not modelled.

5. **Winter data not available.** The real-world dataset covers May–September
   only. Winter setpoints are configurable and the schedule resolution is
   verified logically, but winter performance cannot be assessed against
   measured outside conditions.

---

*End of document — version 0.1 draft*
