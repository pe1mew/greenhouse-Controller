# Release 2.9.1

**Date:** 2026-09-17
**Built on:** 2.9.0
**Fixes:** [gh#72](https://github.com/pe1mew/greenhouse-Controller/issues/72) (a reversal mid-stroke is not judged), including the two gate-state defects folded into it on 2026-09-17

**Patch.** The fix changes no config key, log encoding or public status payload. Bench builds gain a test hook, two diag fields and a counter.

**What it is.** T17's two fault checks (§12.4 rules 1 and 2) now judge each **drive** of M3, where they used to judge each stroke. The sensor gate also stops reporting a state the sensor is not in. Nothing acts on position yet, so greenhouse behaviour is unchanged. These fixes were required before position may act on M3 (Phase 5, 2.10.0).

## What changed

### Each drive is judged (gh#72)

A **stroke** is M3 away from rest; a **drive** is one energisation of an M3 relay. A reversal puts two drives in one stroke.

T2 reverses M3 immediately for a wind override (T3), a manual LCD command and a recalibration. It inserts a 2 s gap with both relays off, and reports that gap as "moving". Until 2.9.1, T17 therefore saw a reversal as one stroke:
- rule 1 kept the first direction's settled verdict, so the new direction was never judged. That included a wind override closing a window that was opening, which is the case where a leaf that does not follow matters most.
- rule 2 kept the first direction too.

Now:
- **T2 counts its relay energisations** per channel, and exposes the count with its drive state through a new accessor, `t2_get_drive()`. The count lives in the two relay helpers that every drive goes through: a stroke start, the end of a reversal gap, and a CLOSE_ALL.
- **A new count value starts a fresh verdict.** Rule 1's grace is timed from the moment T17 first sees the relay energised, so the gap cannot read as a stall. During the gap nothing is judged and no evidence is gathered.
- **`strokes` counts judged drives.** A soak's "judged strokes" (`strokes - at_end_exempt`) therefore includes both halves of a reversal. A new bench counter, `redrives`, counts the second and later drives of a stroke.

### A sensor that reports its own fault no longer makes the gate flap

When the device answers but its reading carries its own fault (wiper open, or the 65535 sentinel), the idle read shuts the gate. The 30 s re-probe read only the identity registers, which still answer, so it re-opened the gate at once. For as long as the fault lasted, the next idle read shut the gate again and the probe re-opened it. That meant:
- two mode rows every ~30 s;
- one more probe failure counted per cycle;
- a stroke starting in between was promoted to POSITION.

The probe now also reads the position and keeps the gate shut on a faulted reading. That reading also serves the orphan-teach check when the gate opens, so a healthy probe costs no more transactions than before.

### A stroke no longer inherits a stale verdict

A stroke's end is not seen while the gate is shut. So a gate that re-opened during a *later* stroke used to carry the old stroke's direction, start time, settled verdict and peak into it, without a fresh derive or `40002` push either. Now:
- **A shut gate forgets the stroke in progress.** A re-open mid-stroke starts a fresh verdict from that moment.
- **The control mode is promoted only for a stroke that T17 saw start from rest with the gate open.** A re-open mid-stroke, or a first look at a stroke that is already running (a boot recalibration in progress when T17 starts), waits for the next stroke.

### Bench builds: a test hook and a fail-first build

- **`POST /api/diag/windowpos {"inject": "none" | "absent" | "fault" | "stuck"}`.** At a moment a test chooses, it makes T17's own reads see the sensor absent (every read times out), faulted (wiper open), or stuck (the position frozen and the rate 0, the way a shorted wiper reads, with the end sensors real).
  - The direct read of the GET and the commissioning path still see the device as it is.
  - Clearing the injection makes a shut gate probe at once.
  - It is RAM only. The GET reports it as `gate.inject`.
- **`-DWPOS_FAILFIRST_GH72`** restores the three old behaviours for a fail-first run. The GET reports it as `gate.failfirst_gh72`, and a release build refuses to compile with it.
- **`bin/at_wp_gh72.py`** is this release's acceptance test, with the stages `flap`, `stale` and `reversal`.

## Who writes and who reads

**The drive counter (`s_drive_epoch`):**
- **Written** by T2 only, in `relay_ch_open()` and `relay_ch_close()` under `s_state_mux`. Every energisation goes through those two: `ch_start_open()`/`ch_start_close()`, the reversal-gap end in `ch_update()`, and `calib_close_all()`.
- **Read** through `t2_get_drive()` by T17, at every travelling pass. No other caller.

**The injection (bench builds):**
- **Written** by T11 (`POST /api/diag/windowpos`).
- **Read** by T17's two read wrappers (`t17_read()`, `t17_ident()`), which carry all of T17's position and identity reads, and by T11's GET (`gate.inject`).

**The counters** (`strokes` with its new meaning, and `redrives`) are written by T17 and read only by the bench diag GET. `bin/at_wp_soak.py`, `bin/at_wp09.py` and `bin/at_wp_gh72.py` consume them.

## Size

|  | 2.9.0 | 2.9.1 | delta |
|---|---|---|---|
| app image | 1 392 336 | **1 392 880** | **+544 B** |
| `.flash.text` | 937 746 | 938 210 | +464 B |
| `.flash.rodata` | 309 884 | 309 964 | +80 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 128 | 42 144 | +16 B |

The bench image is 1 407 760 B (+1 488 B, including the test hook). The web assets are unchanged from 2.9.0 apart from the version stamp.

## Verification

All on 2344 in the dev rig, with the encoder connected, on bench builds of this code. Every stage was run first on the fail-first build (`-DWPOS_FAILFIRST_GH72`) and then on the normal build (`bin/at_wp_gh72.py`). The diag reported which build each was.

| Stage | Fail-first build | 2.9.1 |
|---|---|---|
| `flap`: the device's own fault injected at rest for 100 s | **FAIL**: the gate read "ok" in 64 of 67 samples after the fault was first seen, and `probe_fail` +3 | **PASS**: 0 of 75 samples "ok", `probe_fail` +1, gate open again within a second of clearing |
| `stale`: sensor absent 1.5 s into a CLOSE, back 2 s into the next OPEN | **FAIL**: the OPEN was not counted (`strokes` +0) | **PASS**: the OPEN was counted as a fresh drive (`strokes` +1), and the mode stayed "timed" throughout |
| `reversal` A: reading stuck, CLOSE reversed to OPEN | **FAIL**: 1 stall (reversed 7.1 s in) | **PASS**: 2 stalls, `redrives` +1 (reversed 9.3 s in) |
| `reversal` B: healthy, same keys | pass, as expected: no stall | **PASS**: no stall, `redrives` +1 (reversed 8.7 s in) |

- **gh#73 still holds on this code** (`bin/at_wpos_fitted.py off`): the unit was switched off for 60 s, then on again. Address 40's count grew by 6. The limit is 6: a bench build's `commission_refresh()` adds 2 transactions when the gate opens, over the 4 a release build shows.
- **Builds:** the release and bench builds compile. The source refuses the fail-first flag without `MODBUS_BENCH` (an `#error`), but no build tried it.
- **Not tested on hardware:** a reversal made by T3's wind override itself (it uses the same T2 path as the LCD reversals tested here), and a real device fault (the injection simulates the wiper-open reading).

## Upgrading

- **No partition change, no NVS migration, no configuration change.**
- **In the SD log, a stroke with a reversal can now carry two rule rows** (`ALARM ch6 param 249/250`), one per drive. The encodings are unchanged, and `logparser.py` needs no change (`logparser.md` 1.21 says so).
- **Soak figures:** `strokes` now counts judged drives, so a reversal adds 2.
- **ROTA does not offer 2.9.1 to a unit that runs a pushed 2.9.1 build**, a bench build included: the version compare ignores the `-bench` suffix. 2344 runs `2.9.1-bench` now.
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting.

## Known limitations

- **A drive shorter than rule 1's grace**, min(5 s, `travel_m3`/2), is not judged. That is unchanged, but a reversal now resets the grace, so reversing a CLOSE within 5 s leaves that CLOSE unjudged.
- **The fault thresholds are validated on the rig only.** Unchanged.
- **The at-end exemption's continuity break is not tested on hardware.** Unchanged. The new `stuck` injection makes that test possible; it is a small item.
- **No DEGRADED threshold for the bus** (gh#66); the rig's addresses 1 and 44 are emulated slaves that are out of spec (gh#68).
- **ROTA re-downloads both artefacts on every deferred apply** ([gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71)).
- **Commissioning is bench-only** ([gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77)).
- **Older comments in `window_pos_task.h`** still describe the first design.
