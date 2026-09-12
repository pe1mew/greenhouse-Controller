# Integrating the M3 window position sensor — implementation plan

| Field | Value |
|---|---|
| Document | Implementation plan |
| Date | 2026-09-07 |
| Status | **UNBLOCKED 2026-09-10 — the rig is built and operational.** Wire sensor live on FDA4's Modbus bus, window emulator on M3's open/close relays, end contacts fitted. Both prerequisites shipped (gh#49 in 2.4.1, gh#51 in 2.4.2—2.4.4). Phase 0 tooling committed (`de92c27`) but **not flashed** — FDA4 runs the release build, which excludes `MODBUS_BENCH`. **Phase 0 is ready to run.** Phases 1—4 not started; Phase 5 out of scope |
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

> **Build status 2026-09-10: BUILT AND OPERATIONAL** (operator). The wire position sensor is live on FDA4's RS485 bus — a **third device** alongside FG6485A@1 and S200@44 — the window emulator is driven by M3's open/close relays, and end contacts are fitted. The specification above is now an as-built record.
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

```
  0 V |------------------ potmeter (wire sensor) range ------------------| 3.3 V
         |------------- physical range of the window -------------|
            [LOWER SW]---- opening range of the window ----[HIGHER SW]
                              (e.g. 1000 mm; 1500 mm on this rig)
```

From widest to narrowest:

1. **Potmeter range** — the wire sensor's full electrical travel, 0 V .. 3.3 V. **Wider than
   the window at BOTH ends by design**, so the window never runs the sensor into either rail.
2. **Physical range of the window** — what the mechanism can traverse. Ends at the motor's
   end protection.
3. **Opening range** — **lower end switch to higher end switch. This is 0 % .. 100 %.** It is
   what the operator means by "closed" and "open", and it is narrower than the physical range
   because **blinds overlap** at both ends.

**The end switches mark the fully closed / fully open WINDOW, not the motor limits.** The
motor limits sit outside them, and the leaf overtravels past each switch into the overlap
until the end protection stops it.

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
reads the end switches. The motor is stopped by its own **end protection**, so **every full
stroke ends with the motor stalled against a mechanical limit** for whatever time is left on
the timer.

That is today's design and it works. **The new logic must account for the end switches**
(FR-WP07, contract bit 3) instead of driving blind into the end protection.

### 2a.4 Measured on the rig, 2026-09-10 (Phase 0 teach)

| landmark | raw ADC | how observed |
|---|---|---|
| lower (closed) switch | **0** | bit 3 made; teach captured 0 -> 40005 |
| closed rest (motor limit) | **0** | live read at rest |
| higher (open) switch | **867** | bit 3 made; teach captured 867 -> 40006 |
| open rest (motor limit) | **986** | live read at rest, bit 3 **still set** |

**Open-end overtravel: 119 counts** — the blind overlap, exactly as drawn. Far too large to be
capture staleness (bounded at ~10 counts by 40002 = 100 ms and ~147 mm/s).

**Contract 5.3 sensor-zone check: PASS at the open end.** Bit 3 made at 867 and stayed made
through rest at 986, so "bit 3 set" genuinely means fully open. `OPEN` can be displayed
honestly.

### 2a.5 Consequences that constrain the design

1. **The DEVICE clamps position to `40004`; the overtravel is invisible.** *(Corrected
   2026-09-10 — an earlier draft of this section claimed readings beyond 100 % are normal and
   must not be clamped. Measured on the rig, that is wrong: the device clamps them itself.)*
   `30001` matches `raw × 40004 / (40006 - 40005)` exactly up to the open switch and then
   saturates:

   | raw | reads | |
   |---|---|---|
   | 619 | 1070.9 mm | matches the formula |
   | 775 | 1340.8 mm | matches |
   | 937 | **1500.0 mm** | predicted 1621.1 — **clamped** |
   | 986 | **1500.0 mm** | predicted 1705.9 — **clamped** |

   **Consequence for the new logic: position cannot distinguish "at the open switch" from
   "driven into the end protection".** Both read 100 % and both have bit 3 set. Anything that
   needs to know how far into the overlap the leaf sits must get it from elsewhere — or accept
   that it cannot. 0-100 % is therefore the correct display scale after all.

2. **The end switches are authoritative for 0 % and 100 %; the wire sensor interpolates.**
   This is a cleaner split than earlier drafts assumed, and it degrades well: a wiper fault
   falls back to *today's* behaviour (switch + timer), not to nothing.

3. **Stopping at the switch removes a stall that happens on every stroke today.** Cutting the
   relay when bit 3 makes is a mechanical improvement, not only a positioning one. It also
   means the **effective travel time becomes shorter than the configured `travel_ms`**, which
   today must cover switch-to-limit as well.

4. **`40004` is the switch-to-switch opening range**, never stop-to-stop (contract 6.4:
   "travel between the calibration points" — and the calibration points are the switches the
   teach captured).

### 2a.6 Closed-end headroom — ACCEPTED as a known limitation (operator, 2026-09-10)

Raw reads **0** at the closed switch and at closed rest. Contract 7.3 wants margin at both
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
  between the closed switch and the closed limit is clipped.
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

Re-attaching the wire cleared bit 7 on the **first** good stop-to-stop sequence, as 7.2 says
it should, with raw sweeping 0..990.

**This is the fault no electrical test can find**, and it was validated here by accident
rather than by design. Worth keeping: a bench check for the position path should include
detaching the wire, not just shorting or opening the wiper.

### 2a.9 Committed calibration (2026-09-10, rig)

| register | value | |
|---|---|---|
| `40001` zero offset | 0 | |
| `40002` measurement window | **100 ms** | not the 1000 ms default — see below |
| `40003` averaging window | 10 s | default |
| `40004` full travel | **15000** | 1500 mm, switch-to-switch |
| `40005` raw closed | **0** | at the rail; accepted per 2a.6 |
| `40006` raw open | **858** | |

**Scale: 858 counts over 1500 mm = 1.748 mm per count.**

**`40002` must be set before any teach, not after.** The teach captures `30005` at the sensor transition, and `30005` only refreshes once per measurement window. At the 1000 ms default and ~150 mm/s the capture could be a full second stale — up to 150 mm, ~86 counts of calibration error, with nothing to indicate it. At 100 ms that bound is ~15 mm.

Successive teaches captured the open end at 867, then 904 observed / 858 committed. The spread is real: it is where the switch makes on that pass, plus up to one measurement window of staleness. **Treat a single teach as ~±20 counts (~35 mm) repeatable, not exact** — and re-teach rather than hand-tuning if the number looks wrong.

### 2a.8 Traverse timing, measured (2026-09-10)

| leg | time |
|---|---|
| switch-to-switch (the actual opening range) | **10.0 s** |
| switch-to-limit (the blind overlap) | **~3.5 s** |
| total, limit to limit | **~13.5 s** — consistent with `travel_m3` = 13 s covering both |

**So today's timer-only logic spends ~3.5 s of every stroke stalled against the end
protection.** New logic that cuts the relay on bit 3 removes that (2a.5 item 3), and the
effective travel time it should configure is the **10 s** switch-to-switch figure, not 13.

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

1. **It is free.** The Route B teach (contract §6.2) already drives the leaf to each end stop in turn. Timing those traverses is a by-product of motion that is happening anyway — one operator action, two results.
2. **It measures what configuration cannot express.** `travel_ms` is a **single value used for both directions** (`relay_controller.cpp:439` close, `:500` open), yet requirements §1.4 item 3 states the mechanism *is* asymmetric: the motor lifts the flap against gravity to close and pays it out to open, so rope tension, backlash and slack all differ by direction. The real open and close times probably differ, and no amount of careful typing captures that.
3. **It needs no working wiper.** Time between **end-switch transitions** (bit 3), not between position readings — so the rate is available before the position calibration is itself trusted.

**Design:**

- Configured `travel_m3` becomes the **seed and the sanity bound**, not the source of truth.
- The commissioning teach measures actual traverse time **per direction**, from one bit-3 assertion to the next.
- **Accept only clean, full stop-to-stop traverses.** Reject any interrupted by a wind override, a reversal, a motor alarm, or any fault raised during the run.
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

### Phase 0 — bring-up and commissioning *(bench tooling; **BLOCKED on hardware**)*

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
### Phase 1 — driver

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
### Phase 2 — position task + derived configuration

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
| no `MODBUS_ERR_BUSY`, no added FG6485A/S200 failures over >=24 h (AT-WP05) | **NOT RUN** — needs a soak |

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
#### AT-WP05 soak — RUNNING from 2026-09-10 22:13

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
### Phase 3 — read-only logging *(the first thing with lasting value)*

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

> **The idle logging is deliberately temporary** (operator decision 2026-09-07): it exists to build trust in the implementation. It costs **~2880 rows/day, roughly +37 % of total log volume**, which shortens SD file rotation from ~1.8 days to ~1.3 and so increases daily upload count. Revisit once the traverse record is trusted — the resting state is already carried by ch 2's window bitmask, so idle sampling can be dropped or thinned without losing the terminal states.

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

> **Alarm *handling* is deferred to Phase 5** (operator decision 2026-09-07). This phase **detects and records; it does not act.** Control behaviour is unchanged, which is automatic here because nothing consumes position yet. The likely eventual behaviour is a fall-back to full open/close on wire-sensor failure — i.e. exactly today's time-based control, FR-WP17 — but that is **TBD** and is not implemented here.

- Detect and **log** the fault conditions (§3b), and surface them on the operator-facing surfaces (§6). Do not change any control decision on them.
- T2 may treat "bit 3 set at position ≈ 0 or ≈ `40004`" as travel-complete, **with the travel timer retained as the ceiling**.
- Implement the two controller-side rules from requirements §12.4 that the device cannot self-report:
  1. **Moving means moving.** While the relay is energised, `|30012|` must exceed ~half nominal within ~5 s. This catches a slipped or snapped wire, an obstruction, and — because a shorted wiper reads a *constant* — the wiper short the device cannot detect.
  2. **A stop that arrives too early is a fault, not a success.** A CLOSE that "reaches 0" in far less than `travel_m3` with bit 3 never set is reported, not believed.
- **Never gate anything safety-related on position** (FR-WP18): wind override, motor-alarm handling and boot CLOSE_ALL stay time-based.

**Minimal end result of this phase (operator decision 2026-09-07): a movement trace.** Every stroke is logged at the **measurement interval**, giving a full position-vs-time record of each traverse. That is what earns trust in the implementation before anything acts on it.

Volume is bounded and speed-independent: because interval = `travel_m3` / 100, a stroke is **exactly 100 rows** on the rig and on production alike. At ~3.3 M3 strokes/day that is ~330 extra rows against ~2864 `SENSOR_HR` rows — **+11 %**.

*Exit:* a stroke replays from the log as a clean monotonic ramp; plus AT-WP06 (disconnect mid-operation → fault within 2 poll cycles, ventilation continues), AT-WP07 (wind override still closes with the sensor disconnected), AT-WP09 (obstruct mid-travel → divergence reported).

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
was skipped because M3 already sat on its end switch — and **neither boot
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

Phases 0–4 add a sensor, logging and diagnostics. The greenhouse behaves exactly as it does today. Cross this gate only when the rig has produced clean aperture data over a sustained period **and** the Phase 5 scope decision (§8) has been taken.

---

### Phase 5 — proportional M3 control *(OUT OF SCOPE this cycle — recorded, not planned)*

The larger part of the effort, and the part the rig **cannot validate**.

- **T2:** a target input to `CH_MOVING_OPEN`/`CH_MOVING_CLOSE` that de-energises at target, travel timer retained as ceiling. A partial position becomes a new persisted state — an NVS schema change, so a **minor** version bump.
- **T6:** `VENT_STEP_TABLE` must express partial M3 apertures.
- **`step_width = max(hyst_t / NUM_VENT_STEPS, 1)` is integer division and gets *worse* with more steps** — at 6 steps, `hyst_t = 5` gives width 0 → clamped to 1 → every step 1 °C. **The F8 arithmetic must be reworked before the step table is touched**, not after.

**What the rig can prove:** that the controller commands and reaches intermediate apertures accurately and repeatably.

**What only 5C88 over a summer can prove:** that doing so actually damps the ~42 min / ~4.9 °C limit cycle. The plant model gives a prediction, not evidence.

---

## 6. Operator-facing surfaces

### 6.1 The display rule

Position replaces the binary state on the rich surfaces, but the **end switch stays the authority for the terminal states** (operator decision 2026-09-07):

| Condition | Shown |
|---|---|
| Travelling | **OPENING** / **CLOSING** — unchanged |
| At a stop (bit 3) with position ≈ 0 | **CLOSED** |
| At a stop (bit 3) with position ≈ `40004` | **OPEN** |
| Anything else | **the opening as a percentage** |

> **The percentage scale must not clamp at 0/100 — see §2a.5.** The leaf rests at ~113.7 % at the open end and below 0 % at the closed end, because the end switches mark the *window* extremes while the motor drives on into the blind overlap. Displaying a correctly parked window as "100 %" by clamping hides the overtravel that proves it reached its limit; displaying it as a fault is worse. Render the true value, and reserve fault styling for `65535` and the status bits.

Using bit 3 rather than "position == 0" for the terminal states is the right call: it is a physical witness rather than an inference, and it keeps the display honest about a window that is nearly-but-not-quite shut.

> **One consequence to expect, and it is not a bug.** Contract §5.3: bit 3 reports the *sensor*, not the window. Where the sensor zone is shorter than the overtravel, the leaf comes to rest past the sensor and bit 3 **clears** while the window is fully closed — so the display shows `0 %` rather than `CLOSED`. Phase 0's sensor-zone check tells us which regime this installation is in; if it is the short-zone case, that is the display telling the truth about what it can actually prove.

### 6.2 LCD (T8) — no change

16×2 leaves nothing spare (`M1:C M2:C M3:45%` is exactly 16 characters), and the diagnostic value of a percentage is better carried by an event than by a number the farmer must interpret.

**Rule where a binary state is still needed: `> 0 mm` open counts as OPEN** (operator decision 2026-09-07). No `boerHandleiding` change follows from this phase.

### 6.3 Web GUI

Show the opening percentage per §6.1, with the sensor fault surfaced alongside the existing T/RH and wind faults (FR-WP19). Farmer-visible, so `boerHandleiding` syncs in the same changeset.

**Also hosts commissioning** (admin-only): arm/abort the teach, watch bit 5, and run the §3.5 traverse measurement — **displaying measured open and close times against the configured `travel_m3` for explicit acceptance**. This is the screen that makes the measurement trustworthy rather than merely automatic.

### 6.4 Remote status site

Add the opening to the `windows` object in `build_canonical_status_json`, gated by `STATUS_EXPOSE_WINDOWS`. This is a **payload-shape change → minor version bump**, and the dashboard is a separate site that must tolerate the field being **absent** (not zero) on any unit without a sensor.

**Both operator questions answered yes (2026-09-07):** a "commanded but not moving" divergence is **farmer-visible**, and position faults **do** reach the remote status site — for 5C88 that is the only way anyone off-site would learn of it.

**The research payoff for doing this now rather than at Phase 5:** NS-9 — the wind-direction-dependent `ach_m3` model — is blocked on exactly this variable. Starting the remote record early means the history exists when someone picks it up.

---

## 7. Rollout, including production

Production is **not** waiting for Phase 5. In parallel with firmware development on the FDA4 mock, 5C88's real M3 is being fitted with the wire sensor, end switches and Modbus interface (operator, 2026-09-07).

1. Develop and verify phases 0–4 on the FDA4 mock.
2. Soak on FDA4 until the traverse record is trusted.
3. 5C88's hardware installation completes independently.
4. Production receives the firmware and **starts logging**.

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
3. **Alarm handling** — deferred to Phase 5, see the note in Phase 4.
