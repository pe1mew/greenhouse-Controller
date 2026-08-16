# Window position sensor (M3) — requirements study

**Status:** PRELIMINARY STUDY — not adopted. Nothing in the firmware, FRS, TSDS or test plan has been changed on account of this document. Requirement IDs below (`FR-WP*`, `NF-WP*`) are proposed, not allocated.

**Date:** 2026-07-28
**Scope:** requirements a Modbus window-position sensor must meet to be fitted to **M3** and used to enhance climate control.
**Given:** the transport is decided — **Modbus RTU over the existing RS485 bus**.

---

## 1. Why — the problem this would solve

Three distinct gaps, in descending order of value.

### 1.1 The control loop limit-cycles because M3 is a binary 80 m² actuator

Measured on 5C88, 20 July 2026 (analysis: `model/vent_step_replay.py`):

| property | measured |
|---|---|
| oscillation period | ~42 min |
| temperature swing | ~4.9 °C (peaks 30.5–33.2, troughs 24.9–28.6) |
| M3 openings | 3.1–3.4 per day |

The mechanism is textbook bang-bang control with a minimum on-time: amplitude ≈ cooling-rate × dwell = 0.2 °C/min × 25 min ≈ 5 °C, and period ≈ dwell + reheat ≈ 42 min. Both predictions match the measurement.

The ventilation step ladder jumps from step 2 (M1+M2, ~16 m²) to step 3 (+M3, ~80 m²) — an aperture increase of roughly **6× in a single move**. That discontinuity is the whole cause. The greenhouse overshoots ~5 °C below setpoint, then reheats.

Since amplitude = cooling-rate × dwell, only two levers shrink it: shorten the dwell (which doubles motor cycling) or **reduce the aperture step** (which does not). Position feedback is what makes the second possible: partial opening turns a 6× jump into a graded response. **This is the primary justification.**

