# LCD control of M3: tap to the end, hold to move (gh#93)

**Status:** implemented in 2.17.0 and verified on 2344, 2026-10-10. Host 15 of 15; rig 25 of 25 on the bench build; the
fail-first fails. Results: `bin/2.17.0/release-notes.md`. A minor version (a user-visible feature).
**Requirement:** [gh#93](https://github.com/pe1mew/greenhouse-Controller/issues/93), operator, 2026-10-10.

## 1. What the operator gets

On the LCD's manual-motor screen (status page 6, `#`, the admin PIN, then `3`), the keys `1` (Open) and `2`
(Close) act on M3 as follows. M1 and M2 keep one press = a full stroke.

| Press | What M3 does |
|---|---|
| **Released within 1 s** (a tap) | A full stroke to that end, starting when the key is released |
| **Released between 1 and 2 s** | Nothing. The LCD says: tap under 1 s, hold 2 s or more |
| **Held 2 s or more** | Nothing for the first 2 s. Then M3 moves until the key is released, and stops there, part-open |

The timing, a move starting only after 2 s, is the operator's choice of three options (2026-10-10). It gives
any amount of movement, at the price of a 2 s wait before a hold moves.

**Unchanged:**
- **Refusals:** a motor alarm or a calibration refuses either key, and a wind override refuses Open, with the
  reason on the LCD.
- **The session hold** (gh#65): the menu holds STANDBY, so T6 leaves the positions alone until the admin
  session ends. Then the windows recalibrate and T6 resumes.
- **Dwell:** a manual command bypasses T2's dwell timers.

**New guarantees:**
- **A hold longer than a full traverse ends at the end,** by T2's travel timer, like a full stroke. The
  release then finds M3 at rest and does nothing.
- **Releasing the key never stops a safety close.** If T3 takes M3 during a hold, the release leaves its
  close alone. T8 also sends no stop while the wind override is set.

## 2. Design

### 2.1 T7: a release event

T7 posts a first-press event, then a repeat every 100 ms once the key has been held 500 ms. It has never
said when a key goes up. A hold has to stop at the release, so T7 now posts one: `key_event_t` gains a field,
`released`, **appended**, so every existing initialiser leaves it false. T7 posts a release whenever the key
it was tracking goes away: lifted, or replaced by another key.

**T8 hands a release to no handler except the M3 hold,** before any other logic: a release is not a press
anywhere else, and the message timer must not swallow it. When a message expires and T8 drains Q2, any
release in the drain is still handed to the hold.

**The heartbeat task's Q2 drain goes.** It is a bring-up leftover, and every 5 s it took any key event that
T8 had not yet received, a release included.

### 2.2 The timing: `drivers/m3Hold`

The rule is in a pure library, so that its corner cases are host-tested, as with `drivers/m3Char`. It has no
FreeRTOS and no ESP-IDF, and its times are milliseconds that may wrap. T8 feeds it four events, and each
returns at most one action, together with the direction of the press it belongs to:

| Event | Returns |
|---|---|
| `press(key, opening, now)` | nothing. A press while another is being timed ends that one first (a lost release), and returns its action |
| `repeat(key, now)` | the move's start, when the hold has reached 2 s |
| `release(key, now)` | a full stroke (a tap), nothing-with-a-reason (1–2 s), or the move's stop |
| `tick(now)`, every T8 loop | the move's start at 2 s, and a lost release: no event for 400 ms after the expected repeat, or 900 ms after a press with no repeat. That press is ended at its last sign of life |

A release at or after 2 s that arrives before the start was issued ends with nothing. The press is over, and
the move it would have made is zero.

### 2.3 T2: `CMD_STOP`

The Q1 actions gain `CMD_STOP`, **appended**. It stops M3 where it is:
- **The ending:** the relay goes off, `PART_OPEN` follows, persisted as unknown and logged as a RELAY row,
  with the dwell armed for the direction it was moving. A pulse's ending is the model.
- **At rest it does nothing.** An end reached by the timer is at rest.
- **M3 only.** It is refused for M1 and M2, whose control has no part-open state.

**Ownership:** every drive records the source that last decided where it ends (`drive_src`). A command from
T3 that touches the channel takes it, even one that finds M3 already moving that way. `CMD_STOP` is refused
unless the drive is the operator's own. A safety close therefore cannot be stopped by a release.

T17 already treats a drive that ends in `PART_OPEN` as **not judged: partial** (`NOTJ_PARTIAL`). A stop on
release is neither a fault nor an early stop.

### 2.4 T8

On the M3 action screen, a first press of `1` or `2` is timed instead of acted on. Each action is carried out
like today's command, with the same refusals and `SRC_OPERATOR_MANUAL`. A hold's start and stop show on the
screen itself and not as a transient message, because a message discards keys:

| State | LCD |
|---|---|
| A press being timed | `[M3] CLOSED` / `Hold 2s to move` |
| Moving | `[M3] MOV>` / `Release to stop` |
| After the stop | message `M3 stopped` / `where released` |
| A 1–2 s press | message `Tap <1s: to end` / `Hold >2s: move` |
| A tap | today's `M3 opening` / `command sent` |

### 2.5 Testing on the rig: `GET/POST /api/diag/key` (bench builds only)

`POST {"key":"1","ms":3000}` makes T7 read that key from its matrix for 3000 ms. Press, repeats and release
then come from T7's own code, exactly as from a finger. `GET` reports whether a key is being held, and T8's
state:
- the screen and the status page;
- the motor picked;
- the session;
- whether a message is up;
- the hold;
- the last action, with its press length.

A harness can therefore walk the menus, enter the PIN and judge each case.

## 3. Verification

| Check | Pass |
|---|---|
| Host: `drivers/m3Hold` | Every rule case passes, and each mutated rule fails a test. The rule cases: a tap, the 1 s and 2 s edges, a hold, a lost release with and without repeats, a second key, wrap-around |
| Rig, `bin/at_lcd_m3_hold.py`, 2344, bench build | A tap of Open or Close: a full stroke, OPEN or CLOSED. A 1.5 s press: no movement. Holds of 3 s and 5 s: `PART_OPEN`, the 5 s hold three times further than the 3 s hold's single second. A close hold moves back. A hold past the traverse: OPEN by the timer. M1: unchanged. Logout: the windows recalibrate |
| Fail-first | `M3H_FAILFIRST_NOSTOP` (T2 ignores `CMD_STOP`) fails the 3 s hold: M3 runs to OPEN |
| Release image | Byte-identical to the image tested, no bench routes; an idle check after the push |

## 4. Documents

- **FRS:** a requirement for the LCD's M3 control.
- **TSDS:** T7's release event, T8's M3 hold, T2's `CMD_STOP` and drive ownership, the Q1 and Q2 tables.
- **Manuals:** the beheerder manual's manual-motor section. The menu is admin-only, so the boer manual only
  needs the change where it describes the menu.
- **Release:** the changelog and the release notes.
