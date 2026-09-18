# Release 2.9.2

**Date:** 2026-09-18
**Built on:** 2.9.1
**Fixes:** [gh#79](https://github.com/pe1mew/greenhouse-Controller/issues/79) (a wind override that starts during a long recalibration can be lost)

**Patch.** The fix changes no config key, log encoding or public status payload. The bench build differs from the release build only in its version string and the bench-only code 2.9.1 already had.

**What it is.** T2 does not read its command queue, Q1, while it recalibrates the windows: a blocking CLOSE_ALL sweep that lasts up to 176 s in production. T6 kept posting into the queue during the sweep. T3 posted the wind override's CLOSE_ALL once, without checking whether the queue took it. An override that began once the queue was full was therefore lost. 2.9.2 pauses T6 during a sweep, makes T3 retry, and makes T2 time each command from the moment it is drained. The rule was already in the design: T2's own comment and the TSDS both say T6 holds while `CALIBRATING` is set. The code did not.

## What changed

### T6 pauses while T2 recalibrates

- **`EG1_BIT_CALIBRATING` joins T6's inhibit mask** (`climate_control.cpp`), next to the wind override, the motor alarm, a temperature-sensor fault and STANDBY.
- **T2 sets the bit for the whole of `calib_close_all()`**, which runs in three places:
  - at boot, when the saved window state is not all CLOSED;
  - for `CMD_RECALIBRATE`, which T4 posts on every STANDBY exit: switching back to AUTOMATIC on the LCD or in the web GUI, or the end of the last session that held STANDBY (the LCD menu, a teach);
  - after a motor alarm clears, following a 60 s guard. T6 posts nothing during the guard even unpaused, because the alarm leaves every window UNKNOWN, and T6 commands only a window it knows to be open or closed.
- **Like every inhibit, it resets T6's steps to 0 when it begins.** That changes a decision in one corner only. When the temperature or humidity sits at or just below its maximum, inside the hysteresis band, 2.9.1 could hold the first step and reopen M1 after a sweep. 2.9.2 leaves M1 closed until the value rises above the maximum again, as it already did after every other inhibit.
- **T6 resumes at its first wake after the sweep**, up to one poll interval later:
  - after a STANDBY exit, 2.9.1 acted at that same wake, because its queued commands were deferred until then (see below);
  - after a boot sweep or a motor-alarm recovery, 2.9.1 ran the commands T6 had queued during the sweep at once, so windows now reopen up to one poll interval later there.

### T3 retries the override's CLOSE_ALL

- **Was:** one `xQueueSend(Q1, …, 0)` on the safe-to-unsafe edge, with the result unchecked. If Q1 was full, the command was gone, and T3 posts nothing more while the override lasts.
- **Now:** `post_close_all()` returns whether Q1 took the command.
  - A refused send sets `close_pending`. The loop head retries it on every pass, including T3's 2 s watchdog wakes, until Q1 accepts it or the override ends (clearance, or wind protection switched off).
  - The retry sits before the "no new sensor data" `continue` on purpose, so it runs every 2 s rather than once per sensor reading.
  - T3 still never blocks on Q1.
- **The serial console** logs the refusal, and the number of retries once the command is accepted.

### T2 times each command from the moment it is drained

- **The Q1 drain** (`relay_controller.cpp`, step 4c) now reads the tick count for each command. It read it once per loop pass.
- **Why that mattered.** `CMD_RECALIBRATE` runs the sweep inside `process_command()`. Every command drained after it in the same pass was therefore timed with a clock read up to 176 s earlier:
  - **A dwell deadline set during the sweep looked that much further away**, so a `SRC_T6` command was deferred. This is what hid gh#79's harm (next section).
  - **A command that bypasses the dwell got a drive deadline in the past.** That covers `SRC_OPERATOR_MANUAL` from the LCD or a teach leg, and `SRC_T3`. The drive would have ended on the next 20 ms tick, with T2 recording the window at the end it was sent to: the under-travel failure. This is inferred from the code, not observed, and it was narrow:
    - the LCD refuses a command while `CALIBRATING` is set;
    - a teach leg does not start while M3 moves;
    - a CLOSE to a window that is already CLOSED is a no-op.
- **The motor-alarm path already re-read the clock** after its own sweep, and the boot sweep runs before the loop first reads it. Only the STANDBY-exit sweep had the stale clock.
- **Safe only together with the T6 pause.** With a fresh clock and no pause, T6's stale OPENs after a STANDBY-exit sweep would have run: the harm the stale clock had been hiding.

### What the fail-first test found: gh#79's harm was masked on one of the three sweeps

gh#79 was found by reading the code on 2026-09-17. It predicted that after the sweep, T2 would run T6's stale OPENs, and M1 and M2 would open while the override was active, with nothing to close them until the wind dropped.

- **The first fail-first run on 2.9.1 did not show that.** It used the STANDBY-exit sweep, which is the only one a test can start over the network. M1 and M2 stayed closed.
- **The SD log showed why.** At the sweep's end T2 wrote three `LOG_SYSTEM 29` rows ("T6 move deferred on the dwell timer"):
  - M1 and M2 with 26 s remaining, which is their travel plus margin;
  - M3 with 476 s remaining, which is the 176 s sweep plus its 300 s close dwell on 2344.
  - Those are T6's stale OPENs, deferred because of the stale clock above.
- **The masking is specific to the STANDBY exit.** The boot sweep and the motor-alarm recovery read a fresh clock after the sweep. On 2.9.1 the stale OPENs would run there, and the prediction stands. That is inferred from the code: T2 checks only the motor alarm before running a queued command, never the wind override.
- **The fail-first criterion therefore moved** from the windows to the deferral rows at the sweep's end: 3 on 2.9.1, and 0 required on 2.9.2.
- **The correction posted on the issue** (2026-09-18) says the harm does not happen on current firmware at all. That holds only for the STANDBY exit.

## Who writes and who reads Q1

This release gates a shared queue, so every producer and consumer is listed, from:

```
grep -rn 'xQueueSend[A-Za-z]*(Q1\|xQueueReceive(Q1' firmware/src/
```

| Task | Where | Posts | Source | During a sweep, as of 2.9.2 |
|---|---|---|---|---|
| **T6** climate | `climate_control.cpp:325` (`post_q1()`, called at `:415` and `:425`) | `CMD_CLOSE`, `CMD_OPEN`, one per channel that does not match the step | `SRC_T6` | **Paused** (this release). Posts nothing during the motor-alarm guard before a sweep either, because every window is UNKNOWN then |
| **T3** wind safety | `safety_monitor.cpp:154` (`post_close_all()`) | `CMD_CLOSE_ALL` on override onset | `SRC_T3` | Posts; **retried until accepted** (this release) |
| **T3** wind safety | `safety_monitor.cpp:254`, `:370` | `CMD_RESUME` when the override clears, or when wind protection is switched off during one | `SRC_T3` | Posts once, unchecked. T2 takes no action on `CMD_RESUME`, so a lost one costs nothing |
| **T4** data manager | `data_manager.cpp:2301` (`standby_post_recalibrate()`) | `CMD_RECALIBRATE` on a STANDBY exit | `SRC_OPERATOR_MANUAL` | Posts; the command starts the next sweep |
| **T8** LCD | `ui_display.cpp:2171` | `CMD_OPEN`, `CMD_CLOSE` for one motor (admin menu) | `SRC_OPERATOR_MANUAL` | Refuses while `CALIBRATING` is set ("Calibrating / wait + retry"); unchanged |
| **T17** teach (`commission_tick()`) | `window_pos/commission.cpp:527` | `CMD_OPEN`, `CMD_CLOSE` on M3, one per leg | `SRC_OPERATOR_MANUAL` | Cannot post: a teach holds STANDBY until its session ends, and a leg does not start while M3 moves; unchanged |
| **T2** relays, the **only consumer** | `relay_controller.cpp:1351` | Drains Q1 every 20 ms loop pass, except while blocked in a sweep or the motor-alarm guard | — | Reads the clock per command (this release) |

Q1 is 8 deep. With T6 paused, the producers that can still post during a sweep are T3 (one CLOSE_ALL per onset, one RESUME per clearance) and T4 (one `CMD_RECALIBRATE` per STANDBY exit). None of them can fill it.

## Size

|  | 2.9.1 | 2.9.2 | delta |
|---|---|---|---|
| app image | 1 393 056 | **1 393 376** | **+320 B** |
| `.flash.text` | 938 318 | 938 478 | +160 B |
| `.flash.rodata` | 310 028 | 310 188 | +160 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 152 | 42 152 | 0 |

The bench image is 1 408 256 B, also +320 B. The web assets are unchanged from 2.9.1 apart from the version stamp.

## Verification

All on 2344 in the dev rig, with `bin/at_gh79.py`.

**What the test does.**
1. It makes T6 want all three windows open (`cr_priority` 2, and a lower temperature maximum if needed).
2. It lengthens the sweep to 176 s by setting `travel_m3` to 171.
3. It starts a recalibration (STANDBY, then AUTOMATIC).
4. About 100 s into the sweep, after at least three T6 wakes, it raises a wind override with the direction exclusion arc. No wind is needed at the emulator.
5. It watches M1 and M2 for 90 s after the sweep, and counts the deferral rows at the sweep's end in the SD log.
6. It then restores everything it changed.

| Build | Override confirmed | Sweep ended | Deferral rows at the sweep's end | M1, M2 after 90 s | Verdict |
|---|---|---|---|---|---|
| 2.9.1 (ROTA), 09:12, windows as the criterion | 122 s into the sweep | 09:15:04 | 3: M1 +26 s, M2 +26 s, M3 +476 s | CLOSED, CLOSED | Passed on the old criterion; this run corrected the issue |
| 2.9.1 (ROTA), 09:25, deferral rows as the criterion | 123 s into the sweep | 09:28:12 | **3**: M1 +26 s, M2 +26 s, M3 +476 s | CLOSED, CLOSED | **FAIL**, as required |
| 2.9.2-bench, 09:33 | 121 s into the sweep | 09:36:38 | **0** | CLOSED, CLOSED | **PASS** |

- **The override was still active** at the end of every watch (`wind_override` flag set), and the sweep was still running when it was confirmed.
- **Expected side effect, not a fault:** with `travel_m3` at 171 on the rig's 13 s window, T17 rejects M3's samples as implausible and reports one rule-1 stall for the sweep's M3 drive. That accounts for the soak baseline's `stall_faults` 1 and `rejected` 8.
- **Which build was tested.** The runs and the soak use the bench build of 09:23. Afterwards only comments changed: T6's inhibit list in `climate_control.cpp` and `.h`, and one word in `safety_monitor.cpp`.
  - **Rebuilding the 09:23 source reproduced its images bit for bit**, release and bench, so the build is reproducible.
  - **The final images differ from them in two places only:** the 32-byte `app_elf_sha256` field of the app descriptor, and the image's trailing checksum and digest.
  - **Code and data are identical.** The ELF hash changes because a comment moved lines in the debug line table.
  - SHA-256: release `aa559aa0…35c7`, bench `b2ed2a71…b6fa`.
- **Soak, PASSED** (`bin/at_wp_soak.py`, 2344, `2.9.2-bench`, 2026-09-18 09:41:35 to 21:42): **12.01 h, 18 judged strokes, `stall_faults`, `early_stops`, `rejected_rate` and `err_comm` all 0, no mode change**, the gate in position mode throughout.
  - **Continuous:** the unit booted at 09:32:33 and not again during the window.
  - **The SD log agrees with the counters.** It has no `ALARM ch6` param 249 or 250 row and no other alarm, only the encoder's own status rows (param 246). The one `LOG_SYSTEM 25` row is the sunset flip at 19:45.
  - **T6 kept moving M1 and M2 normally**, through steps 0 to 3. The four `LOG_SYSTEM 29` rows (14:28 to 15:08) are ordinary deferrals of a CLOSE on an open dwell.
  - **Two recalibrations, and no deferral row at the end of either** (LCD logouts at 14:54:29 and 15:58:20). The second is the one that tells 2.9.2 from 2.9.1:
    - the temperature had been held at 35.6 °C for over half an hour, so T6 wanted all three windows open;
    - T6 woke during the 26 s sweep, on the sensor reading at 15:58:27;
    - on 2.9.1 that queues three OPENs, which end as deferral rows at the sweep's end. On 2.9.2 there were none, and T6's first wake after the sweep (15:58:58) opened all three.
  - **How the strokes were made.** The operator set the emulated temperature by hand to step T6 up and down, and ran two LCD sessions with manual M3 moves. M3's dwells were cut to 5 s from 14:31 to 16:27 to allow more strokes, then restored to 1 500 s and 300 s. The strokes fell between 14:25 and 16:32. Nothing moved after that, so the evening tested the rest path.
  - **The encoder's bus was clean:** 4 339 reads on address 40 in the hourly rows, none failed. Address 1, the emulated T/RH, failed once in each of 6 hours: the known out-of-spec emulator (gh#68). It raised no sensor fault.
  - **Two findings not caused by this release** (see Known limitations): the internal-heap low watermark reached 9 KB (gh#81), and the rig's DS1307 stepped to 21 s behind NTP at about 16:08 and stayed there (gh#55).
- **Not tested on hardware:**
  - **T3's retry.** With T6 paused, Q1 no longer fills during a sweep, so the retry path never ran.
  - **The under-travel case** that the clock fix prevents.
  - **The boot sweep and the motor-alarm recovery.** `at_gh79.py` can start only the STANDBY-exit sweep. The recovery needs the RRK-3 alarm input.

## Upgrading

- **No partition change, no NVS migration, no configuration change, no log-encoding change.**
- **In the SD log, a recalibration no longer ends with deferral rows.** Before, T6's queued commands produced `LOG_SYSTEM 29` rows at the sweep's end, reporting the sweep plus the dwell as remaining (M3 +476 s on 2344).
  - **One row can still follow at T6's first wake after a sweep.** That is normal when T6 wants M3 open, because its close dwell (`dwell_close_m3`: 600 s default, 300 s on 2344) holds it CLOSED after the sweep. Such a row reports at most that dwell.
  - M1 and M2 have no close dwell by default, so they are not deferred after a sweep.
- **On the ROTA soak channel since 2026-09-18, seq 52** ([v2.9.2](https://github.com/pe1mew/greenhouse-Controller/releases/tag/v2.9.2)). **2344 pulled it the same evening.**
  - 2.9.1 was pushed back first.
  - The server began offering 2.9.2 between 22:00:21 and 22:01:21, and the unit rebooted into it at about 22:01:33. The download succeeded on the first attempt (`last_dl` 0).
  - Verified after the reboot: `fw_ver` and `asset_version` both 2.9.2, `travel_m3` still 13, `wpos_fitted_m3` still 1, M3's position back in the status, and address 40 reading without error.
- **ROTA does not offer 2.9.2 to a unit that runs a pushed 2.9.2 build**, a bench build included: the version compare ignores the `-bench` suffix. Push 2.9.1 back first.
- **Keep this release's ELF.**
- **Production runs 2.3.1, which has the same gh#79 code**, including the unmasked boot and motor-alarm paths. Regenerate the release comparison before promoting.

## Known limitations

- **The internal-heap low watermark reached 9 KB during the soak** ([gh#81](https://github.com/pe1mew/greenhouse-Controller/issues/81)). That is below the 20 KB gh#40 set as the gate before ROTA on production.
  - The per-minute samples were normal (median 69 KB, lowest 43 KB, no trend), so it was a brief dip. It came with no web session open.
  - 2.9.2's changes allocate nothing. The floor was not recorded for earlier releases at a comparable uptime.
  - It belongs in the release comparison before any promote.
- **A DS1307 that steps stays wrong for up to a day** ([gh#55](https://github.com/pe1mew/greenhouse-Controller/issues/55)). The firmware rewrites it only at boot, on the NTP-synced edge and at T10's 24 h resync, not at lwIP's hourly SNTP update. It has no effect while NTP is synced. Seen on the rig during this soak: 21 s behind from about 16:08. All releases behave the same.
- **A refused and retried CLOSE_ALL reaches the serial console only**, not the SD log. The override's onset row is logged as before.
- **Q1 stays 8 deep.** With T6 paused, nothing that can post during a sweep can fill it, and the retry covers the case anyway.
- **Rule 2 (early stop) is wrong in two cases** ([gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78), from 2.9.1, to be fixed in 2.10.0).
- **The fault thresholds are validated on the rig only.** Unchanged.
- **No DEGRADED threshold for the bus** (gh#66); the rig's addresses 1 and 44 are emulated slaves that are out of spec (gh#68).
- **ROTA re-downloads both artefacts on every deferred apply** ([gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71)).
- **Commissioning is bench-only** ([gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77)).
