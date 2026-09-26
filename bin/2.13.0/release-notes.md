# Release 2.13.0

**Date:** 2026-09-25
**Built on:** 2.12.2
**Fixes:** [gh#86](https://github.com/pe1mew/greenhouse-Controller/issues/86) — mode 2 did not engage until M3 happened to move; [gh#85](https://github.com/pe1mew/greenhouse-Controller/issues/85) — the Motors card could not say why

**Minor**, because the status payload gains two fields. There is **no control-path change**: no task, no
config key, no NVS change, no log encoding, nothing in the ventilation path. Mode 1 and mode 2 behave
exactly as they did in 2.12.2.

> **Nothing in this release has run on a greenhouse.** Production (5C88) is on 2.3.1 and unaffected.

## What changed

### Linear control starts by itself (gh#86)

The law was promoted in exactly one place: at a **stroke boundary**, when a travelling M3 stops. So a
unit with the sensor fitted, answering and taught, and Linear selected, kept running M3 on its travel
time until something unrelated drove the window. On 2344 on 2026-09-25 that was 90 minutes and
counting, because M1 alone was meeting demand and M3 had no reason to move at all.

The comment in the code named the hazard it was guarding: position control must not gain authority
*"underneath a movement already committed to the timer"*. That is real — and **M3 standing still is
outside it** just as surely as a stroke that has ended. The guard was stricter than the thing it
guarded against, and the cost fell on the operator.

The law is now promoted at rest as well, in the poll loop's at-rest branch. Nothing else relaxes:
promotion still cannot fire mid-stroke, demotion is still immediate, gh#72's rule that a gate
re-opening mid-stroke must not promote is preserved, and the two-minute hold-down after a demotion
still lives in `dm_m3_ctrl_mode_eval()`.

### Two operator decisions (2026-09-25)

- **`graded` is the chosen law for mode 2.** It shipped in 2.12.0 as a candidate with the decision
  deliberately left open (`model/closedloop/gradedCandidate.md`). It is now the law, and the contract
  says so.
- **A factory reset starts in mode 2** (`DEF_CTRL_MODE_M3` = 1). A default is read only when the key
  is absent from NVS, so it reaches a unit on a factory reset or a fresh flash and **never through an
  OTA**: an existing unit keeps whatever it was set to. `wpos_fitted_m3` still defaults to 0, so a
  unit with no sensor runs exactly as before — the setting has nothing to act on, and the effective
  mode falls back to timed.

### The card names the condition instead of listing three

When mode 2 was set but not in force, the Motors card printed the same sentence whatever the cause:

> Linear is set but NOT in force: M3 is running on its travel time. It needs a position sensor that is
> fitted, answering and taught, and it resumes by itself once the position is trusted again.

On 2026-09-25 an operator read that on 2344 where the sensor **was** fitted (`wpos_fitted_m3 = 1`),
**was** answering (live position `0`, not the `-1` fault value, with `at_end_sensor` straight from the
encoder's status bit) and **was** taught (`verdict "valid"`, 0…863, 84 % of range) — and went looking
for a fault that did not exist.

The real reason is not in that list. Promotion to position control happens **only at a stroke
boundary**: one place in the firmware, when a travelling M3 stops. The unit had rebooted 90 minutes
earlier to pull 2.12.2, M1 alone was covering demand, and M3 had not moved since. Nothing was wrong
and nothing needed doing.

The card now says which of these it is:

| Condition | What the card says |
|---|---|
| `wpos_fitted_m3` is 0 | *"Position sensor fitted" is set to No, so nothing is read.* |
| the sensor is silent | *the position sensor is not answering … re-probed every 30 seconds.* |
| the sensor reports a fault | *answers but reports its own reading as faulty.* |
| both end sensors made | *both end sensors read as made, which cannot both be true.* |
| still probing after a boot | *still being checked. This clears within a minute of a restart.* |
| a bench build | *position control is disabled by design.* |
| inside the hold-down | *resumes after a short hold-down (about two minutes).* |
| **nothing is wrong** | ***Linear is set and nothing is wrong** … linear control is only taken up at the END of a stroke — it starts by itself the next time M3 moves.* |

### Two fields, because one does not answer it

`windows.M3_ctrl_reason` says why the effective mode is what it is (`setting`, `no_position`,
`held_down`, `resumed`); `windows.M3_pos_gate` says what T17 makes of the sensor (`ok`, `probing`,
`no_sensor`, `bench_build`, `device_fault`, `not_fitted`, `end_sensors`). The pair is what
distinguishes the cases — **`no_position` + `ok` is the "nothing is wrong, waiting for a stroke"
state**, and neither field alone can express it.

Both already existed inside the unit and neither left it: `m3_mode_reason` sat in the status snapshot
and was never serialised, and the gate reason was read into a local in `dm_status_snapshot()` and
dropped. Its only name table lived inside the web server's `#ifdef MODBUS_BENCH` block, so a release
build — the build an operator runs — had no names at all.

### One table per enum

The gate-reason names moved to `windowpos_gate_reason_name()`, beside the enum they name, and the web
server's copy is **deleted**, not duplicated: both the bench diag route and the status payload call the
one function. `dm_m3_mode_reason_name()` does the same for the mode reason. Two hand-maintained copies
of one field list is how gh#57 and gh#64 began.

> **2.13.0 was not published on its own** (operator decision, 2026-09-26). It soaked and passed, but by then the 2.14.0 work had been committed on top of it, and the release script tags the current commit — so a `v2.13.0` tag would have pointed at unsoaked 2.14.0 source. **Everything in 2.13.0 ships inside 2.14.0.** No unit on the soak channel will ever report `2.13.0`; the channel goes from 2.12.2 to 2.14.0.

## Verification

| Check | Status |
|---|---|
| Release image builds | **yes** — RAM 19.9 %, flash 67.3 % (+0.1 pt) |
| Bench image builds | **yes** |
| Every branch of the message, in a real browser | **PASS** — all nine cases rendered by calling the shipped `m3LinearWhyNot()` in the page against `webUiMock`, including the two that no rig will produce on demand (`bench_build`, `held_down`) |
| End-to-end through `handleStatus()` | **PASS** — a synthetic payload renders into `#m3-mode-now` and the group headings flip correctly; with the mode `LINEAR` the sentence is **empty**, as it must be when the setting is in force |
| On hardware, in the exact condition that prompted the issue | **PASS** — 2344, 2.13.0-bench, `"M3_ctrl_mode":"TIMED"`, `"M3_ctrl_reason":"no_position"`, `"M3_pos_gate":"ok"`. That is the "trusted, waiting for a stroke" pair, read from the real unit in the state that produced the misleading message |
| The unit serves the new GUI | **PASS** — `app.js` fetched from 2344 contains the new function |
| No regression on the bench diag route | **PASS** — `GET /api/diag/windowpos` still reports `reason_str: "ok"` after its table was deleted and the shared helper substituted |
| Paired commit | **PASS** — `fw_ver` and `asset_version` both `2.13.0-bench` after the push |
| **gh#86 on the rig — fixed build** | **PASS** — 2344, `bin/at_wp_rest_promote.py`: after the push's reboot the mode was **LINEAR** with M3 **CLOSED at 0.0 % on its end sensor, never having moved** (`reason setting`, `gate ok`) |
| **gh#86 fail-first — bit 4096** | **FAILS AS IT MUST** — a bench build with `-DWPOS_FAILFIRST_212=4096` (`FF212_STROKEONLY`, verified in force: the diag route reported `failfirst_212: 4096`) **stayed TIMED for the full 240 s** with M3 at rest, `reason no_position`, `gate ok` — the defect exactly as the operator met it |
| The test is discriminating | **checked** — a boot sweep that MOVES M3 would promote the mode on the old code too, so the harness requires M3 to start CLOSED on its end sensor and reports INCONCLUSIVE if M3 moves during the window |
| **Overnight soak on 2344, in mode 2** | **PASS** — 2026-09-25 17:43 to 2026-09-26 06:05: **12.36 h, 16 judged strokes**, `stall_faults` / `early_stops` / `rejected_rate` / `err_comm` / `not_reached` / `orphan_aborts` all 0, `mode_changes` 0, gate settled at `position`, **no reboot** (16.0 h uptime spans the window). Linear control was in force all night (`reason setting`, `gate ok`), so gh#86 held. The scripted stroke sessions stopped after 4 of 12 (8 strokes, all clean) because the operator changed `cr_priority` by hand at about 21:00, and the harness stops rather than restoring a value it did not record; T6 made the other 8 judged strokes by itself. Heap: free 67 KB, largest block 25 KB at 16 h; the floor stepped to 6 KB once, at 18:34, and never again |
| Message severity | **PASS** — in a browser: "nothing is wrong" renders muted (`#999`, class `disabled-why why-ok`) while a real fault stays warning-orange (`#ff9800`). A line whose point is that nothing is wrong should not be coloured like an alarm |

## Upgrading

- **The two new keys are always present** with the windows block, like `M3_ctrl_mode` itself — a
  consumer that ignores unknown keys is unaffected.
- `webUiMock/mock_server.py` mirrors both fields, and `POST /api/__mock/m3?gate=…&ctrl_reason=…`
  forces any combination so each message can be rendered without a rig in that condition.

## Known limitations

- The card explains M3 only. M1 and M2 have no position sensor and no linear mode, so there is
  nothing to explain for them.
- `M3_pos_gate` reports T17's view of the sensor, not a diagnosis of the wiring. `no_sensor` means
  the unit asked and got nothing; which of the cable, the slave and the bus is at fault still needs
  the Modbus counters.
