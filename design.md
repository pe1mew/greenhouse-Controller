# Greenhouse Controller Design

## Status

| Section | Status |
|---|---|
| Requirements | In progress |
| Plant model | Pending |
| Control strategy | Pending |
| Simulation | Pending |
| Validation | Pending |

---

## 1. Requirements

### 1.1 Scope

Simulation-only control system for a single-zone greenhouse.
Controlled variables: **temperature** and **relative humidity**.
No hardware target; design is language-agnostic at this stage.

### 1.2 Setpoints

| Variable | Setpoint | Tolerance |
|---|---|---|
| Temperature (T) | TBD °C | ± TBD °C |
| Relative Humidity (RH) | TBD % | ± TBD % |

### 1.3 Sensors

#### Temperature sensor — Munters P-RTS-2

| Parameter | Value |
|---|---|
| Part number | 1212.0.42592 / P-RTS-2 |
| Type | 30 kΩ thermistor (2-wire) |
| Operating range | −40°C to +70°C |
| Typical accuracy | ±0.3°C |
| Max tolerance at 25°C | ±3% |
| Max cable length | 300 m |
| Min wire size | 22 AWG, shielded |
| Interface | Passive resistive (requires ADC or bridge circuit) |

#### Humidity sensor — Rotem RHS-10 SE

| Parameter | Value |
|---|---|
| Brand / Model | Rotem RHS-10 SE (Plus) |
| Operating range | −10°C to +70°C |
| RH range | 0–100% |
| Accuracy | ±2% (10–90% RH), ±3.5% (90–100% RH) |
| Supply voltage | 9–12 V AC or DC |
| Output (selected) | 0–3 VDC (linear, 0% → 0 V, 100% → 3 V) |
| Also available as | 4–20 mA or 0–10 VDC |
| Max cable length | 300 m |
| Interface | Analogue voltage output (requires ADC) |

No outside sensors are available. Outside conditions are treated as unknown disturbances.

### 1.4 Actuators

The only actuators are three motorised windows. There is no heating, cooling, humidification, or dehumidification equipment.

| Actuator | Location | Primary effect | Secondary effect |
|---|---|---|---|
| Window 1 | Roof | Ventilation (lowers T) | Lowers RH when outside is drier |
| Window 2 | Roof | Ventilation (lowers T) | Lowers RH when outside is drier |
| Window 3 | Side wall | Ventilation (lowers T) | Lowers RH when outside is drier |

#### Motor relay box — Hotraco RRK-3

The three motors are driven by a **Hotraco RRK-3** 3-fold window relay box.

| Parameter | Value |
|---|---|
| Model | RRK-3 (3-voudige raamrelaiskast) |
| Manufacturer | Hotraco Industrial BV |
| Motor supply | 3 × 400 VAC / 50 Hz |
| Motor power (standard) | 3 × 0.37 kW / ±1.2 A |
| Motor power (maximum) | 3 × 0.75 kW / ±2.1 A |
| Thermal protection | 3 × 0.85 / 1.3 A (standard) |
| Control voltage | 24 VAC or 24 VDC |
| Enclosure | IP54 |
| Dimensions (L × W × H) | 220 × 270 × 105 mm |
| Alarm relay contact | 0.5 A / 24 V |

**Per motor channel, the RRK-3 accepts these control signals (24 V):**

| Terminal | Signal | Direction |
|---|---|---|
| OPEN sturing | Open command | Controller → RRK-3 |
| DICHT sturing | Close command | Controller → RRK-3 |
| COMM. sturing | Common / return | Controller → RRK-3 |
| eindsch. OPEN | End-switch fully open | Motor mechanism → RRK-3 |
| eindsch. DICHT | End-switch fully closed | Motor mechanism → RRK-3 |
| eindsch. NOODSTOP | Emergency stop | Motor mechanism → RRK-3 |

**Critical observation from the wiring diagram:** The end-switches connect directly to the RRK-3, not to the controller. The RRK-3 cuts motor power autonomously when an end-switch is triggered. The controller **receives no end-switch signal** and has no way to know that the motor has stopped.

**Controller output requirements:** 6 × 24 V digital outputs (or relay contacts) — one OPEN and one CLOSE per window motor.

#### Interface relay — Finder 56.34.8.024.0040

| Parameter | Value |
|---|---|
| Manufacturer part | 56.34.8.024.0040 |
| Type | 4-pole changeover (4 × wisselcontact) |
| Control voltage | 24 VAC |
| Contact rating | 12 A |
| Mounting | Plug-in |
| Enclosure | IP40 |

