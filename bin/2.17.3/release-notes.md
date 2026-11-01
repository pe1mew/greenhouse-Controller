# Release 2.17.3

**Date:** 2026-10-31
**Built on:** 2.17.2
**Changes:** Teach and the window size are greyed while the sensor does not answer, and a verdict of "no device" follows a sensor that answers
([gh#105](https://github.com/pe1mew/greenhouse-Controller/issues/105))

**Patch. Bug fixes only:** no setting, key, log row or payload change.

> **Soaked on 2344 (bench build, 12.25 h, PASS), published to `soak` as seq 67, tag `v2.17.3`, on 2026-11-01.** This soak covers 2.17.2 as well: its own was stopped after one session. `mainstream` is not touched.

## What changed

- **Teach and the window size Apply are greyed while the sensor does not answer.** The whole window size row greys, and its field says `unknown`, not `not set`. The reason sits above the card's body, and Abort stays usable. Before, both stayed live right above a line saying no teach was needed: the firmware refused them, but only after the warning that M3 would move.
- **A teach's failure text is red, not green.** Its progress text stays green.
- **T17 re-judges a verdict of "no device" from its own readings.** `commission_tick()` runs the check again when a real reading arrives, at most every 20 s. This one is needed for the greying to be safe. One config read lost on a bus that otherwise works left the verdict standing with the gate open, and it was re-judged only when the gate re-opened, so a healthy sensor could not have been taught. Start already had this problem.
- **New bench injection `noconf`:** the calibration check's config read fails and T17's reads stay real. **New: `bin/at_cal_rejudge.py`**, bench builds only. `-DWPOS_FAILFIRST_GH105` removes the re-judge, bench builds only.
- **`beheerderHandleiding.md`:** a row for UNKNOWN in the verdict table: do not teach, check the bus and the sensor's power.

**Not changed:** the firmware's own refusals, which remain the guard behind the grey.

### Who reads and writes what

Found with grep on 2026-10-31.

| Item | Enumerated |
|---|---|
| `commission_tick()` callers | T17 only: `window_pos_task.cpp` with NULL at 1472 and 1525 (no sensor, the re-judge cannot run), a real reading at 1621 (at rest, every 30 s) and at 1873 and 2014 (strokes, hence the 20 s limit) |
| `cal_reason` readers | `web_server.cpp` 3423, the status. `app.js`: `cm-detail`, `charWhyNot()`, `commSilent()`, new. `bin/at_wp_char_accept.py` and `bin/at_wp_teach.py` |
| `windowpos_read_config()` in the check | One call, now through `read_config()`. T17's other four calls are untouched, so `noconf` reaches only the verdict |
| `#cm-unavailable` writers | `commSetAvailable()` only |
| `cm-window-row`, `cm-window-btn`, `cm-teach-btn` | New ids, `commRender()` only. `commWindow()` and `commTeach()` are unchanged |

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 216 B, sha256 `40dabdd504d557c8…`, version `2.17.3`. Assets `web-assets-2.17.3.zip`, 176 331 B, sha256 `4bf669bd26585831…`. Pushed to 2344 on 2026-11-01, `fw_ver` and `asset_version` 2.17.3; it has run for under an hour in all, and only the bench build was soaked |
| Bench image on 2344 | `56514b5b63e12936…`. `fw_ver` and `asset_version` 2.17.3-bench |
| `at_cal_rejudge.py`, fail-first, bench image `b243e15c…` with `-DWPOS_FAILFIRST_GH105` | FAIL: still unknown / no_device 45 s after the injection cleared, valid only on a manual refresh |
| `at_cal_rejudge.py`, fixed | PASS: held no_device 40 s with the gate ok, then valid 29.0 s after the injection cleared, with no refresh sent. In the card test below, 20 s |
| Card on 2344, operator's browser, `noconf` in force | The reason above the body, the window size row greyed with `unknown`, Teach and Start greyed, both Aborts live, the characterisation's own reason unchanged |
| Card on the mock | Silent, back, not taught, teach failed, arming and 401: each as intended. Not taught keeps Teach and Apply live, and `not set`. A failed teach computes to the red token, arming to the green |
| Wind override during a run (gh#104), `bin/at_char_wind.py` | Added after the commit, no firmware change. Emulated wind set to 3 m/s with the operator's go, then back to REST, `v_max` lowered as the run's second drive began. On 2.17.3-bench `56514b5b`, rebuilt byte-identical from `3559115`: override landed with M3 opening at 37.5 %, the run ended `wind`, M3 CLOSED 20.2 s later. The log has no PART_OPEN row between the drive and the close, so the stop was refused. **Fail-first**, a throwaway build with `ch_stop()`'s check removed: M3 sat PART_OPEN at about 35 % through 40 s of override |
| `bin/check_cfg_desc.py` | 52 keys, 41 published, all agree |

## Soak

2344 on 2.17.3-bench `56514b5b`, 2026-10-31 17:31 to 2026-11-01 05:46, on 5C88's climate through the emulator.

| Check | Result |
|---|---|
| `at_wp_soak.py` | PASS: 12.25 h, 84 strokes judged (2.17.1: 74), stalls, early stops, rejected samples, comm errors, not reached all 0. No mode change |
| `sd_soak.py` | PASS: no reboot, no coredump, card mounted in all 735 samples, largest gap 31 s, no SYSTEM -1 or 33 row |
| Stroke sessions | 12 of 12 ran, none skipped or failed |
| LCD sessions | 12 of 12, 0 failed |
| Law conformance | 24 decisions, all conform to v2. Rule 1 not exercised in this window |
| Hunt | 17 targeted stops, none outside the 31 mm band, no opposite correction |
| Settings | Identical at 12 h |
| Bus, 14 h | Addr 40, the real encoder: 8538 ok, 0 errors. Addr 1 and 44, emulated: 5 and 1 failed polls, never two in a row, no sensor fault |
| Heap | Steady 64 KB free, 29 KB largest block after 12.4 h, no slope. `heap_min_kb` 2, the lowest of any soak so far: see below |
| Stacks | After a full characterisation: T17 1756 B free of 5120, T7 1072 B free of 3072 |
| Characterisation after the report | Complete: 117 starts, 3613 s, band 33 mm (was 31) |
| `at_char_stop.py` after it | Abort PASS, 0.4 s. Sensor lost PASS, 1.6 s |
| `at_cal_rejudge.py` after it | PASS, valid 12.3 s after the injection cleared |

## The ROTA high-water mark on hardware (gh#99, from 2.17.1)

2344, 2026-11-01. Two deliberately bad releases on `soak`, marked as such in their notes, never on `mainstream`.

| Seq | Release | Image | Result |
|---|---|---|---|
| 65 | `2.17.91-gh99`, DEFLATE zip | 2.17.3 | T13 refused it, no reboot. The next check downloaded and tried again |
| 65 | the same offer | 2.17.0 | The same failure, then `dl` 3 at once: the defect, fail-first |
| 66 | `2.17.92-gh99`, `abort()` 5 s into every boot | 2.17.3 | Installed, crashed, rolled back within 25 s, both versions 2.17.3. Every later check `dl` 3, no loop |

The planned step "the same seq with a good zip" could not run: the server does not re-stage a seq it already holds.

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.

## Known limitations

- **`heap_min_kb` fell to 2 KB,** from 15, in two steps at 20:38 and 22:02, each between two 60 s samples, around web sessions. Earlier soaks ended at 4 to 25 KB. It is a low-water mark, not the steady figure (gh#81), and nothing failed, but the cause of these two steps was not found and nothing in this release is known to explain them.
- **The re-judge in a stroke was not forced.** The test runs at rest. In a stroke it costs one more register read per 20 s, and only while the verdict says no device.
- **A verdict of "no device" can be a read lost to a busy bus,** not a silent sensor: `read_config()` does not tell them apart. It now corrects itself within about 30 s, and the card greys for that long.
- **gh#106**, the cookie's `Max-Age`, is reproduced here and fixed in 2.17.4.
