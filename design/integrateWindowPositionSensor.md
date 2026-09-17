# Integrating the M3 window position sensor — implementation plan

| Field | Value |
|---|---|
| Document | Implementation plan |
| Date | 2026-09-07, last revised **2026-09-17** |
| Status | **Phases 0—3 COMPLETE and hardware-verified** (FDA4, 2026-09-10). **Phase 4 is SPLIT** (2026-09-13): the sensor-presence gate landed and is hardware-verified 2026-09-12, fault logging is done, and the control-side half — travel-complete, the two 12.4 rules, the operator surfaces — moved into the section 5.0 M3 slice. **Nothing consumes position yet, so the GATE before Phase 5 is still uncrossed and greenhouse behaviour is unchanged.** Phase 5 is **sequenced behind section 5.0**, no longer simply out of scope. Both prerequisites shipped (gh#49 in 2.4.1, gh#51 in 2.4.2—2.4.4). **Released in 2.8.0 and in `main` since 2026-09-17** (`ropeSensor` fast-forwarded): observe-only. T17, the presence gate and both §12.4 rules measure, log and report; soak #2 passed; nothing acts on position. **gh#73 is fixed in 2.9.0** by an installation setting, `motor/wpos_fitted_m3`, default not fitted (see *Fitted or not*). **gh#72 is fixed in 2.9.1**: each drive is judged, so a reversal is two, and the gate no longer flaps on a self-reported fault (see *Every drive judged*). **Phase 5 is scoped as of 2026-09-17 (§5b): two control modes, 2.10.0 confirms only, mode 2 is 2.11.0** |
| Requirements | [`windowPositionSensorRequirements.MD`](windowPositionSensorRequirements.MD) — FR-WP01–22, and §12 evaluating this sensor |
| Device contract | [`modbusInterfaceContractSpecification.md`](modbusInterfaceContractSpecification.md) v1.2 (normative source is the sensor project's `design/TDS.md`) |
| Bus architecture | [`refactorSensorConfiguration.md`](refactorSensorConfiguration.md) — the end state this plan deliberately does *not* build |
| Prerequisites | Both **DONE**. (1) [`addModbusMutex.md`](addModbusMutex.md) — gh#49, shipped **2.4.1**: the bus lock is what makes §4's architecture possible. (2) [`fixMotorTimingRefresh.md`](fixMotorTimingRefresh.md) — gh#51, shipped **2.4.2—2.4.4**: motor config written at runtime now reaches T2 without a reboot, is audited in the SD log, and survives a factory reset honestly. §3.5 below depends on all three of those |

---

## 1. What this delivers, and what it does not

**Delivers:** the controller knows where M3 actually is, instead of inferring it from elapsed relay time.

**Does not deliver, on its own:** any improvement to the climate loop. The sensor is a **prerequisite** for proportional M3 control, not a fix (requirements §7.3). That rework is Phase 5, and it is gated.

**Out of scope entirely:** procurement. The draw-wire unit is IP50 against a Must of ≥IP65 (requirements §12.1). That is the installer's to resolve and is noted here only so nobody re-derives it as a software blocker.

---

## 2. The development rig

Development happens on **FDA4**, which is being fitted with a mechanical mock of the M3 window — driven by the controller's own relays, with a representative wire sensor on the same Modbus bus. Per [[feedback-dev-not-production]], this is where the work belongs; 5C88 is production.

| | Production M3 | FDA4 mock |
|---|---|---|
| Full traverse | 171 s | **13 s** (measured, as-built) |
| Speed | 0.585 %/s | **7.69 %/s** (13.2× faster) |

**Representative of:** the entire software path — three devices on one bus, relay drive, position feedback, teach, fault handling, both commissioning checks, T2 stopping logic, the polling architecture.

**Not representative of:** the *consequence* of moving. FDA4 is fed **live measurements from the production environment and acts on them** (operator, 2026-09-07), so T6 step selection and T3 wind safety do run against real weather — but **no window is driven**, so opening a vent changes nothing that comes back as a measurement. The rig is **open-loop**: real inputs, real decisions, no feedback. The limit cycle (requirements §1.1) and `ach_m3` therefore stay unprovable here, as does any control-strategy outcome. Also outside its reach: the 40 m torsional lag (FR-WP22), rope-drum nonlinearity, and wind sway of a hanging flap.

> Do not read this as "FDA4 cannot exercise the climate loop". It exercises the whole of it except the part where the greenhouse answers back.

**Rig specification — agreed, and every requirement is satisfiable:** traverse ≥ 5 s (`CFG_MIN_TRAVEL_S`), sensor address 40 or 45 (both clear of FG6485A@1 and S200@44), +24 V/GND/A/B daisy-chained, both end-stop switches fitted, sensor-zone overtravel checked.

> **Build status 2026-09-10: BUILT AND OPERATIONAL** (operator). The wire position sensor is live on FDA4's RS485 bus — a **third device** alongside FG6485A@1 and S200@44 — the window emulator is driven by M3's open/close relays, and end sensors are fitted. The specification above is now an as-built record.
>
> **`travel_m3 = 13 s` is the emulator's REAL traverse time, not a placeholder.** It is a correct setting and must never be "corrected" to production's 171 s. The 15 s figure used when this plan was written was an estimate; every derived constant below is recomputed for 13 s.
>
> M1 and M2 still drive nothing — their chain ends at the relay contact. No greenhouse window is driven by FDA4 in any case; the emulator is a bench mechanism.

---

## 2a. The physical model — reference and design limitations

*Operator drawing + description, 2026-09-10, corroborated by the Phase 0 teach measurements
below. **Normative for phases 1-5.** The rig was built to this and production is being built
to it.*

### 2a.1 Three nested ranges, not two

> **Terminology — fixed 2026-09-13, and it matters.** These are two different
> devices, and this plan had been calling each by the other's name.
>
> | term | what it is | can firmware see it? |
> |---|---|---|
> | **end switch** | the switch that **stops the motor** at the physical end | **No.** It acts outside the controller's view; firmware never sees it fire |
> | **end sensor** | the indicator that the window is **0 % or 100 %** | **Yes** — it sets **bit 3** (`WINDOWPOS_ST_END_SENSOR`), and **bit 4** when both are active at once, which is a loop fault |
>
> The firmware already uses this convention, so the code was right and the
> document was wrong. **The end sensors define the opening range** — they *are*
> 0 % and 100 %. **The end switches sit outside them**, and the leaf overtravels
> past each end sensor into the blind overlap until an end switch stops it.
>
> Conflating the two is what made the endpoint reasoning in 5a come out
> backwards on 2026-09-12: the endpoints are the **best**-known points in the
> range, precisely because they have their own sensor.

```
  0 V |------------------ potmeter (wire sensor) range ------------------| 3.3 V
     [END SWITCH]--------- physical range of the window --------[END SWITCH]
       motor stops here, firmware never sees it
         [END SENSOR]---- opening range of the window ----[END SENSOR]
              0 %          (1500 mm on this rig)            100 %
                   bit 3 is set at either end sensor
```

From widest to narrowest:

1. **Potmeter range** — the wire sensor's full electrical travel, 0 V .. 3.3 V. **Wider than
   the window at BOTH ends by design**, so the window never runs the sensor into either rail.
2. **Physical range of the window** — what the mechanism can traverse. Ends at the motor's
   end switch.
3. **Opening range** — **lower end sensor to higher end sensor. This is 0 % .. 100 %.** It is
   what the operator means by "closed" and "open", and it is narrower than the physical range
   because **blinds overlap** at both ends.

**The end sensors mark the fully closed / fully open WINDOW, not the end switches.** The
end switches sit outside them, and the leaf overtravels past each end sensor into the overlap
until the end switch stops it.

### 2a.2 Rig vs production — the same geometry, different proportions

| | test rig | expected real window |
|---|---|---|
| Window's share of the potmeter range | **most of it** | a **minority** — 2 m sensor, ~1.5 m window |
| Stop-to-stop time | **~13 s** | ~171 s (**~13x slower**) |
| Switch/overlap/limit geometry | identical | identical |

**The rig behaves exactly as the real window will. The only difference is speed.** So every
constant 3 derives from `travel_m3` transfers; nothing thermal does (2).

The proportion difference matters in one place: production leaves far more unused sensor
range, so its per-count resolution in millimetres will be **coarser** than the rig's for the
same ADC span. Do not read rig resolution figures as production figures.

### 2a.3 What the current firmware does — the thing being replaced

**The shipped logic is timer-only.** T2 energises a relay for `travel_ms` and stops; it never
reads the end sensors. The motor is stopped by its own **end switch**, so **every full
stroke ends with the motor stalled against a mechanical limit** for whatever time is left on
the timer.

That is today's design and it works. **The new logic must account for the end sensors**
(FR-WP07, contract bit 3) instead of driving blind into the end switch.

### 2a.4 Measured on the rig, 2026-09-10 (Phase 0 teach)

| landmark | raw ADC | how observed |
|---|---|---|
| lower (closed) switch | **0** | bit 3 made; teach captured 0 -> 40005 |
| closed rest (end switch) | **0** | live read at rest |
| higher (open) switch | **867** | bit 3 made; teach captured 867 -> 40006 |
| open rest (end switch) | **986** | live read at rest, bit 3 **still set** |

**Open-end overtravel: 119 counts** — the blind overlap, exactly as drawn. Far too large to be
capture staleness (bounded at ~10 counts by 40002 = 100 ms and ~147 mm/s).

**Contract 5.3 sensor-zone check: PASS at the open end.** Bit 3 made at 867 and stayed made
through rest at 986, so "bit 3 set" genuinely means fully open. `OPEN` can be displayed
honestly.

### 2a.5 Consequences that constrain the design

1. **The DEVICE clamps position to `40004`; the overtravel is invisible.** *(Corrected
   2026-09-10 — an earlier draft of this section claimed readings beyond 100 % are normal and
   must not be clamped. Measured on the rig, that is wrong: the device clamps them itself.)*
   `30001` matches `raw × 40004 / (40006 - 40005)` exactly up to the open end sensor and then
   saturates:

   | raw | reads | |
   |---|---|---|
   | 619 | 1070.9 mm | matches the formula |
   | 775 | 1340.8 mm | matches |
   | 937 | **1500.0 mm** | predicted 1621.1 — **clamped** |
   | 986 | **1500.0 mm** | predicted 1705.9 — **clamped** |

   **Consequence for the new logic: position cannot distinguish "at the open end sensor" from
   "driven into the end switch".** Both read 100 % and both have bit 3 set. Anything that
   needs to know how far into the overlap the leaf sits must get it from elsewhere — or accept
   that it cannot. 0-100 % is therefore the correct display scale after all.

2. **The end sensors are authoritative for 0 % and 100 %; the wire sensor interpolates.**
   This is a cleaner split than earlier drafts assumed, and it degrades well: a wiper fault
   falls back to *today's* behaviour (switch + timer), not to nothing.

3. **Stopping at the end sensor removes a stall that happens on every stroke today.** Cutting the
   relay when bit 3 makes is a mechanical improvement, not only a positioning one. It also
   means the **effective travel time becomes shorter than the configured `travel_ms`**, which
   today must cover sensor-to-switch as well.

4. **`40004` is the sensor-to-sensor opening range**, never switch-to-switch (contract 6.4:
   "travel between the calibration points" — and the calibration points are the end sensors the
   teach captured).

### 2a.6 Closed-end headroom — ACCEPTED as a known limitation (operator, 2026-09-10)

Raw reads **0** at the closed end sensor and at closed rest. Contract 7.3 wants margin at both
landmarks; there is none at this end.

**Shifting the mount cannot fix it, and the range budget is why:**

```
closed rest raw 0  ->  open rest raw 986   = 986 counts of mechanical travel
potentiometer usable range                 = 1023 counts
spare, TOTAL across both ends              =   37 counts
```

Giving the closed end the ~52 counts it needs puts open rest at **1038, past the 1023 rail**
— it moves the clipping to the other end and loses the open overtravel with it. **The
draw-wire's electrical range is barely larger than the full mechanical travel**, so meaningful
headroom at both ends is not achievable with this sensor and this overtravel. This is a
hardware constraint, not a mounting error.

**Decision: accept it.** The consequences the design must carry:

- **Bit 6 (implausible) is inert in the closed direction.** A shorted wiper reads 0, which is
  indistinguishable from a genuinely closed window. **Requirements 12.4 rule 1 is the only
  defence** and must be implemented, not assumed.
- **Closed-end overtravel is unmeasurable.** The reading cannot go below 0, so the motion
  between the closed end sensor and the end switch is clipped.
- Bit 7 (not following) still works at this end and is unaffected — it needs excursion, not
  absolute level. Proven on this rig: see 2a.7.

Revisit only if the mechanical overtravel is reduced (freeing counts) or the sensor is
replaced with one whose range materially exceeds the travel.

### 2a.7 Bit 7 validated on a real fault, unplanned (2026-09-10)

While re-mounting the sensor the draw-wire came loose. The device caught it exactly as
specified: **bit 7 set, bit 6 clear, bit 2 clear** — which 7.2 decodes as *"the mechanism:
tangled, snapped or slipping draw-wire; the potentiometer itself is usually fine."* Raw sat at
0 across repeated full strokes while `30011` showed the sensor reading happily and `30009`
showed a clean bus.

Re-attaching the wire cleared bit 7 on the **first** good switch-to-switch sequence, as 7.2 says
it should, with raw sweeping 0..990.

**This is the fault no electrical test can find**, and it was validated here by accident
rather than by design. Worth keeping: a bench check for the position path should include
detaching the wire, not just shorting or opening the wiper.

### 2a.8 Traverse timing, measured (2026-09-10)

| leg | time |
|---|---|
| sensor-to-sensor (the actual opening range) | **10.0 s** |
| sensor-to-switch (the blind overlap) | **~3.5 s** |
| total, switch-to-switch | **~13.5 s** — consistent with `travel_m3` = 13 s covering both |

**So today's timer-only logic spends ~3.5 s of every stroke stalled against the end
switch.** New logic that cuts the relay on bit 3 removes that (2a.5 item 3), and the
effective travel time it should configure is the **10 s** sensor-to-sensor figure, not 13.

### 2a.9 Committed calibration (2026-09-10, rig)

| register | value | |
|---|---|---|
| `40001` zero offset | 0 | |
| `40002` measurement window | **100 ms** | not the 1000 ms default — see below |
| `40003` averaging window | 10 s | default |
| `40004` full travel | **15000** | 1500 mm, sensor-to-sensor |
| `40005` raw closed | **0** | at the rail; accepted per 2a.6 |
| `40006` raw open | **858** | |

**Scale: 858 counts over 1500 mm = 1.748 mm per count.**

**`40002` must be set before any teach, not after.** The teach captures `30005` at the sensor transition, and `30005` only refreshes once per measurement window. At the 1000 ms default and ~150 mm/s the capture could be a full second stale — up to 150 mm, ~86 counts of calibration error, with nothing to indicate it. At 100 ms that bound is ~15 mm.

Successive teaches captured the open end at 867, then 904 observed / 858 committed. The spread is real: it is where the end sensor makes on that pass, plus up to one measurement window of staleness. **Treat a single teach as ~±20 counts (~35 mm) repeatable, not exact** — and re-teach rather than hand-tuning if the number looks wrong.

---

## 3. The central design rule: derive every rate constant from `travel_m3`

**The rig being 11.4× faster is not a caveat — it is the most useful thing about it.** It forces constants out of the code that would otherwise have been silently production-only.

### 3.1 Poll interval

Overshoot when stopping = poll interval × speed. FR-WP04 asks for ≤ 1 % of stroke.

| Poll rate | Production | Mock |
|---|---|---|
| 1 Hz | 0.58 % | **6.67 % — fails FR-WP04** |
| 6.7 Hz | 0.09 % | 1.00 % — OK |

> **Rule: poll interval = `travel_m3` / 100**, clamped to ≥ 50 ms.
> Production 171 s → 1710 ms. Mock 15 s → **150 ms**.

The requirements study's "1 Hz is sufficient" is a *production* conclusion. Hardcoding it makes the rig overshoot 6.7 % and look like a positioning defect that is not one.

> **Recomputed for the as-built 13 s (2026-09-10), and it surfaced two things.**
>
> **1. `poll = travel/100` meets FR-WP04 with EXACTLY zero margin, at every traverse time.**
> Overshoot = poll × speed = (t/100) × (100/t) = **1.00 % by construction** — rig and production
> alike. The rule was written to *hit* the ≤1 % requirement, so it consumes the entire budget and
> leaves nothing for poll jitter, task scheduling, or a Modbus retry. In practice it will exceed
> the requirement. **Recommend `travel/150` (0.67 %) or `travel/200` (0.50 %)** — on the rig that is
> 87 ms or 65 ms, both still above the 50 ms floor. This is a defect in the rule, not in the rig.
>
> **2. At 13 s the `40002` derivation falls below the device's floor.** `poll × 2/3` = **86.7 ms**,
> and the contract's range starts at 100 ms, so the rig runs **clamped up to 100 ms** — a window
> ~15 % longer than the rule wants, i.e. position data slightly staler than designed. Movement per
> 100 ms window at rig speed is 0.77 % of stroke, so it is tolerable — but the clamp is now *active*
> rather than theoretical, and Phase 0 should confirm the device actually accepts 100 ms.

### 3.2 The sensor's measurement window (`40002`) — it inverts

`40002` bounds staleness (device contract §4.2). Movement per update:

| `40002` | Production | Mock |
|---|---|---|
| 100 ms | **0.6 ADC counts — useless**: position does not change between updates and the rate register quantises to nonsense | **6.8 counts — correct** |
| 500 ms | 3.0 counts — correct | 34 counts = 3.3 % staleness — coarse |

The contract's recommended `40002 = 500` is **wrong for the rig**, and `100` would have been wrong for production. Derive it too: **`40002` ≈ poll interval × 2/3**, clamped to the device's 100–60000 ms range.

### 3.3 The FR-WP20 plausibility threshold

Requirements §6 originally wrote 0.58 %/s, which is production's *nominal* speed — zero margin. Corrected to twice nominal. On the rig, nominal is 11.4× higher, so a fixed threshold trips continuously.

> **Rule: reject `|30012|` above 2 × nominal**, where nominal = full-scale rate implied by `travel_m3`.

### 3.4 Consequence for the read

A single-register read of `30001` costs ~27 ms on the wire; the contract's recommended 15-register coherent snapshot costs ~52 ms. At 6.7 Hz the fast path must be the **single register** (18 % bus utilisation while travelling, 2 % on production). Keep the full-map read as a **low-rate diagnostic**, not the control path.
### 3.5 Measure the traverse, do not trust the typed value *(operator proposal 2026-09-07 — adopted)*

Everything in §3.1–3.3 derives from `travel_m3`, which today is a number a human types. That makes the typed value a **single point of failure for three derived constants at once** — mistype it and the poll interval, the sensor's measurement window and the plausibility threshold are all silently wrong, with no symptom except degraded positioning.

**Measure it during commissioning instead.** Three reasons it is better than deriving from configuration:

1. **It is free.** The Route B teach (contract §6.2) already drives the leaf to each end switch in turn. Timing those traverses is a by-product of motion that is happening anyway — one operator action, two results.
2. **It measures what configuration cannot express.** `travel_ms` is a **single value used for both directions** (`relay_controller.cpp:439` close, `:500` open), yet requirements §1.4 item 3 states the mechanism *is* asymmetric: the motor lifts the flap against gravity to close and pays it out to open, so rope tension, backlash and slack all differ by direction. The real open and close times probably differ, and no amount of careful typing captures that.
3. **It needs no working wiper.** Time between **end sensor transitions** (bit 3), not between position readings — so the rate is available before the position calibration is itself trusted.

**Design:**

- Configured `travel_m3` becomes the **seed and the sanity bound**, not the source of truth.
- The commissioning teach measures actual traverse time **per direction**, from one bit-3 assertion to the next.
- **Accept only clean, full switch-to-switch traverses.** Reject any interrupted by a wind override, a reversal, a motor alarm, or any fault raised during the run.
- **Sanity-band against the configured value** (propose ±50 %). Outside the band, reject and report: a 3× discrepancy means something is wrong with the mechanism or the wiring, not that the configuration is merely stale.
- Store in NVS — **new keys, so a minor version bump** when this lands.
- **Log which value is in use at boot**, measured or configured. "Am I running on a measurement or a guess?" must never be a guess itself.

> **The delivery mechanism §3.5 needs now exists (gh#51, shipped 2026-09-10).** When this was written, a measured traverse written to NVS would not have reached T2 until the next reboot — so the operator action that *measures* the value could not *apply* it. T4 now posts `T2_NOTIFY_CFG_CHANGED` on any `motor` write and T2 re-reads at the top of its loop (hardware-verified: M3's pulse tracked 13 s → 171 s live). A travel change also emits a `LOG_PARAM_TRAVEL` audit row, so "which value was in force?" is answerable from the SD log — which it was not before. **§3.5 needs no reboot step, and the commissioning teach can end by applying what it measured.**

**Using two numbers where the firmware wants one.** T2 still has a single `travel_ms` per channel, and changing that is a control-model change — Phase 5 territory. For this cycle: measure both directions, **derive the constants from the shorter** (conservative: a shorter traverse means a faster rate, so a tighter poll interval), and **log both**. If the asymmetry turns out to be material, that is a finding worth having before anyone designs per-direction travel configuration.

**It is a manual GUI action, and that is the primary safeguard** (operator, 2026-09-07). The measurement is initiated by the operator from the web GUI — it never runs in the background, so a rate can never change without someone having asked for it. An earlier draft of this section called a bad measurement "silent"; that was wrong.

**What operator presence does not by itself catch:** a traverse that completes *normally but wrongly*. A slightly binding window taking 18 s instead of 15 s looks like a clean run, sits inside the ±50 % band, and would be accepted. The operator witnessed it without being in a position to judge it.

> **So the GUI must show the result and require explicit acceptance**, not merely start the run. Display the measured open and close times, the implied rate, and the configured `travel_m3` beside them; the operator confirms or rejects. That converts presence into an actual check — the person who just watched the window move is the only party who knows whether it moved *freely*.

Ranked defences, strongest first: **operator acceptance of a displayed result**; clean-traverse-only (reject anything interrupted by a wind override, reversal or fault); the ±50 % band as a machine guard rail behind human judgement; and both values logged, so a later reader can spot a discrepancy without re-running anything.

**Later, optionally:** the device already refreshes `30013`/`30014` at *every* stop arrival for drift detection (contract §6.5). The same principle would let the rate be refined passively on every full traverse. Start with the explicit commissioning measurement; passive refinement is a small addition once the measured path is trusted.


### 3.6 Minimum-move deadband — derived, like everything else here

*(Moved into this section 2026-09-13. It had been two sentences inside 5a's dwell
discussion, which is the wrong place twice over: it is an **actuator** property,
not a control-algorithm one, so the M3 slice needs it the moment it implements
"drive to a setpoint"; and it was given as a flat "2 %", which was invented.)*

Under stepped control the anti-thrash problem was **aperture oscillation**, and
`dwell_*_s` answers it. Under linear control it becomes **motor duty and
mechanical wear**, and the mechanism for that is magnitude-based, not
time-based: **do not energise for a correction smaller than the deadband.** The
two are complements, not alternatives — *dwell* answers "how often may this
window move", *deadband* answers "is this correction worth moving for".

**It has two independent floors. The deadband is the larger of them.**

**Floor 1 — sampling resolution.** The leaf travels `poll_ms — speed` between
samples, so a deadband below that commands a correction smaller than the error
the sampling itself introduces. It will chatter.

| | travel | speed | effective poll | one sample | % of stroke |
|---|---|---|---|---|---|
| dev rig | 13 s | 115.4 mm/s | **170 ms** (measured, AT-WP05 — `vTaskDelay` is relative) | 19.6 mm | **1.31 %** |
| production | 171 s | 8.8 mm/s | 1140 ms | 10.0 mm | **0.67 %** |

**The floor is ~2x tighter on production than on the rig**, which is precisely
why a fixed percentage cannot serve both and why this belongs in section 3: it
derives from `travel_m3` like the poll interval, the measurement window and the
rate threshold. The rig figure is corroborated by `nominal_x10 = 1153` read
from the device.

**Floor 2 — the shortest pulse that actually moves the leaf.** Energising a
contactor for 50 ms to correct 0.5 % is wear for no motion: static friction and
contactor make/break time mean a short pulse may move nothing at all. **This
term is not yet measured** — it is a rig experiment (command progressively
shorter pulses, find where displacement stops tracking pulse width), and it
should be done before a deadband value is fixed.

**The endpoints are exempt.** A setpoint of `0` or `100` terminates on **bit 3**
and runs to the end sensor however small the remaining distance is (see 6.1 and the
corrected constraint 1 in 5a). Applying the deadband there would park the leaf
just short of its stop with bit 3 never asserting — which is also the FR-E16
signature for a broken end-sensor cable, so it would read as a hardware fault.

**Consequence for configuration:** the linear dwell defaults to 0 (5a), but the
**deadband must not** — a zero deadband is the chattering case. Derive it, expose
it, and let the operator raise it; do not let it start at zero.

---

## 4. Architecture: a dedicated position task

Requirements §3 left "who polls at 1 Hz" explicitly unsolved. The rig's 150 ms interval settles it.

| Option | Verdict |
|---|---|
| (a) T5 chunked sleep | **Rejected.** At 150 ms you are slicing the climate task's sleep into fragments, in the one task that must hold an exact 30 s rhythm — and T5's averaging windows are *sample-count* based, so a mistake there silently corrupts climate averages |
| **(b) Dedicated position task** | **Adopted.** T5 keeps its 30 s cadence completely untouched. **Only possible because 2.4.1 shipped the bus mutex** — before that, a second bus caller was a data-corruption hazard |
| (c) Full JIT bus task (`refactorSensorConfiguration` §2.2) | Deferred. The right end state, far larger than this work |

**Task placement:** priority **below T2 and T3**. The mutex fixes correctness, not permission — a transaction can hold the bus ~215 ms, and that must never sit in front of a wind override. This is the standing policy recorded in `modbus_rtu.h` and `architecture.md`.

**Runs only while a channel is travelling.** Idle otherwise; no bus cost when nothing moves.

---

## 5. Phases

### Phase 0 — bring-up and commissioning *(bench tooling; **COMPLETE, FDA4 2026-09-10**)*

> **Status 2026-09-07: cannot start.** The wire sensor is not on FDA4's bus (§2). Everything below needs a device to answer.

> **This phase was mis-scoped as "no firmware".** It is not. The product API only ever addresses the FG6485A (1) and the S200 (44) — there is **no passthrough**, so nothing in the shipped firmware can reach a device at 40/45. A bench access path had to be built before Phase 0 could be attempted at all: `firmware/src/diag/modbus_bench.{h,cpp}` and an admin-only `POST /api/diag/modbus`, both behind `-DMODBUS_BENCH` (env `lolin_s3_bench`, which reports `2.4.1-bench` so a dev build carrying an open Modbus write route can never be mistaken for the release). Writes are confined to addresses 40/45. **That tooling is written, builds, and is unflashed** — it is waiting on the same hardware this phase is.

> **Sequencing note.** §3.5's traverse measurement is a **GUI action**, which is firmware work — a web route plus UI. It cannot land here: you cannot build a GUI for a device the firmware cannot yet talk to. Phase 0 stays a manual bench activity to prove the device works at all; **GUI-driven commissioning (teach, traverse measurement, accept/reject) lands with the GUI work in §6.3**, once the driver and position task exist. Until then no measured rate exists and the configured `travel_m3` is used — exactly the fallback §3.5 specifies.


Confirm the device before writing code against it. Per contract §9: read `30007` (**refuse build type `0x81`** — a bench build with a deliberate hang hook), read the holdings, then teach.

- Teach with **movement** (contract §6.2) — never arm with the leaf resting at a stop it has not moved to since power-on.
- **Our driver has no FC06.** Arm and abort the teach with **FC16, quantity 1**, to `0x0006`; `FR-MB28` rejects only quantity 0.
- Write `40004` = the tape-measured **sensor-to-sensor** distance, not the hard stops.
- **Measure the traverse time per direction** while the teach drives each stop (§3.5). Record both, and record which one the derived constants ended up using.
- Run both installation checks and **record the answers** — they change what the firmware can rely on:
  - **Sensor-zone** (contract §5.3): drive fully closed, confirm bit 3 stays set. If it clears, FR-WP07 is not met on this rig and "bit 3 clear" must be read as *not proven at a stop*, never *proven away from one*.
  - **Electrical headroom** (contract §7.3): with the window fully closed and fully open, `30005` must not sit near 0 or 1023. If it does, **bit 6 is inert** and a shorted wiper will read as a perfectly closed window.

#### Phase 0 results — FDA4, 2026-09-10 (steps 1-3, no movement yet)

Run over `POST /api/diag/modbus` on the `2.4.5-bench` build (`fw_ver` and `asset_version` both
`2.4.5-bench`, so the dev build is unmistakable).

**Device: address 40.** 45 times out. `30007` = `0x0101` — build type **0x01 (release)**, firmware v1.
Not the `0x81` bench build, so §9's refusal does not apply. Uptime 34768 s (9.7 h), `30009` bus CRC
errors **0**.

**The sensor is COMPLETELY UNCOMMISSIONED.** Every holding is at its factory default:

| reg | value | default? | note |
|---|---|---|---|
| 40001 zero offset | 0 | yes | |
| 40002 measurement window | **1000 ms** | yes | plan §3.2 wants ~100 ms for a 13 s traverse — see the conflict below |
| 40003 averaging window | 10 s | yes | |
| 40004 full travel | **10000** (= 1.0 m) | yes | **needs the tape-measured sensor-to-sensor distance** |
| 40005 raw closed | 0 | yes | |
| 40006 raw open | 1023 | yes | |
| 40007 teach | 0 | yes | reads 0 after every reset |

**So every position reading is currently meaningless** — 30001/30015 are scaled against default
endpoints, not taught ones.

**Stale teach captures are present but uncommitted:** `30013` = 6 (closed end), `30014` = 830 (open
end), while 40005/40006 remain 0/1023. A teach was run at some point and abandoned before the
read-to-commit handshake in §6.2(d). **Reading them did NOT commit them** — re-read confirmed the
holdings unchanged, so the commit needs an *armed* session. A fresh teach is required; the captures
are usable as expected values, not as configuration.

**⚠ Electrical headroom (§7.3) looks like it FAILS at the closed end.** With the window at the closed
stop (`30006` = 0x08, end sensor active) the live raw ADC `30005` reads **0**, and the stale capture
agrees at 6. §7.3 requires the raw code not to sit near 0 or 1023 at either stop, because otherwise
**bit 6 (implausible) is inert and a shorted wiper reads as a perfectly plausible closed window**.
The open end is fine (830, i.e. 193 counts of margin below 1023). This needs the proper drive-both-
ends confirmation, but the early reading is unambiguous and it is the check that decides whether
§12.4 rule 1 is the only defence we have.

**⚠ The contract's own worked table contradicts this plan's §3.2 at rig speed.** §4.2 tabulates, for a
reference 8.77 mm/s rig: at `40002` = 100 ms the position changes on roughly *every other* update and
**`30012` quantises to steps of ±180 — twice the true speed**, i.e. the rate register becomes
useless. §3.2 here derives ~100 ms for a 13 s traverse, and §3.3 then depends on `|30012|` for the
FR-WP20 plausibility threshold. **The two rules do not compose at rig speed.** Resolve before Phase 2:
either drop the FR-WP20 rate check on fast rigs, derive the threshold from successive 30001 deltas
instead of 30012, or accept a longer 40002 and a coarser freshness bound.

**Checked, not a fault:** `30011` = 1 is *seconds since the last valid reading* (§7), not a fault
counter — healthy at 40002 = 1000 ms. `30010` = 2 is the served-request count.

**Not yet done — needs movement and a tape measure:** the teach (§6.2), `40004`, the per-direction
traverse timing (§3.5), and the sensor-zone check (§5.3).
### Phase 1 — driver *(**COMPLETE**, all exit criteria met — FDA4 2026-09-10)*

Thin driver over the existing Modbus layer. FC04 for input registers, FC03 for holdings, FC16 for writes.

- **`30012` is signed** — decode as `int16`, or a closing window reads ~65 000.
- Position, status bits, and rate are the control surface; the rest is diagnostic.
- Fault mapping: bit 2 (wiper) and the `65535` sentinel → sensor fault; bits 6/7 → health, operator-visible only.

*Exit:* a bench read returns plausible position, rate sign follows direction, and disconnecting the wiper produces the fault within ~2 s.

#### Phase 1 results — FDA4, 2026-09-10: COMPLETE, all exit criteria met

`drivers/windowPos/` + `firmware/components/windowPos/` proxy, wired into
`firmware/src/CMakeLists.txt`. Exercised on hardware through a dev-only
`GET /api/diag/windowpos` (bench builds only — **0 symbols in the release ELF, 1 in bench**).

**The route is deliberately a SECOND path to the same device.** `POST /api/diag/modbus`
returns raw registers; `GET /api/diag/windowpos` returns what the driver made of them. A wrong
offset or a bad sign decode surfaces as a mismatch instead of a plausible number. All five
cross-checked fields matched.

| exit criterion | result |
|---|---|
| plausible position | **PASS** — tracked 1381.1 -> 132.8 mm smoothly across a full close, percent 92.0 -> 8.8 |
| rate sign follows direction | **PASS** — see below |
| wiper disconnect -> fault | **PASS** — status 0x0C, opening and percent both 65535, `sensor_fault` set |

**The signed decode, proven rather than asserted.** On a forced CLOSE_ALL:

```
driver rate = -1570 (0.1 mm/s)      raw 30012 = 63966
```

63966 as `int16` is -1570. **Read unsigned it says the window is closing at 6.4 m/s** — a
plausible-looking number, which is exactly why this is the trap most likely to ship unnoticed.

**Wiper fault, measured from both sides:** the fault set (status 0x0C, bit 2 + both value
registers at the 65535 sentinel) and cleared within **0.4 s** of reconnection, with `30011`
back to 0. **The ~2 s set latency was NOT measured** — `30011` already read 25 s at first
sample, so the disconnect preceded the observation. The fault is confirmed; the latency figure
remains the device's claim.

**Noticed in passing:** T6 drove the window fully open *while the wiper was disconnected*, and
the controller neither noticed nor cared — the control loop is still timer-only and does not
consume the sensor. That is correct today and is precisely what Phase 2 changes.

**Forcing a stroke without the LCD:** `POST /api/mode` standby then automatic posts
`CMD_RECALIBRATE`, which drives a full CLOSE_ALL. That is the remote lever for any test
needing a closing traverse.
### Phase 2 — position task + derived configuration *(**COMPLETE** — FDA4 2026-09-10)*

The task of §4, with §3's rules implemented as **derived** values, not constants. Log the derived numbers at boot so a wrong `travel_m3` is visible immediately.

*Exit:* during a mock stroke, position advances monotonically at ~6.7 Hz with no `MODBUS_ERR_BUSY` and no added read failures on FG6485A or S200 over ≥ 24 h (this is AT-WP05).

#### Phase 2 results — FDA4, 2026-09-10

**T17 implemented and running**: `firmware/src/window_pos/window_pos_task.{h,cpp}`, priority **4**
(below T2/T3 at 6 and T4/T5/T6 at 5 — in phases 2-3 it is a read-only observer and must never
delay control work), 4 KB stack, idle unless a channel is travelling.

**Derived config, logged at every stroke start and confirmed live:**

```
travel_m3=13 s -> poll=100 ms  window=100 ms  nominal=1153 (0.1mm/s)  reject>3459
```

| exit criterion | result |
|---|---|
| position advances monotonically | **PASS** — 0.0 -> 1500.0 mm, 7/7 non-decreasing |
| polls at the derived rate | **PASS** — snapshot age held ~97 ms against a 100 ms derive |
| idle when nothing moves | **PASS** — age climbed 1490 -> 20334 ms after the stroke ended |
| no `MODBUS_ERR_BUSY`, no added FG6485A/S200 failures over >=24 h (AT-WP05) | **RUN 2026-09-11, PASS DOWNGRADED 2026-09-13** — the criterion was not measurable then. See below |

### Two derivation rules were wrong as specified, and the rig proved both

**1. `poll = travel/100` (3.1) spends the entire FR-WP04 budget.** Overshoot = poll x speed =
(t/100) x (100/t) = **exactly 1.00 %, by construction, at every travel time**. The rule was
written to *hit* the requirement, so nothing is left for jitter, scheduling or a Modbus retry.
**Implemented as /150.** This affects production identically — it was simply invisible while the
number was hypothetical.

**2. `reject above 2x nominal` (3.3) would have false-tripped.** Measured peak rate **2100**
against a 2x threshold of **2306**: 9 % margin. The cause is structural: nominal derives from
`travel_m3` (13 s) but that covers switch-to-**limit**, while the position span is
switch-to-**switch** (10 s) — the leaf spends ~3.5 s in the blind overlap with position clamped.
So the real speed across the measured span is ~30 % higher than nominal, and **2x nominal is not
2x real**. **Implemented as 3x.** A false trip here silently discards good position samples, which
is the worst failure mode available to a plausibility check.

**The device's 100 ms floor on `40002` is the binding constraint on a fast rig**, not our poll
interval: at ~150 mm/s that floor alone is 15 mm = 1 % of a 1500 mm stroke. On the rig FR-WP04 is
met exactly and **cannot be beaten by polling harder** — the sensor is the limit. On production
(171 s, ~8.8 mm/s) the same floor is 0.88 mm, 0.06 % of stroke, irrelevant.

**Observation surface:** `GET /api/diag/windowpos` (bench builds only) now returns T17's own
snapshot and derived config alongside a fresh direct read, so the task is observed rather than
inferred.
#### AT-WP05 soak — run 2026-09-11, **pass DOWNGRADED 2026-09-13**, re-specified

> **Status: NOT CLOSED.** It ran, it was evaluated, the result was never written
> into this document, and on review the pass tested a **weaker proposition than
> the one specified**. Recorded here in full so it stops living in session
> memory — the sibling finding "`strokes` counts ANY channel" decayed exactly
> that way and was re-derived from scratch as a suspected defect on 2026-09-13.

**What was measured (FDA4, 23.3 h, ending 2026-09-11):** 7117 reads, **0
`err_busy` / 0 `err_comm` / 0 `rejected_rate`**, 31 strokes, no heap leak, T5's
poll cadence unperturbed. Reported at the time as a *qualified* pass.

**Why the pass is downgraded.** The requirement is *"**zero added read
failures** on either existing sensor"* (FR-WP13/14). The only evidence available
in 2026-09-11 firmware was the count of `ALARM ch4`/`ch5` rows — and **those
rows appear only on a two-in-one-poll FAULT.** A single failed read left no row,
no counter and no API field anywhere. So the soak could establish *"no added
**faults**"*, which is a much weaker claim than *"no added **failures**"*, and
the difference is the entire point of the test.

We now know the distinction is not academic: with counters, the same bus shows
**8 timeouts + 1 CRC in 13 h**, all of them on T5's slaves. None of those would
have produced a single `ALARM` row.

**Three qualifications, two of which were never recorded here:**

1. **`err_busy` cannot fail with two callers** — the lock timeout is 500 ms
   against a ~215 ms worst-case hold, so the headline counter had no way to trip.
   An instance of the "show the check can fail" pattern; 7117 clean reads proved
   nothing about contention.
2. **The encoder moved only ~105 s in 23.3 h.** Bus load from T17 was therefore
   near its minimum for almost the whole run, so the run barely exercised the
   condition it was testing.
3. **Effective poll was 168 ms, not the derived 100 ms** — `vTaskDelay` is
   relative, so the period is delay *plus* transaction. This is the figure 3.6
   now uses for the deadband floor.

> ### Arms A and B — **RETIRED 2026-09-13, UN-RETIRED 2026-09-14**
>
> **The retirement was wrong, and arm A is what disproved it.**
>
> The argument on 2026-09-13 was: the A/B design existed to answer *did adding
> T17 make the bus worse*, that question was settled on 2026-09-12 by root cause
> (`MODBUS_IFG_US` at the RTU spec floor), and a week of Poisson counting adds
> nothing to a diagnosis that already has a mechanism, a fix and a vanished
> symptom. The arm A run then in progress was relabelled a *presence-gate
> robustness soak* on the grounds that it could never be half of a comparison.
>
> It ran 18 h 33 m and was exactly that half.
>
> | | T5 transactions | failures |
> |---|---|---|
> | **arm B** — encoder connected, T17 polling (the 13 h soak, 2026-09-12/13) | ~6 434 | **9** |
> | **arm A** — encoder unplugged, gate shut (2026-09-13/14) | **6 579** | **0** |
>
> Near-identical denominators. At arm B's rate you would expect ~9 failures in
> 6 579; observing **zero** has **p ≈ 1e-4**.
>
> **Build equivalence was checked, not assumed.** The 13 h soak ended
> 2026-09-13 11:11, before `c5d59dc` (12:44); the arm A build adds only the
> per-slave counters and the §6.3 GUI work — **nothing touching Modbus timing**.
> Same rig, same bus firmware, encoder present or absent as the material
> difference.
>
> **`gated_polls` = 778, so M3 did move during arm A.** This is not a
> quiet-house artefact, and it weakens the competing explanation that arm B's
> failures came from motor switching rather than from T17's traffic — an
> explanation already dented by the 5C88 finding that 0 of 5 faults fell within
> ±120 s of any `RELAY` transition.
>
> **What the retirement got right, and keeps.** Neither arm can answer the two
> questions that are *not* about T17: whether a residual exists independently of
> it (5C88 answers that, and did), and what counts as "too much" for this bus
> (a per-installation baseline plus elapsed time, not an experiment). Those
> stand. What does not stand is the claim that T17's own contribution was
> already settled.
>
> **What the retirement got wrong, worth keeping as a lesson.** A root cause
> that explains a *large* effect does not establish there is no *residual*
> effect. The IFG defect was ~10x and is genuinely fixed; that says nothing
> about a second, smaller mechanism, and "the symptom stopped" was measured
> against the symptom (wind alarms), not against the rate. Retiring a
> measurement because its headline question looks answered discards the very
> thing that would have shown the answer was partial.
>
> ### Arm B — the matched rerun (2026-09-14, encoder reconnected)
>
> Arm B above is borrowed from a soak whose counters were **bus-wide**, with the
> per-slave attribution inferred from `last_fail_addr`. Rerun it on the arm A
> build so both halves share firmware *and* instrumentation:
>
> | | |
> |---|---|
> | build | the arm A build, unchanged — **do not flash 2.8.0 until this completes**, or the arms diverge again |
> | duration | >= 18 h, to match arm A's denominator |
> | measure | per-slave `ok` / `to` on addr 1 and addr 44, from `GET /api/diag/windowpos` |
> | **precondition** | **T17 must actually be polling.** The gate demotes immediately but promotes only at a **stroke boundary**, so after reconnection it stays TIMED until M3 next completes a move. Confirm `gate.mode_str` = `position` before starting the clock — an arm B where T17 never polls is arm A with the plug in |
> | PASS | arm B's addr 1 + addr 44 failure rate is **not materially above** arm A's zero |
>
> A third arm is available for free and worth recording: 5C88 runs continuously
> with **no T17 at all**, so its ~one-fault-per-12-days is the long-baseline
> version of arm A on different hardware.


**The original re-specification, kept because the reasoning above is what
retired it.** The criterion was to become a **comparison between two arms**,
which is what "added" always meant:

| arm | configuration | what it gives |
|---|---|---|
| **A — baseline** | encoder **unplugged**, so the presence gate shuts and T17 stops touching the bus. Same binary, same config | T5's failure rate with **one** bus caller |
| **B — with T17** | encoder connected, gate open, T17 polling normally | T5's failure rate with **two** bus callers |

Measure `modbus_get_counters()` at the start and end of each arm, per slave
address. **PASS = arm B's T5 failure rate is not materially above arm A's.**

> **How long, and be honest about what it can resolve.** T5 runs ~8640
> transactions/day (30 s poll × 3 transactions), so at the measured 0.087 %
> that is **~7.5 failures/day**. Poisson, per arm:
>
> | effect T17 would have | 1 day | 3 days | 7 days |
> |---|---|---|---|
> | doubles it (3.8 — 7.5/day) | 1.1 sigma | 1.9 sigma | 3.0 sigma |
> | 5× (1.5 — 7.5) | 2.0 sigma | 3.5 sigma | 5.3 sigma |
> | 10× (0.75 — 7.5) | 2.4 sigma | 4.1 sigma | 6.2 sigma |
>
> **So the specified 24 h resolves a LARGE contribution (5—10×) and cannot
> resolve a doubling.** That is probably acceptable — the failure this test
> guards against is the inter-frame-gap class, which was ~10× — but the
> criterion should say so rather than imply a precision it does not have.
> Detecting a doubling at 3 sigma needs **~7 days per arm**.

~~**Blocker, must be fixed before arm A can run.** `GET /api/diag/windowpos`
returns early when the direct read fails, and that early return carries `gate`
and `soak` but **not `modbus`**. With the encoder unplugged — arm A by
definition — the bus counters are invisible.~~ **FIXED 2026-09-13** (`c5d59dc`),
and taken further than the one-line fix: the failure path now emits `modbus`,
`reason_str`, **and per-slave rows keyed by address**. Bus-wide totals would
not have been enough — with addr 40 deliberately absent, the gate's 30 s
re-probes dominate the totals and are indistinguishable there from the T5
failures the test is actually measuring.

**The soak could not measure its own criterion until this build.** `MODBUS_ERR_BUSY` was folded
into `WINDOWPOS_ERR_COMM`, so "did the second bus caller cost T5 anything" — the whole question —
was unanswerable. `WINDOWPOS_ERR_BUSY` is now a distinct code and T17 counts outcomes separately.

**Baseline (FDA4, `2.4.5-bench`, uptime 53 s):**

| | value |
|---|---|
| `reads_ok` / `err_busy` / `err_comm` / `rejected_rate` / `strokes` | 0 / 0 / 0 / 0 / 0 |
| `eg1` | 0 |
| SD log | `FDA4_20260909112816.csv`, 845365 B |
| ALARM rows total | 11 (T/RH ch4: **1**, wind ch5: **5**) |

Baseline JSON: `scratchpad/soak_baseline.json`.

**Pass criteria after >= 24 h:**

- `err_busy` **stays 0** — no lock contention with T5. This is the one that matters; it is the
  reason 4 deliberately sits below T5's priority and the reason T17 idles at rest.
- `err_comm` low and flat.
- `rejected_rate` **stays 0** — confirms the 3x threshold does not false-trip. At 2x it would
  have (peak 2100 vs 2306), which is what prompted the change.
- ch-4 / ch-5 ALARM counts do not grow beyond their pre-T17 rate — i.e. T17 has not degraded
  the FG6485A or S200 reads it shares a bus with.

Read the counters any time with `GET /api/diag/windowpos` -> `soak`.

#### 5C88 production baseline — read 2026-09-13 (gh#66 step 1, **DONE**)

`model/campaign-summer-2026/*.log` is the 5C88 SD archive (`model/README.md:169`).
**98.5 days of logged coverage over a 100.2-day window (98 %), 1.35 M rows**, on a
unit running 2.3.1 with **no T17, no encoder on the bus and one bus caller** — the
strongest control this project can get, and it was already on disk.

| | |
|---|---|
| genuine sensor-fault onsets | **8** — 6 on T/RH (addr 1), **2** on wind (addr 44) |
| excluded | **1** — the 2026-06-19 100-minute wind fault is a **pre-commissioning artefact**, not a field failure. The vane went into service at 12:00 that day and the fault ran 09:39—11:19. See the gotcha log, 2026-08-26 and 2026-07-13 |
| rate | **one per 12.3 days** (T/RH one per 16.4 d; wind one per 41.7 d, measured from commissioning) |
| **greenhouse closed by one** | **2** (`ALARM ch0 param 243`), 2026-08-18 and 2026-09-10 — which is **both** genuine wind faults, **2 of 2** |
| wind measured at those two closures | **1.6 m/s** and **1.4 m/s** |
| wind at *every* one of the 8 | **0.4 — 2.6 m/s** |
| trend by month (Jun/Jul/Aug/Sep) | **1 / 2 / 2 / 3** |

**What this settles.**

1. **The residual is real, and it is not ours.** It predates T17, runs on hardware
   that has never seen T17, and sits at a stable rate across four months. gh#66's
   leading hypothesis — *a pre-existing baseline that was never observable* — is
   confirmed on production data.
2. **It has a real consequence, twice in 100 days.** Not a theoretical concern:
   the greenhouse closed on a calm day, twice, and the only person who could
   notice was on site.
3. **It is not weather.** All nine events occurred between 0.4 and 2.6 m/s.
4. **It is not degrading**, so there is no urgency — but nor is it going away.

**Three cautions about this data**, because the log is a blunter instrument than
it looks:

- **A single failed read still leaves no trace.** Only a *double* failure inside
  one poll emits a row, so these 9 are a lower bound on something unmeasured.
  This is precisely what the §5.0 indicators exist to fix.
- **SENSOR_HR rows continue unbroken through a fault and prove nothing.** By
  design (`sensor_poll.cpp` Step 5) a faulted sensor's raw fields carry the *last
  known average* to avoid a gap in T4's ring, and that value keeps changing as the
  window slides — so it reads exactly like a live sensor. I nearly concluded the
  S200 was answering during its own fault.
- **ALARM rows are written to SD 30—55 s after their timestamp**, interleaved out
  of order among rows that were written promptly (Q3 latency; SENSOR_HR takes a
  faster path). **File order is not event order for alarms.** Any analysis that
  sorts by position rather than timestamp will mis-sequence them.

**Six of the eight lasted 58—59 s.** Since T5 clears a fault on the first success
at a *later* poll and polls every 30 s, a 59 s fault did not clear at the next poll
either — a **~1-minute outage of a single slave**, while the other slave on the same
bus kept answering.

> **This was already worked out, more precisely, before today.** The gotcha log's
> 2026-08-26 entry counts the attempts exactly — *failure #1, failure #2 (trigger
> logged), failure #3, success at read #4* — so **each event is exactly three
> failed reads**, and argues that several identically-sized events point at a
> deterministic outage **inside the sensor** (an internal reset or watchdog) rather
> than at the RS485 pair, with the FG6485A's diagnostic registers as the way to
> confirm. It also records that the **motor-noise hypothesis was tested 2026-09-05
> and ruled out**: 0 of 5 faults within ±120 s of any `RELAY` transition.
>
> **Process note.** CLAUDE.md says to check the gotcha log before debugging from
> scratch. This 98-day analysis was run without doing so and re-derived that entry
> less accurately, including counting the pre-commissioning artefact as a fault.
> *A fresh analysis of a subsystem is exactly when that file is most likely to hold
> the answer already, and least likely to be opened.*

**What this pass genuinely adds** to the existing entry: the **wind speed at each
event** (the column that shows none of these is weather, which is what gh#66 needs),
the two new September events, the T17-independence framing, and the ALARM
write-latency trap above.

#### Arm A — ran 2026-09-13/14, **18 h 33 m, PASS with zero failures**

**Result first.** Over 18 h 33 m of continuous uptime (no reboot):

| slave | ok | timeouts |
|---|---|---|
| **addr 1** — FG6485A T/RH | **2 193** | **0** |
| **addr 44** — S200 wind | **4 386** | **0** |
| addr 40 — the absent encoder | 11 | 2 205, every one a 30 s gate re-probe |

`err_busy` **0**, `crc` **0**, `gated_polls` **778** (so M3 moved during the run —
this is not a quiet house). **6 579 live-slave transactions, zero failures.**

Two things this establishes, and they are different in kind:

1. **The presence gate is robust over a long run with the device absent.** It
   demoted once, re-probed ~2 200 times at its specified 30 s, never faulted a
   healthy slave, never leaked, and never mistook contention for absence
   (`err_busy` = 0). That was the whole of what this run was expected to show.
2. **It is also the arm A half of the comparison that had just been retired** —
   see the un-retirement above. Set against the 13 h soak's 9 failures in ~6 434
   transactions with the encoder connected, zero in 6 579 has **p ≈ 1e-4**.

**A link fault, diagnosed and excluded.** During the morning readout roughly one
HTTP request in three stalled ~15 s. The paired ping test (the gotcha log's rule:
run it *before* suspecting code) returned **90 % loss to the unit and 0 % to the
gateway over the same path**, at **−47 dBm** — the 2026-09-12 interference
signature, where a strong RSSI with heavy loss means interference rather than
range. **It does not touch these numbers**: the counters are internal to the
firmware and the audit trail is on the SD card, so nothing here is transported
over the bad link.

**Run record**

FDA4, `2.7.0-bench` (fw **and** asset version both verified post-reboot),
encoder unplugged by the operator at uptime ~280 s.

| | at start (uptime 68 s) | at t0, unplugged (uptime 287 s) |
|---|---|---|
| gate | `timed` / **`ok`** | `timed` / **`no_sensor`** |
| bus total ok / timeout | 12 / 0 | 38 / **8** |
| **addr 1** (FG6485A) ok / to | 1 / 0 | 9 / **0** |
| **addr 40** (encoder) ok / to | 9 / 0 | 11 / **8** |
| **addr 44** (S200) ok / to | 2 / 0 | 18 / **0** |
| `err_busy` | 0 | **0** |
| heap free | — | 73 kB |
| SD | mounted, 1849 / 1880 MB free | |

**Every one of the 8 bus timeouts is on addr 40**, and addresses 1 and 44 are
untouched. That is the per-slave table earning its place on the first reading:
8 timeouts in 5 minutes is an alarming number until you can see they are all
the same absent device. The spacing (219 s / 8 ≈ 27 s) matches
`PROBE_RETRY_MS` = 30 s, so the shut gate is re-probing at exactly its
specified rate and no faster.

**Two honesty notes for whoever reads the result.**

1. **Arm A is not literally "one bus caller".** A shut gate still probes every
   30 s, so T17 keeps making one transaction per 30 s — as this section's own
   gate table intends ("exactly what the Phase 3 idle path already spent").
   The arm A / arm B difference is therefore *T17 polling during strokes*, not
   *T17 present versus absent*. That is the sharper comparison, but the table
   above overstates it and should be read this way.
2. **An overnight run cannot resolve a doubling.** Per the Poisson table above,
   ~1 arm-day resolves a 5—10× contribution at 2.0—2.4 sigma and a doubling at
   only ~1.1 sigma. Report it as what it is: a check for a *large* effect of
   the inter-frame-gap class, not evidence that T17 costs T5 nothing.

**Instrument gap found at t0, not worth a reflash mid-soak:** the diag's
failure path emits **six** `soak` keys where the success path emits eight —
`rejected_rate` and `strokes` are missing. Arm A runs entirely on the failure
path, so neither is available from diag for this run. `rejected_rate` cannot
move when no read succeeds, and strokes are recoverable from the SD `RELAY`
rows, so the run is unaffected — but the two paths should emit the same block.
#### Arm B — ran 2026-09-14/15, **21 h 15 m**, and the pair now answers gh#66's step 2

**Result first.** Arm B is the same rig with the encoder **fitted** and T17
polling, run to a longer window than arm A so the comparison cannot be blamed on
exposure.

| | duration | T5 transactions (addr 1 + 44) | failures |
|---|---|---|---|
| **arm A** — encoder unplugged, gate shut | 18 h 33 m | 6 579 | **0** |
| **arm B** — encoder fitted, T17 polling | 21 h 15 m | 7 583 | **10** (1 in 758) |

Under one shared rate the 10 failures should split ~4.6 / 5.4 by exposure.
Observed 0 / 10 — **one-sided p = 0.0019**.

gh#66 set the decision rule before either arm ran: *"If T5's rate stays ~0.1 %,
the residual is the sensors' own baseline and the bus architecture is done. If
it drops to zero, T17's mere presence still matters and `MODBUS_IFG_US` needs
raising past 20 ms."* **It drops to zero.**

**It replicates across a firmware change.** This is a second arm B, not a
re-reading of the first:

| arm B run | firmware | transactions | failures | rate |
|---|---|---|---|---|
| 2026-09-12/13, ~13 h | 2.7.0-bench | ~6 434 | 9 | 1 in 714 |
| 2026-09-14/15, 21 h | 2.8.0-bench | 7 583 | 10 | 1 in 758 |

**A negative result, recorded because it was expected to go the other way.**
`317ea00` ("poll on M3's travel, not any window's") reduces how often T17 polls,
and therefore how often encoder frames sit adjacent to T5's — the mechanism the
inter-frame-gap defect implicates. At the 5.6 h mark arm B was showing **1
failure in 5 952** and looked like confirmation. The full run lands at 1 in 758,
indistinguishable from the pre-fix rate. **Reducing T17's polling frequency did
not reduce the residual**, and calling the run at 5.6 h would have produced a
wrong and encouraging answer.

**The encoder is the cause, not the victim.**

| slave | transactions | failures | |
|---|---|---|---|
| addr 1 — FG6485A T/RH | 2 533 | **9** | 1 in 281 |
| addr 44 — S200 wind | 5 050 | **1** | 1 in 5 050 |
| addr 40 — the encoder | 8 704 | **0** | none |

addr 40 carries more traffic than the other two combined and fails least, which
matches the earlier soak (3 852 transactions, 0 failures). **The 18x asymmetry
between addr 1 and addr 44 is new and unexplained** — and it is a reversal from
2026-09-12, when the S200 was the slave the inter-frame-gap defect silenced.
Recorded as an observation, not a finding.

**Two caveats that limit what this establishes.**

1. **Both affected slaves are EMULATED on this rig** (CLAUDE.md, corrected
   2026-09-14). Only addr 40 is real hardware. So this measures how an emulator
   tolerates a third caller's bus timing. The *direction* is solid; the *rate*
   must not be carried to 5C88.
2. **It does not explain production.** 5C88 runs arm A's configuration — no T17,
   no encoder, one bus caller — and still produced 8 genuine fault onsets in
   98.5 days, both wind faults closing the greenhouse. Arm A produced zero in
   18.5 h. Not contradictory (18.5 h cannot see a once-per-12-days event), but
   it means **the dev-rig residual and the production faults are probably two
   different phenomena**, and closing one does not close the other.

**Not done here:** raising `MODBUS_IFG_US`. The rule says to, but the value that
suits an emulator may not suit a real FG6485A, a longer IFG slows every
transaction, and T17's cadence derives from `travel_m3` — the interaction wants
checking, and the change wants its own soak.

---

### Phase 3 — read-only logging *(**COMPLETE** — FDA4 2026-09-10. The first thing with lasting value)*

Per CLAUDE.md, `log/logparser.py` **and** `model/campaign-summer-2026/plot_daily.py` learn every new channel **in the same changeset**.

#### 3a. Position samples — channel 3

| Field | Content |
|---|---|
| `ch` | **3** (0/1/2/4/5 are taken; fixed and append-only once assigned) |
| `value_a` | **position, 0.1 mm** from reg `30001` |
| `value_b` | **rate, signed 0.1 mm/s** from reg `30012` |
| `param` | window number (1/2/3), so one channel serves all three if M1/M2 are ever instrumented |

**Log the raw position, not the percentage.** `30015` is derived from `40004`; if the travel figure is ever mis-measured, a logged percentage is corrupt beyond recovery, whereas percentage is always recomputable from a logged millimetre value.

**Cadence:** at the **measurement interval while travelling** (100 rows per stroke, by construction — §3.1), and at the **normal 30 s cadence while idle**.

> **The idle logging was deliberately temporary** (operator decision 2026-09-07): it existed to build trust in the implementation. It cost **~2880 rows/day, roughly +37 % of total log volume**, which shortened SD file rotation from ~1.8 days to ~1.3 and so increased the daily upload count. Revisit once the traverse record is trusted — the resting state is already carried by ch 2's window bitmask, so idle sampling can be dropped or thinned without losing the terminal states.
>
> **Thinned 2026-09-16, once the trace was trusted.** T17 still READS every 30 s at rest (`IDLE_READ_MS`). That read had become load-bearing: the presence gate judges the sensor with it (AT-WP06), and end-sensor events, restarts, orphaned teaches and the teach's STANDBY release are all seen through it. What went is the ROW. At rest a ch 3 row is now written only for:
> - the first read after a stroke (where the leaf settled);
> - the first read after boot, and a change between fault and no fault;
> - movement of at least `deadzone_m3` (5 mm minimum) without a stroke. That catches the motor box's hand switches and slip, which T2 cannot see.

#### 3b. Events — channel 6

Status and failure events, so the log explains *why* a trace looks how it does. `param` is a **`uint8`** and the reserved high band is nearly spent — wind holds 240–243, leaving twelve slots. Spend them on **categories**, with `value_a` carrying the sub-state, exactly as the existing ch 4/5 sensor faults already do (`value_a` 1 = triggered, 0 = cleared):

| `param` | Event | `value_a` |
|---|---|---|
| **244** | Position sensor fault | 1 = set, 0 = cleared |
| **245** | Teach lifecycle | 1 armed · 2 committed · 3 **refused** · 0 aborted |
| **246** | Device status change | the `30006` status bitfield |
| **247** | Device restart detected | new `30008` uptime (`30008` going backwards is the tell) |

248–255 stay free. **A teach that is *refused* matters as much as one that succeeds** — the device signals it by leaving bit 5 set with `40007` still 1 (contract §6.2), which means the captured points were closer than 64 counts: the draw-wire is not following, or the end sensors are too close together.

*Exit:* a stroke replays from the log as a clean monotonic ramp, and a full teach cycle — including a deliberately refused one — is readable end to end.

#### Phase 3 results — FDA4, 2026-09-10

Both parsers learned the encodings **in the same changeset**, per CLAUDE.md.

**Position samples — `SENSOR_HR` ch 3**, `value_a` = 0.1 mm from `30001` (**-1 on fault**, matching
the wind-fault `va=-1` convention), `value_b` = signed 0.1 mm/s from `30012`, `param` = window.
Raw millimetres, never percent: percent derives from `40004`, so a mis-measured travel corrupts a
logged percentage beyond recovery while percent stays recomputable from millimetres.

**Events — `ALARM` ch 6**, params **244** fault, **245** teach, **246** status, **247** restart.
Channel 6 continues T5's ch-based split (4 = T/RH, 5 = wind); the param band continues wind's
240-243 with 248-255 left free.

| exit criterion | result |
|---|---|
| stroke replays as a clean monotonic ramp | **PASS** — 214 rows, **214/214 non-decreasing**, 0.0 -> 1122.3 mm |
| teach cycle readable end to end | **PARTIAL** — armed -> aborted verified live (`245,1` then `245,0`, calibration untouched). **Refused is NOT emitted** |
| both parsers updated | **PASS** — verified against the real log, not synthetic rows |

**Why `245 va=3` (refused) is not emitted.** Refusal leaves bit 5 **set** with `40007` still 1
(contract 6.2) — a *non-transition*, indistinguishable from a teach still in progress. T17 observes
only; it never reads `30013`/`30014` because that read is what commits, and a logger must not commit
a calibration as a side effect. The code is reserved for the commissioning path that drives the
teach (6.3, Phase 4+).

**Real device status decoded from the live log:** `0x88 [end sensor, NOT FOLLOWING]` — the exact
signature of the loose draw-wire from 2a.7, now readable straight out of the parser.

**`plot_daily.py`** gained a fifth panel. It **splits the trace on fault rows** rather than
interpolating across them: a line drawn through an outage would read as "the window moved smoothly",
which is the one thing the log does not say. Teach and restart events are drawn as verticals so
discontinuities have an explanation next to them.

**Noted, not fixed** (operator deferred): a refused manual LCD command leaves no log row at all —
three `EG1` gates each show 1500 ms of text and return. `SENSOR_HR ch=2` bits 12/13/14 reconstruct
which gate was active to within 30 s. See the 2026-09-10 gotcha entry.
### Phase 4 — fault handling and travel-complete *(still no control change)*

> **STATUS, 2026-09-13 — Phase 4 is split, and this section is now the
> SPECIFICATION rather than the tracker.** The detection and observability half
> landed; the control-side half moved into the section 5.0 M3 slice and is
> tracked there. The criteria below are **not** restated in 5.0 — it references
> them — so this is where the detail lives.
>
> | deliverable | status |
> |---|---|
> | sensor-presence gate (4a) | **LANDED** 2026-09-12, hardware-verified |
> | movement trace, every stroke logged at the measurement interval | **done** — built in Phase 3 |
> | fault detection and logging (`ALARM ch6` 244—248) | **done**, both parsers + `logparser.md` 1.12 |
> | T2 treats bit 3 as travel-complete, timer as ceiling | **not started** — T2 makes no `windowpos_*` calls at all → 5.0 |
> | 12.4 rule 1, *moving means moving* | **DETECTOR LANDED 2026-09-15** — `LOG_PARAM_WPOS_STALL` = 249, `stall_faults` counter, parser + `logparser.md` 1.14. **Reports, does not act** (see below); the response belongs with the T2 change → 5.0 |
> | 12.4 rule 2, *an early stop is a fault* | **DETECTOR LANDED 2026-09-15** — `LOG_PARAM_WPOS_EARLY` = 250, `early_stops` counter, parser + `logparser.md` 1.15. **Reports, does not act**, same reasoning as rule 1 |
> | surface faults on the operator surfaces (6) | **LANDED** — `3c7b479` put M3 opening on the status payload and in the web GUI (§6.3), and the admin commissioning screen is built (`firmware/src/window_pos/commission.cpp`). *This row said "not started — `app.js` carries no position reference" until 2026-09-15; it had been stale since `3c7b479`.* |
> | alarm *handling* | deferred 2026-09-07, now inside the 5.0 slice |
>
> **Nothing consumes position yet**, so the ▲ GATE below is **still
> uncrossed** and greenhouse behaviour is unchanged — which is what Phase 4
> promised.

> **Alarm *handling* is deferred to Phase 5** (operator decision 2026-09-07). This phase **detects and records; it does not act.** Control behaviour is unchanged, which is automatic here because nothing consumes position yet. **Resolved in §4a (2026-09-12); this paragraph is kept for context.** On sensor fault the controller falls back to **time-based open-loop control, treating M3 as a binary actuator** (FR-WP17). **It does NOT drive the window anywhere.** Demotion changes the *control law*, not the position: the leaf stays where it is and the *next* command runs on the timer. FR-WP17 is explicit that *"loss of the sensor shall not disable ventilation"*, and AT-WP06's criterion is *"falls back to time-based control; ventilation continues"*. Closing on sensor loss would be a control action taken because a **diagnostic** failed — the same pathology that makes a wind-sensor read error close the greenhouse, which is correct there only because wind is a *safety* input and position explicitly is not (FR-WP18). An earlier draft of this sentence said "fall-back to full open/close", which reads as *drive to an end* rather than *revert to binary control*, and was misread that way on 2026-09-13.

- Detect and **log** the fault conditions (§3b), and surface them on the operator-facing surfaces (§6). Do not change any control decision on them.
- T2 may treat "bit 3 set at position ≈ 0 or ≈ `40004`" as travel-complete, **with the travel timer retained as the ceiling**.
- Implement the two controller-side rules from requirements §12.4 that the device cannot self-report:
  1. **Moving means moving.** While the relay is energised, `|30012|` must exceed ~half nominal within ~5 s. This catches a slipped or snapped wire, an obstruction, and — because a shorted wiper reads a *constant* — the wiper short the device cannot detect.

     > **DETECTOR IMPLEMENTED 2026-09-15** in `window_pos_task.cpp`, stroke-local and edge-triggered: `ALARM ch6` param **249** (`LOG_PARAM_WPOS_STALL`), `value_a` = peak `|rate|` seen, `value_b` = the threshold it had to beat, both 0.1 mm/s so the row never has to re-derive nominal. One row per stroke, plus a `stall_faults` counter on `GET /api/diag/windowpos`.
     >
     > Three decisions worth keeping. **(a)** The grace is `min(5 s, travel_ms / 2)` — `travel_m3` may legally be `CFG_MIN_TRAVEL_S` = 5 s, exactly the ungated grace, so a fixed 5 s would expire only as the stroke ended and the rule would never reach a verdict on the fastest windows. **(b)** Only **accepted** samples count as evidence of movement; a sample the FR-WP20 plausibility check rejected says nothing in either direction, by that check's own reasoning. The consequence is deliberate — a stroke whose every sample is implausible trips the rule, because there is then no trustworthy evidence the leaf moved. **(c)** It requires **at least one accepted sample** before reaching a verdict. Without that, an encoder that goes absent mid-stroke trips this rule (reads fail → peak stays 0 → grace expires) and reports *not following* when the truth is *no sensor*, which `WPOS_GATE_NO_SENSOR` already states correctly. The gate does shut first in practice (2 failed reads, ~340 ms, against a 2.5–5 s grace) but that ordering is a timing accident, not a basis for attributing a fault.
     >
     > **It reports and does not act.** Nothing consumes position yet, so demoting the gate here would change no behaviour while committing to a recovery policy with no consumer to validate it — in particular what re-promotes after a trip, given that reads keep succeeding on a snapped wire. That decision belongs with the T2 change that first makes position drive the actuator.
  2. **A stop that arrives too early is a fault, not a success.** A CLOSE that "reaches 0" in far less than `travel_m3` with bit 3 never set is reported, not believed.

     > **DETECTOR IMPLEMENTED 2026-09-15**, stroke-local like rule 1: `ALARM ch6` param **250** (`LOG_PARAM_WPOS_EARLY`), `value_a` = elapsed seconds, `value_b` = `travel_m3` seconds, so the row states its own basis for "too early". One row per stroke, plus an `early_stops` counter.
     >
     > **Bit 3 is the load-bearing condition; the timing is corroboration.** At the closed switch the device reads 0 **and** makes bit 3 (§2a, teach table), so the two arrive together — a CLOSE claiming ~0 with bit 3 never made for the whole stroke is a position claim nothing supports. That is also what keeps a legitimate part-way CLOSE safe: a window starting at 30 % genuinely reaches 0 at 30 % of travel, well inside the "too early" window, but it arrives *at the switch*, bit 3 is made, and the rule stays silent. Reading the timing as the trigger instead would false-trip on every partial close.
     >
     > **"~0" is `deadzone_m3`**, not a new constant — that key is the operator's own statement of the smallest position error worth acting on, so it is already the definition of "close enough to closed", and inventing a second threshold here would let the two disagree. This is its first consumer.
     >
     > **Bit 4 (`both_end_sensors`) withholds judgement.** It means the end-sensor loop is faulted and bit 3 cannot be believed in either direction, so the rule resets rather than guessing. Two consecutive confirming samples (~1.3 % of any stroke, since the poll is travel/150) cover the race where position reads 0 one poll before the switch is made.
     >
     > **Two of the assumptions above are wrong ([gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78), found 2026-09-17, to be fixed in Phase 5).**
     > - **The ~0 reading and bit 3 do not arrive together.** On the rig, position reads 0 for about 1.2 s (7 polls) before the closed end sensor makes, in every close checked. So a part-way CLOSE that reaches ~0 in under half `travel_m3` trips the rule on its second ~0 sample, although it closes normally. Examples are a wind override or an LCD reversal while M3 is opening, and a recalibration of a partly open M3.
     > - **Bit 3 at the OPEN end counts as "end seen".** A full CLOSE from OPEN starts on that sensor, so the rule is off for the whole drive and cannot catch a position that races to 0 on a full close.
     >
     > The soaks never showed either, because every close in them started at the open end.
     >
     > **Known limitation, deliberate:** a genuinely closed window whose *end sensor* is faulty or unwired trips this rule. That is correct in the sense that something is wrong — either the position or the end sensor is lying — but the row names the position, so read it with the end-sensor wiring in mind before blaming the encoder.
- **Never gate anything safety-related on position** (FR-WP18): wind override, motor-alarm handling and boot CLOSE_ALL stay time-based.

**Minimal end result of this phase (operator decision 2026-09-07): a movement trace.** Every stroke is logged at the **measurement interval**, giving a full position-vs-time record of each traverse. That is what earns trust in the implementation before anything acts on it.

Volume is bounded and speed-independent: because the interval derives from
`travel_m3`, a stroke costs a fixed number of rows on the rig and on production
alike. At ~3.3 M3 strokes/day that is a few hundred extra rows against ~2864
`SENSOR_HR` rows — of the order of **+11 %**.

> **Two corrections to the original arithmetic, both measured (2026-09-13).**
> The divisor is **/150, not /100** (see 3.1 — /100 spends the entire FR-WP04
> budget by construction), so a stroke is ~150 rows, not "exactly 100". And the
> trigger is currently `any_channel_travelling()`, **deliberate scaffolding**
> (see 5a) — so an M1-only or M1+M2 vent step also produces a full burst of rows
> labelled M3. Real overnight measurement: **2721 `ch3` rows in 13 h** across 10
> bursts. The +11 % figure is the right order; the "exactly 100 rows" claim is
> not, and neither number should be quoted as a bound while the scaffolding
> stands.

*Exit:* a stroke replays from the log as a clean monotonic ramp; plus AT-WP06 (disconnect mid-operation → fault within 2 poll cycles, ventilation continues), AT-WP07 (wind override still closes with the sensor disconnected), AT-WP09 (obstruct mid-travel → divergence reported).

**Exit status, 2026-09-13:**

| criterion | status |
|---|---|
| **AT-WP06** | **PASSED** — and only after failing first and exposing a real defect (the idle read swallowed failures, so the gate was blind at rest). Recovery measured at **31 s** against `PROBE_RETRY_MS` = 30 s. See below. |
| **AT-WP07** | **PASSED on hardware 2026-09-15, FDA4 `2.8.0-bench`.** Encoder disconnected at the bus (gate `timed` / `no_sensor`), M3 open, wind raised to **6.6 m/s against `v_max` 6** at the emulator: the override rose at t+124.5 s (`eg1` 0x81) and **M3 reached CLOSED 18.4 s later**. That 18.4 s is the point — `travel_m3` 13 s + the fixed 5 s margin = 18 s, so it closed **on the timer**, which is exactly what FR-WP18 demands of a safety path. **FR-WP18 holds on the wind path with no position sensor present.** Two things the run revealed. (1) **The unit was in STANDBY throughout** (`eg1` bit 7), so `--hold-open` was **inert** — T6 is suspended and a setpoint change moves nothing; M3 was open because it was opened by hand. The harness now detects STANDBY and says so instead of printing "holding M3 open" while nothing holds it. It also makes the attribution *cleaner* rather than weaker: T6 was not competing for the window at all. (2) The wind override therefore also demonstrably works **in STANDBY**, which is correct for a safety input. Harness: `python bin/at_wp07.py --host <ip> [--hold-open] [--force-vmax]`; it refuses on 5C88, restores what it changed in a `finally`, and refuses a vacuous pass if M3 is already closed, the gate is still POSITION, or no override is observed |
| **AT-WP09** | **RE-RUN 2026-09-17 on the 2.8.0 code (d8c7332, `2.8.0-bench`), PASS, with `bin/at_wp09.py --sequence`.** Healthy OPEN and CLOSE were silent (96 readings each, 1 500 mm of travel). With the draw-wire detached at OPEN, a CLOSE and an OPEN each reported the stall 6 s in (`param 249`: peak 0.0 mm/s against 57.6 mm/s), with 0 exempt. The sensor was back at the open end within seconds of reattaching, and the logout's CLOSE of the closed window was exempt. **What it did not exercise: the exemption's continuity break** (the end sensor dropping as the leaf leaves, which is what keeps a shorted wiper judged). The detached wire held its last reading, 1500 mm (the open end), not 0, so the CLOSE never met the exemption's position condition. For the OPEN, where the reading did sit at the target end, bit 3 had already released at the closed end, 1 s after it made: contract §5.3, a sensor zone shorter than the overtravel. With the wire attached the same sensor stayed made at rest. The script now reports, per fault stroke, whether the exemption's start conditions held. *Earlier record:* **DIVERGENCE HALF PASSED on hardware 2026-09-15, FDA4 `2.8.0-bench`.** Draw-wire detached from the leaf, M3 driven OPEN: `stall_faults` 0 -> 1, logged as `ALARM ch6` param **249**, `value_a=0` (peak rate 0.0 mm/s) `value_b=576` (half of nominal 1153) at **22:36:39**. *(This row previously said rule 1 "fired twice, once per boot, with identical values", counting a 21:58:38 row as a second detection. **It was not one.** The 2026-09-16 soak exposed it: 21:58:38 was the **second reboot of an OTA push**, whose CLOSE_ALL drove an **already-closed** M3 into its end switch — the wire was still attached. Identical values were the tell that went unread: a false positive and a real detection look the same in `value_a`/`value_b`. See the soak row below.)* `logparser.py` decodes it off the real SD log with **0 `raw:` fallbacks across 766 KB**. **Caveats, both material.** (1) The fault mode was a **detached wire, not the obstruction AT-WP09 specifies**: a detached wire *also* makes the device report the `65535` sentinel and set `sensor_fault`, so the gate oscillated `device_fault` <-> `ok` and rule 1 fired inside an open-gate window. Rule 1 is therefore shown to catch a non-following leaf **even through an intermittently faulting sensor**, which is stronger than the specified test in one way and weaker in another — an obstruction with a healthy sensor is still unexercised. (2) **The published control mode stayed `timed` for the whole stroke** — promotion waits for a stroke *boundary* while demotion is immediate, so a stall can be detected while the mode still reads TIMED. Do not read `mode_str` as "is T17 watching". **`--healthy` PASSED 2026-09-15**, wire reattached, on an **LCD manual CLOSE** (`SRC_OPERATOR_MANUAL`, so dwell-free and a full traverse). Non-vacuous by construction: `reads_ok` 73 -> 171, i.e. **98 accepted samples** during the stroke, `strokes` 1 -> 2, `rejected_rate` and `err_comm` both 0 — and `stall_faults` **unchanged at 1**. The mechanism, not just the outcome: peak `|rate|` was ~1750 (0.1 mm/s) against rule 1's threshold of **576**, a 3x margin, so the silence is comfortable rather than marginal. Trace was strictly monotonic descending, 7 989 -> 122 in ~5 s. (Measured rates exceed nominal 1153, which is why `RATE_LIMIT_MULT` is 3 and not 2 — `travel_m3` is switch-to-limit, the position span is switch-to-switch. See §3.) |
| monotonic ramp from the log | **DEMONSTRATED 2026-09-15, FDA4, `2.8.0-bench`.** `python bin/at_wp_ramp.py --host <ip> --induce` lowered `t_max_ngt` 20 -> 16 at night so T6 demanded full ventilation and stepped to M3; the stroke began 8.5 s later and ended OPEN at 26.1 s. **45 samples, span 0 -> 15 000 (0.1 mm) — the full calibrated travel — 0 samples rejected, monotonic throughout.** `t_max_ngt` restored to 20 and confirmed. The dwell did not defer it, so `dwell_open_s[2]` was not armed at the time. **Full travel is not published by the diag endpoint but is recoverable exactly as `nominal_x10 x travel_m3`: 1153 x 13 = 14 989 against the calibrated `40004` = 15 000.** |

---

#### 4a. The sensor-presence gate — landed 2026-09-12 *(operator decision; resolves the TBD above)*

The paragraph opening this phase left the failure behaviour **TBD**. The
operator settled it:

> *"T17 failure report will select one of two modes: 1: with working rope sensor the window will be controlled using opening distance, 2: without working rope sensor the fallback will be timed opening as we have currently implemented."*

So the gate is not a bus-time optimisation that happens to report a fault — it is
**the authority that selects the control law**, and it publishes that choice:

```c
windowpos_ctrl_mode_t windowpos_task_ctrl_mode(windowpos_gate_reason_t *out_reason);
```

**What it fixes.** All three startup branches in `window_pos_task.cpp` previously
logged and then fell through into the polling loop: *"T17 idle"* did not idle and
*"REFUSING"* did not refuse. On a unit with no sensor at address 40 that cost a
failed ~215 ms transaction per poll, and because the derived poll floor is
`DEVICE_MIN_WINDOW_MS` = 100 ms, on the 13 s rig the period is **shorter than the
Modbus timeout** — the bus was held continuously for the whole stroke, against
T5, whose receive loop never yields.

**It follows T5's house pattern on purpose** (`sensor_poll.cpp`): two
consecutive failures flip the state, recovery is on the first success, the
transition is edge-logged. What differs is the *consequence*. T5 reports a fault
and keeps polling, because T3 safe-fails on `EG1_BIT_SENSOR_FAULT_W` — there the
fault bit **is** the feature. Position is optional, so here the consequence is to
stop touching the bus and fall back.

| Decision | Why |
|---|---|
| Demotion to TIMED is **immediate**; promotion to POSITION happens **only at a stroke boundary** | Dropping to the timer mid-stroke is safe — the timer is what would have run anyway. Gaining position authority underneath a consumer already committed to a timed stroke is not. |
| A shut gate re-probes every **30 s** | One failed transaction per 30 s is exactly what the Phase 3 idle path already spent, so a shut gate costs no more at rest and strictly less during a stroke. A sensor connected mid-session recovers by itself. |
| `WINDOWPOS_BUILD_BENCH` is a **permanent latch** | Contract 9. It cannot change without reflashing the device, so there is nothing a re-probe could discover. |
| `WINDOWPOS_ERR_BUSY` never counts as a failure | Losing the bus lock to T5 says the bus was busy, not that the sensor is absent. Counting it would let ordinary contention disable a working sensor — the opposite of what gh#49's mutex was for. |
| A device-reported fault gets its **own** reason code | "Talking but says its own reading is bad" is not "absent", and the log has to tell them apart. |

**The GATE below is still not crossed.** The mode is published; **nothing
consumes it yet**. T2 still stops on its timer and T6 still steps on
temperature, so greenhouse behaviour is unchanged. Acting on the mode is the
next step, and it is the one that crosses the gate.

**Observability.** `LOG_PARAM_WPOS_MODE` = **248** on the `ALARM ch6` band,
edge-triggered, `value_a` = mode, `value_b` = `windowpos_gate_reason_t`;
documented in [../log/logparser.md](../log/logparser.md) and decoded by
`logparser.py`. Plus `gate.mode` / `gate.reason` and three counters
(`probe_fail`, `mode_changes`, `gated_polls`) on `GET /api/diag/windowpos`
— but **that endpoint is `#ifdef MODBUS_BENCH`, so a release build has the
audit log and nothing else.** That is why the log row carries the *reason* and
not merely the fact.

#### Verification — FDA4, 2026-09-12, `2.7.0-bench`

FDA4 was refitted to the dev rig and took `2.7.0-bench` by push-OTA;
**`fw_ver` and `asset_version` both read `2.7.0-bench`** after reboot. The
asset zip is 2.7.0's with only `manifest.json` rewritten — a content diff proved
`index.html`/`app.js`/`style.css` identical to `firmware/data/` on this branch,
so the branch carries no GUI of its own. `travel_s` read `[21, 21, 13]` before
the flash, so the rig's 13 s M3 value had survived the module swap.

**What the hardware proved:**

| Behaviour | Evidence |
|---|---|
| Gate opens at boot, sensor identified | `gate: {mode: timed, reason: ok}`, `build: 1` (not BENCH, so the latch correctly did not fire) |
| **Mode is NOT promoted at boot** | `mode_str: timed` with `reason_str: ok` while at rest — the intermediate state the asymmetry is for, reading as *not yet promoted* rather than *still probing* |
| Promotion happens at the stroke boundary | `timed -> position` at the `CMD_RECALIBRATE` sweep; `strokes: 1`, `mode_changes: 1` |
| Poll cadence still derived | `poll_ms: 100` (13000/150 = 86, floored at `DEVICE_MIN_WINDOW_MS`) |
| Edge-triggered logging | **3 rows for 2 boots + 1 stroke against 162 polls.** `logparser.py` decodes the real rows, and the whole 3577-row file parses with **zero** `raw:` fallbacks |
| No spurious demotion | 75 s continuous observation: `probe_fail: 0`, `err_comm: 0`, `gated_polls: 0`, gate never shut |
| Idle sampling resumes after the stroke | `reads_ok` flat from ~31 s, then +1 at ~61 s — the 30 s idle cadence |

**Still NOT verified, and why:**

##### AT-WP06 — failed first, then passed after a fix

**Run 1 FAILED and found a real defect.** The operator pulled the encoder for
50 s. **The gate did not demote**: `err_comm` stayed 0 and the mode stayed
`POSITION` with nothing on the other end of the cable. The idle branch was
`if (windowpos_read(...) == OK) { ... }` **with no else**, so it swallowed both
failed reads — visible in the log only as a **91 s hole** in the `ch3` rows
(17:30:16 to 17:31:47) and, on reconnect, a `param 247` row with `value_a = 18`:
the encoder's own uptime register going backwards.

Every other demotion path needs a stroke in progress or an already-shut gate, so
the gate was **blind whenever M3 was at rest** — which is most of the time, and
all night. The published authority could have claimed `POSITION` for hours after
the sensor vanished. Fixed by giving the idle read the same
two-consecutive-failure treatment as the stroke poll.

**Run 2 PASSED.** Same 50 s pull, measured end to end:

```
17:45:46  ALARM ch6 248,0,2   TIMED [no sensor answering at addr 40]   <- demotion
17:46:17  ALARM ch6 248,0,0   TIMED [sensor present and trusted]       <- recovery
17:46:17  ALARM ch6 247,21,0  device RESTARTED (uptime now 21 s)       <- encoder power-cycled
```

and over the diag endpoint: `timed / no_sensor` with `err_comm = 2`,
`probe_fail = 1`, then `timed / ok` once the re-probe succeeded. **Recovery
landed 31 s after demotion, against `PROBE_RETRY_MS` = 30 000** — the re-probe
cadence, measured. `err_comm = 2` is exactly the two consecutive idle-read
failures that tripped it; before the fix that counter read 0.

Two by-design zeros worth reading correctly: `mode_changes` stayed **0** because
the mode was already TIMED (no stroke since the reflash) so only the *reason*
moved; and `gated_polls` stayed **0** because it counts only ticks with a stroke
in progress, and M3 was at rest throughout.

**A 50 s pull is enough** — the *effective* outage is longer, because the encoder
power-cycles on reconnect and must complete a measurement window before it
answers again. Run 1's 50 s pull produced a 91 s hole.

**Run 2 also exposed a second defect, now fixed.** For the whole outage
`GET /api/diag/windowpos` returned nothing but
`{"ok":false,"err":"read_failed","status":1}`: the handler does its direct read
first and returns early, so `gate.reason` — the one field you want when the
sensor is missing — was never printed. Gate state is task state and needs no bus,
so the failure path now carries `gate` and the counters too. **Compiler-verified
only:** confirming that JSON needs another pull.
- **`WPOS_GATE_DEVICE_FAULT` is narrower than it looks.**
  `windowpos_reading_t::sensor_fault` is the **wiper-open** bit or the `65535`
  sentinel, so pulling the *bus* cable yields `NO_SENSOR`; `DEVICE_FAULT` needs
  the *wiper* wire open specifically. Two different physical tests.
- **`WINDOWPOS_ERR_BUSY` never counting as a failure.** `err_busy` stayed 0 even
  with three bus callers, for the same reason AT-WP05 was a qualified pass: the
  500 ms lock timeout comfortably exceeds the ~215 ms hold. The rule is correct
  by construction but **untested on hardware.**
- **The BENCH latch.** Needs a sensor running a bench firmware.

#### Release build without an encoder — FDA4, 2026-09-16, `2.8.0` (the path 5C88 takes)

**Why.** Every earlier gate check ran a bench build, but 5C88 runs a release build and has no encoder. A shut gate still re-probes addr 40 every 30 s. Each probe is one read of one register, which holds the bus for the 200 ms timeout plus the 20 ms frame gap: about 0.75 % of the time. That is new traffic on the unit whose fault rate is still unexplained (gh#66 / gh#68).

**Method.** The encoder was unplugged, and FDA4 took the `2.8.0` release build (d8c7332) by push-OTA: bank B to A, `fw_ver` and `asset_version` both `2.8.0`. A release build serves neither `/api/diag/windowpos` nor `/api/diag/commission` (both 404). So the public `/api/status` was polled every 30 s for 30 min, and the SD log was read afterwards. Then the encoder was plugged back in.

| Check | Result |
|---|---|
| Gate at boot | Shut 35 s after boot, at the second failed probe, with one row: `M3 CONTROL MODE -> TIMED [no sensor answering at addr 40]`. No other window-sensor row in the 30 min, and no `ch3` rows |
| Re-probe cadence | 60 failed probes at addr 40 in 1 801 s: exactly one per 30 s |
| T5 (addr 1, addr 44) | 59 and 118 reads, **0 errors, 0 `busy`**. No sensor-fault or wind-override rows |
| Status | AUTOMATIC with no flags throughout. The M3 position keys were absent (not zeroed), and there was no `sensor_fault_position` flag |
| Heap | Never below 68 KB free or a 26 KB largest block. No reboot |
| Deadzone setting | Served by `/api/config` (`deadzone_m3_mm` 20) and `/api/config/limits` ([1, 200]) |
| Reconnect | The first probe after the plug-in found the encoder, 30 s after the last failed one, and logged `TIMED [sensor present and trusted]`. `/api/status` carried the position again 2 s later. At the next stroke boundary (a forced recalibration) the mode went to `POSITION`, and the CLOSE of the already-closed window raised no stall |

**Not exercised: a stroke while the encoder was out.** The boot skipped its calibration, because the saved state was all CLOSED, and T6 kept the windows closed for the whole 30 min. With the gate shut, T17 does nothing during a stroke except count `gated_polls`. Arm A counted 778 such ticks on a bench build with the same shut-gate path.

**What 30 min can and cannot say.** It shows that the release build takes the path it should, and that the probes do not collide with T5 (0 `busy`), with gh#70's UART changes in place. It cannot resolve a change in T5's failure *rate*. The evidence for that is arm A: 18 h 33 m, 6 579 T5 reads, 0 failures, same probe traffic.

#### Fitted or not: the installation setting (gh#73, 2.9.0)

**Why.** The run above shows the cost on the path 5C88 takes. The gate cannot tell "no sensor
fitted" from "a fitted sensor that stopped answering", so a unit without an encoder re-probes
address 40 every 30 s for ever. Since 2.8.0's per-slave counters (gh#66), each failed probe also
shows on the Bus card and in the hourly log as a failing slave. On 5C88 that would read as a broken
sensor on a unit that has none. The other case was wrong too: in the run above the encoder was
unplugged and no fault flag appeared, which FR-WP19 requires.

**Decision (operator, 2026-09-17).** An installation setting, `motor/wpos_fitted_m3`, **default
not fitted**, released on its own as 2.9.0; the T2-on-position step moves to 2.10.0. With that
default, 5C88 takes the fix by ROTA with no site visit. The rig modules, FDA4 and 2344, are switched
on once. Requirement: FR-WP23 in the requirements study.

| | Not fitted (0, the default) | Fitted (1) |
|---|---|---|
| T17 on the bus | nothing at all | as before: gate, 30 s re-probe, stroke and idle reads |
| Mode row (param 248) | `TIMED`, reason `5`: once at boot and at each switch-off | as before; a switch-on logs `TIMED [probing]`, then the verdict |
| Status payload | no `M3_*` keys, no fault flag, no address 40 in `bus` | `M3_*` keys when trusted; `sensor_fault_position` when the sensor is absent, refused or faulted |
| Hourly bus rows | none for address 40 | as before |
| GUI | *Linear control* greyed below the setting, with the reason above it | as before |
| Commissioning (bench) | refused with `not_fitted`, except abort | as before |

**Details that matter.**
- **A change is followed within one idle tick** (500 ms). T17 reads the setting at most that often,
  through a single-field accessor that falls back to a lock-free read, not to the default, when the
  lock is busy.
- **A switch-on starts over:** no verdict, no failures counted, the bench-build latch cleared, and a
  probe due at once.
- **A switch-off forgets the last reading** and the event baseline. It does not count as a probe
  failure, and it abandons a stroke in progress, so a later switch-on mid-stroke starts a fresh
  verdict.
- **T17 waits for T4 to load the configuration** (up to 10 s). T4 is created first but loads NVS in
  its own task body; reading the zeroed shadow would log a not-fitted row on every fitted unit's
  boot.
- **"Fault" now means fitted and unusable.** The status snapshot raises `sensor_fault_position` for a
  fitted sensor that is absent, refused (bench firmware) or faulted, and never for an unfitted one.
  Before 2.9.0 an absent sensor raised nothing, which is why the run above saw no flag.

**Verified on 2344, 2026-09-17, with the 2.9.0 release image and the encoder connected**
(`bin/at_wpos_fitted.py`, stage by stage):

| Stage | Result |
|---|---|
| `unfitted`, right after the update | PASS. The setting read 0; for 300 s no status reading showed address 40, a position or the flag |
| `on`, at 331 s uptime | PASS. The sensor appeared 0.1 s after the read-back. Address 40 had **4** transactions since boot, the switch-on's own; a T17 still reading at rest would have had ~22 |
| `off`, then on again | PASS. Gone 0.1 s after the read-back, absent for 300 s, and on switch-on the count had grown by **4** (~20 if T17 had kept reading) |
| `log` (SD) | PASS. One `TIMED [not fitted]` row per boot, 7 s after the boot row; each switch, including one made through the web interface outside the test, logged its param-49 audit row and the mode rows within a second; no position rows while not fitted |
| `unplug` | PASS (16:18). Fault flag 31 s after the first failed read, position gone with it, address 40 still listed; re-plugged, both back at the next re-probe. One SD mode row each way. An earlier attempt timed out with the encoder never unplugged |
| hourly bus rows | PASS. At 14:50, with the setting on: addresses 1, 40 and 44. At 15:49, with it off: 1 and 44. T4's "hour" is 59 min here |

#### Every drive judged, and a gate that no longer flaps (gh#72, 2.9.1)

**Why.** Rules 1 and 2 must judge every movement before position may act on M3 (Phase 5). Three
defects stood in the way:

1. **A reversal was one stroke to T17.** T2 reports its 2 s reversal gap as moving, so rule 1 kept
   the first direction's settled verdict and rule 2 kept its direction. A wind override closing
   an opening window, the case that matters most, was never judged.
2. **A self-reported device fault made the gate flap.** The 30 s re-probe read only the identity,
   re-opened the gate, and the next idle read shut it again: two mode rows per cycle, a probe
   failure per cycle, and promotions in between.
3. **A stroke could inherit a stale verdict.** A shut gate kept the stroke it shut in, so a gate
   that re-opened during a later stroke judged it with the old direction, start time and verdict.

**Fix.**

1. **Judge per drive.** T2 counts relay energisations (`t2_get_drive()`), and a new count starts a
   fresh verdict, timed from the energised relay. Nothing is judged in the gap. `strokes` counts
   judged drives, and `redrives` counts the extra ones.
2. **The probe also reads the position** and stays shut on a faulted reading.
3. **A shut gate forgets the stroke**, and promotion needs a stroke seen to start from rest with
   the gate open (`rest_seen`).

**Test hook, bench builds only.** `POST /api/diag/windowpos {"inject": ...}` makes T17's own reads
see the sensor absent, faulted or stuck (a shorted wiper). A `-DWPOS_FAILFIRST_GH72` build restores
the old behaviour. Both are reported by the GET, so a fail-first result cannot be mistaken for a
real one. The `stuck` injection also makes the continuity-break test possible without handling the
wire (a small item).

**Verified on 2344, 2026-09-17** (`bin/at_wp_gh72.py`, fail-first first):

| Stage | Fail-first build | 2.9.1 |
|---|---|---|
| `flap` (device fault at rest, 100 s) | FAIL: "ok" in 64 of 67 samples, `probe_fail` +3 | PASS: 0 of 75, `probe_fail` +1, re-opened on clearing |
| `stale` (absent in a CLOSE, back mid-OPEN) | FAIL: `strokes` +0 | PASS: `strokes` +1, mode stayed timed |
| `reversal` A (stuck, CLOSE reversed to OPEN) | FAIL: 1 stall | PASS: 2 stalls, `redrives` +1 |
| `reversal` B (healthy) | no stall (expected) | PASS: no stall, `redrives` +1 |

**A fourth defect, found after the commit and fixed before publishing: a drive joined late.**
After a power cycle with M3 OPEN, T2 recalibrates at boot, and T17 comes up about 6 s into that
CLOSE. T17 timed the drive from its first look, so rule 2 reported *"claimed closed after 5 s
of a 13 s traverse"* for a drive that had really run 12 s. The leaf reads ~0 about a second
before the closed end sensor makes (§2a.6). T2 now also records the energise time
(`t2_get_drive(ch, &epoch, &started_ms)`), and T17 times the drive from there.

**A power cycle is not a fail-first test for this.** How late T17 joins at boot varies, and the
old timing only trips when the join is late enough (about 5.5 s on the rig):

| Boot | T17 joined after | Old timing |
|---|---|---|
| 17:44 | about 5.5 s | tripped |
| 18:41, on the fail-first build | about 2.5 s | did not trip, so that run passed |

The `latejoin` stage therefore makes the late join on purpose:
1. The sensor is made absent at rest, and the gate shuts.
2. The operator closes M3 from the LCD.
3. The injection is cleared 7 s into the CLOSE, so the gate re-opens mid-drive.

| Build | Gate re-opened | Leaf then at | Result |
|---|---|---|---|
| Fail-first | 8.1 s into the CLOSE | 441 mm | **FAIL**: `param 250` *"closed after 2 s of a 13 s traverse"* for an 11 s drive |
| 2.9.1 | 7.8 s into the CLOSE | 510 mm | **PASS**: no early stop, no stall; ~0 came 11 s after T2's start |

Both runs were on 2344, 2026-09-17.

**The fix does not cure rule 2 for a part-way CLOSE.** See the correction under §12.4 rule 2
above ([gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78), Phase 5).

**Operator timing matters.** A "press Open when M3 is closed" instruction produced a reversal on
the first run. T2 drives on for its 5 s margin after the leaf stops, and still reports the stroke
until then. The stale stage now asks for the second key only after the controller reports CLOSED.

**Not tested on hardware:** a reversal made by T3 itself, which takes the same T2 path, and a real
device fault; the injection simulates the wiper-open reading.

#### Rig finding — T17's stroke poll starves T5, and it looks like a wind alarm

The operator reported a **wind alarm persisting after a reset while the wind
sensor was reading valid**. It is not wind. Across four SD log files (2026-09-05
to 09-12, ~78 000 rows, 66 boots) there are **8** `EG1_BIT_SENSOR_FAULT_W`
onsets, and the measured wind at every one was **0.0—1.9 m/s** — nowhere near any
`v_max`. The chain is:

1. T5 loses **two consecutive** S200 reads and sets `EG1_BIT_SENSOR_FAULT_W`.
2. T3 **safe-fails** on that bit and raises a wind override — logged as
   `ALARM ch0 param 243`, *"wind sensor fault safe-fail"*.
3. The GUI/LCD still show a plausible wind speed, because on a failed read T5
   fills `wind_speed_ms10` from `avg_get(&s_avg_ws)` — **the last known average,
   carried forward** to avoid a gap in T4's ring. So the operator sees a valid
   reading and an alarm at the same time, which is exactly the symptom.

**5 of the 8 coincide with T17 stroke-polling** (63—158 `ch3` samples within
+/-30 s). That is the rig-only pathology: a `travel_m3/150` poll floored at
`DEVICE_MIN_WINDOW_MS` = **100 ms**, against a **~215 ms** Modbus transaction, in
a receive loop that never yields — i.e. **a poll period shorter than one
transaction, so bus duty is ~100 % for the whole stroke** and T5 cannot get in
within its 500 ms lock timeout. Production polls at 1140 ms (~18 % duty) and
does not have it.

**3 of the 8 had no `ch3` samples at all**, and two of those had M3 relay rows
within +/-30 s — so there is a **second contributor independent of T17**, most
plausibly relay-switching noise on the shared RS485 run. **2344 shows 0 faults
across 13 boots** in the pre-encoder era, so the encoder's presence on the bus
is implicated in the T17 half but cannot explain the other half.

**"After a reset" is a red herring, and this was measured.** A reset is normally
*followed* by M3 movement, and the movement is the trigger. The 2026-09-12
17:39/17:40 reflash produced two boots with **`strokes: 0`** — boot calibration
was skipped because M3 already sat on its end sensor — and **neither boot
produced a wind fault**. No stroke, no alarm.

**Reproduced on demand and then FIXED — 2026-09-12.** The operator moved M3
manually and raised the wind alarm **3 times out of 3**, with the wind measuring
1.1—1.7 m/s, unpolluted by any diagnostic polling of mine. That is a fail-first
baseline, so the fix has something to be measured against.

**Root cause, at the third attempt.** Two defects were found on the way, and
only the second one was firing. Recording both, because the first is real and
the sequence is the lesson.

##### Three defects found. The third one was the cause.

Recording all three, in the order they were found, because the two wrong
answers were both real bugs — which is exactly why they were convincing.

**Defect 1 — drivers collapse `MODBUS_ERR_BUSY` into `*_ERR_COMM`.** The Modbus
layer keeps BUSY distinct from a timeout (gh#49 put it there so a lost bus race
could not be read as a dead device, and `modbus_rtu.h` says so). Both sensor
drivers threw it away in `map_status()`, so *"I could not get the bus"* reached
T5 indistinguishable from *"the sensor did not answer"*. Fixed: `S200_ERR_BUSY`
and `FG6485A_ERR_BUSY` appended as 3, carried through, and T5 treats BUSY as no
evidence about the sensor — bounded by `SP_BUSY_TOLERANCE_MS` = 60 s, a
deadline rather than a retry count, because `poll_interval` is
operator-configurable 15—120 s and a count of two would tolerate 240 s of
blindness on a safety input. **`busy` counted 0 for the entire investigation, so
this was never it.**

**Defect 2 — four of six transaction exits leave the RX FIFO dirty.** Success
and timeout drain it; `MODBUS_ERR_CRC`, `MODBUS_ERR_EXCEPTION` and both
`MODBUS_ERR_FRAMING` exits returned with bytes still in the buffer, which the
next transaction — a different task, a different slave — then consumes as its
own echo and response. Fixed by flushing before every transmit and draining on
every error exit. **`crc` counted 0, then 1, so this was not it either.**

**Defect 3 — THE CAUSE: the inter-frame gap was the spec floor plus 9.7 %, and
a single caller never exercised it.**

`MODBUS_IFG_US` was **4000 us** = **3.84 character times** at 9600 baud 8N1,
against an RTU t3.5 minimum of 3.646 ms. The driver's own comment called that
"a comfortable margin".

The decisive observation is that **with one bus caller the IFG guard never ran
at all.** Frames sat 30 s apart, so `elapsed` always dwarfed `MODBUS_IFG_US` and
the wait was skipped every single time. The constant became load-bearing only
when T17 joined the bus and made frames *adjacent* — which is precisely when
the S200 began failing to answer (operator: never observed before the wire-rope
work, and never with the semaphore and a single task).

If the S200's receive-idle timer wants more than 3.84 character times — very
plausible for a device whose idle timer runs on a coarse tick — then a request
arriving at the floor is appended to the preceding addr-40 frame, the merged
frame fails CRC, and **a compliant slave must stay silent on a bad-CRC
request.** So the requester gets *zero bytes and a timeout*, never a CRC error.

That is exactly what was measured, and it is why the instrumentation was worth
the detour:

| measurement | value | what it ruled out |
|---|---|---|
| `to_received` / `to_expected` | **0 of 29** | not a truncated frame — the slave said nothing at all |
| `crc` | **0** | not Defect 2 |
| `busy`, `lock_wait_ms` | **0**, **0 ms** | not lock contention; the semaphore was working correctly |
| `last_fail_addr` | **44** | the S200, while T17 read addr 40 flawlessly in the same stroke |

**Result of raising it to 20 ms (~19.2 character times, 5.5x t3.5):**

| | 4 ms IFG | 20 ms IFG |
|---|---|---|
| transactions | ~600 | **779** |
| timeouts | 4 | **0** |
| CRC | 1 | **0** |
| wind alarms | one per stroke session | **none** |

**Kept at 20 ms rather than tuned down.** The only argument for a smaller gap
was T17's sample resolution, and it does not survive the arithmetic: the rig
sample interval is 1.54 % of stroke at 4 ms and 1.70 % at 20 ms, because the
dominant terms are the 100 ms poll delay and the 36 ms response, not the gap.
The rig was **already over the 1 % FR-WP04 budget** before this change (AT-WP05
measured 168 ms effective against 100 ms intended, `vTaskDelay` being relative),
and on production the poll is 1140 ms so any of these values is under 0.2 %. The
threshold lies somewhere in (3.84, 19.2] character times and was deliberately
**not** bracketed: locating it would cost several flash-and-stroke cycles and
buy robustness in the wrong direction for a path whose failure mode is *the
greenhouse closes on a calm day*.

`wait_ifg()` yields the millisecond-scale part of the wait rather than spinning
it. `delayMicroseconds()` busy-waits inside the bus lock at task priority, and
20 ms across ~6 transactions/s during a stroke would be ~120 ms/s of pure spin
— enough to starve T9 and the HTTP server, and to worsen this driver's already
documented TWDT exposure.

**The wait is paid before releasing the lock, not after taking it** (operator,
2026-09-12). That makes the lock's guarantee *"the bus is yours AND it is
idle"*, so a caller may transmit the moment it acquires. Waiting at
acquisition instead makes every caller pay for silence it did not create, and
leaves the invariant resting on each caller remembering to ask — the same trap
as draining the RX FIFO at the next transaction's start rather than at this
one's exit. Total lock hold is unchanged (~234 ms worst case, still 2.1x inside
`MODBUS_LOCK_TIMEOUT_MS`); what changes is who waits, and what the lock promises.

> **Rule worth keeping:** on a shared RTU bus, mutual exclusion is necessary and
> **not sufficient**. The protocol also requires *silence* between frames, and a
> gap set at the spec floor stays untested for as long as there is only one
> caller. gh#49 made two callers legal; it did not make the inter-frame gap
> adequate for two, and the audit that added the mutex had no reason to examine
> a constant that had never once been reached.


Two things this leaves for this plan, neither now load-bearing: **do not poll
`GET /api/diag/windowpos` hard during a stroke** (it adds a third ~215 ms caller
— that is how the 17:20:47 fault was first provoked), and **the
`DEVICE_MIN_WINDOW_MS` poll floor is still below one Modbus transaction**, so
T17 runs at ~68 % bus duty during a rig stroke. That is now a fairness and
efficiency question rather than a correctness one, and raising it would trade
against the FR-WP04 analysis in section 3.1 — so it is deliberately **not**
changed here. The relay-noise contributor (3 of the 8 historical faults, no T17
samples, M3 relay rows nearby) is untouched by this fix and still open: those
are genuine CRC/timeout errors and *should* be reported.

---

### ▲ GATE — everything above changes no control behaviour

Phases 0–4 add a sensor, logging and diagnostics. The greenhouse behaves exactly as it does today. Cross this gate only when the rig has produced clean aperture data over a sustained period **and** the Phase 5 scope decision has been taken.

> **Status of the two prerequisites, checked 2026-09-15.** *(The scope-decision
> clause cited "§8" until now; §8 is **Risks**. The decisions live in **§10**,
> items 1 and 5 — a cross-reference is a claim about a file, and this one was
> wrong.)*
>
> | prerequisite | state |
> |---|---|
> | Phase 5 scope decision taken | **YES** — §10.1 (2026-09-07) and §10.5 (2026-09-13). Note what was decided: the *vent algorithm* does not change. Positioning M3 by sensor is gate 1 and is in the §5.0 slice; changing the algorithm is gate 2. |
> | clean aperture data over a sustained period | **MET 2026-09-17: SOAK #2 PASSED** on FDA4 (`2.8.0-bench`, d8c7332): 12.02 h, **14 judged strokes** (7 OPEN, 7 CLOSE, none exempt), `stall_faults`, `early_stops`, `rejected_rate` and `err_comm` all 0, one mode change (the promotion at the first stroke), no reboot, heap flat over 722 samples. Two caveats travel with it: the thresholds are validated on the rig's 13 s window only, and a reversal mid-stroke is not judged (gh#72; one LCD sequence with two reversals counted as a single stroke). AT-WP09 was re-run on 2026-09-17 and passed, but the exemption's continuity break is still not exercised (see the AT-WP09 row). *The history below is kept for the record.* **PARTLY, and materially stronger as of 2026-09-15.** **Two clean traverses in both directions**: an OPEN ramp (45 samples, full 0 -> 15 000, 0 rejected) and a CLOSE ramp (98 accepted samples, 7 989 -> 122 monotonic, 0 rejected). **All three acceptance tests now pass** — AT-WP06 (2026-09-12), and AT-WP09 and AT-WP07 on 2026-09-15. What remains for *sustained* is **elapsed time alone**. **SOAK #1 FAILED 2026-09-16, and it failed usefully: it found a false positive in 12.4 rule 1 that no single stroke could have.** After 12.23 h and 14 strokes, `stall_faults` +1 at **09:32:06**. The log shows an **admin LCD session closing** -> STANDBY released -> `session_close()` **recalibrating** with a CLOSE_ALL, while **M3 was already closed** (position 0 before, during and after). The end switch cut the drive exactly as designed, the leaf correctly did not move, and rule 1 read that as a stall. **Systematic, not rare:** it fires on every CLOSE_ALL of a closed M3 — each boot (twice per OTA push) and each LCD logout. `--healthy` could not catch it because a healthy stroke that *traverses* never tests "driven toward the end it is already at". **Fix:** rule 1 now exempts a stroke whose leaf **began and stayed** on the end it is driven toward — bit 3 made (bit 4 clear) **and** position within `deadzone_m3` of the target end, **on every sample of the grace window**, counted as `at_end_exempt`. Continuity is the safety property: a shorted wiper reads a constant 0, so on an *open* window a CLOSE starts at "0, bit 3 made" (the open end sensor) — but bit 3 drops as the leaf leaves that end (~1.8 s, inside the 5 s grace), so it is still judged. T2's belief could not substitute: after a reboot it is `WIN_UNKNOWN`, exactly for the stroke that needs the exemption. **Replayed on the real strokes:** both false positives (09:32, 21:58:29) -> EXEMPT; the genuine detached-wire OPEN (22:36) and a real traverse from open (21:58:12) -> still JUDGED. Only the position half is replayable — `SENSOR_HR ch3` carries no status bits, so bit-3 continuity was read off the edge-triggered `ALARM 246` rows, and **the shorted-wiper case it exists for is argued, not tested**. `at_wp_soak.py` now counts **judged** strokes (`strokes - at_end_exempt`) towards its sample, so a soak made of recalibrations cannot pass on strokes rule 1 never looked at. **SOAK #2 ENDED 2026-09-16 at ~12:00 by the flash of `a3d2c2a` and has NOT been restarted** — the teach work after it flashed FDA4 four more times. *Its last restart, for the record:* **SOAK #2 RESTARTED AGAIN 2026-09-16** (baseline `strokes=4`, all fault counters 0, gate `position`) after a flash of commit `3006cce` and a **teach run on the rig before the baseline** so its strokes are outside the soak. **The teach itself was wrong, and two fixes chased a symptom before that was seen (corrected 2026-09-16).** Every GUI teach ended *teach complete* beside *INVALID — teach still armed*. `3006cce` (judge on a fresh read) and `a3d2c2a` (a CHECKING state, `CAL_ERR_VERIFYING`) both treated that as a read/persist race, and **both diagnoses were wrong**. The teach drove **one** traverse from a parked end and then read `30013`/`30014` as if that read were the commit. But the sensor captures an end only when its end sensor **makes** after arming, so the end the leaf started on was never captured and the device never committed on the teach's account. From OPEN it *appeared* to work only because **T6 reopened the window afterwards** and made the second capture (SD log: 11:49:50 closed sensor made -> 11:50:01 T6 `MOVING_OPEN` -> 11:50:12 open sensor made -> 11:50:13 COMMITTED). From CLOSED, T6 wanted the window closed, was held off by dwell, and `a3d2c2a`'s 30 s timeout disarmed the device at 12:01:48 — the very second T6's CLOSE began. The *848 vs 850 persist race* was the device still holding its previous calibration. The outcome depended on T6, not on the teach. **Correction to `3006cce`'s commit message**, which says the operator's earlier teach left the calibration "unaffected (still 0..858)": **it did not** — a later T6 stroke completed that teach and moved it 858 -> 848; the 858 on screen was the old calibration, still in force when the teach was judged. **Operator requirement: direction must be irrelevant during any teach.** **Fix, hardware-verified on FDA4 2026-09-16:** the teach arms, waits for a reading with bit 5 set, then drives **legs** — each the opposite way to T2's belief, because T2 ignores a command to go where it thinks M3 already is, at most 3 — and is committed when the device clears bit 5 after **two end sensors have made**. There is no explicit capture read: T17's full 15-register poll already satisfies the read handshake (contract §6.3), which is also how T6 completed the old teach. M3 may start anywhere; the at-an-end precondition is gone. Failure reasons now say what happened: `end_missed` (a stroke ended between the sensors — `travel_m3` too short?), `refused` (both ends made, bit 5 stayed set), `no_move`, `no_start`, `dropped`, `m3_busy`. `CAL_ERR_VERIFYING` and the CHECKING display are removed, and the server's teach tables are guarded by `_Static_assert` against their enums (shown to fail the build with a string missing). **`bin/at_wp_teach.py` PASSED on three successive builds, the last with the orphan assertion below:** a teach from CLOSED (2 legs, ~33 s, COMMITTED), then a teach aborted after its first end sensor (nothing left armed, calibration unchanged), then a teach from OPEN (2 legs, ~32 s, COMMITTED), with `stall_faults`/`early_stops` unmoved. Repeatability over the eight teaches of the day: open end **847–857** counts, a spread of about 18 mm at rig speed — the same order as the 100 ms measurement window's ~15 mm freshness bound there, which production's slower traverse shrinks to about 1 mm (contract §8.1). **Orphaned teaches are now aborted.** The sensor keeps an armed teach across a *controller* restart, and because T17's poll is the read handshake, T6's next two strokes would commit it unwatched. T17 now aborts any armed teach the commissioning path does not own — logged as param 245 value 4, counted as `orphan_aborts` — at gate open and on every reading, in release builds too. Verified both ways on FDA4: planted 6 s before an OTA reboot -> aborted at gate open (12:45:09); planted while T6 was moving M3 -> aborted within a second, calibration unchanged. **Found and fixed during that test:** the harness's own abort was logged as an orphan, because bit 5 lags the disarm while the state already says idle; a 5 s grace after our own disarm cures it, and `at_wp_teach.py` now fails if `orphan_aborts` moves. **A boot panic found on the way, fixed, and not caused by the teach:** after one OTA push FDA4 panicked twice (T17, `LoadProhibited` in `uart_get_buffered_data_len`). T5's entry `modbus_init()` deleted the UART under a T17 read while T2's boot recalibration drove M3 — the use-after-delete `design/addModbusMutex.md` §6.1 had logged as safe only while T5 was the sole caller. A re-init now takes the bus mutex — **gh#69, closed after a fail-first test** (`bin/at_modbus_reinit.py`: the lock compiled out crashed 5 of 5 runs, twice with the field signature; the real build passed 500, 2 000 and 3 000 re-inits under traffic, and four natural boots of the scenario did not panic). **This was a merge blocker for the branch as a whole**: in production M3's boot recalibration lasts 176 s. **So, in a milder form, was gh#70 (fixed 2026-09-16):** during an OTA's flash writes the encoder's replies were lost because the DE/RE line was released by the task, which closed T17's gate whenever it was polling fast, and the same stall could make T5 raise a wind-sensor safe-fail. The UART drives the line now; `bin/at_modbus_ota.py` failed on a task-driven build and passed on the fix (baseline 2 166 reads / 0 failures (was ~1 in 540), firmware upload 686 / 0 (was 16 and 13), asset extraction 112 / 0 (was 7 and 9)). **Not yet exercised:** a teach started part-way (nothing on the web API can stop M3 mid-travel), a teach whose first leg is a no-op because T2's belief is wrong, and any failure branch other than the abort. *Previous restart, for the record:* **SOAK #2 RESTARTED 2026-09-16** after a further flash that fixed the commissioning card (see below); the restart baseline is again `strokes=1 at_end_exempt=1 stall_faults=0`, gate `position`, and the flash's own boot CLOSE_ALL once more showed the exemption working. **The commissioning card reported a calibrated sensor as UNKNOWN after every reboot.** `commission_refresh()` ran only after an operator action — set window size, teach, abort, or a `refresh` POST the GUI never sends — and the GET handler serves a cached status, so a fully calibrated encoder showed verdict UNKNOWN, window size 0, taught 0..0, **right beside a "Teach (moves M3)" button**: an invitation to re-teach a working sensor, which moves the window and can mis-calibrate it. The tell was `cal_reason: "none"` — a refresh that had run and failed would say `no_device`, so "none" with everything zero meant it had never run. **Fix:** T17 calls `commission_refresh()` each time the presence gate OPENS (not once at task start), so a sensor absent at boot or refitted later is re-judged when it returns. **Verified with a GET only, no refresh sent:** UNKNOWN / 0 / 0..0 -> **VALID, 1500 mm, taught 0..858, 83 % of range**. The GUI also stops a range input's default midpoint from reading as a real ~2550 mm when no size is set: the field says `not set` and the row is dimmed but still usable. *First start, for the record:* **SOAK #2 STARTED 2026-09-16 on the fixed build** (FDA4 `2.8.0-bench`, commit `2326ad1`), after the three fixes were each **confirmed on hardware** first: `/api/diag/commission` 404 -> **200** (route registration); the served `app.js` greys rather than hides (GUI rule); and — the one that matters here — **the boot CLOSE_ALL of an already-closed M3 gave `at_end_exempt 1`, `stall_faults 0`**, on the exact stroke that had false-tripped twice per OTA push, with `reads_ok 83` showing it was judged on real samples rather than skipped. Baseline `strokes=1 at_end_exempt=1 stall_faults=0 early_stops=0 rejected_rate=0 err_comm=0 mode_changes=1`, **gate already `position`** — unlike soak #1, which started `timed`. Judge with `python bin/at_wp_soak.py --report --host <ip>`; it counts **judged** strokes only. *Soak #1, for the record:* **started 2026-09-15 on FDA4 `2.8.0-bench`**, encoder refitted, wind override cleared, STANDBY released; baseline `strokes=3 stall_faults=2 early_stops=0 rejected_rate=0 err_comm=2 mode_changes=4`. Judge it with **`python bin/at_wp_soak.py --report --host <ip>`**, which requires **>= 12 h AND >= 10 strokes** before it will say anything at all: zero faults across three strokes is evidence that little happened, not that the detectors stay quiet. The criteria it checks are `stall_faults` and `early_stops` both 0 (the 12.4 rules must not false-trip across many *real* strokes — one healthy stroke only proved they *can* stay silent), `rejected_rate` and `err_comm` 0, and `mode_changes` <= 2 so the gate is shown to **settle** in POSITION rather than flap, which would make the period a mix of traced and untraced strokes. |
>
> **So this gate is NOT crossable today, and the blocker is evidence rather than
> code.** What it needs is a deliberate OPEN stroke with the gate in POSITION,
> producing a ramp, plus AT-WP09. Writing T2's position consumer first would be
> building the control path on a sensor that has never once been shown to trace a
> traverse — precisely what the gate exists to prevent.

---

### Phase 5 — proportional M3 control *(**SEQUENCED behind 5.0**, not started — was "out of scope, recorded not planned" until 2026-09-13)*

The larger part of the effort, and the part the rig **cannot validate**.

- **T2:** a target input to `CH_MOVING_OPEN`/`CH_MOVING_CLOSE` that de-energises at target, travel timer retained as ceiling. A partial position becomes a new persisted state — an NVS schema change, so a **minor** version bump. **Scheduled 2026-09-17 into 2.11.0, not 2.10.0: see §5b.**
- **T6:** `VENT_STEP_TABLE` must express partial M3 apertures.
- **`step_width = max(hyst_t / NUM_VENT_STEPS, 1)` is integer division and gets *worse* with more steps** — at 6 steps, `hyst_t = 5` gives width 0 → clamped to 1 → every step 1 °C. **The F8 arithmetic must be reworked before the step table is touched**, not after.

**What the rig can prove:** that the controller commands and reaches intermediate apertures accurately and repeatably.

**What only 5C88 over a summer can prove:** that doing so actually damps the ~42 min / ~4.9 °C limit cycle. The plant model gives a prediction, not evidence.

#### 5.0 SEQUENCING GATE — finish M3 end to end before touching the control logic

**Operator decision, 2026-09-13:** build the **full M3 implementation with the
window sensor** — including the GUI and LCD integration, alarm handling and
logging — **before** any change to the control logic.

So there are now **two gates**, not one, and they are crossed in order:

| | Gate | What it permits |
|---|---|---|
| 1 | the existing ▲ GATE below | M3 may be *positioned* using the sensor. The vent algorithm is untouched. **Narrowed for 2.10.0 to confirmation only — §5b.** |
| 2 | this section 5.0 | the vent algorithm may change (section 5a: per-window setpoints, central algorithm, PID/fuzzy). |

**What "full implementation" means concretely**, given phases 0—4 are built:

- M3 **acted on** by position: travel-complete detection, de-energise at target,
  travel timer retained as the ceiling — while the **existing stepped logic
  still decides** open/closed. The actuator changes; the decision does not.
- The two **controller-side rules from requirements 12.4** that the device
  cannot self-report: *moving means moving*, and *a stop that arrives too early
  is a fault, not a success*. These must exist **before** position is trusted for
  control, not alongside the algorithm that consumes it. **Their criteria are
  specified in Phase 4 and are not repeated here** — that section stays the
  specification, this one is the tracker.
- **Operator surfaces, section 6**: the display rule (6.1), the web GUI (6.3)
  and the remote status payload (6.4) are **BUILT** (2026-09-13, mock-verified —
  see §6.3). What remains here is the **admin commissioning screen** alone,
  which needs the rig.
- **Alarm handling** — which Phase 4 deliberately deferred to Phase 5 ("detects
  and records; it does not act"). Under this sequencing it belongs to the M3
  slice, ahead of the control change.
- **Logging** — already built (SENSOR_HR ch3, ALARM ch6 244—248, both parsers,
  logparser.md 1.12), so this part is done and only needs extending for whatever
  the alarm handling adds.
- **Performance indicators for the bus and for the sensor** (operator,
  2026-09-13), built here **as the template for the other Modbus actors** — see
  immediately below.
- **The operator manuals**, which state the opposite of what this slice builds
  — see immediately below. They are a deliverable of the slice, not an
  afterthought to it.

##### The manuals are a dependency of this slice

Both manuals rest, in seven places, on *the controller has no position
feedback*. Six of them stay **correct until the ▲ GATE is crossed** and must
not be edited before then: nothing consumes position today, so an edit now
would claim feedback the controller does not use — a worse error than the one
it fixes. They come due **in this slice**, with the control change, and the
sharpest is `beheerderHandleiding:1471`, which calls the CLOSE_ALL calibration
*the only way* to re-align the internal assumption with physical reality. A
flap-mounted encoder is a second way, and a better one.

| file | line | claim | when it breaks |
|---|---|---|---|
| `beheerderHandleiding` | 101 | "geen positie-feedback van motoren … werkt op tijd-gestuurde commando's" | gate crossed |
| `beheerderHandleiding` | 1470 | tracks positions internally from its own commands | gate crossed |
| `beheerderHandleiding` | **1471** | CLOSE_ALL is **the only way** to re-align the assumption | gate crossed — **flatly false** then |
| `beheerderHandleiding` | 1503 | the power-cycle recovery procedure that rests on 1471 | gate crossed |
| `boerHandleiding` | 1196, 1216 | same power-cycle advice, farmer wording | gate crossed |
| `boerHandleiding` | **1187** | positions shown on LCD **and in the web interface** are the controller's assumption | **already**, see below |

**1187 was already inconsistent and is FIXED (2026-09-13).** §6.3 shipped a
*measured* percentage for M3 in the web interface that same morning, into the
same manual whose §8 now documents it. The correction is worth more than its
own accuracy: §15 is the manual-takeover section, and a hand-crank at the
RRK-3 is precisely the case a flap-mounted encoder **does** see, because it
measures the leaf and not the commands. **M3 with a sensor is the one window
where a manual move is visible** — and on the web only; the LCD still shows
the assumption, which is why §6.2 left it alone.

That asymmetry is a **finding about the sensor's value**, not merely a
documentation fix: the failure mode §15 exists to warn about is one the sensor
partially detects. Worth carrying into whatever alarm handling this slice adds.

##### Performance indicators — build them in this slice, as the template

**Operator decision, 2026-09-13.** The M3 slice adds performance indicators for
**the bus** and for **the sensor**, and that implementation is the **template
the other Modbus actors follow** — the FG6485A at addr 1, the S200 at addr 44,
and anything added later.

**Half of it already exists**, which is exactly why this is the right place to
finish it. Both were built on 2026-09-12 while diagnosing the wind alarm:

| what | where | holds |
|---|---|---|
| bus-wide | `modbus_counters_t` | `ok` / `timeout` / `crc` / `exception` / `framing` / `param` / `busy`, the last status **and the last FAILING status** kept apart, `to_received` vs `to_expected` for the last timeout, and the last bus-lock wait |
| the sensor | `windowpos_counters_t` | `reads_ok`, `err_busy`, `err_comm`, `rejected_rate`, `strokes`, `probe_fail`, `mode_changes`, `gated_polls` |

**What is missing, and it is the part that makes them a template:**

1. **Per-slave, not bus-wide.** `modbus_counters_t` totals every transaction
   together. It was `last_fail_addr` alone that made the 2026-09-13 diagnosis
   possible — addr 40 clean while addr 1 and addr 44 failed. **Key the counters
   by slave address inside the driver** and every actor gets its own indicators
   for free, with no per-task code at all. That is the template insight: it is
   not a struct to copy into each task, it is one table in the driver that each
   task reads its own row from.
2. **Production-visible.** Both surfaces are behind `#ifdef MODBUS_BENCH`, so a
   release build — i.e. **every unit in service** — exposes nothing. 5C88 is
   behind NAT, so `/api/status` plus a **sparse** `LOG_SYSTEM` row is the only
   path that reaches anyone off-site. Sparse matters: this must not become
   another `IDLE_LOG_MS` volume problem.
3. **A rolling rate, not only since-boot totals.** 5C88 has run 43 days; a
   cumulative counter says nothing about trend on that timescale.
4. **`consecutive_fail_max`** — the one number that actually predicts a fault,
   since T5 faults on consecutive failures rather than on a rate.

###### Where they land (decided 2026-09-13)

**Today they land nowhere.** `diag_windowpos_get_handler` is inside
`#ifdef MODBUS_BENCH` (`web_server.cpp:3148—3424`), so on a release build — every
unit in service, 5C88 included — the endpoint does not exist. On the bench they
are read on demand and cumulative since boot, so a reboot erases them. During
AT-WP05 the only thing preserving them is an operator curling them into a CSV
off-device. That is not a surface.

**Two destinations, for different readers.**

| | reader | why that one |
|---|---|---|
| `/api/status` | GUI + remote dashboard | the live view, and the only thing that can be trended off-device |
| a periodic `LOG_SYSTEM` row | whoever reads the SD/ROTA trail | 5C88 is behind NAT, so this is the **only** channel that reaches anyone off-site |

**The log row encoding.** A row is 12 bytes and `LOG_SYSTEM` spends `value_a` on
the subtype, which leaves `channel`, `param_id` and one `int16`. Use them like
this:

| field | carries |
|---|---|
| `value_a` | the new subtype — **derive it from the emitters, not from the header comment** (gh#59 was filed claiming 22 was free when 22/23/24 had been ROTA since 2.2.0) |
| `channel` | **the slave address** (1 / 40 / 44 — all fit a `uint8`, and `channel` is already documented as "0 for non-motor events") |
| `param_id` | **which KPI** — ok-count, error-count, later `consecutive_fail_max` |
| `value_b` | the value, as a **delta for the interval, never cumulative** |

Delta rather than cumulative is not a detail: a cumulative counter resets at
reboot and the series then reads as a cliff, and a delta *is* the rate the
DEGRADED level needs.

**This encoding is what makes it a template.** Any Modbus actor gets rows simply
by having an address, and a new KPI is a new `param_id` — not a new subtype,
which is the scarce resource here.

**Cadence: hourly, and unconditional.** Emitting only when something went wrong
gives errors with no denominator, and a rate cannot be computed from that.
`0 errors in 1080 transactions` is the datum that establishes 5C88's baseline,
which is the entire reason the DEGRADED threshold is still unset. Cost is
3 slaves x 2 metrics x 24 = **144 rows/day**, under **2 %** of current log
volume — set against T17's idle logging at ~2880 rows/day, so this is not the
`IDLE_LOG_MS` problem in miniature. Hourly counts run to the hundreds
(T5 ~360/h, T17 ~120/h at rest), comfortably inside `int16`.

**GUI: two separate things that must not be merged.**

- **the numbers** on the System tab, admin-facing. A farmer does not act on a
  0.09 % bus error rate.
- **a DEGRADED badge** on the Alarms tile when the threshold is crossed. This is
  the farmer-visible half, and it is precisely the missing middle level: today
  the bus goes from *no signal at all* to *closing the greenhouse*, with two
  failed attempts 100 ms apart as the only step between.

**One consequence to accept before building it.** `/api/status` is shared by the
local GUI and the remote POST through `build_canonical_status_json()`, differing
only by expose mask. Putting the KPIs there means deciding they go off-site —
which for 5C88 is the whole point, but it makes this a payload-shape change for
the dashboard too: **minor** bump, and the fields must be **absent** rather than
zero on older firmware, exactly as §6.3 handled `M3_percent_x10`.

`logparser.py` **and** `logparser.md` learn the encoding in the same changeset
(CLAUDE.md rule).

**Why this slice and not later.** The indicators are how the M3 implementation
is shown to work at all: the presence gate, the promotion/demotion asymmetry and
the 12.4 controller-side rules each produce a counter, and without them "it
behaved correctly overnight" is an assertion. The 2026-09-12 investigation is
the worked example — three wrong root causes were proposed before instrumenting,
and one reading (`to_received = 0 of 29`, `crc = 0`, `busy = 0`) ended it.

###### The bus will never be flawless, and it is not supposed to be

**Operator, 2026-09-13.** A faultless bus is *preferable*, not *required*. The
bus **shall be robust to some level of errors** and shall fault only when that
level becomes too much. What must always be possible is **observing performance
and degradation before failure**. During **development** the target stays
**faultless** — a deliberate asymmetry, tight in the lab and tolerant in
service.

This corrects the criterion used for the 2026-09-12/13 soak, which was set at
**zero** bus errors and therefore reported 9 errors in 10,286 transactions as a
failure. By the standard above that soak was a **pass with a degradation figure
attached**: 0.087 %, against 0.83 % before the inter-frame-gap fix, and **zero
sensor faults and zero wind overrides** across 13 h and 9 vent cycles.

**Two levels, and today there is only one.**

| level | trigger | consequence |
|---|---|---|
| **DEGRADED** | an error *rate* over a window | observable and logged. **No control change.** |
| **FAULT** | consecutive failures, as now | `EG1_BIT_SENSOR_FAULT_*`, T3 safe-fails, the greenhouse closes |

Today only the second exists, and it is reached from two failed attempts 100 ms
apart — so the bus goes from *no signal at all* to *closing the greenhouse* with
nothing in between. The indicators above are what make the first level possible:
a rate and a trend, not an event.

**There is already one instance of the shape**, added 2026-09-12:
`SP_BUSY_TOLERANCE_MS` lets T5 tolerate a busy bus for 60 s before faulting —
tolerate, bounded, then fault. Generalising that to the other error classes is
what this asks for.

**On "should there be a minimum threshold?"** — yes, but **the number should be
measured, not invented**, and that is the reason for building the indicators
first:

- The threshold is a **departure from a known baseline**, not an absolute. The
  dev rig now sits at 0.087 %; 5C88's baseline is unknown and may legitimately
  differ (different cable run, different noise environment, no encoder on the
  bus).
- So: instrument, establish a per-installation baseline over a meaningful
  period, then set DEGRADED at a multiple of it. A number chosen today would be
  a guess dressed as a specification.
- **Per slave, not per bus** — for the same reason the counters are keyed by
  address. One degrading sensor should not be hidden by two healthy ones.
- **The FAULT threshold is a separate decision and a safety one.** Loosening it
  slows detection of a genuinely dead wind sensor, and that path protects the
  structure (FR-WP18 keeps the safety paths time-based for the same reason).
  DEGRADED may be tuned freely; FAULT may not.

**Tracked separately as gh#66**, which carries the T5 half: the residual ~0.087 %
non-response rate on addr 1 and addr 44, observed on the dev rig and reported on
production. The template built here is what that issue consumes.

**How much of gh#66 this feature actually closes** — close to all of it, but not
all, and the remainder is the safety-shaped part:

| gh#66 | this feature | |
|---|---|---|
| **Part 2** — per-sensor indicators | **closes it outright** | it *is* Part 2 |
| **Part 1**, question 2 of the operator's reframing: *is it observable before it becomes a failure?* | **closes it** | today the answer is no; this is what makes it yes |
| **Part 1**, question 1: *is the rate within tolerance?* | **enables, does not answer** | a tolerance needs a **baseline**, and a baseline needs the indicators shipped **plus run time**. 5C88 cannot be characterised at all today |
| **Part 1**, investigation step 2 — the control experiment | **already running** as AT-WP05 arm A | and its result decides whether Part 1 needs any code at all: if T5's rate holds at ~0.1 % with T17 off the bus, the residual is the sensors' own baseline and there is nothing to fix; if it drops to zero, T17's mere presence still matters and `MODBUS_IFG_US` needs raising past 20 ms |
| **Part 1**, question 3: *is the FAULT threshold appropriate?* | **does NOT close it** | adding DEGRADED is instrumentation and is in scope. **Moving FAULT is a control change and a safety decision** — a slower fault means slower detection of a genuinely dead wind sensor, and that path protects the structure. gh#66 fences this off deliberately and so does this plan |

So: build this, and gh#66 reduces to **one measurement that needs elapsed time**
and **one safety decision that needs an explicit call**. Neither is a coding
task. The issue should not be closed when this ships — it should be updated to
say so.

**Why this order is right**, beyond it being the instruction:

- **Attribution.** Change the actuator and the decision logic together and no
  observed behaviour change is attributable to either. The stepped logic is the
  known-good reference; keeping it fixed makes the actuator the only variable.
- **It is Phase 4's own principle extended.** Phase 4's stated minimal end result
  is a movement trace, *"what earns trust in the implementation before anything
  acts on it"*. The same argument applies one level up: earn trust in
  position-as-actuator before rebuilding the thing that commands it.
- **The operator surfaces are what make the sensor trustworthy**, not decoration.
  A position the farmer cannot see and an alarm nobody is told about is not a
  working implementation, it is an instrumented one.
- **It de-risks the part that cannot be validated on the rig.** Section 5 already
  notes the rig can prove the controller reaches intermediate apertures, but only
  5C88 over a summer can prove that doing so damps the limit cycle. Finishing M3
  first means the unprovable part is the *only* thing outstanding when it starts.

**The LCD stays as section 6.2 has it** (operator, 2026-09-13, confirming the
2026-09-07 decision): **no change, and LCD control uses full open/close, not a
percentage.** So the linear setpoint surface is the web GUI and the central
algorithm only; the LCD remains a binary commander, and the `> 0 mm` counts as
OPEN display rule stands. This matters beyond the display: **the LCD is an
operator-manual command source** (`SRC_OPERATOR_MANUAL`, which bypasses the
dwell timers), so the actuator API must accept a plain open/close from it and
reach the end sensor, with no percentage anywhere in that path. No
`boerHandleiding` change follows.

#### 5a. Target architecture — operator brief, 2026-09-12

Recorded from the operator, and it reframes Phase 5: the goal is not "M3 gains a
position input", it is **a per-window actuator abstraction with declared
capability**.

**The shape:**

1. A **central algorithm** holds per-window state and issues a setpoint per
   window: `open`, `close`, or `0—100 %`.
2. Each window has a **delegated actuator process** that applies the setpoint and
   **reports back: done, or failed**. T17 is M3's; M1 and M2 get siblings.
3. Each window **declares its own capability**: **linear** while a position
   sensor is fitted and responsive, **digital (open/closed, timed)** when the
   sensor is faulty or absent. Absent a sensor, every window is digital — which
   is exactly what `main` ships today.
4. The central algorithm is **informed of that capability per window** and plans
   against it.
5. **Dwell timers stay** for timed full open/close. Linear control gets its **own
   dwell, configurable, default 0 (off)**.

**The current stepped logic** — one window per degree over threshold — is to be
replaced by something dynamic (PID or fuzzy). That decision belongs to the
central algorithm; everything below constrains what the actuator layer must
offer it.

##### What already exists, and what does not

**The capability contract is already prototyped.** Phase 4's
`windowpos_task_ctrl_mode()` returns exactly this: a mode (POSITION / TIMED) plus
a `windowpos_gate_reason_t` saying why. It generalises to per-window unchanged,
with M1/M2 siblings always reporting digital. Its asymmetry is the right
semantic and should survive into the final shape: **demotion immediate,
promotion only at a stroke boundary**, so no consumer sees a window gain linear
authority underneath a movement already committed to the timer.

**The feedback half does not exist in any form.** `Q1` is documented as
*consumer-only* — commands in, no return path — so "applies it and feeds back
when done or fails" is new construction, not an extension. Worth stating the
upside plainly: building it **retires a known defect class** rather than merely
adding a feature. gh#51's symptom was T6 being silently refused by a dwell timer
at `ESP_LOGD`, invisible for up to 25 minutes on M3. That is precisely a missing
completion/failure channel, and the promoted "a failure that only reaches the
serial console has not been logged" pattern is the same gap seen from the log
side.

##### Four constraints that are already measured

**1. The endpoints are the BEST-known points in the range, not the worst — they
have their own sensor.** *(Corrected 2026-09-13. The first draft of this section
claimed the opposite and recommended an "interior-only" linear range. That was
wrong, and the correction changes the design for the better.)*

Section 2a.1 is normative: the **opening range is lower end sensor to higher end
switch, and that IS 0 % .. 100 %**. The endpoints are not inferred from the
analogue value at all — they are reported directly by **status bit 3**
(`WINDOWPOS_ST_END_SENSOR`), and they *define* the scale. It is the **interior**
that depends on the analogue reading and on `40004`/`40005`/`40006` being
calibrated correctly.

The `40004` clamp is real but answers a different question. It means **position
must not be used to detect an endpoint** — clamped, "at the end sensor" and
"overtravelled into the overlap" read identically — which is precisely why bit 3
exists. Two distinct mechanisms, easily conflated: the **end sensors** mark the
fully closed / fully open *window* and are visible over Modbus; the **end switch** sits outside them, stops the leaf in the overlap, and is invisible
to the controller (2a.1).

**Consequence for the actuator API, and it is an improvement on today:**

- A setpoint of `0` or `100` **terminates on bit 3**, with the travel timer as a
  **ceiling**, not the other way round. Today's logic is timer-only, and 2a.3
  records the cost: *every full stroke ends with the motor stalled against a
  mechanical limit for whatever time is left*. De-energising at the end sensor ends
  that stalling — a win that has nothing to do with proportional control and
  could land ahead of it.
- **"Timer expired without bit 3" is a reportable failure**, not a silent
  completion. That is the first real customer for the *failed* half of the
  done/failed channel: FR-E16 is an acknowledged blind spot — an **open
  end-sensor cable means bit 3 never sets at all**, and the contract's own
  cross-check for it is "commanded closed + position 0 + rate 0 + bit 3 never
  set" (contract 5.4).
- **Bit 4 (`WINDOWPOS_ST_BOTH_ENDS`) voids bit 3.** Both switches active at once
  is an **end sensor loop fault and an alarm** (FR-E16); position keeps tracking
  unchanged, so the actuator must fall back to the timer and report degraded
  rather than trust the endpoint signal.
- **Bit 6 stays inert closing** on this rig (no closed-end ADC headroom), so a
  shorted wiper reads as a plausible closed window. That is an argument for
  requirements 12.4 rule 1 — "moving means moving" — not against commanding 0 %.

So the linear interface **can be a uniform 0—100**, with the endpoints served by
a better signal than the middle rather than a worse one.

**2. Capability loss mid-move is the sharpest unresolved case.** A window
commanded to 60 % that loses its sensor at 30 % has unknown remaining travel. The
contract must define the outcome — likely a timed full open or close plus a
**failed, position unknown** report — because **FR-WP18 keeps every safety path
time-based regardless**: wind override, motor-alarm handling and boot CLOSE_ALL
never gate on position.

**3. Dwell default 0 for linear is right, but the anti-thrash problem changes
form rather than disappearing.** Under stepped control it was aperture
oscillation; under linear control it becomes **motor duty and mechanical wear**.
The natural mechanism for that is a **minimum-move deadband** — do not energise
for a 2 % correction — which is magnitude-based, not time-based. Keep both
knobs, and be explicit that dwell answers "how often may this window move" while
deadband answers "is this correction worth moving for".

**4. Safety stays centralised even though positioning is delegated.** T2/T3 keep
the motor alarm, the wind override and CLOSE_ALL; a delegated actuator must not
acquire its own safety path. Delegation is about *positioning*, not authority.

##### Two prerequisites, not nice-to-haves

- **gh#64's descriptor refactor comes first.** Per-window capability and
  per-window config keys land straight in the six-table drift problem (gh#57,
  gh#64). Adding a per-window key set to six hand-maintained tables is how the
  next silent gap ships.
- **The `step_width` arithmetic must be reworked before the step table is
  touched** (see the bullet above): integer division that gets *worse* with more
  steps is a landmine directly under "express partial apertures".

##### Note on the present intermediate state

T17 currently polls while **any** channel is travelling, not only M3 —
`any_channel_travelling()`. **This is deliberate scaffolding** (operator,
2026-09-12): it generates bus activity for testing, measurement and debugging
while the final shape is undecided, and it is what made the 2026-09-12 soak a
real load test rather than an idle one. It is **not** the target behaviour: the
cadence derives from `travel_m3`, the log row is hardcoded to
`LOG_PARAM_WINDOW_M3`, and `t2_get_window_states()` already documents index 2 as
M3, so narrowing it is a one-line change whenever the scaffolding is no longer
wanted. Recorded here because it reads as a defect on inspection — it was
diagnosed as one before the operator corrected it.

---

#### 5b. Decided 2026-09-17 — two control modes, the position path, and the 2.10.0 / 2.11.0 split

Recorded from the operator on 2026-09-17, after the gh#72 work and a review of the summer
campaign. It concretises §5a and **narrows gate 1 for 2.10.0**.

**Two control modes, operator-visible.**

| Mode | M1, M2 | M3 | Fallback |
|---|---|---|---|
| **1** (what `main` ships) | timed, 3 steps | timed, binary | — |
| **2** | timed, **2 fixed steps** | **linear**, from the wire sensor | to mode 1 when the sensor fails |

M3 therefore has two actuator modes — **binary** (timed full open/close) and **linear** (a target
opening). M1 and M2 stay binary permanently: no sensor is planned for them, so their sibling
actuators declare `digital` for good (§5a item 3).

**Mode 1's "one degree per step" is arithmetic, not a constant.** `step_width =
max(hyst_t / 3, 1)`, so it is 1 °C for `hyst_t` up to 5 and 2 °C from 6 — which is why F8 found
only three distinct behaviours across the whole legal range. Mode 2's mapping must not inherit
that integer division (see the rework note under Phase 5).

**What each release contains.**

| Release | Content |
|---|---|
| **2.10.0** | **Confirm only.** T2 keeps its timed drives and **drives on to the timer**. The sensor confirms each drive's end (done or failed) and checks `travel_m3` against the measured traverse (§3.5). On a sensor fault mid-drive the drive finishes on the timer and the fault is reported; position control stays off until a clean stroke is seen. **This supersedes gate 1's "de-energise at target, stop on bit 3" for 2.10.0.** |
| **2.11.0** | **Mode 2.** T2 gains the target input, T6 the graded law and the fallback. Designed while 2.10.0 soaks. |

##### The position path: one owner, one copy

The operator's principle is a **single source of truth**, with no information outside the planned
paths. Decided: **T4 exposes a pass-through accessor** that calls T17 and returns the reading with
its age. The call graph is T2/T6 → T4 → T17, and **nothing is buffered**.

**Why T4 must not hold its own copy of the position.** T4's loop waits on Q6 with a 1 s timeout, so
a T4-held reading is up to ~1 s old *on top of* T17's own sample age — and for positioning that age
is overshoot (age × leaf speed):

| | From T17's own sample | With a buffered T4 hop |
|---|---|---|
| Production — 176 s traverse, 1.17 s poll | ≤ 0.67 % of stroke | ≤ 1.25 % |
| Rig — 13 s traverse, 0.1 s poll | ≤ 0.9 % | ≤ 9 % |

FR-WP04/FR-WP05 ask for 1 % resolution and ±1 % repeatability, so the hop alone spends the budget
in production and exceeds it tenfold on the rig. The second copy is also this codebase's own
recurring defect: gh#51 and gh#52 were both a cached duplicate of a value another task owned, and
T2's private travel/dwell copy still needs `T2_NOTIFY_CFG_CHANGED` to stay honest.

**What does travel through T4, because T4 owns it:** the desired mode, `wpos_fitted_m3`, travel and
dwell, the deadband, and the fault state the surfaces display.

##### Mode selection is two variables

- **Desired mode** — a config key (T4, NVS, GUI and LCD), default mode 1.
- **Effective mode** = desired mode 2 **and** M3's linear capability from T17 (fitted, gate open,
  no active fault). Computed in one place.
- **Every change logs with its own `param_id`.** `LOG_MODE_CHANGE` already has two emitters (T6's
  vent step, and STANDBY as param 47) and all three consumers decode only the first — gh#54. A
  third emitter needs its own id and a parser branch in the same change.
- **Anti-flap:** demotion immediate, promotion only at a stroke boundary (§4a), plus a hold-down
  before mode 2 resumes. The soak's `mode_changes <= 2` criterion exists because gate flapping was
  real.
- **The fallback leaves M3 at an end.** Mode 1 has no partial state, so a drop-out finishes the
  drive on the timer and M3 is then driven to an end; until it arrives, its state is not "open".

##### What the target costs in T2

- **Q1 carries no target today.** Add a target field and a **separate action** rather than
  overloading `CMD_OPEN`, so an old-style open can never read as "target 0 %". Five tasks post to
  Q1 — enumerate them in the release notes, per the standing rule for a shared queue.
- **A new terminal state.** Only CLOSED and OPEN are terminal and persisted, and the boot shortcut
  needs all three CLOSED, so a partial M3 always forces a boot recalibration. `SENSOR_HR ch2`
  already uses all four 2-bit codes per channel, so a fifth state needs a new encoding plus
  `logparser.py` and `plot_daily.py` in the same change.
- **Stop rule:** within the deadband of the target, travel timer as the ceiling. For the 0 % and
  100 % targets 2.10.0's "drive on to the timer" still applies; a partial target must stop
  mid-travel, so that rule cannot be universal in mode 2.
- **Minimum move, and a minimum interval.** §3.6's floor 2 (the shortest pulse that actually moves
  the leaf) is still unmeasured, and the deadband default is a fixed 20 mm rather than derived.
  Measure floor 2 on the rig before linear control issues small moves, and give M3 a minimum
  interval between moves (§5a's linear dwell). Continuous control otherwise replaces a 25-minute
  dwell with motor chatter — count motor starts per hour in the soak.
- **Safety unchanged (FR-WP18).** The wind close-all, the motor alarm and the boot sweep ignore
  position and may interrupt a positioning move at any point.

##### What T6 needs

- **A mapping from demand to (M1/M2 step, M3 %)** — the control law itself, and §10's open
  decision 9. A proportional map with a rate limit is the simplest candidate and is replayable
  offline; a PID's integral term is the risk against an actuator that takes 176 s and reports late.
  **The law sits behind the model contract in §5c**, so choosing it is not a prerequisite for
  building mode 2 — and replacing it later costs one file.
- **Achieved, not demanded.** T6 remembers the step it *asked for*; with a linear M3 that
  difference becomes visible, so it needs the done/failed result and the achieved position back —
  the feedback half §5a records as missing.
- **Wind direction.** The campaign measures M3's effect varying 30-100x by direction, and the
  **2026-07-20 limit cycle ran under north (windward) wind**: 7 of its 8 M3 openings at 321-354°,
  about 3 m/s (log analysis, 2026-09-17). Across the campaign, of 370 M3 openings **151 were north
  (315-45°), 91 south-west (200-290°) and 128 in sectors the campaign never characterised**. One
  fixed demand-to-aperture curve will therefore be wrong in one regime: keep the curve's parameters
  in config, log the computed target next to the demand, and treat direction gating as a later step
  (NS-9).

##### Prerequisites before mode 2 may be trusted

- **[gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78)** — rule 2's two flaws. The
  detectors are the guard against a position that lies, so they come first.
- **T17's missing fallbacks:** bit 4 (both end sensors, a wiring fault) does not shut the gate, and
  the device's start-up bits are never read.
- **AT-WP02** (ten moves to one target, spread within ±1 %) and **AT-WP03** (endpoints) — runnable
  only once T2 can hold a partial target.
- **5C88:** the sensor bought, fitted and taught
  ([gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77)). Until then
  `wpos_fitted_m3` = 0 keeps mode 2 unavailable there, which is the right default.

#### 5c. The T6 model boundary — a contract, so the control law can be replaced later

**Operator requirement, 2026-09-17: it shall be possible to adapt mode 2's model later in a simple
way.** The reason is in the campaign: nobody can yet say what the right law is. There is no
measured aperture-to-airflow curve for a part-open M3, M3's effect varies 30-100x with wind
direction, the plant model misses its accuracy targets and the closed-loop simulation has never
been run. The first real evidence arrives over a production summer, so the law **will** be changed
after it ships, probably more than once. That makes the boundary around it a requirement of its
own, not a refinement.

**The shape: the law is a pure function behind one interface, and T6 keeps everything else.**
Mode 1's existing stepped logic and mode 2's graded law become **two implementations of the same
interface**, so a third (PID, fuzzy, a direction-aware curve) is a new file plus one table row —
never a change to T6's plumbing, to Q1, or to the log format.

##### The interface lives in its own document

**[`ventModelContract.md`](ventModelContract.md) is the normative interface**, written to be read on
its own by whoever develops or tunes a model — a separate session, a separate agent or a person. It
carries the header (`vent_in_t`, `vent_out_t`, `vent_state_t`, `vent_model_t`), the units, the call
contract, the file layout, the test and replay requirements, the campaign evidence and the
mechanical constraints. Kept there rather than here so there is one copy to maintain.

In summary, the model receives: monotonic time and day/night; temperature, humidity and wind, raw
and averaged, each with a validity flag; the day/night-resolved setpoints and tuning; and per window
its actuator state, its capability (linear or digital), M3's aperture with its age, the last target
and how it ended, and the time since its last drive. It returns, per window, `HOLD`, `CLOSE`, `OPEN`
or `TARGET` with an aperture in 0.1 %, plus a reason code and its own demand values for the log.

##### The rules that make it swappable

| Rule | Why |
|---|---|
| **The model is a pure function**: const input, its own state struct, an output struct. No queue, no NVS, no logging, no blocking, no clock of its own. | It can be replayed, unit-tested on the host and diffed against the law it replaces. |
| **Its state lives in the caller.** T6 owns `vent_state_t` and resets it at boot, on a mode change and on an inhibit onset. | An integrator that survives a wind override would wind up invisibly; and a caller-owned state is snapshot-able for a replay. |
| **No ESP-IDF headers, integer units only.** | Same source in the firmware, the host tests and the replay — the guard against "it behaved differently offline". |
| **T6 resolves the day/night setpoints, the averaging and the validity flags** before the call. | The model never learns where a value came from, so the sensor layer can change under it. |
| **T6 enforces the actuator limits after the call**: a `TARGET` for a digital window is a model error (logged, treated as `HOLD`); a target inside `m3_deadzone_x10` of the current position is dropped; one inside `m3_min_move_ms` of the last move is deferred; targets clamp to 0..1000. | The chattering and the impossible command are caught in one place, so every future model inherits the protection instead of reimplementing it. |
| **T6 owns Q1 and the log row**, including the source tag and the model's `reason`, `demand_*` and the resulting target. | The SD log stays the record of *why* a window moved, which is what makes a law argued about after the fact — the standing rule that a decision changing behaviour leaves a row. |
| **Safety is outside the boundary.** The wind close-all, the motor alarm, standby and the boot sweep are handled by T3 and T2; while any inhibit is set T6 does not call the model at all, and its last output is discarded. | FR-WP18, and a new model can never regress a safety path it cannot reach. |
| **The active model's name and version are published and logged** at boot and on every mode change. | A log can be attributed to the law that produced it, which is the minimum for comparing two summers. |

##### Two implementations at the boundary from day one

| Model | Mode | Output shape |
|---|---|---|
| `stepped` | 1 | M1, M2, M3 each `OPEN`/`CLOSE`/`HOLD` — today's 3-step table, unchanged in behaviour |
| `graded` | 2 | M1, M2 `OPEN`/`CLOSE`/`HOLD` at two fixed steps; M3 `TARGET` with a percentage (or `OPEN`/`CLOSE` while M3's capability is digital) |

**Acceptance for the refactor itself:** `stepped`, compiled behind this interface, must reproduce
the logged decisions of real SD data at least as well as today's replay does (96.8 % of 378
decisions — [`campaignResults_summer2026.md`](../model/campaignResults_summer2026.md) F8). That is the fail-first check that the boundary changed
nothing before the graded law is allowed anywhere near the greenhouse.

**Replay is part of the deliverable, not a follow-up.** `model/vent_step_replay.py` already
reconstructs T6's inputs from SD rows and refuses to project unless it first reproduces the logged
demands. Mode 2 needs the same treatment with the model compiled in — a host harness that feeds
`vent_in_t` rows and records `vent_out_t` — so a candidate law can be scored against real weather
before it is shipped.

## 6. Operator-facing surfaces

### 6.1 The display rule

Position replaces the binary state on the rich surfaces, but the **end sensor stays the authority for the terminal states** (operator decision 2026-09-07):

| Condition | Shown |
|---|---|
| Travelling | **OPENING** / **CLOSING** — unchanged |
| At a stop (bit 3) with position ≈ 0 | **CLOSED** |
| At a stop (bit 3) with position ≈ `40004` | **OPEN** |
| Anything else | **the opening as a percentage** |

> **The percentage scale must not clamp at 0/100 — see §2a.5.** The leaf rests at ~113.7 % at the open end and below 0 % at the closed end, because the end sensors mark the *window* extremes while the motor drives on into the blind overlap. Displaying a correctly parked window as "100 %" by clamping hides the overtravel that proves it reached its limit; displaying it as a fault is worse. Render the true value, and reserve fault styling for `65535` and the status bits.

Using bit 3 rather than "position == 0" for the terminal states is the right call: it is a physical witness rather than an inference, and it keeps the display honest about a window that is nearly-but-not-quite shut.

> **One consequence to expect, and it is not a bug.** Contract §5.3: bit 3 reports the *sensor*, not the window. Where the sensor zone is shorter than the overtravel, the leaf comes to rest past the sensor and bit 3 **clears** while the window is fully closed — so the display shows `0 %` rather than `CLOSED`. Phase 0's sensor-zone check tells us which regime this installation is in; if it is the short-zone case, that is the display telling the truth about what it can actually prove.

### 6.2 LCD (T8) — no change

> **Reconfirmed 2026-09-13** when the Phase 5 sequencing was set: no change, and
> **LCD control uses full open/close, not a percentage**. The percentage surface
> is the web GUI (6.3) and the remote status payload (6.4). See section 5.0.

16×2 leaves nothing spare (`M1:C M2:C M3:45%` is exactly 16 characters), and the diagnostic value of a percentage is better carried by an event than by a number the farmer must interpret.

**Rule where a binary state is still needed: `> 0 mm` open counts as OPEN** (operator decision 2026-09-07). No `boerHandleiding` change follows from this phase.

### 6.3 Web GUI

Show the opening percentage per §6.1, with the sensor fault surfaced alongside the existing T/RH and wind faults (FR-WP19). Farmer-visible, so `boerHandleiding` syncs in the same changeset.

**Also hosts commissioning** (admin-only): set the window size, teach the sensor, and read the calibration verdict.

> **2.9.0 (gh#73): *Linear control* starts with *Position sensor fitted*.** While it is No, everything below it (deadzone and commissioning) is greyed, and the reason sits above the greyed block rather than inside it. A dimmed parent dims its children whatever their own opacity says, so a reason inside the block cannot be shown at full strength. The commissioning card's own reason for a 404 now names the cause: on a release build, teaching needs a bench build; on a bench build (`fw_ver` ending in `-bench`), a route failed to register, which is a fault.

> **Superseded 2026-09-13→14.** An earlier draft of this item timed the traverse and asked the operator to accept the measured seconds. That was built on a wrong premise about what the teach is for. **The teach maps the sensor's raw ADC onto a KNOWN distance** — the gap between the two end sensors, written to the device's `40004` — so a completed teach is self-consistent *by construction* and there is nothing in it for an admin to ratify. The screen publishes a **machine verdict** instead, and a re-teach happens when that verdict says so rather than on a schedule. See §6.3a.

> **The commissioning surface stays inside `#ifdef MODBUS_BENCH` — accepted by the operator, 2026-09-14.** It sits beside the teach, which already lived there. The cost is explicit: commissioning a sensor on a production unit means flashing a build that also opens the arbitrary Modbus write route, so it is a deliberate, temporary state and the release build must be restored afterwards.

> **A teach pauses automatic control — operator decision, 2026-09-16.** Until then T6 stayed in charge during a teach and could move M3 between two legs, which ends the run with `m3_busy` at best.
>
> **How it works.** Once its refusals have passed, a teach **holds** STANDBY (`dm_standby_hold()`) until two things are true: the admin session that started it has ended, and no teach is running. A session ends by logout, idle timeout, or eviction from the four-slot session table. The release works like the LCD session end: the dwell debt is dropped and the windows recalibrate with a CLOSE_ALL.
>
> **One deliberate difference from the LCD model: a hold is never written to NVS.** The LCD's menu STANDBY was persisted, but the flag that lets its session end clear it was not, so a reboot stranded the unit in STANDBY (gh#65). A hold and its release both live in RAM, so a reboot ends both. **gh#65 was fixed the same day by moving the LCD menu onto the same hold** (`DM_STANDBY_HOLD_LCD`). The two can hold the pause together, and it ends only when both sessions have. Verified on hardware with `bin/at_lcd_standby.py` (fail-first against the build before the fix).
>
> **Other rules.**
> - An operator's own STANDBY is left alone.
> - An explicit mode choice during a hold wins: STANDBY makes the pause persistent, AUTOMATIC ends it.
> - The hold is logged as `MODE param 47` with `value_b` = 1.
> - The GUI shows the pause under the teach button for as long as it lasts.
>
> **Verified on FDA4** with `bin/at_wp_teach_standby.py`, on the committed build (a bench build of `260d1fe`, pushed and confirmed by a bank flip). All four cases passed:
> - **A (logout):** STANDBY was on from the start of the teach and still on after `done`. It cleared at logout, and a recalibration followed.
> - **B (timeout):** with a 60 s session timeout, STANDBY cleared 78 s after the session's last request. The extra time is T17's check, at most 30 s at rest.
> - **C (operator's own STANDBY):** it was not held and still on 45 s after logout.
> - **D (reboot):** with the teach's session left open, STANDBY was off after a reboot.
>
> The original `bin/at_wp_teach.py` passes too, with three teaches under one hold.
>
> **Fail-first.**
> - The build without the hold failed case A: STANDBY was never set.
> - A build with `DM_FAILFIRST_PERSIST_STANDBY_HOLD`, which persists the hold the way the LCD persists its STANDBY, failed case D: STANDBY came back after the reboot with nothing left to release it — the gh#65 shape.
>
> **That second check first PASSED, and so exposed a flaw in case D itself.** The test logged out after the upload that reboots the unit. The logout reached the unit before the reboot and released the hold, so the earlier "reboot" passes had tested nothing. Case D now abandons the session instead, and refuses to judge a hold that ended before the reboot. Both builds were then run again, with the results above.

**Status 2026-09-13 — the read-only half is BUILT, the commissioning screen is NOT.**

| | |
|---|---|
| **Built** | The snapshot carries the opening (`app_types.h`), `data_manager` fills it from the gate + T17, `build_canonical_status_json()` emits `M3_percent_x10` / `M3_mm_x10` / `M3_at_end_sensor`, the GUI applies the §6.1 rule, the `Window sensor fault` badge is surfaced, and the M3 tooltip explains the over-100 % rest position. `boerHandleiding` §2 and §8 synced in the same changeset |
| **Split out** | The **admin commissioning screen** — teach arm/abort, bit 5, and the §3.5 traverse measurement with explicit acceptance. It needs new admin routes **and the rig**, because it cannot be finished without moving M3. It belongs to the §5.0 M3 slice |
| **Verified** | Against `webUiMock` only (a `/api/__mock/m3` backdoor drives the opening, so the display rule can be walked by hand). **Not yet seen on hardware** — the rig is committed to AT-WP05 |

Two deliberate choices worth not relitigating:

- **The keys are OMITTED, not zeroed**, when no sensor is fitted. A consumer must be able to tell *no sensor* from *fully closed*, and an older dashboard must be unaffected. It is a payload-shape change either way, so the next release is a **minor** bump.
- **No EG1 bit for the position fault** (operator, 2026-09-13). EG1 is what T3 reads, and FR-WP18 forbids any safety path depending on position — a bit there would invite exactly the coupling the requirement rules out. The flag rides the status payload instead, the way `standby` does.

### 6.4 Remote status site

Add the opening to the `windows` object in `build_canonical_status_json`, gated by `STATUS_EXPOSE_WINDOWS`. This is a **payload-shape change → minor version bump**, and the dashboard is a separate site that must tolerate the field being **absent** (not zero) on any unit without a sensor.

**Both operator questions answered yes (2026-09-07):** a "commanded but not moving" divergence is **farmer-visible**, and position faults **do** reach the remote status site — for 5C88 that is the only way anyone off-site would learn of it.

**The research payoff for doing this now rather than at Phase 5:** NS-9 — the wind-direction-dependent `ach_m3` model — is blocked on exactly this variable. Starting the remote record early means the history exists when someone picks it up.

---

## 7. Rollout, including production

Production is **not** waiting for Phase 5. In parallel with firmware development on the FDA4 mock, 5C88's real M3 is being fitted with the wire sensor, end sensors and Modbus interface (operator, 2026-09-07).

1. Develop and verify phases 0–4 on the FDA4 mock.
2. Soak on FDA4 until the traverse record is trusted.
3. 5C88's hardware installation completes independently.
4. Production receives the firmware and **starts logging**.

**Since 2.9.0 (gh#73) that last step needs one setting.** *Position sensor fitted* defaults to No, so a unit that takes the firmware by ROTA ignores address 40 until someone says otherwise. When 5C88's encoder is installed, set it to Yes on site (5C88 has no remote GUI path). The sensor also has to be taught, which needs a bench build.

So the "absent field on production" concern is time-boxed, not permanent — but the dashboard must still handle absent for the interim, and for any unit that never gets a sensor.

Note what production logging unlocks that the rig cannot: a **real** 171 s traverse on a 40 m hanging flap, which is the only place FR-WP22 (torsional lag across the span) and the rope-drum linearity question can actually be answered.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| A rate constant gets hardcoded to a production value | §3's derived rules, with the computed values logged at boot. **Largely retired by §3.5**: the rate is measured at commissioning rather than typed |
| A traverse that completes *normally but wrongly* (slight binding) is accepted | It is a manual GUI action, so it never happens unnoticed — but presence alone cannot judge a plausible-looking run. **The GUI displays the result for explicit operator acceptance** (§3.5), behind clean-traverse-only, the ±50 % band, and both values logged |
| Open and close traverse times differ materially | Expected — requirements §1.4 says the mechanism is asymmetric. Measured per direction; constants derive from the shorter. Acting on the asymmetry is Phase 5 |
| Bit 6 inert on this installation (no electrical headroom) | Phase 0 records it; if inert, a shorted wiper reads as *closed* and §12.4 rule 1 is the only defence |
| Position polling disturbs climate averaging | §4 keeps T5 untouched — that is the whole reason for a separate task |
| The rig's speed masks a slow-turnaround problem | AT-WP05's 24 h coexistence run; the contract's measured 4.08 ms response is the reference |
| Phase 5 lands on a thermal prediction rather than evidence | The gate, and the honest statement above that only a production summer settles it |

---

## 9. What this plan deliberately does not do

- **The `refactorSensorConfiguration.md` bus refactor.** This adds one well-behaved caller under the existing policy; it does not restructure ownership. That study's roles/JIT-queue end state remains the right destination and is untouched here.
- **Fit anything to 5C88.** All of phases 0–4 happen on the rig.

---

## 10. Open decisions

~~1. Is Phase 5 in scope for this cycle?~~ **Decided 2026-09-07: no.** Phases 0–4 stand alone and deliver position logging, mechanical fault detection and faster power-loss recovery without touching control.

2. **Operator-facing surfaces** — LCD, web GUI, remote status site — settled, see §6.
   **Reconfirmed 2026-09-13:** LCD unchanged, and **LCD control uses full
   open/close, not a percentage**.
3. ~~**Alarm handling** — deferred to Phase 5.~~ **Superseded 2026-09-13:** the
   §5.0 sequencing gate pulls alarm handling **into the M3 slice**, ahead of the
   control change.

**Decided since, and recorded where they belong:**

4. ~~What happens on sensor failure?~~ **Decided 2026-09-12** — two control laws,
   POSITION with a trusted sensor and TIMED without one, selected by the
   presence gate. See §4a.
5. ~~Does the control logic change with the M3 work?~~ **Decided 2026-09-13** —
   no: finish M3 end to end first. See §5.0.
6. ~~Must the bus be error-free?~~ **Decided 2026-09-13** — no. Robust to a level
   of errors, observable before failure, faultless only as a *development*
   target. See §5.0.

**Still open:**

7. **The DEGRADED threshold value** — deliberately unset until a per-installation
   baseline is measured (§5.0). The FAULT threshold is a separate, safety
   decision, and gh#66 carries both.
8. **The minimum-move deadband value** — floor 2 (the shortest pulse that actually
   moves the leaf) is unmeasured; see §3.6.
9. **PID or fuzzy** for the central algorithm, and how a mixed
   discrete/continuous plant is expressed to it. See §5a. **Narrowed 2026-09-17:** the plant is
   expressed as M1/M2 at two fixed steps plus a linear M3 (§5b), and a proportional map with a rate
   limit joins PID and fuzzy as a candidate. The law itself is still open.
10. **Two M3 config keys, specified but NOT yet created** (noted 2026-09-13 when
    the operator looked for the deadband setting and found none). They are
    correctly absent today, and the reasons are worth keeping because
    *correctly absent* and *forgotten* look identical from outside:

    | key | default | answers |
    |---|---|---|
    | **minimum-move deadband** | **derived** from `travel_m3`, and **never 0** (§3.6) | *is this correction worth moving for?* |
    | **linear dwell** | **0, off** (§5a, operator) | *how often may this window move?* |

    Easy to conflate, and their defaults point opposite ways: a **zero deadband
    is the chattering case**, whereas zero dwell is the intended starting point.

    **Why not now:**
    - **Nothing consumes them.** The deadband governs linear control, and
      nothing consumes position — T2 makes no `windowpos_*` calls and the
      ▲ GATE is uncrossed. A key today is a knob the operator can turn that
      changes nothing, which is this codebase's own promoted anti-pattern:
      *an affirmative success signal for something that did not happen*. gh#51
      was a green tick on a travel time T2 was ignoring; a deadband slider with
      no consumer is the same shape.
    - **gh#64 comes first**, as §5.0 already records. A new key must enter six
      hand-maintained tables (`cfg_clamp()`, `ns_key_to_log_id()`,
      `cfg_key_kind()`, `LIMITS_JSON`, `cfg_limits.h`, and the mock's
      `CONFIG_LIMITS`); the matrix already carries **51 keys with 17 declared
      gaps**.

    Sequence: **gh#64 refactor → linear control consumes position → then the
    keys**, all inside the §5.0 M3 slice. `bin/check_cfg_tables.py` is the gate
    that will catch a partial addition.

    > **Confirmed by the operator 2026-09-14: address gh#64 first, then add the
    > deadzone key.** So the deadzone control shipped in §6.3 item 4 is present
    > in the *Linear control* group but **disabled**, labelled "stored once
    > linear control reads it". That is deliberate and should not be read as an
    > unfinished edge: a settable value that nothing consumes is the knob that
    > lies, and the group heading carries the honesty instead of a disclaimer.
    > Wiring it is a clean follow-on once gh#64 lands — one key, six tables,
    > verified by `check_cfg_tables.py`.

    > **STALE, corrected 2026-09-17.** Both premises have moved. **gh#64 landed
    > 2026-09-14**, so a key is now *one row* in `firmware/config/cfg_desc.inc`,
    > checked by `bin/check_cfg_desc.py` — not six hand-maintained tables. And the
    > **deadband key now exists and is consumed**: `deadzone_m3_mm`
    > (`cfg_desc.inc:97`, default `DEF_DEADZONE_M3_MM` = 20 mm), read by T17 as
    > §12.4 rule 2's "~0" band and by rule 1's at-end exemption, with a live GUI
    > control. Two things from the row above still hold: its default is a **fixed
    > 20 mm rather than derived** from `travel_m3` as §3.6 requires, and **linear
    > dwell** — the minimum interval between M3 moves — is still uncreated. Both
    > belong to mode 2 (§5b).

**Decided 2026-09-17 — see §5b:**

11. ~~Two control modes, and what each does to which window?~~ **Mode 1** = today's
    timed stepping of all three; **mode 2** = M1/M2 at two fixed steps with a linear
    M3, falling back to mode 1 when the sensor fails. M3 has two actuator modes,
    binary and linear; M1 and M2 stay binary.
12. ~~How does the position reach T2 and T6?~~ Through a **T4 pass-through
    accessor**: one owner (T17), one copy, T4 in the call graph but never buffering
    the value. The measured reason is in §5b — a buffered hop costs the entire
    1 % overshoot budget in production and ten times it on the rig.
13. ~~What does 2.10.0 contain?~~ **Confirmation only**, with T2 still driving to the
    timer. Mode 2 is **2.11.0**, designed while 2.10.0 soaks.
14. ~~How is the control law kept replaceable?~~ **Operator requirement 2026-09-17:** mode 2's
    model shall be adaptable later in a simple way, so the law lives behind the **pure-function
    contract in §5c** — host-compilable, caller-owned state, T6 keeping the queue, the limits, the
    logging and the safety outside it. `stepped` (mode 1) and `graded` (mode 2) are the first two
    implementations; the refactor is accepted only when `stepped` reproduces today's replay match
    rate. **Which law `graded` uses is still open** (decision 9).