These relays serve as the interface between the controller's 24 V digital outputs and the RRK-3 control inputs.

**Motor interface:**

Each window motor accepts two commands: `OPEN` and `CLOSE`. The motor runs until an end-switch stops it (fully open or fully closed position). The controller has **no feedback** on:
- Current window position (no encoder or potentiometer)
- End-switch activation (no signal reaches the controller)

**Implication:** The controller cannot modulate window opening proportionally. Each window is effectively a binary actuator: **fully open** or **fully closed**. Intermediate positions are possible by timing the motor, but are unreliable without position feedback.

**Estimated state:** The controller must maintain a software-tracked estimated state for each window (`OPEN` / `CLOSED` / `MOVING`). State is inferred from commands issued and elapsed time using a conservative motor run-time estimate (a fixed timeout after which the window is assumed to have reached its end position).

### 1.5 Control Limitations

Because the only actuators are ventilation windows:

- The system can **only cool** (open windows). It cannot actively heat.
- The system can **only dehumidify** (open windows). It cannot actively humidify.
- Opening windows is beneficial only when outside conditions are more favourable than inside. If outside is hotter or more humid, opening windows makes things worse. Since there are no outside sensors, this is a fundamental uncertainty.
- When T and RH call for conflicting actions (e.g. T too high but RH already too low), a conflict resolution strategy is needed.

### 1.6 Disturbances

| Disturbance | Symbol | Notes |
|---|---|---|
| Solar radiation | Q_solar(t) | Primary driver of daytime T rise |
| Outside temperature | T_out(t) | Unknown to controller; affects ventilation effectiveness |
| Outside humidity | RH_out(t) | Unknown to controller; affects ventilation effectiveness |
| Plant transpiration | m_transp(t) | Continuous moisture source; raises RH and slightly raises T |

---

## 2. Plant Model

### 2.1 Overview

The greenhouse is modeled as a single well-mixed zone with two coupled state variables:

- **T(t)**: indoor air temperature [°C]
- **RH(t)**: indoor relative humidity [%]

Coupling: a change in T shifts the saturation vapour pressure, which changes RH even without adding or removing moisture.

### 2.2 Window Ventilation Model

Each window, when open, creates an air exchange with the outside. The effective ventilation rate depends on window area, wind speed, and stack effect (especially for roof windows). Since none of these are measured, an aggregate effective air change rate is used:

- `ACH_i` — air changes per hour contributed by window i when fully open [h⁻¹]
- Total ventilation rate: `ACH_total(t) = sum of ACH_i for all open windows`

Roof windows benefit from stack effect (hot air rises), making them more effective for heat removal than the side window.

### 2.3 Temperature Dynamics

First-order thermal model:

```
C * dT/dt = Q_solar(t) - UA * (T - T_out) - ACH_total * V * rho * cp * (T - T_out)
```

Where:
- `C` — thermal capacitance of greenhouse air and contents [J/°C]
- `Q_solar(t)` — solar heat gain through glazing [W]
- `UA` — envelope heat loss coefficient (conduction + infiltration) [W/°C]
- `ACH_total` — total ventilation air change rate from open windows [s⁻¹]
- `V` — greenhouse air volume [m³]
- `rho` — air density [≈ 1.2 kg/m³]
- `cp` — specific heat of air [≈ 1005 J/(kg·°C)]
- `T_out` — outside temperature [°C] (disturbance, not measured)

The term `UA * (T - T_out)` captures passive envelope losses/gains.
The term `ACH * V * rho * cp * (T - T_out)` captures active ventilation through open windows.

### 2.4 Humidity Dynamics

Mass balance on water vapour, using absolute humidity (AH) as the internal state:

```
V * d(AH)/dt = m_transp(t) - ACH_total * V * (AH - AH_out)
```

Where:
- `AH` — indoor absolute humidity [kg/m³]
- `AH_out` — outside absolute humidity [kg/m³] (derived from T_out, RH_out; disturbance)
- `m_transp(t)` — plant transpiration rate [kg/s] (disturbance)
- `ACH_total` — ventilation rate from open windows [s⁻¹]

**Converting AH to RH** using the Magnus formula:

```
p_sat(T) = 610.78 * exp(17.27 * T / (T + 237.3))   [Pa]
AH_sat(T) = 0.622 * p_sat(T) / (p_atm - p_sat(T)) * rho_air
RH = AH / AH_sat(T) * 100                            [%]
```

### 2.5 Coupling

