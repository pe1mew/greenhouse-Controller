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

### A drive that T17 joins late is timed from its real start

Found on 2344 after 2.9.1 was committed, and fixed before it was published (operator,
2026-09-17).

T17 can join a drive that is already running:
- **At boot**, T2's recalibration of a window that was not closed has already started by the
  time T17 is up, about 6 s later.
- **After a gate re-open**, T17 joins mid-drive.

T17 timed a drive from its own first look, so rule 2's "~0 in under half the traverse" was
judged on a truncated elapsed time. The leaf reads ~0 about a second before the closed end
sensor makes (the closed-end headroom, plan §2a.6), and that gap then reads as an early stop.
That is what happened after a power cycle with M3 open:
- `param 250`, *"claimed closed after 5 s of a 13 s traverse"*;
- the drive had really run 12 s.

2.8.0 had the same timing; this was the first restart with M3 open under test.

**Fix.** T2 now also records when it energised the relay (`t2_get_drive(ch, &epoch,
&started_ms)`), and T17 times rule 1's grace and rule 2's elapsed time from that moment:
- A drive joined after its grace gets rule 1's verdict on its first sample. The at-end
  exemption still applies to what T17 sees.
- A start older than the longest possible drive is not trusted; T17 then falls back to its
  first look.
- A join more than 1 s late is logged on the serial console.

