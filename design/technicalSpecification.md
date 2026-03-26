### 1.4 Physical Layout

The greenhouse is rectangular. When viewed on a floor plan with north at the top, the **north wall is the long left wall** (the building length runs east–west). The left side pointing north means: standing inside looking east along the length, the north long wall is on your left.

**Floor plan (top view):**

```
                         N
                         ↑
   ╔══════════════════ M3 (Zijwandbeluchting) ════════════════╗  ← North wall
   ║  W                                                     E ║
   ║ ─ ─ ─ ─ ─ ─ ─ ─ ─ M2 Dakbeluchting noord ─ ─ ─ ─ ─ ─ ─ ─ ║  ← ¼ width from N
   ║                                                          ║
   ║  · · · · · · · · · · · ·  ridge  · · · · · · · · · · · · ║  ← ½ width
   ║                                                          ║
   ║ ─ ─ ─ ─ ─ ─ ─ ─ ─ M1 Dakbeluchting Zuid  ─ ─ ─ ─ ─ ─ ─ ─ ║  ← ¾ width from N
   ║                                                          ║
   ╚══════════════════════════════════════════════════════════╝  ← South wall
                         ↓
                         S
         ←──────── Length (east–west) ────────►
```

**Cross-section (looking east along the length, N at left):**

```
  N wall                 ridge                   S wall
  ║                       /\                        ║
  ║   M2 vent            /  \           M1 vent     ║
  ║  [─────]            /    \         [─────]      ║
  ║         \          /      \       /             ║
  ╠══════════╲════════/════════\═════/══════════════╣
  ║  [M3 win] ────────────────────────────────────  ║
  ║  (north wall, full length)                      ║
  ╚═════════════════════════════════════════════════╝
  │←─ ¼W ─→│←─────── ¼W ───────→│←─ ¼W ─→│
```

**Window positions:**

| Dimension | Description |
|---|---|
| Greenhouse orientation | Length east–west; north long wall on the left when facing east |
| Roof type | Gabled; ridge runs east–west along the full length |
| M2 vent position on roof | North roof slope, at ¼ of the N–S width from the north wall |
| M1 vent position on roof | South roof slope, at ¾ of the N–S width from the north wall (¼ from south wall) |
| M1 and M2 vent extent | Full length of the greenhouse (east to west) |
| M3 vent position | North wall (long wall), over the full east–west length of that wall |

**Greenhouse dimensions:**

| Parameter | Value |
|---|---|
| Length (east–west) | 40 m |
| Width (north–south) | 16 m |
| Gutter (eaves) height | 3.0 m |
| Ridge height | 4.5 m |
| Floor area | 640 m² |
| Air volume | 2400 m³ (rectangular base 40 × 16 × 3 = 1920 m³ + triangular roof ½ × 16 × 1.5 × 40 = 480 m³) |

### 1.5 Actuators

The only actuators are three motorised ventilation windows. There is no heating, cooling, humidification, or dehumidification equipment.

| Actuator | Dutch name | Location | Primary effect | Secondary effect |
|---|---|---|---|---|
| M1 | Dakbeluchting Zuid | South roof slope, ¾ width from N, full length | Ventilation (lowers T) | Lowers RH when outside is drier |
| M2 | Dakbeluchting Noord | North roof slope, ¼ width from N, full length | Ventilation (lowers T) | Lowers RH when outside is drier |
| M3 | Zijwandbeluchting | North wall (side wall), full length | Ventilation (lowers T) | Lowers RH when outside is drier |

**Window opening dimensions (fully open):**

| Actuator | Opening width / height | Length | Opening area |
|---|---|---|---|
| M1 (Dakbeluchting Zuid) | 0.20 m (roof gap) | 40 m | 8 m² |
| M2 (Dakbeluchting Noord) | 0.20 m (roof gap) | 40 m | 8 m² |
| M3 (Zijwandbeluchting) | 2.0 m (wall height) | 40 m | 80 m² |

**Motor run-times (measured):**

| Actuator | Closed → Open | Open → Closed |
|---|---|---|
| M1 (Dakbeluchting Zuid) | 21 s | 21 s |
| M2 (Dakbeluchting Noord) | 21 s | 21 s |
| M3 (Zijwandbeluchting) | 171 s | 171 s |

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

**Circuit documentation:**

| Document | File | Description |
|---|---|---|
| Electrical schematic | `documentation/VentilationSystem/ElectriscSchema_001.jpg` | Full electrical schematic — denboer engineering, 12-2-2026, scheme 04, first release |
| Connection diagram | `documentation/VentilationSystem/AansluitSchema_001.jpg` | Hotraco RRK-3 connection diagram with motor labels (M1 = LUCHING R ZUID, M2 = LUCHING L NORD, M3 = SCHERM/ZIJ) |
| Full schematic PDF | `documentation/VentilationSystem/Kasventilatiesysteem schema rev 1.pdf` | Greenhouse ventilation system schematic, revision 1 |

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

**Decision — partial opening not supported:** Timed motor stop to achieve an intermediate window position is explicitly **not implemented**. The reasons are:
- No position feedback is available; a timed stop gives no guarantee of actual opening fraction.
- Repeatability is poor: friction, wear, and temperature affect motor speed over time.
- The control strategy (§3) achieves graduated ventilation by selecting how many windows to open, not by modulating individual window position.

Each window is therefore commanded only to its fully-open or fully-closed end position.

**Estimated state:** The controller must maintain a software-tracked estimated state for each window (`OPEN` / `CLOSED` / `MOVING`). State is inferred from commands issued and elapsed time using a conservative motor run-time estimate (a fixed timeout after which the window is assumed to have reached its end position).

### 1.6 Control Limitations

Because the only actuators are ventilation windows:

- The system can **only cool** (open windows). It cannot actively heat.
- The system can **only dehumidify** (open windows). It cannot actively humidify.
- Opening windows is beneficial only when outside conditions are more favourable than inside. If outside is hotter or more humid, opening windows makes things worse. Since there are no outside sensors, this is a fundamental uncertainty.
- When T and RH call for conflicting actions (e.g. T too high but RH already too low), a conflict resolution strategy is needed.