Opening a window simultaneously:
1. Lowers T (by replacing warm inside air with cooler outside air)
2. Lowers AH (if outside is drier than inside)
3. Changes RH both directly (via AH change) and indirectly (via T change affecting AH_sat)

This tight coupling means T and RH cannot be controlled independently.

### 2.6 Plant Parameters

| Symbol | Description | Value | Unit | Source |
|---|---|---|---|---|
| C | Thermal capacitance | TBD | J/°C | |
| UA | Envelope heat loss coefficient | TBD | W/°C | |
| V | Air volume | TBD | m³ | |
| ACH_roof | Air change rate per open roof window | TBD | h⁻¹ | |
| ACH_side | Air change rate for open side window | TBD | h⁻¹ | |
| m_transp | Plant transpiration rate | TBD | kg/s | |
| t_motor | Motor run-time to reach end position | TBD | s | |

### 2.7 Sensor Signal Conditioning

The controller must read both sensors via analogue inputs.

**Temperature (P-RTS-2 thermistor):**
- The 30 kΩ NTC thermistor requires a resistor voltage-divider or Wheatstone bridge to produce a voltage readable by an ADC.
- Resolution needed: ≤0.1°C → ADC resolution must be sufficient to resolve the non-linear thermistor curve across the relevant operating range.

**Humidity (RHS-10 SE, 0–3 VDC output):**
- Direct analogue voltage input (0 V = 0% RH, 3 V = 100% RH).
- Sensor accuracy: ±2% RH in the 10–90% range; ±3.5% at extremes.
- The ±2% inherent sensor uncertainty sets the floor on controllable precision — hysteresis bands in the control logic should be wider than this.

---

## 3. Control Strategy

### 3.1 Constraints

The control strategy must account for:

1. **Binary actuators** — windows are either open or closed (no proportional control)
2. **No position feedback** — estimated state only, updated from commands + motor timeout
3. **No end-switch feedback** — controller cannot confirm when a window has reached its limit
4. **Ventilation-only** — can only cool and dehumidify; cannot heat or humidify
5. **Actuator coupling** — all windows affect both T and RH simultaneously
6. **Unknown outside conditions** — cannot predict whether opening a window will help or harm

### 3.2 Approach

Given binary actuators and high uncertainty, a **rule-based hysteresis controller** is more appropriate than PID for the actuation layer. PID is unsuitable here because there is no continuous actuator to modulate.

**Architecture:**

```
[T sensor] ──┐
             ├──► [Control logic] ──► [Window state manager] ──► [Motor commands]
[RH sensor] ─┘                              ▲
                                    [Estimated window states]
```

**Two-layer design:**

- **Layer 1 — Decision logic:** Determines the *desired* ventilation level (how many/which windows to open) based on T and RH error and priority rules.
- **Layer 2 — Window state manager:** Tracks estimated state of each window and issues motor commands to reach the desired state. Handles motor timeouts and prevents issuing conflicting commands.

### 3.3 Decision Logic

Define ventilation demand as an integer level `V_demand ∈ {0, 1, 2, 3}` representing the number of windows to open.

**Temperature rule** (primary):

```
if T > T_sp + dT_high:   T_demand = 3          (all windows open)
elif T > T_sp + dT_mid:  T_demand = 2
elif T > T_sp + dT_low:  T_demand = 1
elif T < T_sp - dT_hyst: T_demand = 0          (close all, retain heat)
else:                     T_demand = current    (deadband, no change)
```

**Humidity rule** (secondary):

```
if RH > RH_sp + dRH_high: RH_demand = 3
elif RH > RH_sp + dRH_mid: RH_demand = 2
elif RH > RH_sp + dRH_low: RH_demand = 1
elif RH < RH_sp - dRH_hyst: RH_demand = 0
else:                        RH_demand = current
```

**Conflict resolution** (T takes priority over RH, as excessive heat is more immediately harmful):

```
V_demand = max(T_demand, RH_demand)
```

Exception: if T is below setpoint and RH is above setpoint, opening windows risks further cooling. In this case a minimum ventilation is applied only if RH exceeds a critical threshold `RH_critical`.

### 3.4 Window Selection Priority

When `V_demand` windows should be open, the selection order is:

1. Roof window 1 (highest stack-effect benefit)
2. Roof window 2
3. Side window (lowest stack-effect; also provides cross-ventilation)

### 3.5 Window State Manager

Each window has an estimated state: `OPEN`, `CLOSED`, or `MOVING`.

