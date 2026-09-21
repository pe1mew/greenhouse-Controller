# Release 2.12.0

**Date:** 2026-09-20
**Built on:** 2.11.0
**Implements:** plan §5b and §5c of [`design/integrateWindowPositionSensor.md`](../../design/integrateWindowPositionSensor.md) — linear control of M3, and the control-law boundary it needs

**Minor.** Two control modes for M3, two new config keys, three extended log encodings, one new status field — and the control law leaves T6 for a replaceable library. **Default behaviour is unchanged:** a unit ventilates exactly as before unless an operator switches M3 to linear control, and even then it falls back on its own whenever the position cannot be trusted.

> **Nothing in this release has run on a greenhouse.** It has run on the dev rig; production (5C88) is on 2.3.1 and unaffected until someone promotes a build to `mainstream`, which this is not.

## What changed

### The control law left T6 (plan §5c)

Mode 1's stepped law existed **twice** from 2026-09-17: inline in `climate_control.cpp` and as the tested reference in `drivers/ventModel`, kept in step by hand. That ends here. `VENT_STEP_TABLE`, `step_from_deviation()`, `vent_step_channels()`, `vent_step_required_t()`, `vent_step_required_rh()` and `vent_resolve_conflict()` are **deleted**; T6 calls the library through a new IDF component (`firmware/components/ventModel`), which has no `REQUIRES` because the library sees no ESP-IDF at all — that is what lets the same source compile into the firmware, the host tests and the offline replay.

**What stays in T6** is everything around the law: the EG1 inhibit mask, the snapshots, the actuator limits, the command order, Q1, the SD log rows and the state resets ([`ventModelContract.md`](../../design/ventModelContract.md) §3).

Three things the refactor had to preserve, and how each was shown:

- **The `LOG_MODE_CHANGE` row is byte-identical**, and so is its trigger. The comparison starts at 0 and returns to 0 at an inhibit onset, because the inline law began every run and every inhibit with `current_step_t = current_step_rh = 0`. A `VENT_STEP_NONE` (−1) baseline — the obvious reading of the constant — would have added a step-0 row at boot and after every wind override, STANDBY exit and calibration sweep, rows `logparser.py` and `plot_daily.py` read as ventilation decisions. The closed-loop simulator's emulated T6, written independently, uses the same 0 baseline.
- **One snapshot of T2's window states**, shared by the decision and the commands. A second read would let a window change state in between and silently drop a command the model had asked for.
- **The window-state enums are pinned** with a `static_assert` per value. They match by design, and T2 gains a new one in this very release.

### Mode 2: linear control of M3 (plan §5b)

**Two variables, and they are not the same thing.**

| | Where | What it means |
|---|---|---|
| **Desired** | `motor/ctrl_mode_m3`, 0 timed / 1 linear, **default 0** | What the operator asked for. A firmware update never changes it. |
| **Effective** | `dm_m3_ctrl_mode()` in T4 | What is actually driving M3: the setting **and** a position T17 trusts **and** the anti-flap. Computed in exactly one place — two places would eventually disagree, and the SD log would record a decision under a mode that was not in force. |

- **Demotion is immediate**; **promotion** waits for a stroke boundary and a 120 s hold-down. Turning linear control *off* takes effect at once, because that is a deliberate act.
- **The fall back needed no code in the law — and one fix in T6, found by the soak.** Mode 1's law sees a part-open M3 as being at neither end and asks for whichever end its step wants. T6's apply filter, copied from the inline code that predates `PART_OPEN`, **dropped that request**: after mode 2 left M3 part-open, mode 1 could neither close nor open it. On 2026-09-20 it stayed stranded for 28 minutes until a wind override closed it. Fixed on 2026-09-21; `bin/at_wp_fallback.py` is the fail-first test.

**The actuator.** Q1 gains `CMD_TARGET` with a `target_x10` field (0..1000, 0.1 %), read for that action and no other — four of the five Q1 producers build commands with positional initialisers, where a trailing field is zero, and **a zero read as a target means "close it"**. T2 refuses a target when the window has never been taught, when the position is not trusted, or when the reading is older than 3 s; a target at or within one deadband of an end becomes an ordinary full-travel drive; and a position lost mid-move leaves the drive to finish on the travel timer, at an end.

**The stop rule** stops on arrival **or on passing** the target. The leaf moves ~0.67 % of the stroke between two position samples, so a deadband narrower than that would be stepped over and the drive would run on to the end.

