# Integrating the M3 window position sensor — implementation plan

| Field | Value |
|---|---|
| Document | Implementation plan |
| Date | 2026-09-07 |
| Status | **PLAN — nothing implemented.** Scope is **phases 0–4**. **Phase 5 (proportional control) is OUT OF SCOPE for this cycle** (operator decision 2026-09-07) and is kept below as the recorded end state, not as work |
| Requirements | [`windowPositionSensorRequirements.MD`](windowPositionSensorRequirements.MD) — FR-WP01–22, and §12 evaluating this sensor |
| Device contract | [`modbusInterfaceContractSpecification.md`](modbusInterfaceContractSpecification.md) v1.2 (normative source is the sensor project's `design/TDS.md`) |
| Bus architecture | [`refactorSensorConfiguration.md`](refactorSensorConfiguration.md) — the end state this plan deliberately does *not* build |
| Prerequisite | [`addModbusMutex.md`](addModbusMutex.md) — **shipped in 2.4.1**, and it is what makes §4's architecture possible |

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
| Full traverse | 171 s | **15 s** |
| Speed | 0.585 %/s | **6.67 %/s** (11.4× faster) |

**Representative of:** the entire software path — three devices on one bus, relay drive, position feedback, teach, fault handling, both commissioning checks, T2 stopping logic, the polling architecture.

**Not representative of:** anything thermal. No greenhouse means the limit cycle (requirements §1.1), `ach_m3`, and T6 step selection **cannot be validated on the rig**. It proves the mechanism, never the control strategy. Also outside its reach: the 40 m torsional lag (FR-WP22), rope-drum nonlinearity, and wind sway of a hanging flap.

**Confirmed met:** traverse ≥ 5 s (`CFG_MIN_TRAVEL_S`), sensor address 40 or 45 (both clear of FG6485A@1 and S200@44), +24 V/GND/A/B daisy-chained, both end-stop switches fitted, sensor-zone overtravel checked.

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

**Using two numbers where the firmware wants one.** T2 still has a single `travel_ms` per channel, and changing that is a control-model change — Phase 5 territory. For this cycle: measure both directions, **derive the constants from the shorter** (conservative: a shorter traverse means a faster rate, so a tighter poll interval), and **log both**. If the asymmetry turns out to be material, that is a finding worth having before anyone designs per-direction travel configuration.

**The risk this introduces, and it is real:** a bad measurement is *silent*, whereas a typed value at least the operator knows they typed. If an obstruction slows a traverse during commissioning, a wrong rate gets baked in. The sanity band, the clean-traverse requirement, and logging both values are the defences — the last one matters most, because it lets a later reader spot the discrepancy without re-running anything.

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

### Phase 0 — bring-up and commissioning *(no firmware)*

Confirm the device before writing code against it. Per contract §9: read `30007` (**refuse build type `0x81`** — a bench build with a deliberate hang hook), read the holdings, then teach.

- Teach with **movement** (contract §6.2) — never arm with the leaf resting at a stop it has not moved to since power-on.
- **Our driver has no FC06.** Arm and abort the teach with **FC16, quantity 1**, to `0x0006`; `FR-MB28` rejects only quantity 0.
- Write `40004` = the tape-measured **sensor-to-sensor** distance, not the hard stops.
- **Measure the traverse time per direction** while the teach drives each stop (§3.5). Record both, and record which one the derived constants ended up using.
- Run both installation checks and **record the answers** — they change what the firmware can rely on:
  - **Sensor-zone** (contract §5.3): drive fully closed, confirm bit 3 stays set. If it clears, FR-WP07 is not met on this rig and "bit 3 clear" must be read as *not proven at a stop*, never *proven away from one*.
  - **Electrical headroom** (contract §7.3): with the window fully closed and fully open, `30005` must not sit near 0 or 1023. If it does, **bit 6 is inert** and a shorted wiper will read as a perfectly closed window.

### Phase 1 — driver

Thin driver over the existing Modbus layer. FC04 for input registers, FC03 for holdings, FC16 for writes.

- **`30012` is signed** — decode as `int16`, or a closing window reads ~65 000.
- Position, status bits, and rate are the control surface; the rest is diagnostic.
- Fault mapping: bit 2 (wiper) and the `65535` sentinel → sensor fault; bits 6/7 → health, operator-visible only.

*Exit:* a bench read returns plausible position, rate sign follows direction, and disconnecting the wiper produces the fault within ~2 s.

### Phase 2 — position task + derived configuration

The task of §4, with §3's rules implemented as **derived** values, not constants. Log the derived numbers at boot so a wrong `travel_m3` is visible immediately.

*Exit:* during a mock stroke, position advances monotonically at ~6.7 Hz with no `MODBUS_ERR_BUSY` and no added read failures on FG6485A or S200 over ≥ 24 h (this is AT-WP05).

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

Using bit 3 rather than "position == 0" for the terminal states is the right call: it is a physical witness rather than an inference, and it keeps the display honest about a window that is nearly-but-not-quite shut.

> **One consequence to expect, and it is not a bug.** Contract §5.3: bit 3 reports the *sensor*, not the window. Where the sensor zone is shorter than the overtravel, the leaf comes to rest past the sensor and bit 3 **clears** while the window is fully closed — so the display shows `0 %` rather than `CLOSED`. Phase 0's sensor-zone check tells us which regime this installation is in; if it is the short-zone case, that is the display telling the truth about what it can actually prove.

### 6.2 LCD (T8) — no change

16×2 leaves nothing spare (`M1:C M2:C M3:45%` is exactly 16 characters), and the diagnostic value of a percentage is better carried by an event than by a number the farmer must interpret.

**Rule where a binary state is still needed: `> 0 mm` open counts as OPEN** (operator decision 2026-09-07). No `boerHandleiding` change follows from this phase.

### 6.3 Web GUI

Show the opening percentage per §9.1, with the sensor fault surfaced alongside the existing T/RH and wind faults (FR-WP19). Farmer-visible, so `boerHandleiding` syncs in the same changeset.

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
| A *bad* traverse measurement is baked in silently | §3.5's three defences: clean-traverse-only, ±50 % sanity band against configured `travel_m3`, and both values logged so a discrepancy is visible after the fact |
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
