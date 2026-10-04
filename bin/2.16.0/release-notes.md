# Release 2.16.0

**Date:** 2026-10-04
**Built on:** 2.15.1
**Changes:** M3 is characterised on the unit itself, and the deadband follows from what it measured
(plan §5e, `design/integrateWindowPositionSensor.md`); T17 is woken at every drive start of M3

**Minor: a new admin capability, a new config key and an NVS record.** An administrator can now run a
**characterisation** of M3 from the Motors card. The unit measures how its own M3 moves and derives the
deadband (the arrival band of a targeted move) from it, instead of the typed 20 mm. **A unit changes
behaviour only once such a run has completed:** until then the band in force is the typed `deadzone_m3`,
as in 2.15.1. The other change in behaviour is that T17 now reads a drive of M3 from its start instead
of up to 500 ms in. On the rig that took short corrections from a median 34 mm past their target to
3.8 mm.

> **Soaked** on the bench build of the same code (12.25 h, PASS). Every acceptance row of plan §5e passed
> on 2344, and the release stages ran on **this image, byte for byte** (see *Verification*).
> **Published to the ROTA soak channel as seq 61** on 2026-10-04, and 2344 pulled it by itself.

## What changed

- **The characterisation run** (Motors → M3 → *Commissioning* → **Characterise M3**, admin-only).
  - **Five phases, about 130 motor starts:**
    1. the cruise speed;
    2. the loss on a reversal;
    3. the shortest pulse that reliably moves the leaf;
    4. AT-WP02 (ten approaches to 50 % from ±15 %);
    5. the band check: a candidate deadband b₀ from those figures, then eight corrections of 1.25 × b.
       It raises b by 25 % (at most four times) until every one comes to rest within it.
  - **The pace:** a rest between motor starts, set on the card for each run (2–300 s, default 30 s:
    at most 120 starts an hour). On 5C88 a run takes about 1.5–3 h.
  - **STANDBY, the teach's way.** The run carries on after its admin logs out, and STANDBY is released,
    with a recalibration, once both have ended. The boer manual now says so.
  - **It ends at once**, commanding nothing further, on:
    - Abort;
    - the wind override, the motor alarm or a recalibration;
    - AUTOMATIC chosen;
    - M3 moved by anything else;
    - a sensor fault or absence, or both end sensors;
    - a drive that does not start or end in time.

    When it cannot start, the card is greyed with the reason above it.
  - **The results** are kept in NVS (`motor/m3char`) and shown on the card. An aborted run keeps the
    last complete run's band.
- **The deadband in force** comes from the new key `motor/deadzone_src_m3`: 1 = measured (the default),
  0 = typed. Measured is the last complete run's band, and the typed `deadzone_m3` until there is one.
  - T2's arrival band, T6's end snap and the law follow the band in force.
  - T17's own fault checks keep the typed value.
- **T17 wakes at every drive start of M3.**
  - **Before:** at rest it slept 500 ms between looks, so a short correction could be past its aim
    before T2 had a single reading of it. The run's band check measured the result: the band rose to
    40–44 mm on the rig.
  - **Now:** T2 notifies T17 from `drive_epoch_bump()`, and T17 waits for T2 to publish the moving state
    (≤ 20 ms; T2 energises the relay first).
- **The run checks its hold before a recalibration**, so choosing AUTOMATIC mid-run is reported as
  that, not as "a recalibration ran".
- **ROTA's quiet gate** waits for a characterisation run as well as a teach.
- **The Motors card:**
  - *M3 control* sits directly under the M3 heading, outside the Linear group;
  - the deadzone gets a *Deadzone source* row and a line saying which band is in force;
  - the commissioning block gets *Characterise M3*.
- **The log:**
  - `ALARM` channel 6 param **253**: the run's figures, one row each, values in 0.1 mm **rounded**;
  - `SETPT` param **57**: `deadzone_src_m3`;
  - both are decoded by `log/logparser.py` (`logparser.md` 1.25).
- **T2's relay pulse** (`CMD_PULSE`) is in every build, for the run. The stop log stays bench-only.
- **Bench builds only, none of it in this image:**
  - the fail-first mask `types/failfirst_216.h`;
  - a motor-alarm injection;
  - a probe of ROTA's apply gate;
  - `bin/at_wp_char_accept.py` and `bin/at_wp_hunt.py`.
- **Version 2.16.0.**

## Verification