**`CH_PART_OPEN`** is a terminal state but deliberately **not persisted**: `persist_ch_state()` maps anything that is not CLOSED or OPEN to UNKNOWN, so a part-open M3 forces the boot CLOSE_ALL instead of taking the "all three closed" shortcut. That requirement needed no new code — only the discipline not to add any.

### The linear dwell

**`motor/min_intv_m3`** (seconds, 0–1500, **default 600**). In mode 2 it **replaces M3's open and close dwell** ([contract §7](../../design/ventModelContract.md)): `ch_dwell_ms()` in T2 arms M3's dwell from this key whenever linear control is in force, and T6 refuses a target inside the same interval. One key, two enforcers, one number. Leaving T2's 25-minute dwell in place would have made the interval irrelevant and mode 2 unable to move a window at all.

**The default is 600 s**, decided on 2026-09-20 from a simulated summer of 5C88's own weather (`model/closedloop/linearDwell.md`). The plan had specified 0; the evidence changed it:

> - **It costs nothing today.** `graded` already holds ten minutes between moves, so 0, 300 and 600 give an identical summer — same swing, same 21.6 M3 drives a day, not one target deferred.
> - **It is the floor that protects the next law.** The interval is the *caller's* protection; a hold inside a law is the law's own, and the contract exists because the law will be replaced. `graded` with its hold removed — a law re-deciding every 30 s — gives 42.5 M3 drives a day at 0, against mode 1's 7.7; at 600 it gives 19.3, with a better swing than mode 1. Shipping 0 means the protection exists only while the law happens to be well behaved.
> - **Ten minutes is the loop's dead time**, not a round number: the reading lags the air 3.5–5.5 min, T5's averaging adds to it, and a 25 % move takes 44 s in production.
>
> **Never above 900**, where mode 2 swings as much as mode 1 while still driving M3 twice as often. **0 stays right for a deliberate test** — it is what the law alone does, and one setting away.

### Rule 1's at-end exemption, for a state this release created

A part-open stop can leave M3 at the closed end's **position** but short of its **switch**: the encoder reads 0 about 1.2 s of travel before the closed end sensor makes. T17's rule 1 excused a drive toward the end the leaf sits at only if the switch was made **and** the position was at that end on **every** sample from the first — so a CLOSE from there reported a stall that was not one (2344, 2026-09-20: `stall_faults` 1). With T6 now able to close a part-open M3, ordinary operation reaches that state, so it is fixed in this release: the position must never have **left** the target end's region, and the switch must be made **at the verdict**. A leaf stuck short of its switch, and a shorted wiper, are still reported (`bin/at_wp_rule1.py`, fail-first bit 32). Nothing acts on rule 1; it reports.

### Log encodings — all appended, all with their consumers

| Encoding | Change |
|---|---|
| `RELAY value_a` | **7** = `PART_OPEN`. The ordinal is stored in every archived row, so it is appended, never inserted. |
| `SENSOR_HR ch 2` | A **part-open qualifier bit per channel** (6, 7, 8) on top of the four 2-bit codes, which were all spoken for — widening the fields would have shifted M2's and M3's bits and silently re-decoded every archived row. `value_b`, a hard zero until now, carries M3's opening in 0.1 % (−1 = no trusted position). |
| `MODE_CHANGE param 54` | A **third** emitter on this row type: the control law actually in force. `value_a` 0 timed / 1 linear, `value_b` the reason. Written at boot as well as on every change, because which law a unit came up under is not inferable from silence. |
| `ALARM ch 6 param 251` | A new not-judged reason, **6**: a targeted drive, no end was asked for. |
| `SETPT params 53, 55` | The two new keys. |

`logparser.py`, `logparser.md`, `plot_daily.py`, `vent_step_replay.py` and the closed-loop simulator all learn these **in this release**. A second meaning on one row is what gh#54 cost.

### Surfaces

- **Status payload:** `M3_ctrl_mode` (`"TIMED"` / `"LINEAR"`), present whenever the windows block is, including on units with no sensor. "Which law is driving my greenhouse" must not be a question whose answer is an absent field.
- **Web GUI:** *M3 control* and *Minimum interval* join the Linear control group — inside `#wpos-dep`, so both grey out with the reason above them when no sensor is fitted. Below the selector, a line says which law is **in force**, and when linear was asked for and is not in force, it says what is missing and that it will resume by itself.
- **LCD:** no new settings — every other motor setting is web-only too — but its three window-state renderers learned `PART`, because all three fell through to `UNK`, which tells an operator that the position is not established.
- **Manuals:** `boerHandleiding` 1.22, `beheerderHandleiding` 1.25.

## Size

