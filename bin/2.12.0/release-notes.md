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

### Targeted stops land on the target (FR-WP05)

AT-WP02 found that the same commanded aperture did not give the same physical one: stops at 50 % rested ~2 % high when opening and ~2 % low when closing. Two causes, both fixed:

- **T17 published a mid-coast position.** It read "at once" after a stroke, which after a targeted stop is while the leaf still coasts (up to 1 % short on the rig), and published that for 30 s — to the GUI, to T6 and into the "where it settled" log row. It now reads **one second plus one measurement window** after every stroke.
- **T2 did not lead the overrun.** The leaf rests ~3 % of the stroke past where the relay is cut; T2 cut on *entering* the arrival band. It now cuts at an **aim**, the target less the expected overrun, **learned per direction** from every settled stop (it starts from a default scaled by `travel_m3`, and keeps it in RAM). The learned values are in `GET /api/diag/windowpos` (`t2`).

A move shorter than the lead is cut on its first reading of the drive: the shortest move the mechanism can make, still unmeasured (Known limitations).

### What the law is told: three corrections from the simulator (2026-09-21)

The model session replays 2.12.0 as built in its closed-loop simulator (`model/closedloop/`), and found three places where T2 or T6 did not do what the contract says. **None changes a decision `graded` makes today**; each would mislead a law that took the contract at its word.

- **A deferred reversal lost the stroke's target.** `ch_start_open()` / `ch_start_close()` disarmed an armed target *before* gh#48's in-travel guard deferred T6's reversal, so the stroke under way lost its stop point and ran on to the end switch: M3 closing to 30 %, the law now asking for 70 %, and M3 closed fully, then sat out `min_intv_m3`. `graded` repeats its target while M3 moves, so it never does this (the simulator found no case over 13-29 July); a law that corrects mid-stroke would. **A deferred command now leaves the stroke exactly as it found it**: the stroke stops at its own target, and T6 asks again. A command that is *not* deferred still disarms, so a safety close is never stopped short. Fail-first bit 256; `bin/at_wp_target.py deferred`.
- **ABORTED was never sent.** T6 inferred every result from where M3 came to rest, so a window taken by a safety close, the operator or a recalibration read as FAIL_TIMEOUT, which `vent_model.h` reserved for "did not arrive". T2 now counts the drives the law did not command that change what M3 does (`t2_get_taken()`: a start, a reversal, a targeted stroke made full; the sweep; a motor alarm), and T6 reports **ABORTED** when that count moved between its command and the judgement, ahead of the position. A **repeat** of the outstanding target keeps the count it went out with, or a take still moving M3 at a T6 wake would be folded into the baseline and missed. `graded` rebases on either result, so no decision changes. Fail-first bit 512; `bin/at_wp_fallback.py aborted`.
- **`ms_since_move` = 0 meant two things.** T6 read it as "no drive since boot" and skipped the interval; a law is entitled to read it as "just moved" and hold M3. "None since boot" is now **UINT32_MAX**, what the simulator already used. The recalibration sweep, which drove every window without ending a "move", **now ends one**, and it **arms the dwell of the mode in force**: `min_intv_m3` for M3 in mode 2, where it armed the close dwell. A motor alarm that stops a drive ends one too. Fail-first bit 1024; `bin/at_wp_fallback.py sincemove`.

The contract ([`design/ventModelContract.md`](../../design/ventModelContract.md) and `vent_model.h`) now says **how `last_result` is judged** (ABORTED first, then FAIL_FAULT, then DONE inside the deadband, FAIL_TIMEOUT anywhere else, including a command that was deferred and never started) and **defines UINT32_MAX**. Two new host tests pin what `graded` does with each. The bench diag gains a `t2.taken_m3` / `t2.ms_since_move_m3` pair and a `t6` block (last target, result, the count at the post), and the target hook takes `"source":"t6"` so a harness can send a target the way T6 does, dwell and gh#48 included.

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

The bench image is 1 417 456 B. Both control laws are compiled in: `stepped` drives mode 1 and **`graded` drives mode 2**, selected by T6's model table whenever linear control is in force. Which law mode 2 should run is still a decision, not a choice (`model/closedloop/gradedCandidate.md`). *(Until 2026-09-21 this paragraph said nothing selected `graded`; the table has selected it since plan §5c step 2.)* **These sizes predate the 2026-09-21 corrections; regenerate the table at the release build.**

