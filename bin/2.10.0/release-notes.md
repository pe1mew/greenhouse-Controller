# Release 2.10.0

**Date:** 2026-09-19
**Built on:** 2.9.2
**Implements:** plan §5d, "confirm only" (Phase 5, first release)
**Fixes:** [gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78) (rule 2: a false alarm on a close from part-way, and blind on a full close)

**Minor.** New log params (`ALARM ch6` 251 and 252), a new gate reason (6) and three new status flags, so the payload changes. No config key, no NVS change.

**What it is.** With a position sensor fitted to M3, the controller now checks whether each drive of M3 got where T2 sent it, and whether `travel_m3` fits the window. **It only reports.** T2 still drives M3 on to its timer and records OPEN or CLOSED, whatever the sensor says; position drives the actuator only in 2.11.0 (mode 2). Units without a sensor (`wpos_fitted_m3` = 0, the default and production's setting) see no change at all.

## What changed

### A verdict on every M3 drive

T17 judges each drive of M3 when T2 ends it, from what it saw during the drive. `t2_get_drive()` (gh#72) gives the drive, its counter and its start; T2's window state afterwards says how it ended.

| Verdict | When | What it does |
|---|---|---|
| **Confirmed** | The target end sensor made after the leaf left its starting end, and the position was within `deadzone_m3` of that end. A drive toward the end the leaf already sits at (the recalibration of a closed M3) is confirmed without leaving | A log row |
| **Not reached** | The drive ran its full timer and the target end was never confirmed | A log row, the flag `m3_not_confirmed` and the web badge *M3 not confirmed*, until the next confirmed drive |
| **Not judged** | The drive ended early (a reversal, or a motor alarm), the sensor was lost or faulted during it, both end sensors were active, or it gave no usable reading | A log row |

- **One `ALARM ch6` param 251 row per drive.** `value_a` is the verdict signed by direction (`+` OPEN, `-` CLOSE): `1` confirmed after a full traverse, `2` confirmed from part-way or at the end, `3` not reached, `4` not judged. `value_b` is the time from relay-on to the target end sensor (0.1 s), the opening where a not-reached drive stopped (0.1 %), or the reason a drive was not judged.
- **Only the target end counts.** The sensor at the starting end stays made for about the first 1.8 s of every full drive, so a make is the target end's only after bit 3 was clear on two readings in a row.
- **Why it matters.** Under-travel is the real risk of a wrong `travel_m3`: the end switch protects against over-travel and does nothing for under-travel. A rig value (13 s) in production (a 176 s traverse) opens M3 about a tenth while the log records OPEN, with no stall and no alarm. That drive is now *not reached*.

### The travel check

On every **confirmed full traverse** (bit 3 made at T17's first look, within two polls of relay-on), per direction, the time from relay-on to the target end sensor is compared with `travel_m3`:
- **too short**, `m3_travel_short`: the end sensor made later than `travel_m3`, so only T2's fixed 5 s margin still carried the drive;
- **much longer than needed**, `m3_travel_long`: it made within half of `travel_m3`. The mechanism does not mind (the end switch cuts the drive), but T17 derives its poll and plausibility limits from `travel_m3`, and those go wrong.

It warns only: an `ALARM ch6` param 252 row when a direction's state changes, including back to "within band", and a flag while it stands. **Nothing changes `travel_m3`.** Bit 3 alone is enough, so the check works even when a wrong `travel_m3` makes every moving position sample implausible.

### Rule 2 (early stop), gh#78

- **Corroboration:** only the CLOSED end sensor making after the leaf left its starting end counts. Before, the open end sensor, made at the start of every full close, switched the rule off for the whole drive.
- **Timing:** a ~0 claim waits up to `travel_m3` / 4 for that sensor, instead of being judged on its second sample. The position reads 0 for about 1.2 s before the closed end sensor makes (the closed-end headroom), so a close from part-way used to false-trip.
- A drive that began and stayed at the closed end corroborates itself.
- The row's encoding is unchanged: `value_a` is the claim's time into the drive, `value_b` `travel_m3`.

### Bit 4 and the start-up bits

- **Both end sensors active (bit 4)** is an end-sensor wiring fault: the gate shuts with reason 6 (`end_sensors`), the sensor fault flag is raised, and drives are not judged until it clears.
- **Start-up bit `0x01`** (no measurement window completed since T17 wrote `40002`, one window long) holds back the **position** of a reading. Bit 3 still counts, and `0x02` (averaging not filled) is not tested: it concerns only the averaged register and lasts 10 s, and refusing readings on it would have made the first drive after every `travel_m3` change unmeasurable.

### Operator surfaces

- **Web GUI:** the badges *M3 not confirmed*, *M3 travel time too short* and *M3 travel time too long* (amber), next to *Window sensor fault*. The LCD is unchanged (plan §6.2).
- **Manuals:** boer manual 1.20 and beheerder manual 1.23 explain the badges and what to check. `beheerderHandleiding:101` no longer says the controller has no position feedback at all. The passages on the CLOSE_ALL re-aligning T2's belief stay true until 2.11.0.

### Bench builds

- **Two injections** for `POST /api/diag/windowpos`: `ends` (both end sensors) and `race` (the position reads 0 whatever the leaf does, gh#78).
- **`-DWPOS_FAILFIRST_292`** restores 2.9.2's behaviour in everything this release changes. The diag reports it as `gate.failfirst_292`, and the source refuses it outside bench builds.
- **The diag's `soak` block** carries `confirmed`, `not_reached`, `not_judged`, `not_confirmed`, `travel_state` and `traverse_ms`.
- **`bin/at_wp_confirm.py`** is the acceptance test, with nine stages. It drives M3 through T6 and recalibrations, so nobody has to be at the rig, and restores what it changes.
- **`bin/at_wp_soak.py`** now also judges `not_reached` (must stay 0).

## Who writes and who reads

- **The verdict state** (`s_drv`) is T17's own. **`s_confirm`** (the flag and the travel states) is written by T17 under `s_mux` and read through `windowpos_task_confirm()` by T4's status snapshot and by the bench diag.
- **The three flags** come out of `build_canonical_status_json()`, the one builder behind `GET /api/status`, the WebSocket push and T14's post to the remote status site. The remote dashboard lives in its own repo and does not show them until it learns them.
- **No queue changes.** Q1, Q4 and T2 are untouched.

## Size

|  | 2.9.2 | 2.10.0 | delta |
|---|---|---|---|
| app image | 1 393 376 | **1 395 584** | **+2 208 B** |
| `.flash.text` | 938 478 | 940 182 | +1 704 B |
| `.flash.rodata` | 310 188 | 310 700 | +512 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 152 | 42 208 | +56 B |

The bench image is 1 410 880 B (+2 624 B). The web assets grow by 637 B, the three badges.

## Verification

All on 2344 in the dev rig, with the encoder connected, on bench builds, with `bin/at_wp_confirm.py`. Every stage was run first on the fail-first build (`-DWPOS_FAILFIRST_292`) and then on the normal build; the diag reported which build each was.

**What the test does.** It moves M3 through T6: it sets `cr_priority` and the active temperature maximum so that T6 wants M3 open or closed, cuts M3's dwells to 5 s, and restores everything it changed. Reversals and drives toward an end M3 already sits at use a recalibration (STANDBY, then AUTOMATIC). It reads the verdicts from the SD log and the counters from the diag.

| Stage | How | Fail-first build (2.9.2 behaviour) | 2.10.0 |
|---|---|---|---|
| `healthy` | A full OPEN and a full CLOSE | **FAIL**: no verdict row, `confirmed` +0 | **PASS**: both confirmed after a full traverse, end sensor at 11.7 s (OPEN) and 12.2 s (CLOSE); no travel row, no flag |
| `notreached` | `travel_m3` 5 for an OPEN, then 13 for a CLOSE | **FAIL**: nothing reported for an OPEN that stopped part-way | **PASS**: the OPEN *not reached*, the leaf at 85.4 %, and the badge raised; the CLOSE confirmed (from part-way, 10.3 s) and the badge cleared |
| `short` | `travel_m3` 10, then 13 | **FAIL**: no warning | **PASS**: "too short" in both directions (11.6 s and 12.2 s against 10 s), cleared in both at 13 |
| `long` | `travel_m3` 171, then 13 | **FAIL**: no warning | **PASS**: "much longer" in both directions (11.6 s and 12.7 s against 171 s), cleared in both at 13. The two rule-1 rows here are the expected side effect of the wrong value |
| `lost` | Sensor `absent` from 3 s into an OPEN | **FAIL**: no verdict | **PASS**: *not judged* (sensor); the drive finished on T2's timer; the gate open again after the re-probe |
| `reversal` | An OPEN reversed by a recalibration 2 s in | **FAIL**: `early_stops` +1 on the part-way CLOSE, gh#78's false trip | **PASS**: the OPEN *not judged* (interrupted), the part-way CLOSE confirmed (4.8 s), `early_stops` +0 |
| `atend` | Recalibration of a closed M3 | **FAIL**: only rule 1's exemption (`at_end_exempt` +1), no verdict | **PASS**: confirmed, already at that end; no stall, no early stop |
| `race` | A full CLOSE with the position reading 0 throughout (inject `race`) | **FAIL**: `early_stops` +0, rule 2 blind on a full close | **PASS**: `early_stops` +1, the row 0 s into the drive. The drive itself was confirmed (12.3 s): at its end the end sensor and the position agreed |
| `ends` | Both end sensors (inject `ends`) during an OPEN | **FAIL**: the gate stayed "ok", no sensor fault, no verdict | **PASS**: gate reason `end_sensors`, the sensor fault flag raised, the OPEN *not judged* (bit 4); the gate open again once the injection was cleared |

- **The first normal run failed two stages, and both were fixed before this one.**
  - `long` warned for the CLOSE only. A drive counted as a full traverse only if the first position reading showed the leaf at the far end, and with `travel_m3` 171 every moving sample is rejected, so the OPEN never qualified. **Firmware fix:** the start is now judged on bit 3 at T17's first look, which the end sensors give whatever the position does.
  - `atend` recalibrated an M3 that was opening: the previous stage had left T6 wanting it open. **Harness fix:** every stage now sets T6's wish before it moves M3.
- **The verdict rows match the counters.** Over the final run's boot the SD log holds 17 *confirmed*, 1 *not reached* and 3 *not judged* rows, the same as the diag's counters. `logparser.py` decodes every one of them, and the travel, early-stop and mode rows (gate reason 6 included).
- **The traverse on the rig:** OPEN 11.6 to 11.7 s and CLOSE 12.1 to 12.3 s at `travel_m3` 13, in all 9 full traverses of this run (4 OPEN, 5 CLOSE) and in the first run's 7. The measurement is made at T17's poll cadence (`travel_m3` / 150, at least 100 ms), which is why the CLOSE read 12.7 s at `travel_m3` 171, where one poll is 1.1 s.
- **Which build was tested.** Normal: the bench build of 11:11, pushed to 2344 at 11:14. After that only a comment changed, and **the final source rebuilds to the 11:11 images bit for bit**, release and bench, web assets included (SHA-256: release `0432ca50…23fb6`, bench `607158db…9338f`). Fail-first: a bench build of 10:07, before the last three refinements (the start-up bit narrowed to `0x01`, the debounce on leaving the start end, the full-traverse start on bit 3). All three are compiled out of that build or feed only code that is compiled out, so they cannot change its result. That rests on reading the code: the fail-first image was not rebuilt.
- **The run had to wait for the clock.** The unit's boot SNTP sync failed, and so did the first two retries; it synced 15 min after boot. The test refuses to run without it, because rows written before the sync may carry the DS1307's time (memory/gotcha-log.md, 2026-09-19). The likely cause is the rate limit that repeated pushes provoke (gotcha log, 2026-07-13): this was the third push to 2344 in 57 minutes. Nothing in this release touches the network.
- **Builds:** the release and bench builds compile. The source refuses the fail-first flag without `MODBUS_BENCH` (an `#error`), but no build tried it.
- **Soak, PASSED** (`bin/at_wp_soak.py`, 2344, `2.10.0-bench`, 2026-09-19 11:49:25 to 2026-09-20 08:54): **21.09 h, 19 judged drives, `not_reached` 0, `stall_faults`, `early_stops`, `rejected_rate` and `err_comm` all 0, one mode change**, the gate in position mode throughout.
  - **Continuous:** the unit booted at 11:14:28 and not again; the window holds no BOOT row.
  - **The one mode change is the promotion** from timed to position at the first drive after the acceptance run, which is where a promotion is allowed to happen.
  - **The SD log agrees with the counters.** It holds exactly 19 param 251 rows, every one *confirmed after a full traverse* (9 OPEN, 10 CLOSE), and **no param 249, 250 or 252 row at all**. The only other alarm rows are the encoder's own status rows (param 246, 38 of them) and the single mode row.
  - **The traverse stayed inside the band:** OPEN 11.7 to 12.0 s, CLOSE 12.3 to 12.5 s against `travel_m3` 13. The slowest CLOSE leaves 0.5 s, and no travel warning fired.
  - **How the drives were made.** `bin/at_wp_strokes.py` ran 8 sessions 75 min apart, from 11:57 to 20:44, none skipped and none failed. Each drives M3 closed, open and closed through T6 by setting the active temperature maximum and `cr_priority`, with M3's dwells cut to 5 s for the session and every setting restored afterwards. T6 added one drive of its own at 20:17, opening M3 for the emulated night humidity; it was confirmed like the rest.
  - **The bus was clean:** address 40 had **0 failures in 6 554 reads** in the hourly rows. Address 44 (the emulated wind) 0 in 4 878; address 1 (the emulated T/RH) 15 in 2 454, the known out-of-spec emulator (gh#68). Neither raised a sensor fault.
  - **Heap** (gh#81, unchanged by this release): free heap per minute had a median of 69 KB and a lowest of 43 KB over 1 266 samples, with the largest block never under 17 KB. The low-water mark since boot fell from 14 KB at 12:16 to 11 KB at 20:10 and stayed there, while the per-minute free heap stayed flat.
  - **What it did not exercise.** Nothing moved between 20:44 and 08:54, so the night tested the rest path, not the detectors. The window contains no reversal, no sensor fault and no drive that fell short, so every failing verdict rests on the acceptance run above.
- **Not tested on hardware:**
  - **A real bit 4** (injected only) and **a drive ended by a motor alarm** (*not judged*, reason 2: needs the RRK-3 alarm input).
  - **The `stuck` wiper** on this release. By design rule 1 reports it, and the verdict is *not reached*.
  - **Production's timing.** 5C88 has no sensor.

## Upgrading

- **No partition change, no NVS migration, no configuration change.**
- **In the SD log:** a param 251 row per M3 drive, and a param 252 row when the travel check changes state. `logparser.md` 1.22 decodes them; `plot_daily.py` reads only the 240-243 band and needs no change.
- **A unit without a sensor sees nothing new.**
- **On the ROTA soak channel since 2026-09-20, seq 53** ([v2.10.0](https://github.com/pe1mew/greenhouse-Controller/releases/tag/v2.10.0)). **2344 pulled it on the first offer.**
  - **2.9.2 was pushed back to 2344 first.** ROTA ignores the `-bench` suffix, so a unit running 2.10.0-bench counts as up to date against 2.10.0.
  - Published at about 08:59; the server began offering it between 09:09:46 and 09:10:46, and the unit rebooted into it at 09:10:54. The download succeeded on the first attempt (`dl` 0) and the apply was not deferred, the checking session being exempt from the quiet gate.
  - **Verified after the reboot:** `fw_ver` and `asset_version` both 2.10.0, `travel_m3` still 13, `wpos_fitted_m3` still 1, M3's position and end sensor in the status, no flags.
  - **The release build judges drives as the bench build does.** A recalibration at 09:12:50 promoted the gate to position and wrote `ALARM ch6` param 251 `a=-2 b=0` — *confirmed, already at that end*. `GET /api/diag/windowpos` answers 404 there, as a bench-only route should.
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting, and settle gh#81 (the heap floor) first.

## Known limitations

- **The travel check's thresholds and the verdict are validated on the rig only.** Production's traverse to the end sensor is unmeasured until 5C88 has a sensor (gh#77).
- **The rig's margin to "too short" is thin.** The end sensor makes 11.6 to 11.7 s (OPEN) and 12.1 to 12.3 s (CLOSE) into a 13 s `travel_m3`, so a CLOSE 0.7 s slower than the slowest seen would warn.
- **A recalibration of a closed M3 whose closed end sensor is not made at rest** would read *not reached*. That happened on the rig only with a detached wire (AT-WP09).
- **The verdict and the flags are RAM only**; a restart clears the badge.
- **The internal-heap floor** ([gh#81](https://github.com/pe1mew/greenhouse-Controller/issues/81)), and **a DS1307 that steps** ([gh#55](https://github.com/pe1mew/greenhouse-Controller/issues/55)), are unchanged.
- **No DEGRADED threshold for the bus** (gh#66); the rig's addresses 1 and 44 are emulated slaves that are out of spec (gh#68).
- **Commissioning is bench-only** ([gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77)).