| Check | Result |
|---|---|
| Release image | 1 430 864 B, sha256 `cf8e7c807a7efb28…`, version string `2.16.0`, no bench suffix; assets stamped `2.16.0`, STORE |
| The image is the one tested | **byte-identical** to the image the release stages and the wind stage ran on (`cf8e7c80…`) |
| What the image carries | `characterise_tick`, `m3c_band_check`, `wait_at_rest` and `pulse_tick` are in the ELF; none of the bench tools are (no `t2_get_cut`, `t2_bench_inject_motor_alarm`, `ota_client_bench_quiet_gate`, `diag_windowpos_*`, `windowpos_task_inject`; the binary has no `/api/diag/windowpos` or `/api/diag/modbus`) |
| `python bin/check_cfg_desc.py` | **PASS**: 52 keys, 41 published |
| Host tests | `drivers/m3Char` (step 1): the suite reproduces the rig harnesses' figures from the archived data, and `tools/mutation_check.py` catches all 12 broken rules |
| **Release stages** (2344, this image) | refusals, hold (`hold_lost`), Abort in each of the five phases (35 checks), a restart mid-run (an OTA push: there is no reboot route), the band source, and a rollback to 2.15.1 and back: **ALL PASS** |
| Full run (this image) | 111 starts; **band 30 mm**, passing its check in round 1 (worst landing 17.5 mm); AT-WP02 spread 0.70 %, hysteresis −0.04 %; 21 of 21 SD rows match the record. Reversal loss 7.6 / 16.1 mm: outside the plan's 7–15 mm, which the operator restated to 5–17 mm the same evening (nine runs read 5.4–10.9 opening, 11.9–16.1 closing) |
| Wind (this image, the emulator set with the operator's go) | `v_max` dropped below a steady 3 m/s mid-run: the override 27 s later, the run ended `wind`, no start after it: **PASS** |
| Bench stages | the sensor's fault and absence, the motor alarm, M3 driven by something else (`m3_busy`), the ROTA gate shut in 616 of 616 probes during a run: **ALL PASS** |
| Fail-firsts | bit 1 (guards removed) fails the sensor, alarm and wind rows; bit 4 opens the gate in 582 of 582 probes; bit 8 (T17's old sleep) cuts 24 of 24 short stops on their first reading. Bit 2 (no band check) no longer fails: its premise was the latency the T17 fix removed |
| **Soak** (bench build `b3ded03f…`, 2026-10-03 22:13 → 2026-10-04 10:28) | **PASS**: 12.25 h, 20 judged strokes, every fault counter 0, no mode change, gate `position`, no reboot. No stop answered by a correction back to its target (`at_wp_hunt.py`; the soak's ten stops came to rest −7.4 to +9.9 mm from target). 21 of 21 law decisions conform (`law_conformance.py`). The measured 30 mm band was in force at the start and the end. Heap flat: floor 23 KB, no drop |
| Soaked code against this image | the bench build is the same source with `MODBUS_BENCH` added, built the same day; this image is byte-identical to the release image that passed the release stages |
| **ROTA publish** | **verified**: GitHub Release `v2.16.0`, published 2026-10-04 11:45:42. Not a draft or prerelease, marked latest, the tag is on `d6325fb`, all three files downloaded back hash-identical, manifest seq 61. **The channel, verified from the device:** a forced check at 11:59:01, 13 min after the publish, was offered 2.16.0 |
| **ROTA pull on 2344** | **PASS**, from plain 2.15.1 (pushed at 11:42, so that a release could be offered). The forced check's download failed 2 s later with sub-code 2 (SHA-256/size mismatch: the server was still fetching the artefacts), although the admin session had logged out before it began. The unit's own hourly check found 2.16.0 again at 12:55:49, downloaded and verified it in 9 s, and committed the apply at 12:56:11. After the 12:56:16 boot, `fw_ver` **and** `asset_version` read **2.16.0**, bank B, accepted. The next check, 12:56:51, read up to date. M3 came up in Lineair 40 s after the boot (gate `ok`, law `graded v2`, with the matching `MODE` param 54 and 56 rows), all nine rig settings survived, and the band in force is still the measured 30 mm (`deadzone_src_m3` 1, from the run of 2026-10-03 22:11). Heap at 134 s: 69 KB free, 28 KB largest block |

## Upgrading

- **Nothing to do.** The new key defaults to *Measured*, and without a completed run the typed
  `deadzone_m3` stays in force, so a unit behaves as before until an administrator runs a
  characterisation.
- **To use it:** teach the sensor if it is not taught, then *Characterise M3*. Choose the rest from the
  motor's nameplate (30 s, the default, is at most 120 starts an hour). The derived band is in force the
  moment the run completes. *Deadzone source: Typed* brings the typed value back.
- **Units in mode 2 with a fitted, taught sensor:** short targeted moves are read from their start, and
  land closer to their targets.
- **A rollback to 2.15.x** ignores the new key and record, and runs on the typed band. Coming back finds
  both as they were (verified).
- **5C88 (production)** runs 2.3.1 on `mainstream` with no sensor. Nothing changes there until a
  promotion, a fitted and taught sensor (gh#77), and a run.

## Known limitations

- **Measured on the rig only.** The rig's M3 traverses in 13 s at ~139 mm/s; 5C88's takes 176 s at
  ~8.5 mm/s. Every distance quoted here is about 16 times smaller there. The run is what measures 5C88
  itself.
- **On the fast rig the band is near its edge:** one of eight 40 mm corrections came to rest +33.9 mm
  from its target. T2 carries a reading forward only after two readings of the drive, and the second
  comes ~180 ms (~25 mm) after the first. The soak saw no short corrections (a cold night), so its
  no-hunting result does not settle this.
- **"Calibration not valid" was not exercised on hardware.** No bench hook makes the verdict invalid
  short of un-teaching the sensor. It is covered by the code and the mock.
- **The reversal loss scatters by ~5 mm between runs**, closing always the larger: the rig's rope. It is
  reported, and the band is not derived from it.
- **2.16.0's own ROTA apply has not run yet.** The pull was 2.15.1's client applying 2.16.0; what it
  showed of 2.16.0 is the boot and the check after it. 2.16.0's apply gate, which now also waits for a
  characterisation run, was shown shut by bench probes (616 of 616 during a run). Watching it hold off
  a real apply needs the next release, pulled by a unit that runs 2.16.0.