## Verification

**Every rig run is done** (2026-09-22, 2344). Each of the three fixes has both arms: the stage passes on the fixed build and fails on its own fail-first bit. Everything below is measured.

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
| **`bin/at_wp02.py`** — AT-WP02 repeatability, AT-WP03 endpoints, `settle` | **Fixed build: all PASS** — AT-WP02 spread **1.8 %** (≤ 2.0), hysteresis −0.1 %; `settle` worst **0.2 %** (≤ 0.3); AT-WP03: both ends exact, "nearly closed" (8.2 %, PART_OPEN) distinct; leads learned 3.56 % opening, 2.66 % closing. **A second run with the leads learned: AT-WP02 2.5 % — FAIL** (mean −0.1 %, hysteresis −0.1 %, `settle` 0.1 %): the offset is gone, the remaining per-stop scatter (σ ≈ 0.55-0.75 %) makes AT-WP02 marginal on the rig's 13 s window; the requirements name AT-WP02 on the real window as the acceptance. **Fail-first** (bits 64 + 128): AT-WP02 **3.8 % FAIL**, `settle` **0.9 % FAIL**. The first run (spread 3.0 %) read T17's mid-coast cache and is superseded — 2344, 2026-09-21 |
| `drivers/ventModel` host suites after the 2026-09-21 contract change | **40/40 pass** (23 stepped, 17 graded), including two new `graded` tests: ABORTED rebases, and "never moved" (UINT32_MAX) is long ago while 0 is "just moved" |
| **`bin/at_wp_target.py deferred`** — a deferred T6 reversal keeps the stroke's target, fail-first bit 256 | **PASS** on the fixed build and **FAIL** on bit 256 — 2344, 2026-09-22. Fixed: from CLOSED a T6 target of 70 %, then a T6 target of 10 % three seconds in; M3 stopped **PART_OPEN at 70.1 %** against its own target, and `taken_m3` did not move (5 → 5), so T6's two commands took nothing. Fail-first bit 256: the stage FAILS, the stroke running on to the end switch |
| **`bin/at_wp_target.py taken`** — the operator and a recalibration take M3, T6 does not | **PASS** — 2344, 2026-09-22: an operator's target took the window once (`taken_m3` 10 → 11), **T6's target moved M3 and took nothing** (11 → 11), a recalibration took it once (11 → 12). The soak says the same over a longer run: 24 T6 drives in 12 h took nothing |
| **`bin/at_wp_fallback.py aborted`** — T6 tells the law ABORTED, fail-first bit 512 | **PASS** on the fixed build — 2344, 2026-09-22, through the real T6 in mode 2: `graded` posted **target 25.0 %** after its 618 s hold, the operator closed M3 mid-drive, and T6 judged the command **ABORTED (result 4)** with `taken_m3` 25 against 24 at the post. Before this release that read FAIL_TIMEOUT. **Fail-first bit 512: the stage FAILS** — same sequence, and T6 judged the taken window **FAIL_TIMEOUT (result 2)** with `taken_m3` 4 against 3 at the post, which is the defect exactly. (The first attempt was INCONCLUSIVE for a harness reason, not the rule — see below) |
| **`bin/at_wp_fallback.py sincemove`** — "never" is UINT32_MAX, the sweep is a drive and arms `min_intv_m3` in mode 2, fail-first bit 1024 | **PASS** on the fixed build — 2344, 2026-09-22: as M3's part of a recalibration ended, `ms_since_move_m3` read **1 081 ms** (the sweep, not the operator drive a minute before), and **in mode 2 the sweep armed `min_intv_m3` (600 s), not the 5 s close dwell** — a T6 target 20 s later waited. The "never" value is only visible on a boot that skips its sweep, and was **seen at the push**: `ms_since_move_m3` 4294967295 with `taken_m3` 0. **Fail-first bit 1024: all three checks FAIL** — "no drive since boot reads 0"; after the sweep `ms_since_move_m3` read **58 738 ms**, the operator's drive a minute earlier rather than the sweep; and the sweep armed the 5 s close dwell, so the T6 target went and left M3 part-open |
| Regression after the 2026-09-21 corrections | **ALL PASS** — 2344, 2026-09-22: `at_wp_target.py` **9/9** (the seven earlier stages plus the two new ones), `at_wp_fallback.py` **4/4**, `at_wp_confirm.py` **9/9** (2.10.0's stages, including `race` still tripping `early_stops` and `atend` still exempt). Nothing the three fixes touch moved anything else |
| **Soak ≥ 12 h with scripted strokes** | **PASS on 9c53be7** (bench image `de7d9228…`, 2344, 2026-09-21 17:55 to 2026-09-22 06:05): **12.17 h, 24 judged strokes, all 24 confirmed**, `stall_faults` 0, `early_stops` 0, `rejected_rate` 0, `err_comm` 0, `not_reached` 0, `mode_changes` 0, gate settled at `position`, no reboot. All 12 scripted sessions ran, none skipped or failed. Still clean when re-read at **25.94 h**. Measured traverse 11.9-12.4 s against `travel_m3` 13. Heap at 12.2 h and at 26 h alike: **67 KB free, 30 KB largest block** (TC-09's steady state); during a TLS status POST it dips to 42 KB free / 19 KB largest (measured live at 26 h). **One flag, not a soak failure:** the since-boot low watermark `heap_min_kb` fell from 21 KB at 12.2 h to **1 KB** by 26 h — one deep handshake, not the routine dip, and deeper than gh#81 ever recorded (9-11 KB/day). About 1 KB of that is this bench build's own diag buffer, which release builds do not carry. Two earlier runs are superseded: 12.04 h on 2026-09-20 (the stranded-M3 image) and 2.06 h on 045a39c, aborted to take this release's last fixes |

**Two harness faults, found while running the arms on 2026-09-22 — neither is a firmware defect.**

- **A freshly pushed image has not stroked, so the gate is `timed`** and mode 2 cannot engage: T17 promotes to position
control only at a stroke boundary. Both mode-2 stages (`aborted`, `sincemove`) reported "M3 never came under LINEAR
control" on their fail-first images and said nothing about the rule they exist for. `at_wp_fallback.py` now strokes M3
once with the hook before asking for mode 2 (`ensure_position()`); on a unit that has been running, it is a no-op.
- **`at_wp_fallback.py` returns M3 to an end with a recalibration BEFORE its settings are restored**, so that sweep armed
M3's dwell from the test settings — `min_intv_m3` 600 s in mode 2. The next harness then waited 420 s for a window that
could not move for 600, and reported a setup failure. Re-run after the dwell expired: all nine stages passed.

## Upgrading

- **No partition change, no NVS migration.** Two new keys appear at their defaults, which reproduce today's behaviour exactly.
- **The web assets change**, so firmware and assets go together as always.
- **What an operator gains:** nothing until they ask for it. A unit that is updated and left alone ventilates exactly as it did before.
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting, and record the heap as TC-09 now asks (steady-state free and largest block at a stated uptime, not `heap_min_kb`).

## Known limitations

- **AT-WP02 is marginal on the rig.** With the lead, stops land centred on the target from both directions, but each stop still scatters by σ ≈ 0.55-0.75 % on the rig's fast 13 s window, so ten stops span ~2-2.5 % against the 2.0 % the test allows (one run passed at 1.8 %, one failed at 2.5 %). The timing share of that scatter should be ~13x smaller on 5C88's 176 s window; that is an estimate, and AT-WP02 on the real window, once 5C88 has its sensor (gh#77), is the acceptance.

- **`graded` is a candidate, not a choice.** Mode 2 runs whichever law the table selects; the evidence for `graded` v1 is in `model/closedloop/gradedCandidate.md` and the decision is the operator's.
- **The minimum MOVE is still unmeasured** (§3.6 floor 2: the shortest pulse that actually shifts the leaf). Until it is, small moves are bounded only by the deadband, which is a different quantity.
- **The feedback is partly inferred.** Whether a window was *taken* is counted by T2 and reported as ABORTED (2026-09-21). Everything else T6 still judges from where M3 came to rest, because T2 reports a state and not an outcome: a drive that ends at an end when a partial target was asked for reads as FAIL_TIMEOUT whatever stopped it, and so does a command that was deferred and never started.
- **Mode 2 has never run on a greenhouse window**, only on the rig's 13 s test window. Production's traverse is 176 s, and everything about positioning scales with it.
