# Release 2.17.2

**Date:** 2026-10-31
**Built on:** 2.17.1
**Changes:** a characterisation that ends early stops M3, and the card no longer calls an unreadable sensor untaught
([gh#104](https://github.com/pe1mew/greenhouse-Controller/issues/104), [gh#105](https://github.com/pe1mew/greenhouse-Controller/issues/105))

**Patch. Bug fixes only:** no setting, key, log row or payload change.

> **Not soaked, not published.** Both changes ran on 2344 or the mock, as below. 2.17.1 is the soaked base and was never published.

## What changed

- **A run that ends early stops its own drive** (gh#104). `run_end()` now sends `CMD_STOP` while the run's drive is starting or under way. Before, the drive carried on: to its target, or with the position lost to the end on T2's timer. On 5C88 on 2026-10-31 a target of 20 % ran open for 118 s, ended only by a false wind override. This also covers the Abort button.
- **The safety paths are unchanged.** `ch_stop()` refuses a drive that is not `SRC_OPERATOR_MANUAL`'s, so a wind close, a recalibration or T6 cannot be stopped this way. An `m3_busy` end sends nothing, because the drive is someone else's.
- **M3 now rests PART_OPEN after such a run**, with STANDBY still held. Logging out recalibrates, as the card says.
- **The card says "not answering" when the sensor does not answer** (gh#105). It used to say "not taught ... Run a teach first", which invited a re-teach of a good calibration on a bus that had just failed. Reasons a teach can cure keep the old text.
- **The run's failure text is red, not green** (gh#105).
- **New: `bin/at_char_stop.py`**, the acceptance test for gh#104. Bench build only.

**Not changed:** a T6 target that loses the position still finishes on the timer, at the end, by design. Whether that is the right place to finish when opening is a separate decision, recorded on gh#104.

### Who reads and writes what

Found with grep on 2026-10-31.

| Item | Enumerated |
|---|---|
| `CMD_STOP` producers | T8 `ui_display.cpp` (LCD hold, two sites); T17 `characterise.cpp` `run_end()`, new. Consumer: T2 `ch_stop()` |
| `#ch-hint` writers | `app.js`, two: the card's text for a failed run, and "Not started" when a start is refused. Both are failures, so the class is fixed |
| `charWhyNot()` callers | The card's render, once. `c.cal_reason` is the same field `cm-detail` already reads |

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 168 B, sha256 `e97a4b5fa59e3e07…`, version `2.17.2`. Assets `web-assets-2.17.2.zip`, 175 167 B, sha256 `77bc1d0901bcc36a…`. **Built, not run on hardware** |
| Bench image on 2344 | `cdfbfb618ee66720…`. `fw_ver` and `asset_version` 2.17.2-bench |
| `at_char_stop.py abort`, fail-first on 2.17.1-bench | FAIL: M3 ran on from 27.5 % to 78.8 %, resting 5.7 s after the abort |
| `at_char_stop.py absent`, fail-first on 2.17.1-bench | FAIL: M3 ran on to the OPEN end, 17.6 s after the injection. **This reproduces 5C88's failure on the rig** |
| `at_char_stop.py abort`, fixed | PASS: M3 rests PART_OPEN 0.5 s after the abort, at 35.7 % |
| `at_char_stop.py absent`, fixed | PASS: M3 rests PART_OPEN 0.7 s after the injection, at 26.7 %. Unit back in AUTOMATIC, M3 closed, gate ok |
| Card text on the mock | `no_device` gets the new text; `not_taught` and `span_narrow` keep theirs; a running run still takes priority. `#ch-hint` computes to the red token |
| `bin/check_cfg_desc.py` | 52 keys, 41 published, all agree |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.

## Known limitations

- **Not soaked, not published.**
- **A wind override during a run was not forced.** That T2 will not stop a safety close rests on `ch_stop()`'s check, from gh#93.
- **The new card text was checked on the mock.** On 5C88 the screenshot shows the firmware sends `no_device` in that state, but the text itself has not been seen on hardware.
- **gh#98 and gh#99 from 2.17.1 are still open:** the logged-in check on the rig, and the publish test.