This fixes the late join for a full close. It does not stop rule 2 from false-tripping on a
CLOSE that starts part-way open (see Known limitations, gh#78).

### Bench builds: a test hook and a fail-first build

- **`POST /api/diag/windowpos {"inject": "none" | "absent" | "fault" | "stuck"}`.** At a moment a test chooses, it makes T17's own reads see the sensor absent (every read times out), faulted (wiper open), or stuck (the position frozen and the rate 0, the way a shorted wiper reads, with the end sensors real).
  - The direct read of the GET and the commissioning path still see the device as it is.
  - Clearing the injection makes a shut gate probe at once.
  - It is RAM only. The GET reports it as `gate.inject`.
- **`-DWPOS_FAILFIRST_GH72`** restores the old behaviours for a fail-first run, including
  the first-look timing. The GET reports it as `gate.failfirst_gh72`, and the source has an
  `#error` for it outside bench builds.
- **`bin/at_wp_gh72.py`** is this release's acceptance test, with the stages `flap`, `stale`,
  `reversal` and `latejoin`.

## Who writes and who reads

**The drive counter and start time (`s_drive_epoch`, `s_drive_start_ms`):**
- **Written** by T2 only, in `relay_ch_open()` and `relay_ch_close()` under `s_state_mux`. Every energisation goes through those two: `ch_start_open()`/`ch_start_close()`, the reversal-gap end in `ch_update()`, and `calib_close_all()`.
- **Read** through `t2_get_drive()` by T17, at every travelling pass. No other caller. Both
  values are read under the same lock, and the start time is in the tick-based
  milliseconds T17 also uses.

**The injection (bench builds):**
- **Written** by T11 (`POST /api/diag/windowpos`).
- **Read** by T17's two read wrappers (`t17_read()`, `t17_ident()`), which carry all of T17's position and identity reads, and by T11's GET (`gate.inject`).

**The counters** (`strokes` with its new meaning, and `redrives`) are written by T17 and read only by the bench diag GET. `bin/at_wp_soak.py`, `bin/at_wp09.py` and `bin/at_wp_gh72.py` consume them.

## Size

|  | 2.9.0 | 2.9.1 | delta |
|---|---|---|---|
| app image | 1 392 336 | **1 393 056** | **+720 B** |
| `.flash.text` | 937 746 | 938 318 | +572 B |
| `.flash.rodata` | 309 884 | 310 028 | +144 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 128 | 42 152 | +24 B |

The late-join fix accounts for 176 B of the image, in the release and the bench build alike. The bench image is 1 407 936 B, including the test hook. The web assets are unchanged from 2.9.0 apart from the version stamp.

## Verification

All on 2344 in the dev rig, with the encoder connected, on bench builds of this code. Every stage was run first on the fail-first build (`-DWPOS_FAILFIRST_GH72`) and then on the normal build (`bin/at_wp_gh72.py`). The diag reported which build each was.

| Stage | Fail-first build | 2.9.1 |
|---|---|---|
| `flap`: the device's own fault injected at rest for 100 s | **FAIL**: the gate read "ok" in 64 of 67 samples after the fault was first seen, and `probe_fail` +3 | **PASS**: 0 of 75 samples "ok", `probe_fail` +1, gate open again within a second of clearing |
| `stale`: sensor absent 1.5 s into a CLOSE, back 2 s into the next OPEN | **FAIL**: the OPEN was not counted (`strokes` +0) | **PASS**: the OPEN was counted as a fresh drive (`strokes` +1), and the mode stayed "timed" throughout |
| `reversal` A: reading stuck, CLOSE reversed to OPEN | **FAIL**: 1 stall (reversed 7.1 s in) | **PASS**: 2 stalls, `redrives` +1 (reversed 9.3 s in) |
| `reversal` B: healthy, same keys | pass, as expected: no stall | **PASS**: no stall, `redrives` +1 (reversed 8.7 s in) |
| `latejoin`: sensor absent at rest, CLOSE from OPEN, gate re-opened 7 s into it | **FAIL**: `early_stops` +1, *"claimed closed after 2 s of a 13 s traverse"* for an 11 s drive (re-opened 8.1 s in, leaf at 441 mm) | **PASS**: no early stop, no stall, `strokes` +1 (re-opened 7.8 s in, leaf at 510 mm) |

- **Which build each stage ran on.** `flap`, `stale` and `reversal` ran before the late-join fix
  was added; `latejoin` ran on the final code, and so did the builds listed under Size. The fix
  changes only when a drive's verdict starts: from T2's energise time instead of T17's first
  look. For a drive T17 sees from its start, that is up to one idle tick (500 ms) earlier.
- **A power cycle did not work as the fail-first test.** The first `latejoin` design
  power-cycled the unit with M3 OPEN. On the fail-first build T17 then joined the boot
  recalibration only about 2.5 s late (about 5.5 s at 17:44), so the old timing did not trip
  and the run passed. The stage now makes the late join on purpose, through the gate re-open
  code path.
- **Overnight soak, PASSED** (`bin/at_wp_soak.py`, 2344, final `2.9.1-bench`, 2026-09-17 19:05:56
  to 2026-09-18 08:12): **13.12 h, 12 judged strokes, `stall_faults`, `early_stops`,
  `rejected_rate` and `err_comm` all 0, no mode change**, the gate in position mode throughout.
  - **Continuous:** the unit booted at 18:57:57 and never again during the window.
  - **The SD log agrees with the counters:** across both log files covering the window, no
    `ALARM ch6` param 249 or 250 row, and no other alarm row at all (no sensor fault, wind
    override or motor alarm). The two `LOG_SYSTEM` 25 rows are the sunset and sunrise day/night
    flips.
  - **The encoder's bus was clean:** 4 728 reads on address 40, no timeout, CRC or framing error.
    Address 1 (the emulated T/RH) timed out 7 times in 1 572 reads, the known out-of-spec
    emulator (gh#68); it raised no sensor-fault alarm.
  - **What it did not exercise:** all 12 strokes fell between 19:05 and 23:35, and nothing moved
    overnight, so the last 8.5 h tested the rest path and the gate's stability, not the
    detectors. Nor did it produce a late join: no reboot and no gate re-open occurred, so that
    path rests on the controlled `latejoin` test above.
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

- **Rule 2 (early stop) is wrong in two cases** ([gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78), found on 2026-09-17 during these tests, to be fixed in 2.10.0).
  - **False alarm on a part-way close.** Position reads 0 for about 1.2 s (7 polls) before the closed end sensor makes, and the rule waits only 2. So a CLOSE that reaches ~0 in under half `travel_m3` without first passing an end sensor is reported as an early stop. On the rig that is a close from below about 58 % open, for example a wind override or LCD reversal while M3 is opening, or a recalibration of a partly open M3.
  - **Blind on a full close.** The open end sensor counts as confirmation, so a full close from OPEN is never judged by rule 2.
  - Neither was seen in a soak, because every close there started at the open end. The rule only logs and counts.
- **A wind override that starts during a long recalibration can be lost** ([gh#79](https://github.com/pe1mew/greenhouse-Controller/issues/79)), and M1 and M2 can then open while it is active. Found by reading the code on 2026-09-17 and not reproduced. All releases have the same code, 2.3.1 included. To be fixed in 2.9.2.
- **A drive shorter than rule 1's grace**, min(5 s, `travel_m3`/2), is not judged. That is unchanged, but a reversal now resets the grace, so reversing a CLOSE within 5 s leaves that CLOSE unjudged.
- **The fault thresholds are validated on the rig only.** Unchanged.
- **The at-end exemption's continuity break is not tested on hardware.** Unchanged. The new `stuck` injection makes that test possible; it is a small item.
- **No DEGRADED threshold for the bus** (gh#66); the rig's addresses 1 and 44 are emulated slaves that are out of spec (gh#68).
- **ROTA re-downloads both artefacts on every deferred apply** ([gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71)).
- **Commissioning is bench-only** ([gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77)).
- **Older comments in `window_pos_task.h`** still describe the first design.
