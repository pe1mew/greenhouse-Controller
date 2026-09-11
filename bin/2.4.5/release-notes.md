# Release 2.4.5

**Date:** 2026-09-10
**Built on:** 2.4.4
**Fixes:** LCD manual window control left the controller in AUTOMATIC and inert for up to 25 minutes

## The defect

Drive the windows by hand from the LCD, log out. Mode reads AUTOMATIC, `/api/status` shows the real window states, T6 is running — and nothing happens. The controller looks correct and does nothing.

### T6 was never confused — T2 was refusing it

This is worth stating plainly, because the natural diagnosis is wrong. `reconcile_to_step()` is **level-triggered**: it runs every cycle unconditionally (`climate_control.cpp:589`), reads **actual** window states via `t2_get_window_states()`, and posts per-channel `CMD_OPEN` / `CMD_CLOSE` for whatever disagrees. It does not carry a stale step model and does not care how the windows got where they are.

The block is in T2. `ch_start_open()` / `ch_start_close()` defer `SRC_T6` while a dwell deadline is pending (`relay_controller.cpp:483`):

```c
/* only SRC_T6 (climate) observes; SRC_T3 (safety) + SRC_OPERATOR_MANUAL (admin) bypass. */
if (source == SRC_T6 && (int32_t)(now_ms - c->dwell_deadline_ms) < 0) { ... return; }
```

**The asymmetry is the bug.** `SRC_OPERATOR_MANUAL` bypasses the dwell on the way *in* — deliberate and correct. But *completing* any move sets the dwell deadline inside `ch_update()`, which **has no source parameter**, so a hand-set position leaves exactly the same debt as a T6 move. T6 then inherits an anti-thrash debt it never incurred.

On FDA4 `dwell_open_m3` is **1500 s**. Manually open M3, log out, and T6 retries every 30 s and is refused for **25 minutes**. `dwell_close_m3` is 600 s, so the reverse costs 10.

### And it left no trace

The deferral logged at `ESP_LOGD` — below the default log level, serial-only, absent from the SD log. Nothing on the web GUI, nothing in the audit trail. This is why it took a code read rather than a log read to find.

## The fix

`session_close()` now does two things where it previously suppressed one:

- Posts the new **`T2_NOTIFY_CLEAR_DWELL`** (`relay_controller.h`), so T2 drops the dwell debt **in its own context**. T2 remains the sole writer of `s_ch[]` — no cross-task race on the state gh#51 had just finished making single-writer. Ordering works out: T2 consumes the notification at the top of its loop, then drains Q1 and finds the recalibrate command.
- Passes `recalibrate_on_clear = true`, so session end returns the windows to a known CLOSED baseline that T6 resumes from.

Clearing the dwell is belt-and-braces — `calib_close_all()` bypasses the dwell check anyway — but it matters when the calibration *doesn't* run: Q1 full, or a motor alarm aborting the sweep. Without it T6 would still be stuck.

The two dwell deferrals now log at **`ESP_LOGI`**, latched to one line per episode via `dwell_defer_logged` (reset wherever a fresh dwell is set), so a 25-minute wait produces one line rather than fifty.

## Operator-visible change

**Manual window positions no longer survive past the session.** Ending an admin manual session — explicit logout *or* the 5-minute idle timeout — now runs a CLOSE_ALL calibration.

**The respect window is unchanged.** Positions still hold for the entire session.

This is a deliberate partial revert of rc.1.5.2, which fixed the 2026-05-26 complaint (*"during manual operation climate control kicked in and took over"*) two ways at once: it moved the STANDBY clear to session-end — the respect window, which is what actually fixed the complaint — **and** suppressed the CLOSE_ALL. The second half went further than the complaint required and caused the defect above. The first half is kept.

## This was a recurrence, and that is the more useful finding

The problem was solved once already, in May 2026. The rc.1.5.2 reasoning was written up carefully — **in a code comment inside `session_close()`** — and nothing went into the gotcha log. Anyone searching for *"T6 out of sync after manual control"* found nothing, because the symptom surfaces in a different task from the decision that caused it.

The comment recorded **why the suppression was chosen**. It never recorded **what would now not happen**. Four months later the consequence was rediscovered from scratch.

Three rules are now in `memory/gotcha-log.md`, with a new *"Windows, climate & manual control (T2, T6, T8)"* index section and a pointer in `CLAUDE.md`:

- If a command source bypasses a timer, decide explicitly whether it should also clear the debt that timer leaves behind. Bypassing on the way in and not on the way out is the trap — it looks correct at both sites.
- If a fix suppresses a mechanism, log what will now not happen, not just why you suppressed it.
- Never diagnose "T6 is out of sync" as a T6 state-model problem. Look at what is refusing its commands: dwell, the gh#48 in-travel guard, the MOTOR_ALARM discard, or an EG1 inhibit bit.

## Verification

**OTA verified** per the standing rule: `fw_ver` **and** `asset_version` both 2.4.5, read post-reboot on FDA4. Settled at `eg1 = 0`, AUTOMATIC, all channels CLOSED.

**The fix is VERIFIED on hardware** (operator, 2026-09-10): manual drive from the LCD followed by logout starts the calibration as intended. The path is LCD-only — there is no web or API route to manual motor control, by design (gh#29: manual control is a physical-presence activity). The procedure used, kept for future regressions:

1. Log in as admin on the LCD, drive M3 open by hand.
2. Log out (or let the 5-minute idle timeout fire — **both paths should now behave identically**).
3. Confirm a CLOSE_ALL calibration starts, and that the serial log shows `manual session ended: dwell deadlines cleared on all channels`.
4. Confirm T6 then controls normally rather than sitting inert.

**Note that FDA4 now has the M3 window emulator fitted**, so that calibration will physically drive it — this is no longer a relay-only exercise.

`ntp_synced` read `false` immediately after the push; that is the documented SNTP rate-limit from repeated OTA reboots, not a defect, and it recovers on the 300 s retry cadence.

## Upgrade notes

No configuration migration, no new NVS keys, no web-asset changes.

Soak on FDA4 before promoting to `mainstream` (5C88) — and note this one changes what happens to the operator's windows, so it deserves a deliberate decision rather than a routine promote.