|  | 2.11.0 | 2.12.0 | delta |
|---|---|---|---|
| app image | 1 402 256 | **1 407 840** | **+5 584 B** |
| `.flash.text` | 944 954 | 949 286 | +4 332 B |
| `.flash.rodata` | 312 572 | 313 836 | +1 264 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 312 | 42 360 | +48 B |
| web assets | *(pending build)* | *(pending)* | |

The bench image is 1 417 456 B. Both control laws are compiled in: `stepped` drives mode 1 and `graded` is present for mode 2, though **nothing selects `graded` yet** — the law for mode 2 is still a decision, not a choice (`model/closedloop/gradedCandidate.md`).

## Verification

**PENDING — to be completed before this release is published.** Nothing below is a claim yet.

| Check | Status |
|---|---|
| `pio test -e native` in `drivers/ventModel` | 38/38 pass |
| `model/vent_step_replay.py`, 378 logged decisions | 96.8 %, the baseline exactly, driving the library itself |
| `model/closedloop/closed_loop.py gate-control`, 381 decisions | 97.1 %, PASS |
| `bin/check_cfg_desc.py` | 55 keys, 44 published, mock agrees |
| Release and bench images build | yes |
| Every new log encoding through the real parser | decoded |
| GUI against the mock, in a browser | the three badge cases and the greying rule |
| **`bin/at_wp_target.py`** — seven stages on the rig | **all seven PASS in one run** on the fixed build (band 50.9 %, twice 51.2 then 28.7 %, closeshort reached the end) — 2344, 2026-09-21 |
| **`bin/at_wp_rule1.py`** — rule 1's exemption, four stages, fail-first | fail-first bit 32: `headroom` **FAILS** (`stall_faults` +1, this incident's signature), the other three pass; fixed build: **all four PASS** (the late switch excused; a leaf short of its switch and a shorted wiper still reported) — 2344, 2026-09-21 |
| **`bin/at_wp_confirm.py`** — 2.10.0's nine stages, regression for the rule-1 change | **all nine PASS** on the fixed build (`atend`: excused, `at_end_exempt` +1, no stall; `race`: rule 2 still trips) — 2344, 2026-09-21 |
| **`bin/at_wp_fallback.py`** — the fall back through T6, fail-first | unfixed build and fail-first bit 16: **both stages FAIL** (M3 stranded part-open for 200 s); fixed build: **both PASS** (closed after 49 s, opened after 47 s) — 2344, 2026-09-21 |
| **`bin/at_wp02.py`** — AT-WP02 repeatability, AT-WP03 endpoints | **AT-WP02 FAIL**: ten moves to 50 % spread **3.0 %** against the 2.0 % of FR-WP05. Each direction is repeatable (1.0 % from below, 1.5 % from above); the failure is a **directional offset of +1.5 %** — T2 cuts the relay on entering the ±1.33 % band and the leaf coasts ~2 % further, so an opening lands ~0.8 % high and a closing ~0.7 % low. **AT-WP03 PASS**: 0.0 % and 100.0 % with the end sensors made, "nearly closed" (9.9 %, PART_OPEN, no end sensor) distinct — 2344, 2026-09-21 |
| **Soak ≥ 12 h with scripted strokes** | **to be re-run on the fixed build.** A 12.04 h run on 2344 (2026-09-20, 25 judged strokes, no reboot) used the image with the stranded-M3 defect, and recorded one `stall_faults` — the rule-1 exemption false positive, whose fix is designed but not made |

## Upgrading

- **No partition change, no NVS migration.** Two new keys appear at their defaults, which reproduce today's behaviour exactly.
- **The web assets change**, so firmware and assets go together as always.
- **What an operator gains:** nothing until they ask for it. A unit that is updated and left alone ventilates exactly as it did before.
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting, and record the heap as TC-09 now asks (steady-state free and largest block at a stated uptime, not `heap_min_kb`).

## Known limitations

- **`graded` is a candidate, not a choice.** Mode 2 runs whichever law the table selects; the evidence for `graded` v1 is in `model/closedloop/gradedCandidate.md` and the decision is the operator's.
- **The minimum MOVE is still unmeasured** (§3.6 floor 2: the shortest pulse that actually shifts the leaf). Until it is, small moves are bounded only by the deadband, which is a different quantity.
- **The feedback is inferred, not measured.** T6 judges its own target from where M3 came to rest, because T2 reports a state and not an outcome. A drive that ends at an end when a partial target was asked for reads as a failure, whatever stopped it.
- **Mode 2 has never run on a greenhouse window**, only on the rig's 13 s test window. Production's traverse is 176 s, and everything about positioning scales with it.