> Note: raising `hyst_t` was evaluated and rejected — it reduces cycling only by pushing M3's entry threshold from 31 °C to 33 °C, costing ~55 % of M3's ventilation time. A step-down dead band (`vent_hyst`) was also evaluated and rejected: the temperature makes full ~5 °C excursions past the threshold rather than hovering at it, so a practical 1–2 °C band merges no openings at all (gh#47, closed).

### 1.2 Position is currently open-loop dead reckoning

T2 infers position purely from elapsed relay-energisation time (`travel_s` + 5 s margin). Nothing verifies the window actually moved. Invisible today: motor slip, drive-belt/chain failure, obstruction, ice, end-switch failure, a window physically blocked part-way. The firmware would report `OPEN` for a window that never left its seat.

### 1.3 Position is lost across power interruption mid-travel

NVS persists terminal states only (`OPEN` / `CLOSED` / `UNKNOWN`). Power loss during a stroke leaves `UNKNOWN`, requiring a full CLOSE_ALL recalibration — 171 s of M3 travel — before normal control resumes.

### 1.4 The M3 mechanism (confirmed by the operator, 2026-07-28)

Established during this study; it was not previously recorded in the repository and it materially constrains the sensor choice.

**Geometry** (from `manual/images/KasRaamlocaties.png`): the greenhouse is 40 m × 16 m (10 bays × 4 m, two 8 m spans). M3 — *zijwand noord* — runs the **full 40 m** of the north wall. At 80 m² that is a continuous aperture roughly **2 m high**. It is a foil greenhouse ("FIX Folie", insect net).

**Mechanism:** a **hanging flap suspended on ropes** in front of the opening. The ropes are wound up and down by a **single 40 m line shaft driven by one motor**.

- **Closed** = flap raised, covering the opening.
- **Open** = flap lowered, hanging below the opening.
- Motion at the flap is **vertical translation**, roughly 2 m of travel over 171 s ≈ **11.7 mm/s**.
- Motion at the drive is **rotary**, many turns of the shaft.

Four consequences follow, and they drive the requirements below:

1. **The flap does not tilt.** Angular sensing (inclinometer on the leaf) is ruled out — see §9.
2. **Drive-side measurement is unreliable here.** Rope winding on a drum is not necessarily linear with flap height: if the rope spools in layers the effective drum radius changes through the stroke, so shaft rotation maps non-linearly — and not necessarily repeatably — onto aperture. Add rope stretch, slack and possible slip on the drum. This makes FR-WP03 (measure the flap, not the drive) a hard requirement rather than a preference.
3. **Raising and lowering are mechanically asymmetric.** The motor lifts the flap against gravity to close, and pays it out to open. Rope tension therefore differs by direction, so backlash and slack behave differently in each — a likely source of directional hysteresis (see AT-WP02).
4. **A single rigid shaft synchronises all ropes**, which largely answers the uniformity concern a 40 m aperture would otherwise raise. It does not eliminate it: a 40 m shaft driven from one end will wind up torsionally under load, and per-drum differences accumulate along the run, so the far end may lag the driven end.

**A hanging flap is also wind-exposed by construction** — it hangs free in front of the opening rather than being rigidly framed. Sway and billowing under wind are expected, which introduces a measurement question that a rigid window would not have (FR-WP21).

---

## 2. What the sensor must measure

| ID | Requirement | MoSCoW |
|---|---|---|
| FR-WP01 | The sensor **shall** report the position of the M3 window leaf over its full mechanical stroke, from fully closed to fully open, as a monotonic value. | Must |
| FR-WP02 | The reported position **shall be absolute** — valid immediately at power-on without a homing move, reference run, or prior knowledge of history. | Must |
| FR-WP03 | Measurement **shall** be taken from the **window leaf or its driven linkage**, not from motor shaft rotation, so that drive slip, belt/chain failure and obstruction are observable as a divergence between commanded and measured position. | Must |
| FR-WP04 | Resolution **shall be ≤ 1 %** of full stroke. | Must |
| FR-WP05 | Repeatability **shall be ≤ ±1 %** of full stroke over the operating temperature range — the same commanded aperture must produce the same physical aperture. | Must |
| FR-WP06 | Absolute accuracy **should be ≤ ±2 %** of full stroke. | Should |
| FR-WP07 | The sensor **should** report, or allow the controller to derive, a fully-closed indication distinguishable from "near closed", so weather-tightness can be confirmed. | Should |
| FR-WP21 | The sensor and its mounting **shall** tolerate wind-induced sway and billowing of the hanging flap without loss of calibration, and the reported position **should** represent the flap's mean vertical position rather than its instantaneous excursion. Filtering to achieve this **shall not** breach the group-delay limit in FR-WP09. | Must / Should |
| FR-WP22 | The measurement point **should** be chosen to represent the aperture as a whole. Given the 40 m span and one-end drive, the far end from the motor is the worst case for torsional lag; measuring there, or characterising the two ends once during commissioning, **should** be considered. | Should |

**Reasoning for FR-WP02 (absolute).** An incremental encoder would reintroduce §1.3 — the exact failure the sensor is meant to remove — and would need a homing move after every power cut.

**Reasoning for FR-WP04/05 (resolution and repeatability).** If M3 is to be driven to, say, 25/50/75/100 % apertures, positioning must be accurate to well under half a step. Repeatability matters more than absolute accuracy here: the climate loop cares that "50 %" is always the same aperture, not that it is exactly half. Absolute accuracy only matters for the plant model (§7.2).

---

## 3. Timing — the requirement most likely to be missed

This is a **closed-loop positioning** application, not a monitoring one, and the polling cadence follows from the mechanics rather than from convenience.

M3 traverses its full stroke in 171 s, i.e. **0.58 % of stroke per second**. To stop at a target aperture the controller must detect the crossing and de-energise the relay. Overshoot is therefore ≈ (sampling interval) × 0.58 %/s:

| sampling interval | overshoot per move |
|---|---|
| 30 s (today's T5 cadence) | **17.5 %** of stroke — unusable |
| 5 s | 2.9 % |
| **1 s** | **0.58 %** |
| 0.5 s | 0.29 % |

| ID | Requirement | MoSCoW |
|---|---|---|
| FR-WP08 | The sensor **shall** support being read at **≥ 1 Hz** while the window is in motion, with a fresh (not cached or heavily filtered) value each read. | Must |
| FR-WP09 | Internal filtering/averaging **shall not** introduce more than ~1 s of group delay, or **shall** be configurable down to that. | Must |
| FR-WP10 | The sensor **shall** complete a Modbus transaction within **200 ms** (`MODBUS_TIMEOUT_MS`), and **should** respond within 50 ms so 1 Hz polling leaves ample bus margin. | Must |

**Bus-capacity check.** A holding-register read at 9600 8N1 is **≈ 25 ms** (~1.04 ms/byte; 8-byte request + 9-byte response + 3.5-character silence each way). At 1 Hz that is ~2.5 % bus utilisation, alongside the existing 30 s polls of FG6485A and S200 (~0.08 % each). **Bus bandwidth is not the constraint.** Fast polling also only runs while a window is travelling — roughly 15 strokes/day × 176 s ≈ 3 % duty — so the averaged cost is negligible. *(Calculated, not measured; confirm on the bench, as a device with slow turnaround could be several times this.)*

**Polling is the only option — a threshold interrupt is not available (settled 2026-07-31).** Modbus RTU cannot deliver one: it is strictly master–slave, a slave transmits only when addressed, and broadcast runs master → slaves (write-only, no response). The industrial alternative is a sensor **setpoint/threshold digital output** wired to a GPIO — the pattern already used here for the RRK-3 motor alarm (`PIN_OPTO_INPUT`, GPIO 42, deferred ISR). A particularly neat variant would be to *write* the stop target over Modbus before a move and take a hardware edge when it is reached, giving exact stopping with zero polling latency.

**That is ruled out on this hardware.** `realisation/installation.md` lists J10 as the only opto-isolated input terminal and it carries the motor alarm; there is no spare input, and adding one means a PCB revision, which the operator has excluded at this stage. **Consequently a threshold/setpoint output is NOT a selection criterion for the sensor** — it may still be worth noting as a nice-to-have for a future board generation, but no candidate should be preferred or rejected on it today. The requirements in this section stand on polling alone, and the arithmetic above shows polling is sufficient: ~12 mm of overshoot on a ~2 m stroke.

**Architectural constraint — flagged, not solved.** `sensor_poll.h` states T5 has **exclusive** ownership of the Modbus bus, and T5 sleeps on a `poll_interval_s` cadence (default 30 s). Nothing in the present design can poll at 1 Hz during a stroke while T2 owns the relay. Reconciling this is a firmware-design question that must be answered **before** procurement, because it may constrain the choice (e.g. a sensor that can be polled by whichever task holds the bus, versus one needing a persistent session). Options to evaluate: a fast-poll mode in T5 triggered while any channel is moving; moving bus ownership behind a mutex shared with T2; or a dedicated position-poll task. **No option is endorsed here.**

---

## 4. Modbus / electrical interface

The bus is fixed and already populated. A candidate sensor must join it without disturbing the two devices in service.

| Parameter | Existing bus | Requirement on the new sensor |
|---|---|---|
| Physical layer | RS485 2-wire half-duplex, transceiver DE/RE on GPIO 8, TX GPIO 17, RX GPIO 18 (UART1) | must be 2-wire RS485, half-duplex |
| Protocol | Modbus RTU | must be Modbus RTU (not ASCII, not TCP) |
| Baud / framing | **9600, 8N1** | must support 9600 8N1 — fixed baud is acceptable, configurable preferred |
| Response timeout | 200 ms | see FR-WP10 |
| Addresses in use | **1** = FG6485A (T/RH), **44** = S200 (wind) | address must be configurable and set to something other than 1 or 44 |

| ID | Requirement | MoSCoW |
|---|---|---|
| FR-WP11 | Slave address **shall** be user-configurable and settable without returning the unit to the supplier. | Must |
| FR-WP12 | Position **shall** be readable in a **single transaction** (one contiguous register block) so a read is atomic with respect to motion. | Must |
| FR-WP13 | The sensor **shall not** require broadcast writes, bus-master behaviour, or any proprietary framing that a generic RTU master cannot issue. | Must |
| FR-WP14 | The device **shall** observe standard RTU inter-frame silence (3.5 char) and release the line promptly, so it cannot stall the shared bus. | Must |
| FR-WP15 | Register semantics **shall** be documented by the manufacturer: address, function code, data type, scaling, endianness, and word order. | Must |
| NF-WP01 | Supply voltage **should** match what is already distributed to the window location; if not, the additional supply becomes part of the installation scope. **Open item — the available voltage and cable run to the M3 window are not recorded in this repository and must be surveyed.** | Should |
| NF-WP02 | Wiring **should** reuse the documented RS485 junction arrangement (`hardware/ModBusRS485SingleCableJunction/`); the M3 spur must preserve bus topology (daisy-chain, no long stubs) and termination/bias practice. | Should |

---

## 5. Environment

M3 is the north **wall** vent (*Zijwandbeluchting*). The sensor sits at the window, i.e. in greenhouse air with outdoor exposure at the aperture.

Measured on-site (5C88 SD logs, June–July 2026): indoor air to **33 °C**, RH to **92 %**, and RH above 85 % through most nights — condensing conditions are routine, not exceptional.

| ID | Requirement | MoSCoW |
|---|---|---|
| NF-WP03 | Operating temperature range **shall** cover at least **−20 °C to +70 °C**. (Dutch winter minimum with margin; upper bound allows for solar gain on hardware near glazing, well above the 33 °C air maximum measured.) | Must |
| NF-WP04 | The sensor **shall** be rated for **100 % RH, condensing**. | Must |
| NF-WP05 | Ingress protection **shall be ≥ IP65**; **IP67 preferred** where the sensor sits at the aperture and may be rain-wetted with the vent open. | Must / Should |
| NF-WP06 | Materials **shall** withstand continuous UV exposure and the greenhouse chemical environment (humidity, fertiliser aerosols, occasional washdown). | Must |
| NF-WP07 | The sensor **shall** tolerate motor-induced vibration and end-stop shock without drift or loss of calibration. | Must |
| NF-WP08 | Service life **should** exceed the actuator's, at ~3–4 full strokes/day (measured: 3.1–3.4 M3 openings/day) — of the order of 25 000 cycles over 20 years. Contacting/wiper technologies must be assessed against this. | Should |

---

## 6. Failure behaviour and safety

**The governing principle: this sensor is an enhancement and must never become a dependency of any safety function.**

Wind protection (T3) closes windows on the wind average alone and must remain able to do so with the position sensor absent, failed, or reporting nonsense. Nothing in the wind-override path may consult window position.

| ID | Requirement | MoSCoW |
|---|---|---|
| FR-WP16 | A failed or disconnected sensor **shall** be detectable — it **shall not** return stale, frozen, or plausible-but-wrong data. Silence (timeout) or an explicit error is required. | Must |
| FR-WP17 | On sensor fault the controller **shall** fall back to the existing time-based open-loop control and continue operating with M3 as a binary actuator. Loss of the sensor **shall not** disable ventilation. | Must |
| FR-WP18 | Wind override, motor-alarm handling and the boot CLOSE_ALL recalibration **shall not** depend on the position sensor in any way. | Must |
| FR-WP19 | The sensor fault **shall** be surfaced to the operator on the same footing as the existing T/RH and wind sensor faults (event log, status payload, LCD/GUI indication). | Must |
| FR-WP20 | Out-of-range or implausible readings (outside stroke, or a jump exceeding the mechanically possible rate of 0.58 %/s) **should** be rejected by the controller rather than acted upon. | Should |

**Fit with existing fault handling.** T5 already flags a sensor fault after **two consecutive read failures**, raising `EG1_BIT_SENSOR_FAULT_T` / `_W`; T5 logs faults on `channel` 4/5 with the ch-based encoding. A position sensor would follow the same pattern and needs its own channel code and EG1 bit. Note the wind-sensor precedent is **safe-fail** — a failed wind sensor forces windows closed. A failed *position* sensor must **not** do that; it should degrade to open-loop, per FR-WP17. The distinction is deliberate and worth stating explicitly in any future FRS text.

---

## 7. What this does and does not buy

### 7.1 Enabled
- **Proportional M3 control** — the limit-cycle cure (§1.1). Halving the effective aperture roughly halves the overshoot amplitude at unchanged cycling frequency.
- **Closed-loop verification** — commanded vs measured divergence exposes slip, obstruction and mechanical failure that are invisible today.
- **Fast recovery after power loss** — position known immediately, no 171 s recalibration (§1.3).
- **Travel-time self-calibration** — `travel_s` could be measured rather than configured per unit.

### 7.2 Enabled for the plant model
The summer-2026 campaign established that `ach_m3` is strongly wind-direction dependent (~0.05 /h leeward SW, ~3–8 /h windward N/NE) and that NS-9 (a direction-dependent M3 model) is blocked on data. Aperture measurement would allow `ach_m3 = f(aperture, direction, speed)` instead of a binary open/closed term — a materially better model. This is a research benefit, not an operational one, and does not on its own justify the fitting.

### 7.3 NOT solved by the sensor alone
**A position sensor changes nothing by itself.** The limit cycle is only damped if the *control logic* is also reworked to command intermediate apertures — the vent step table, `NUM_VENT_STEPS`, T2's positioning logic and T6's step selection all change. The sensor is a prerequisite, not a fix. That firmware work is out of scope here and is the larger part of the effort.

---

## 8. Open items — resolved 2026-07-28

All five original items are now closed or reduced to a site measurement.

| # | Item | Outcome |
|---|---|---|
| 1 | **M3 mechanism and stroke geometry** | **RESOLVED** (operator, 2026-07-28). Hanging flap on ropes, wound by a single 40 m line shaft; vertical travel ~2 m over 171 s; raised = closed, lowered = open. Full description and its four consequences in §1.4. Sensor type narrowed in §9. |
| 2 | **Polling architecture** | **RESOLVED — feasible.** Not achievable by configuration (`CFG_MIN_POLL_S` = 30, `SP_POLL_MIN_S` = 15; even 15 s leaves 8.75 % overshoot), and there is **no mutex on the Modbus bus** — T5 is sole owner by convention, and T2 holds no Modbus references at all. But T5 can sleep in 1 s chunks instead of one 30 s block and read *only the position register* on each chunk while a channel is moving. That yields 1 Hz with **no mutex, no new task, and the climate cadence unchanged at exactly 30 s**. See the warning below. |
| 3 | **Supply voltage and cable run** | **MOSTLY RESOLVED.** `realisation/installation.md`: existing sensors take **+24 V, GND, RS485-A, RS485-B** on 4-pin terminals (J9 FG6485A, J5 S200), shielded twisted pair, shield earthed one end. A third device uses the identical pattern, and `hardware/ModBusRS485SingleCableJunction/` exists for adding drops. At 9600 baud over 40 m, RS485 is electrically trivial. **Remaining: the physical cable route to the M3 flap — a survey item, not a design risk.** |
| 4 | **One sensor or three** | **RESOLVED — M3 only.** M1/M2 are ~8 m² each at 21 s travel and contribute 16 m² spread over two steps; they are already graded. The limit cycle is caused specifically by M3's 80 m² arriving as a single ~6× aperture jump. Fitting all three triples cost to fix a problem only M3 has. |
| 5 | **Existing drive feedback** | **RESOLVED — none.** Operator confirms OPEN/CLOSE windings only, consistent with `installation.md`. A standalone Modbus position sensor is the only route. |

> **Warning attached to item 2 — do not speed T5 up naively.** T5's sliding averages are *sample-count* based (`window = avg_win_min × 60 / poll_interval_s`). Simply lowering `poll_interval_s` to get faster position sampling would silently shorten every climate averaging window and corrupt the T/RH/wind averages the control loop depends on. The chunked-sleep approach above exists precisely to avoid this: the climate sampling cadence must stay at 30 s while only the position register is read faster.

> **Also for item 2:** do not place blocking Modbus I/O inside T2. It is High priority and WDT-subscribed; a 200 ms bus timeout in its loop could delay wind-override response. T5 should read the position and hand off to T2, not the reverse.

### Remaining before procurement

1. **Site survey**: cable route from the controller to the M3 flap, and the physical mounting point for the sensor (which also settles FR-WP22 — where along the 40 m span to measure).
2. **Confirm the rope-drum winding**: single-layer grooved drum, or multi-layer spooling? This determines whether the shaft-encoder fallback in §9 is viable at all (§1.4 item 2).
3. **Decide the control-logic scope.** The sensor is a prerequisite, not a fix (§7.3). The vent step table, `NUM_VENT_STEPS`, T2 positioning and T6 step selection all change, and that work is unscoped. **Procuring the sensor without committing to this yields better diagnostics and nothing else.**

## 9. Candidate technologies (assessment only — none endorsed)

Now that the mechanism is known (§1.4 — hanging flap, vertical travel ~2 m, rope-wound from a 40 m shaft), the field narrows sharply.

| Type | Fit for a rope-hung vertical flap | Verdict |
|---|---|---|
| **Draw-wire (string pot), Modbus output**, wire attached to the flap | Measures flap height **directly** — immune to drum-radius nonlinearity, rope stretch and slip (§1.4 item 2). 2 m stroke is mid-range for commodity units. Absolute. | **Preferred** — the only option that satisfies FR-WP03 cleanly. Risks: wire fatigue over ~25 000 cycles, ingress at the wire exit, and the wire itself is another thing hanging in the aperture. |
| **Multi-turn absolute rotary encoder on the line shaft** | Easy mechanical fit, sealed, excellent life, no moving part in the aperture. | **Viable fallback**, but it is a **drive-side** measurement and therefore violates the intent of FR-WP03: it cannot see rope slack, flap binding, or a broken rope, and inherits any drum-winding nonlinearity. Acceptable only if commissioning shows the shaft-to-height relationship is repeatable, and accepting that §1.2 (slip/obstruction detection) is largely forfeited. |
| **Non-contact ToF / ultrasonic** measuring to the flap edge | No mechanical attachment, nothing in the load path. | **Doubtful.** Condensation, dust and insects on the optical/acoustic path in a 100 % RH greenhouse; a billowing foil flap is also a poor, non-flat target (FR-WP21). |
| **MEMS inclinometer on the flap** | — | **Ruled out.** The flap translates vertically; it does not tilt. Confirmed with the operator. |
| Linear potentiometer | 2 m of exposed track | **Ruled out.** Wiper wear plus 100 % RH condensing. |

**Recommendation for evaluation:** draw-wire as first choice, multi-turn absolute shaft encoder as the pragmatic fallback if a wire in the aperture proves impractical. The trade is explicit — the encoder is easier to fit and more durable, but buys only §1.1 (proportional control) and §1.3 (power-loss recovery), not §1.2 (mechanical fault detection).

---

## 10. Acceptance tests (proposed)

To be run before the sensor is trusted by control logic.

| Test | Method | Pass criterion |
|---|---|---|
| AT-WP01 Absolute-on-power-up | Move to a mid position, power-cycle the sensor, read | Position within ±2 % of pre-cycle value, no homing move (FR-WP02) |
| AT-WP02 Resolution and repeatability | Drive to the same target 10× from both directions | Spread ≤ ±1 % of stroke; hysteresis quantified (FR-WP04/05) |
| AT-WP03 Endpoint agreement | Full close and full open | Agrees with the mechanical end-stops; closed indication distinct (FR-WP07) |
| AT-WP04 Sample rate under motion | Poll at 1 Hz during a full 171 s stroke | ≥ 170 distinct, monotonic values; no repeated stale readings (FR-WP08/09) |
| AT-WP05 Bus coexistence | Run 1 Hz position polling alongside normal FG6485A + S200 polling for ≥ 24 h | Zero added read failures on either existing sensor (FR-WP13/14) |
| AT-WP06 Fault detection | Disconnect the sensor mid-operation | Controller flags a fault within 2 poll cycles and falls back to time-based control; ventilation continues (FR-WP16/17) |
| AT-WP07 Safety independence | Disconnect the sensor, then force a wind override | Windows close normally and completely (FR-WP18) |
| AT-WP08 Environmental soak | Full-stroke cycling through a condensing night, and at temperature extremes if a chamber is available | No drift beyond ±2 %; no ingress (NF-WP03/04/05) |
| AT-WP09 Slip detection | Physically obstruct the window mid-travel | Commanded/measured divergence detected and reported (FR-WP03) |

---

## 11. Summary

A Modbus RTU position sensor on M3 is technically well matched to the existing bus — 9600 8N1, one free address, ~2 % added bus load at 1 Hz.

The three requirements most likely to be overlooked, and most likely to make a purchase useless:

1. **Absolute, not incremental** (FR-WP02) — otherwise the power-loss problem remains.
2. **≥ 1 Hz fresh readings during motion** (FR-WP08) — at the present 30 s cadence, positioning would overshoot by 17.5 % of stroke, and the architectural question of *who* polls that fast is unresolved (§3).
3. **Measured at the leaf, not the motor** (FR-WP03) — otherwise slip and obstruction stay invisible, discarding a large part of the value.

And the honest caveat: the sensor is a **prerequisite** for fixing the ventilation limit cycle, not a fix. The control-logic rework that would actually use it is the larger piece of work and has not been scoped.
