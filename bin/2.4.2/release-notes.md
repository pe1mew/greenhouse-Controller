# Release 2.4.2

**Date:** 2026-09-10
**Built on:** 2.4.1
**Closes:** gh#51 (motor travel/dwell changes only took effect after a reboot)

## Why a patch, not a minor

The CLAUDE.md heuristic asks: a user-visible feature, a new task, a new NVS namespace/key, or a payload-shape change? **None of those.** No new setting appears; an existing one starts behaving the way the manual has always described. `T2_NOTIFY_CFG_CHANGED` is an internal notification bit, not a payload shape. Web assets carry no changes.

## The defect

`beheerderHandleiding.md` states twice that motor travel and dwell times apply to the next movement — the reboot table at `:249` (*"Nee — geldt voor volgende beweging"*) and `:1479` (*"nieuwe waardes worden direct toegepast op de volgende beweging"*). Neither was true.

`apply_config_update()` clamped the value, wrote NVS, and updated T4's in-RAM shadow. It never told T2, which caches travel and dwell per channel in a one-time init loop at task entry and re-reads NVS nowhere. Every motion path used that cached copy, including `calib_close_all()` — so the one operation that looks like a re-initialisation was not one.

The structural tell: **`relay_controller.cpp` was the only task in the firmware that never called `dm_cfg_snapshot()`.** T6 and T3 snapshot once per loop iteration, which is exactly why climate and wind parameters genuinely were live and these were not.

### Why it mattered more than a stale display

Three consequences, in increasing order of seriousness:

1. **The GUI asserted the new value with a success tick.** `GET /api/config` serves `travel_s` from T4's shadow, already updated; the ✓ on the Apply button confirms only the NVS write. Nothing distinguished "in force" from "pending reboot".

2. **Commissioning silently invited over-setting.** The manual's own troubleshooting row (`:1536`) says *"Travel-time te kort? → travel verhogen"*. Raise it, see ✓, re-test, observe no change — and the natural next step is to raise it again.

3. **The wind-safety close used the unapplied value.** T3's `CMD_CLOSE_ALL` drives the same travel pulse. An operator raising `travel_m3` because M3 was not fully closing had every reason to believe the safety close was fixed; it kept the old short pulse until the next reboot. And because T2 marks a channel CLOSED on travel-timer expiry rather than on position, the GUI, the LCD and the SD log all reported `CLOSED` for a window that had stopped short.

## The fix

T4 posts `T2_NOTIFY_CFG_CHANGED` on any `motor` namespace write; T2 consumes it at the top of its next loop pass and re-reads NVS via a new `load_motor_timings()`.

- **`load_motor_timings()` touches only the three timing fields.** Channel state and the three deadlines stay in the boot path. They lived in the same loop before this release only because that loop ran exactly once — folding them into a live refresh would discard window state and cancel a running travel or dwell timer mid-stroke. The function's `@warning` says so, at length, deliberately.
- **In-flight strokes are unaffected.** `relay_deadline_ms` is computed once when a stroke starts, so a new value first governs the *next* stroke. That is what the manual describes, and it falls out of the existing structure rather than needing logic.
- **A task notification, not a Q1 message.** `process_command()` discards every Q1 message while `EG1_BIT_MOTOR_ALARM` is set (FR-MA03), so a change made during a motor alarm would have been dropped silently and stayed stale until reboot. Q1 is also 8 deep and can back up behind the blocking CLOSE_ALL. The notification bit has neither failure mode, survives the calibration, and coalesces repeated changes.
- **`now_ms` is sampled after the refresh**, because `load_motor_timings()` performs nine NVS reads and the alarm debounce compares against that timestamp. The file already showed this discipline — it re-samples after `handle_alarm_clearance()` for the same reason.

Having T2 call `dm_cfg_snapshot()` was rejected: it would put a 200 ms-timeout mutex acquisition and a large struct copy into the relay task, which runs at priority 6 above T4/T5 and must react to wind-safety commands.

## Verification — fail-first, on hardware (FDA4, 2026-09-10)

No serial or LCD access was needed. The instrument exploits the only network-reachable path that produces a full-travel stroke: `POST /api/mode` standby → automatic makes `dm_set_standby(false)` post `CMD_RECALIBRATE`, and `calib_close_all()` energises all three channels unconditionally using `s_ch[ch].travel_ms`. Window states are in `/api/status`, so polling at 1 Hz measures the pulse the firmware actually used. Measurements read ~2 s short of the true duration because the poll baseline starts after the mode POSTs; the offset is constant across runs.

| Run | Firmware | `travel_m1` in NVS | T2's boot-loaded value | Measured M1 pulse | Reading |
|---|---|---|---|---|---|
| 1 (fail-first) | 2.4.1 | set to **40** | 21 | **24.0 s** (≈26 = 21+5) | T2 used the OLD value — **defect reproduced** |
| 2 | 2.4.2 | set to **21** | 40 | **23.6 s** (≈26 = 21+5) | T2 used the NEW value, no reboot |
| 3 | 2.4.2 | set to **60** | 40 | **63.3 s** (≈65 = 60+5) | Unambiguous — matches neither 26 nor 45 |

Run 1 is the fail-first requirement: the same instrument, against the unpatched build, **failed**. Without it a green run would prove nothing.

Runs 1 and 2 both measure ~24 s — the same number from a defect and a fix, separated only by what NVS held at boot. Run 3 exists because that is too easy to misread: 60 s matches neither the cached 40 (45 s) nor the old 21 (26 s), so only a genuine runtime reload explains it. Run 3 also carries its own control — **M2 (unchanged, 21) stayed at 23.9 s and M3 (unchanged, 13) at 15.7 s in the same sweep.** Only the channel that was written moved.

Post-OTA state verified per the standing rule: `fw_ver` **and** `asset_version` both 2.4.2, read after reboot. `eg1 = 0x0`, mode AUTOMATIC, all channels CLOSED, `travel_s` restored to `[21, 21, 13]`.

## Not fixed in this release

Group A of `design/fixMotorTimingRefresh.md` only. Still open, tracked there and on gh#51:

- **No audit trail for `travel_mX`** — it maps to `LOG_PARAM_NONE`, so no SETPT row is written and T2's boot line is serial-only. The SD log contains no record of travel times at all. Defensible while the value was reboot-only; less so now that it takes effect immediately.
- **LCD IO0 menu case 2** (*"Reset all NVS namespaces + PINs; no reboot"*) still displays "Defaults loaded" while T4's shadow and T2's cache both keep pre-reset values — the same root cause, and broader, since climate and wind shadows are stale too.
- **`cfg_shadow_t.dwell_open_min[]` / `dwell_close_min[]` carry seconds**, are documented as minutes, and are exposed under those names in `/api/config`. Confirmed on live data this release: FDA4 reports `dwell_open_min = [300, 300, 1500]`, and 1500 is `CFG_MAX_DWELL_OPEN_S`.
- **Six wrong dwell tooltip bounds** in `index.html` (three say "Max 600 s", three "Max 300 s"; the limit is 1500).

## Upgrade notes

No configuration migration. No new NVS keys. Existing travel and dwell values are read at boot exactly as before; the only change is that a later write now reaches T2 without one.

Soak on FDA4 before promoting to `mainstream` (5C88).