```
on command OPEN  window i:
    if estimated_state[i] == CLOSED:
        issue motor OPEN command
        set estimated_state[i] = MOVING
        start timeout timer(t_motor)
    elif estimated_state[i] == MOVING:
        wait (do not issue new command)

on timeout for window i:
    set estimated_state[i] = target_state  (OPEN or CLOSED)

on command CLOSE window i:
    symmetric to above
```

Motor commands are not re-issued while a window is `MOVING` to prevent conflicting signals.

### 3.6 Hysteresis Parameters (to be tuned)

| Parameter | Description | Initial value |
|---|---|---|
| T_sp | Temperature setpoint | TBD °C |
| dT_low | T threshold for 1 window | TBD °C above T_sp |
| dT_mid | T threshold for 2 windows | TBD °C above T_sp |
| dT_high | T threshold for 3 windows | TBD °C above T_sp |
| dT_hyst | T hysteresis band (close threshold) | TBD °C below T_sp |
| RH_sp | Humidity setpoint | TBD % |
| dRH_low | RH threshold for 1 window | TBD % above RH_sp |
| dRH_mid | RH threshold for 2 windows | TBD % above RH_sp |
| dRH_high | RH threshold for 3 windows | TBD % above RH_sp |
| dRH_hyst | RH hysteresis band (close threshold) | TBD % below RH_sp |
| RH_critical | RH level forcing ventilation even when T < T_sp | TBD % |
| t_motor | Motor run time to reach end position | TBD s |

---

## 4. Simulation Plan

### 4.1 Scenarios

| # | Description | Purpose |
|---|---|---|
| S1 | Daytime solar gain, windows start closed | Validate T control; check window open sequence |
| S2 | High humidity (transpiration), cool day | Validate RH control; check conflict handling |
| S3 | Full 24-hour day/night cycle | Validate disturbance rejection over time |
| S4 | T below setpoint, RH above critical | Validate conflict resolution logic |
| S5 | Motor timeout / stall (window stuck) | Validate state manager robustness |

### 4.2 Performance Metrics

| Metric | Definition |
|---|---|
| Time above T_sp + dT_high | Duration at worst-case overshoot temperature |
| Time T within tolerance | Fraction of simulation time T is within ± tolerance of T_sp |
| Time RH within tolerance | Fraction of simulation time RH is within ± tolerance of RH_sp |
| Window actuation count | Number of open/close commands issued (proxy for motor wear) |
| Conflict events | Count of T/RH conflict situations triggered |

### 4.3 Simulation Parameters

- Time step: TBD (e.g. 10 s)
- Simulation duration: TBD (e.g. 86400 s for 24-hour runs)
- Solver: TBD (e.g. Euler or RK4 for the ODEs)
- Outside T and RH profiles: TBD (e.g. sinusoidal approximation for day/night)

---

## 5. Open Questions

**Resolved:**
- [x] Sensor types and specifications confirmed (Munters P-RTS-2 thermistor, Rotem RHS-10 SE)
- [x] Motor drive confirmed (Hotraco RRK-3 relay box, 24 V control interface)
- [x] No end-switch feedback to controller confirmed (RRK-3 wiring diagram)
- [x] Controller output interface confirmed (6 × 24 V digital outputs, OPEN + CLOSE per motor)
- [x] Interface relay confirmed (Finder 56.34.8.024.0040, 4-pole, 24 VAC)

**Still open:**
- [ ] Confirm setpoints for T and RH
- [ ] Confirm greenhouse dimensions (floor area, height → air volume V)
- [ ] Estimate motor run-time to fully open/close each window (measure on physical system)
- [ ] Decide whether partial window opening (timed motor stop) should be supported
- [ ] Define outside T and RH profiles for simulation (sinusoidal day/night approximation)
- [ ] Estimate plant transpiration rate
- [ ] Confirm window priority order (roof vs. side first)
- [ ] Choose simulation language/tool
- [ ] Confirm controller hardware platform (what will generate the 24 V digital outputs and read analogue inputs)

---

## 6. Revision History

| Date | Change |
|---|---|
| 2026-03-05 | Initial draft — requirements and design structure |
| 2026-03-05 | Major revision — updated to reflect actual hardware: 3 motorised windows, no position/end-switch feedback, ventilation-only actuation, rule-based hysteresis controller replacing PID |
| 2026-03-05 | Added hardware specifications from documentation: Munters P-RTS-2 temperature sensor, Rotem RHS-10 SE humidity sensor, Hotraco RRK-3 relay box, Finder 56.34.8.024.0040 interface relay; added sensor signal conditioning notes; updated open questions |
