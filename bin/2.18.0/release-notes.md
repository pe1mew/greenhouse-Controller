# Release 2.18.0

**Date:** 2026-11-01
**Built on:** 2.17.4
**Changes:** one failed wind poll no longer closes the greenhouse, every failed bus transaction is logged with how it failed, and the S200's unused second request is gone

**Minor:** a new log row, and FR-W04 changed. No setting, key or payload change.

> **Published to `soak` as seq 68, tag `v2.18.0` on `cddf504`, on 2026-11-01. Soaked clean on 2344 on 2026-11-02/03 (bench build, 12.25 h, PASS), after the rig's RS485 transceiver was replaced: the two earlier soaks ran on a bus with its A line shorted to ground, see below.** 2.17.4 was never soaked alone: this soak covers it. `mainstream` is not touched.

## What changed

- **A wind fault needs the sensor unread for its whole averaging window** (FR-W04, the operator's decision). `avg_win_wind` in polls, at least two: 6 minutes by default. Before, one failed poll raised the fault and T3 closed every window, and every wind fault since commissioning did so, in 0.6 to 1.6 m/s of wind. **A dead wind sensor now closes the windows up to that window later.** Before the first good read since boot one failed poll still counts, because there is no average yet.
- **`SYSTEM value_a = 34`: one row for every failed bus transaction.** `ch` = address, `value_b` = status and bytes received, so `0x0100` is a slave that stayed silent. Until now timeout, CRC and exception all arrived as one `S200_ERR_COMM`, so "the sensor went silent" could not be told from "the reply was damaged". `logparser.py` reads the rows.
- **The S200 is read with one request, not two.** The second fetched the heating temperature, which nothing used. A failure of it threw away the wind reading already in hand and counted towards the wind fault. Address 44 now shows one transaction per poll, half the former count.
- **The sensor fault and wind override rows use the system clock.** `dm_get_unix_time()` is up to 60 s old, and that put those rows a poll or two before the readings they belong to, which led to a wrong sequence in the first 5C88 analysis.
- **New:** `bin/at_wind_holdoff.py`, the bench injection `nowind`, and `-DWPOS_FAILFIRST_WINDPOLL`, bench builds only.
- **Manuals:** both say what a wind fault now means.

**Not changed:** the T/RH fault, which stops nothing but T6, and T3's safe-fail once the fault is raised.

### Who reads and writes what

Found with grep on 2026-11-01.

| Item | Enumerated |
|---|---|
| `EG1_BIT_SENSOR_FAULT_W` setters | T5 only, one site. Clear: T5, on the first good read |
| Its readers | T3 (the safe-fail), T1 (amber LED), `status_json.cpp` (`sensor_fault_wind`, the GUI's badge). All now see it later, none changes |
| `value_a = 34` | Free: no other emitter. One emitter, T5's `post_bus_failure()`, called by the driver's hook on T5 and T17 |
| `heating_temperature_c` | Only `drivers/s200` and its tests. Nothing in `firmware/src` |

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 440 B, sha256 `25e6e8f0ccc966e4…`, version `2.18.0`. Assets `web-assets-2.18.0.zip`, 176 331 B, sha256 `b5dcb242ad534ba9…`. Pulled by 2344 over ROTA, see below |
| ROTA pull on 2344 | Pushed first with a throwaway `2.17.5-rota`, this source under a lower number: every real older release still does the second wind read, which was timing out and would have held the unit in wind override, and that blocks an apply. Check 13:01:34 found 2.18.0, download OK 13:01:57, apply handed over 13:02:10, installed 13:02:13. `fw_ver` and `asset_version` 2.18.0, Lineair, rig settings intact, no coredump |
| Bench image on 2344 | `a5f8381868ed2ce0…`. `fw_ver` and `asset_version` 2.18.0-bench |
| Host tests | `modBus` 16 of 16, 4 new. `s200` 9 of 9, and UT-S200-011 now proves the one request |
| Fail-first on 2344, one-poll rule compiled back in | FAIL: fault and override 2 s into the injection |
| `at_wind_holdoff.py short`, 2.18.0-bench | PASS: 5 of 6 polls failed in 145 s, no fault, no override |
| `at_wind_holdoff.py long` | PASS: fault at 176 s of a 180 s window, override set, both cleared 26 s after the injection |
| `value_a = 34` on hardware | Rows written for addresses 1 and 44, `0x0100` each, during the rig's outage below, read back from the card. The parser was tried on sample rows of all four kinds |

## Soak

### The clean soak, 2026-11-02 21:38 to 2026-11-03 09:52

2344 on 2.18.0-bench `a5f83818`, standard rig: both slaves emulated on 5C88's climate, spare off, both end sensors on, band 34 mm. The controller's SIT65HVD08P had been replaced about 40 minutes before (its A output had been shorted to 0 V since 2026-11-01 12:05, gh#108).

| Check | Result |
|---|---|
| `at_wp_soak.py` | **PASS:** 12.25 h, 66 strokes judged, stalls, early stops, rejected samples, comm errors, not reached all 0, no mode change |
| `sd_soak.py` | PASS: no reboot, no coredump, card mounted in all 735 samples, largest gap 31 s, no SYSTEM -1 or 33 row |
| Stroke sessions | 3 of 12 ran, 9 skipped: the greenhouse was 7 to 10 °C, below the lowest `t_max`, so the law had nothing to open for. Not a fault |
| LCD sessions | 12 of 12, 0 failed |
| Law conformance, hunt, settings | 10 decisions, all conform. 9 targeted stops, none outside the band, no opposite correction. Settings identical |
| Bus, 14 h | **Encoder 7 313 reads, 0 failures.** Emulated T/RH 3 of 1 671, wind 1 of 1 657, single requests, retry answered, no fault. `SYSTEM 34` rows: 2, both address 1, silent |
| Heap | Steady 64 KB free, 31 KB largest after 12.4 h, no slope. `heap_min_kb` 20 |
| Stacks | After a full characterisation: T17 1748 B free of 5120, T7 1072 B free of 3072 |
| Characterisation after the report | Complete: 117 starts, 3613 s, band 35 mm (was 34) |
| `at_char_stop.py`, `at_cal_rejudge.py`, `at_session_cookie.py`, `at_wind_holdoff.py` | All PASS on the soaked unit. Wind fault at 181 s of a 180 s window |

So the firmware holds over 12 h on a sound bus, and the rig's bus is sound again: the 2.17.3 soak's figures are back.

### The first soak, 2026-11-01 13:27 to 2026-11-02 01:42, not clean: the rig's transceiver was shorted

2344 on 2.18.0-bench `a5f83818`. **The rig differed from every earlier soak: a real spare FG6485A on address 1, the emulator's T/RH on address 2, wind still emulated.**

**Result: NOT CLEAN on the bus counters.** The firmware's other checks passed. The cause was found the next day: the controller's RS485 transceiver with A shorted to ground (gh#108), not the firmware, the sensor, or the rig's end sensors, which only tipped a single-ended bus over its margin.

| Check | Result |
|---|---|
| `at_wp_soak.py` | **NOT CLEAN:** 120 strokes judged, stalls and early stops 0, not reached 0, but `err_comm` +133, `rejected_rate` +10, `mode_changes` +4 (need ≤ 2). The last three are the encoder failing to answer |
| `sd_soak.py` | PASS: no reboot, no coredump, card mounted, largest gap 31 s, no SYSTEM -1 or 33 row |
| Sessions | 12 of 12 stroke sessions ran, 12 of 12 LCD sessions passed |
| Law, hunt, settings | 41 decisions conform. 40 targeted stops, none outside the 33 mm band. Settings identical |
| Wind faults | **None, and no wind override**, despite 304 failed wind requests: T5's retry answered each time |
| Heap | Steady 64 KB free, 26 KB largest after 12.4 h, no slope. `heap_min_kb` 8 |
| Stacks | After a full characterisation: T17 1752 B free of 5120, T7 1072 B free of 3072 |
| Characterisation after the report | Complete: 117 starts, band 34 mm (was 33) |
| `at_char_stop.py`, `at_cal_rejudge.py`, `at_session_cookie.py`, `at_wind_holdoff.py` | All PASS on the soaked unit. Wind fault at 180 s of a 180 s window |

**The `SYSTEM 34` rows, in the 12.25 h window:**

| Address | Failed requests | Type | At rest | Opening | Closing |
|---|---|---:|---:|---:|---:|
| 40, the real encoder | 157 | All silent | 28 | 5 | **124** |
| 44, emulated wind | 304 | All silent | **280** | 12 | 12 |
| 1, the real FG6485A | **0** | | | | |

Over the whole run including the characterisation: address 40 181 failed of 9 910, address 44 375 of 2 024, address 1 0 of 1 664. Address 40 had 0 failures in the 2.17.1 and 2.17.3 soaks. 135 of the 157 came 3 s or more from a T5 poll, so they do not follow the FG6485A's own reads.

A control soak on the standard rig on 2026-11-02 (08:36 to 17:24, stopped) failed the same way, which cleared the sensor and the emulator's address; the stroke tests that followed cleared the firmware (2.17.4 failed alike) and led to the transceiver. The record is `temp/unit-5c88/research.md`.

## What the rig showed on the way

- **From 12:05:51, when the emulator's T/RH moved to address 2 and the spare went on, the controller got no bytes back for the second request to address 44**, every poll, with the first request answered. It survived an emulator restart, and happened with the spare connected and disconnected. **The cause is not known, and the emulator is not shown to be it:** its code serves that register on the same path as the first request, and whether it saw the request or replied was not checked. The controller only records that nothing arrived. With one request it no longer arises.
- **The wind hold-off met it by chance:** the fault came on the 6th failed poll, 155 s after the first.
- **A real FG6485A on the rig, for the first time.** Characterisations at T17's fastest polling, with the emulator's T/RH on address 2:

  | Run | Spare FG6485A | Result | Encoder | Wind |
  |---|---|---|---|---|
  | 12:32 | On | **Ended `sensor` at 3 starts** | 3 silent of 176 | 2 silent of 4 |
  | 12:39 | Off | 55 starts, stopped by hand | 0 of 684 | 0 of 7 |
  | 12:45 | On | 28 starts, stopped by hand | 0 of 419 | 1 silent of 5 |

  Every failure was a silent slave. The FG6485A may matter, but **one failure in two runs proves nothing**. The rig soak runs with the spare on to give it a sample.

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.
- **Check `avg_win_wind` on a unit before relying on the timing:** it is the delay before a dead wind sensor closes the windows, up to 30 minutes.

## Known limitations

- **A wind rise during a sensor outage is not seen until the fault closes the windows.** That is the decision above.
- **`heap_min_kb` 2 KB in the 2.17.3 soak:** the cause was not found.
