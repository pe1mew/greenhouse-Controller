# Release 2.17.0

**Date:** 2026-10-10
**Built on:** 2.16.2
**Changes:** on the LCD, a tap of M3's Open or Close drives M3 to the end, and a hold of 2 s or more
moves it until the key is released
([gh#93](https://github.com/pe1mew/greenhouse-Controller/issues/93))

**Minor: a user-visible feature (FR-MM08).** It changes only the LCD's manual-motor screen for M3, which is
admin-only. No setting, key, NVS record, status payload or log encoding changes.

> **Not soaked, not published.** Every acceptance row below ran on 2344: on the bench build of the same
> code, with keys held by T7 itself, and with a fail-first. The release image was pushed and booted.

## What changed

- **M3's keys on the LCD are timed** (status page 6, `#`, the admin PIN, `3`):

  | Press of `1` (Open) or `2` (Close) | What M3 does |
  |---|---|
  | **Released within 1 s** (a tap) | A full stroke, starting at the release |
  | **Released between 1 and 2 s** | Nothing; the LCD shows `Tap <1s: to end` / `Hold >2s: move` |
  | **Held 2 s or more** | Nothing for 2 s (`Hold 2s to move`), then M3 moves until the key is released (`Release to stop`), and stops there, part-open |

  - **The timing is the operator's choice** of three options (2026-10-10). Nothing moves for the first 2 s,
    so a hold can set any amount.
  - **A hold longer than a full traverse ends at the end** by T2's travel timer, and the release changes
    nothing.
  - **M1 and M2 are unchanged:** one press, a full stroke at once.
  - **The refusals are as before:** a motor alarm or a calibration refuses either key, and the wind override
    refuses Open. They are now one check shared by M1, M2 and M3.
- **T7 posts a release event** (`key_event_t.released`, appended). It is sent when the key T7 was tracking
  goes up, lifted or replaced by another key.
  - T8 hands a release to nothing but the M3 hold.
  - Releases, and the repeats of a press being timed, are routed before any other key logic, so the
    transient-message timer cannot swallow the release that ends a move.
  - A press whose release is lost ends when its repeats stop (400 ms).
- **The timing rule is `drivers/m3Hold`,** a pure library with no FreeRTOS, host-tested like
  `drivers/m3Char`.
- **T2 gets `CMD_STOP`** (appended to `cmd_action_t`). It ends a drive the way a pulse does: the relay goes
  off where M3 is, it rests `PART_OPEN` (persisted as unknown, logged as a RELAY row), and the dwell is
  armed for the direction it was moving.
  - **At rest it does nothing.**
  - **M3 only.**
  - T17 records such a drive as *not judged: partial*, as a targeted stop, so a hold is never a fault.
- **Drive ownership:** every drive records the source that last decided where it ends (`drive_src`). Any T3
  command takes the drive, even one that finds M3 already moving its way, and `CMD_STOP` is refused unless
  the drive is the operator's own. So a release can never stop a safety close. T8 also sends no stop while
  the wind override is set.
- **The heartbeat task no longer drains Q2.** That drain was a bring-up leftover, and every 5 s it took any
  key event T8 had not yet received, a release included.
- **Bench builds only, none of it in this image:**
  - `GET/POST /api/diag/key`: hold a key in T7 for a set time; read T8's screen, the hold and its latest
    action;
  - the fail-first `M3H_FAILFIRST_NOSTOP`: T2 ignores `CMD_STOP`;
  - `bin/at_lcd_m3_hold.py`.
- **Version 2.17.0.**

### Who posts to Q1 and Q2, and who reads them

Found with grep on 2026-10-10, as the release rule for a change to a shared queue requires.

| Queue | Producers | Consumer |
|---|---|---|
| **Q1** | T6 (`climate_control.cpp`); T3 (`safety_monitor.cpp`, three sites); T4 (`data_manager.cpp`, the recalibration); T8 (`ui_display.cpp`, one site through `post_manual()`: OPEN, CLOSE and, new, **`CMD_STOP`**); T17's teach (`commission.cpp`) and characterisation (`characterise.cpp`); bench routes (`web_server.cpp`, two sites) | T2 only (`relay_controller.cpp`) |
| **Q2** | T7 only (`keypad_scan.cpp`): press, repeat and, new, release. Bench key holds go through T7's own scan | T8 only (`ui_display.cpp`). The heartbeat task's drain is removed |

**`CMD_STOP` has exactly two call sites, both in T8:** the release of a hold, and leaving the screen mid-hold.
A `key_event_t` is built positionally at three sites (two in T7, one in T8), and all three now name all
three fields.

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 200 B, sha256 `e7d85262234ec6da…`, version `2.17.0`. No bench suffix or routes (`/api/diag/key` absent, 404 on the unit). RAM 20.1 % (65 900 B), flash 68.4 %. Assets stamped `2.17.0`, STORE, sha256 `b4b8f846df2fe0d8…` |
| Host tests, `drivers/m3Hold` | **15 of 15** (UT-M3H-001..015): a tap, the 1 s edge, 1–2 s, a hold, the start by repeat and by tick, a single start, a release at 2 s before the start, lost releases (none, held, moving), a second key, another key's events, no press, the counter wrapping, a close press. Seven mutations of the rule each fail at least one test |
| **Rig acceptance, bench build** (`5256009e…`), `bin/at_lcd_m3_hold.py` | **25 of 25.** Each case: |
| ↳ `tap` | A 300 ms press of Open: FULL, M3 OPEN at 100.0 %. Of Close: FULL, CLOSED at 0.0 % |
| ↳ `mid` | A 1500 ms press: action MID, the LCD showed `Tap <1s: to end` / `Hold >2s: move`, M3 stayed at 0.0 % |
| ↳ `hold` | T8 started each move **2060–2061 ms** into the press. M3 was seen moving 2.18–2.26 s after the key went down, and the LCD went `Hold 2s to move`, `Release to stop`, `where released`. Each release was a MOVE_STOP of the press's length (5005, 3002, 5008 ms), and M3 rested PART_OPEN each time: **12.7 %**, **22.8 %** (+10.1 for 3 s) and **51.6 %** (+28.8 for 5 s), **a ratio of 2.85** for 3 s of drive against 1 |
| ↳ `back` | A 4 s hold of Close: PART_OPEN at 34.3 %, from 51.6 % |
| ↳ `long` | A 21 s hold: OPEN by T2's timer 20.7 s in, with the key still down; the release changed nothing |
| ↳ `m1` | M1 moving 0.11 s after the press (on the press, as before), and the M3 hold recorded nothing |
| ↳ logout | The LCD session logged out (`*`, `*`, `A`, `3`, `3`): the menu's STANDBY hold released and the windows recalibrated |
| The first bench run | 18 of 19. The one failure was the harness: it measured a 3 s hold from CLOSED, where the rig's rope takes up its slack for about the first second of a drive, and M3 stopped PART_OPEN at 0.0 %. The case now sets M3 part-open first and compares the 3 s and 5 s holds |
| **Fail-first**, `M3H_FAILFIRST_NOSTOP` (`babbe4e6…`) | **`hold` FAILS** (4 of 11 verdicts). T8's timing was right (moves 2060–2065 ms in, MOVE_STOP sent), but M3 ran on past the release: MOVING_OPEN at 48.3 % after the 5 s hold, then fully open |
| **Release image on 2344** | Pushed: `fw_ver` and `asset_version` read **2.17.0**, bank B, accepted. AUTOMATIC, Lineair (`graded v2`), position gate ok. Heap at 76 s: 70 KB free, 26 KB largest block. The nine rig settings unchanged |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.
- **A rollback to 2.16.2 or earlier** brings back today's M3 keys (one press, a full stroke), and nothing
  else changes.

## Known limitations

- **Not soaked, not published.**
- **The hold was tested with keys held by T7, not by a finger.** The bench route makes T7 read the key
  from its matrix scan, so every event downstream is T7's own. The physical keypad was not pressed.
- **The safety-close rule was not exercised on the rig.** The rule is that a release never stops a T3
  close. That would need the wind override during a hold, and the emulator is not changed without the
  operator's go. It follows from the code: `drive_src` is taken by any T3 command, and T8 sends no stop
  under the wind override.
- **The release image ran no hold.** It has no bench route. Its LCD code is the bench build's, without the
  hooks.
