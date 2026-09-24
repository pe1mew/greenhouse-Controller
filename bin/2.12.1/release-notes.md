# Release 2.12.1

**Date:** 2026-09-24
**Built on:** 2.12.0
**Fixes:** [gh#83](https://github.com/pe1mew/greenhouse-Controller/issues/83) — mode 2 never finished a close

**Patch.** One defect, found in ordinary operation the first night mode 2 ran on the dev rig. No config key, no
NVS change, no log-encoding change. **Mode 1 was never affected**, and mode 1 is what every unit runs as delivered.

> **Nothing in this release has run on a greenhouse.** Production (5C88) is on 2.3.1 and unaffected.

## What changed

### A close to 0 % now reaches the closed end sensor (gh#83)

**What happened.** On 2344, the night of 2026-09-23, mode 2 closed M3 in four targeted drives ten minutes apart. The
last one stopped at **20.8 mm — 1.3 % — and the closed end sensor never made.** Nothing moved M3 for the rest of the
night. At 08:27 the operator switched to mode 1; its timed close made the end sensor **1.6 s** later. So the leaf really
was a couple of centimetres short, and the window stood open while the log recorded the law's intent as satisfied.

**Why.** A targeted stop ends on a *reading*, not on a switch. The arrival band is `deadzone_m3_mm / window_mm` —
20 / 1500 mm, so **1.3 %** — and once a stop landed anywhere inside it:

- **T6** read `|pos - want| <= band` and called the target arrived, so it issued nothing further;
- **the law** (`graded`) read `off <= m3_deadzone_x10` and held, on the reasoning that the caller would drop the
  command anyway.

Both were true of an ordinary aperture and wrong of an end. **A window resting one deadzone short of its end switch is
an open window**, and neither side asked again.

**The rule now.** *A target within the deadzone of an end IS that end.* It is snapped to 0 or 1000 and commanded as the
**ordinary end drive** — the same `CMD_CLOSE` / `CMD_OPEN` mode 1 issues, which runs into the end switch on the travel
timer — and **arrival at an end is the window's STATE, never its position**:

- `plan_target()` snaps the target, and treats `PART_OPEN` inside the band as *not* arrived;
- `post_target()` posts the end command, so the drive also gets an end drive's verdict from T17
  (`ALARM ch6 251 = ±1/±2`) instead of "not judged: no end was asked for" (`-4, 6`);
- `judge_m3_target()` reports DONE for an end target only in the terminal state;
- `vent_model_graded` keeps asking for 0 or 1000 while the window is `PART_OPEN`, whatever the reading says.

Both halves are needed: with only the caller fixed the law never asks, and with only the law fixed the caller drops it.

The contract ([`design/ventModelContract.md`](../../design/ventModelContract.md)) carries the rule, since it changes
what a target at an end means for any law.

**Fail-first:** `-DWPOS_FAILFIRST_212=2048` restores T6's position-based arrival, and `bin/at_wp_fallback.py endstop`
reproduces the night: M3 is left resting inside the deadzone with the law wanting it shut, and the closed end sensor
must make.

### Noted, not changed: the dwell across a mode promotion

The same night, a 21-second setpoint blip opened M3 fully under mode 1, and the effective mode was promoted to LINEAR at
the end of that stroke. The dwell had already been armed — mode 1's **1500 s** open dwell, not `min_intv_m3` — so the
first mode-2 move waited 25 minutes. That is the documented gh#51 rule (*a dwell already running keeps its old length*)
behaving as specified. It is recorded here because it surprised the operator, and because a law that expects its own
interval will meet it once per promotion.

## Verification

| Check | Status |
|---|---|
| `pio test -e native` in `drivers/ventModel` | **41/41 pass** (23 stepped, 18 graded). The new test — an end target holds out for the end — **fails on the pre-fix law** ("expected TARGET, was HOLD") |
| Release and bench images build | yes |
| **`bin/at_wp_fallback.py endstop`** on the rig, both arms | **PASS / FAIL as required** — 2344, 2026-09-24. *Fixed build:* M3 parked ajar at 1.1 % with the closed end sensor released, and the law closed it onto the switch after **617 s** — its own ten-minute hold, so the close came from `graded`, not from anything the harness did. *Fail-first bit 2048:* the same setup at 0.2 %, and M3 **stayed ajar for the full 1 200 s**, end sensor released — the night of 2026-09-23 reproduced on demand. *(An earlier run of this stage passed on BOTH builds and proved nothing: it released STANDBY to let T6 act, and leaving STANDBY recalibrates, so the sweep closed the window. The stage now stays in AUTOMATIC throughout.)* |
| Regression on the rig: `at_wp_target.py`, `at_wp_fallback.py`, `at_wp_confirm.py` | **ALL PASS** — 2344, 2026-09-24: **9/9**, **5/5** (including the new `endstop`) and **9/9**. `at_wp_confirm.py` had to be run in mode 1 and the rig put back into mode 2 afterwards: it predates mode 2 and waits for M3 to reach fully OPEN, which a linear M3 never does on its own |
| Soak | **PENDING** |

## Upgrading

- **No partition change, no NVS migration, no new key.**
- The web assets are unchanged in content but rebuilt with the version stamp, so firmware and assets still go together.
- **A unit in mode 1 sees no difference at all.** Only `ctrl_mode_m3 = 1` reaches this code.

## Known limitations

Unchanged from 2.12.0: AT-WP02 is marginal on the rig's 13 s window, the minimum move is unmeasured, `graded` is a
candidate rather than a chosen law, and mode 2 has never run on a greenhouse window.
