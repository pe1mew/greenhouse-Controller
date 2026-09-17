# Release 2.8.0

**Date:** 2026-09-17
**Built on:** 2.7.1
**Fixes:** [gh#65](https://github.com/pe1mew/greenhouse-Controller/issues/65) (LCD STANDBY survived a reboot),
[gh#69](https://github.com/pe1mew/greenhouse-Controller/issues/69) (Modbus re-init panic),
[gh#70](https://github.com/pe1mew/greenhouse-Controller/issues/70) (RS-485 replies lost under flash writes)
**Partly addresses:** [gh#66](https://github.com/pe1mew/greenhouse-Controller/issues/66) (Part 2: per-sensor bus indicators)
**Follow-ups filed:** [gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71) (ROTA re-downloads on every deferral),
[gh#72](https://github.com/pe1mew/greenhouse-Controller/issues/72) (a reversal mid-stroke is not judged),
[gh#73](https://github.com/pe1mew/greenhouse-Controller/issues/73) (a unit without an encoder shows address 40 as failing)

**Minor.** A new task, new log encodings, a new config key and new status payload keys.

**What it is, and what it is not.** 2.8.0 is the first release with the M3 window-position
sensor. The controller now measures where M3 is, logs it, and reports two kinds of fault.
**Nothing acts on the measurement yet**: T2 still stops M3 on its timer, and T6 still steps on
temperature and humidity, so greenhouse behaviour is unchanged. A unit without an encoder, such as
5C88 today, runs the same timed control as before (see *Known limitations* for what it shows).
Acting on position is the next step, 2.9.0, with its own soak.

It is also the release that made the shared RS-485 bus safe for a second caller. T17 put its frames
next to T5's, and that exposed four bus defects that a single caller never had. All four are fixed
here.

## What changed

### The M3 window-position sensor

**T17** (`firmware/src/window_pos/`, priority 4, 4 KB stack) reads a wire encoder at Modbus
address 40 through a new driver (`drivers/windowPos/`, IDF component `windowPos`). It runs in
every build. While M3 travels it reads every `travel_m3`/150 (100 ms minimum): 168 ms in practice
on the 13 s rig, and 1 140 ms for production's 171 s window. At rest it reads every 30 s.

**A presence gate selects the control law.** POSITION with a trusted sensor, TIMED without one.
- Demotion is immediate. Promotion waits for a stroke boundary, so position never gains authority
  under a stroke already committed to the timer.
- A shut gate re-probes address 40 every 30 s, so a sensor fitted later is found without a reboot.
- A sensor reporting a BENCH firmware is refused for good.
- A busy bus never counts against the sensor.

Each change of mode or reason is one `ALARM ch6 param 248` row. Nothing consumes the mode yet.

**Two fault checks from contract §12.4, reporting only:**

| Rule | Fires when | Log | Counter |
|---|---|---|---|
| 1, "moving means moving" | the leaf has not reached half its nominal speed within min(5 s, `travel_m3`/2) of the relay energising | `ALARM ch6 param 249` (peak rate, threshold) | `stall_faults` |
| 2, "an early stop is a fault" | a CLOSE reads ~0 (within `deadzone_m3`) in under half `travel_m3` without the end sensor ever making | `ALARM ch6 param 250` | `early_stops` |

Rule 1 is the only detector for a shorted wiper, which reports a constant, plausible position with
every status bit clear.

**Rule 1 exempts a stroke toward the end the leaf already sits at.** The end switch stops that leaf,
so it should not move, and without the exemption every CLOSE of a closed window tripped the rule:
at each boot, twice per OTA push, and at each LCD logout (soak #1 found this). The exemption holds
only while, on every sample of the grace window, the end sensor stays made and the position stays
within `deadzone_m3` of the target end. Such strokes are counted as `at_end_exempt`.

**Position is logged as `SENSOR_HR ch3`** (0.1 mm, −1 on a fault, with the signed rate) at every
read during a stroke. At rest, a row is written only when the reading says something:
- the first read after a stroke or a boot;
- a change between fault and no fault;
- movement of at least `deadzone_m3` (5 mm minimum), which catches the motor box's hand switches.

The 30 s rows the first builds wrote at rest had added about 37 % to the log.

**Device events are `ALARM ch6` params 244–247:** device fault set or cleared, teach events,
status-bit changes, and a sensor restart.

**`motor/deadzone_m3`** (1–200 mm, default 20, audit param 48) is the band for rule 2's "near
zero", rule 1's end tolerance and the rest-logging threshold. It is the first key added through the
gh#64 descriptor table: one row.

**Commissioning, bench builds only.** `GET`/`POST /api/diag/commission` and a GUI card set the
window size, judge the calibration, and run a teach. The teach arms the sensor and drives M3 in up to
three legs until both end sensors have made, so M3 may start anywhere. The GUI polls the teach, and
the teach **holds STANDBY** until its admin session ends (see below).

In every build, T17 aborts any armed teach that the commissioning path does not own (`param 245`
value 4, `orphan_aborts`). The sensor keeps a teach armed across a controller restart, and T17's own
polls would otherwise commit it unwatched.

### The shared RS-485 bus

| Defect | Fix |
|---|---|
| The inter-frame gap was 4 ms, the RTU floor plus 9.7 %, and a single caller had never once paid it. With T17's frames adjacent, the S200 read two requests as one and stayed silent, and T3's safe-fail turned that into a wind alarm on a calm day. | `MODBUS_IFG_US` = 20 ms, paid **before** the bus lock is released. The worst-case bus hold rises from about 215 to about 235 ms. |
| Four of six error exits left bytes in the receive FIFO, so one corrupt frame broke the next transaction too. | The FIFO is drained before every transmit and on every exit. |
| A busy bus was reported as a sensor failure. | The drivers return `*_ERR_BUSY`, and T5 forgives a busy bus for up to 60 s per sensor. A dead sensor is still detected on the old schedule. |
| **gh#69:** T5's second `modbus_init()` deleted the UART under a T17 read (two boot panics on FDA4). | A re-init takes the bus lock. If the lock is not free within 2 s, it leaves the installed driver alone. |
| **gh#70:** the task released the direction line (DE/RE) 2 ms after transmitting. A flash write stalled it by up to 665 ms, and the encoder's reply, due after ~5 ms, was lost. | The UART drives DE/RE (RS-485 half-duplex on GPIO 8), with its interrupt in IRAM (`CONFIG_UART_ISR_IN_IRAM=y`). A build without that option stops with `#error`. |

**Per-sensor indicators (gh#66 Part 2).**
- `modbus_get_counters()` keeps OK, failure and busy counts per address, plus the longest run of
  consecutive failures.
- `/api/status` publishes them as `bus`, and a **Modbus bus** card on the Status page shows them.
- T4 writes them hourly as `LOG_SYSTEM value_a = 31`: param 50 OK and 51 failed in the last hour,
  and 52 the longest failure run since boot.
- No DEGRADED badge is built. It needs a threshold, and the threshold needs a measured baseline,
  which is what the hourly rows exist to produce.

### Operator surfaces

- **Status payload** (`/api/status`, the WebSocket push and the remote status POST):
  - `windows.M3_percent_x10`, `M3_mm_x10` and `M3_at_end_sensor`. They are **omitted**, not
    zeroed, unless a trusted reading exists.
  - The mode flag `sensor_fault_position`.
  - The top-level `bus` array.

  On FDA4, a full `/api/status` with all of it is 1 000 B.
- **Web GUI:**
  - M3's opening on the Status page.
  - OPENING/CLOSING on all three windows.
  - The bus card.
  - The Motors tab grouped per window, with M3's *Linear control* group and the deadzone setting.
- **A control the operator cannot use is greyed out with the reason beside it, never hidden.** The
  rule was adopted after two bench routes failed to register at boot (a fixed limit of 36 handlers
  for 38 routes), and the GUI simply hid the deadzone setting. The limit now follows the route
  table, and the boot log says "N of M routes registered".
- **Admin routes answer `401 {"error":"session_expired"}`** for an unknown or expired session, where
  they said `403 admin only`. A farmer session still gets 403. The GUI now sends a lapsed session to
  the login dialog instead of showing "admin only".
- **The LCD is unchanged on screen.** Its manual menu now uses the STANDBY hold below.

### STANDBY holds (gh#65)

The LCD manual menu paused automatic control by writing STANDBY to NVS, while the flag that let the
session end clear it lived only in RAM. A reboot during the session, from a power blip or a ROTA
night update, kept the STANDBY and lost the flag, so ventilation stayed paused until someone
noticed.

**Both the menu and the new teach now take a STANDBY *hold*.** It is never written to NVS, so a
reboot ends it together with the session.

| Situation | Result |
|---|---|
| Menu or teach starts, STANDBY off | STANDBY on, held (`MODE param 47`, `value_b` 1) |
| The other one starts while the first holds it | joins the hold; nothing is logged |
| The operator's own STANDBY is already on | no hold; the session end leaves it alone |
| One holder's session ends, another still holds | STANDBY stays on |
| The last holder's session ends | T2's dwell debt is dropped, STANDBY clears, and the windows recalibrate (close once) |
| A reboot during the session | the hold is gone; the unit boots in AUTOMATIC |
| An explicit STANDBY request during a hold | wins: the pause becomes the operator's own, and is saved |

The teach releases on logout at once, and on an idle timeout or an eviction at T17's next reading.
The LCD menu releases in `session_close()`, on logout or timeout. A reboot simply ends both.

### OTA: a broken upload no longer wedges the unit

A firmware upload cut off at 60 % by a lossy link left the state at `fw_writing`, with the OTA
event bit set. Every later upload was refused and ROTA skipped every check, until someone reset the
unit.

**Every exit that does not install now releases the session:**
- the fallback timer, the esp_ota handle, the staged image, the ZIP buffer and the event bit;
- a reason-coded status text that ends in *nothing installed*;
- a `LOG_SYSTEM value_a = 32` row (ch 1 firmware, 2 assets; `value_b` = reason << 8 | progress).

A sender that goes silent is dropped after 30 s with **HTTP 408**. A lost connection answers 500
with a JSON reason.

**One behaviour change:** a broken **asset** upload now discards the verified firmware instead of
installing it alone. Upload both again.

### Build and tests

- **Reproducible builds.**
  - The app image's version field is now `FIRMWARE_VERSION`. It used to be `git describe --dirty`
    of the release commit's parent; 2.7.1's reads `v2.7.0-17-g9755d0e-dirty`.
  - The library link order is fixed (`scripts/deterministic_link_order.py`).
  - The build timestamp is off (`CONFIG_APP_COMPILE_TIME_DATE=n`).

  A rebuild of the same tree in the same checkout is byte-identical.
- **`build_release.ps1 -Environment <env>`** packages any environment, and refuses a binary whose
  version string, app version field or timestamp is wrong. Before this, it had packaged the release
  binary for a bench build.
- **The Modbus host tests no longer hang:** the mock now releases a reply only after the request,
  and its clock moves.
- **New acceptance scripts:**
  - `at_wp07`, `at_wp09` (with a guided `--sequence`), `at_wp_ramp`, `at_wp_soak`, `at_wp_teach`,
    `at_wp_teach_standby`, `at_lcd_standby`;
  - `at_modbus_reinit`, `at_modbus_ota`, `at_ota_abort`.

  Four older harnesses now log out on exit as well, and `at_wp_soak` notices any reboot during a
  soak.
- **Bench builds** add actions to `/api/diag/modbus`: `reinit`, `reinit_status` and `traffic`.

## Who writes and who reads

The release rule: when a change gates a shared state, list every producer and consumer.

**STANDBY** (`EG1_BIT_STANDBY`, NVS `system/mode_standby`):

| Writers | Readers |
|---|---|
| `nvs_restore_standby_at_boot()`, from NVS | T6's inhibit mask (`climate_control.cpp`) |
| `dm_set_standby()`: `POST /api/mode` (T11), LCD screen 3 toggle (T8) | status snapshot → `mode.current` and the `standby` flag |
| `dm_reload_all_cfg()`: the IO0 settings reset (T8) | LCD screen 3, "Mode: STANDBY" |
| `dm_standby_hold()` / `dm_standby_release()`: the LCD manual menu (T8) and the teach (T11/T17) | `/api/mode` response |

**The Modbus bus**, in a release build:
- T5 reads addr 1 (FG6485A) and addr 44 (S200).
- T17 reads addr 40 (encoder).
- The bus is initialised at boot (`main.cpp`) and at T5's start; a re-init takes the lock.

Bench builds add the `/api/diag/windowpos` direct read, the commissioning writes and the
`/api/diag/modbus` bench actions. The `lolin_s3_mbprobe` environment adds the probe.

**`deadzone_m3`:**
- declared by one descriptor row, and set through `POST /api/config` (the Motors tab);
- read by T17 (rules 1 and 2, rest logging);
- published by `GET /api/config` (`deadzone_m3_mm`) and `/api/config/limits`;
- mirrored in `webUiMock/mock_server.py`;
- decoded by `logparser.py` (param 48).

**The new log encodings** are decoded by `log/logparser.py` (`logparser.md` 1.19). Two scripts
handle them too: `model/campaign-summer-2026/plot_daily.py` decodes ch3/ch6 and skips
`SYSTEM 31`, and `model/vent_step_replay.py` skips `MODE param 47`.

**The new status keys** are consumed by `firmware/data/app.js`, the remote status site (through
T14's POST, within the operator's `status_expose` mask), and `webUiMock/mock_server.py`.

## Size

|  | 2.7.1 | 2.8.0 | delta |
|---|---|---|---|
| app image | 1 378 256 | **1 391 472** | **+13 216 B** |
| `.flash.text` | 930 190 | 937 162 | +6 972 B |
| `.flash.rodata` | 306 556 | 309 596 | +3 040 B |
| `.iram0.text` | 117 363 | 120 535 | **+3 172 B** |
| static RAM (`.data`+`.bss`) | 64 064 | 64 852 | +788 B |

Flash is 66.3 % used (1 391 061 of 2 097 152 B) and RAM 19.8 %. T17's 4 KB stack comes from the
heap.

**The IRAM growth is gh#70's price.** 3 089 B of new IRAM code, compared symbol by symbol:
- the UART receive interrupt handler (2 111 B) and its HAL helpers;
- the VFS select path that `CONFIG_UART_ISR_IN_IRAM` pulls in with it.

The bench image is 1 405 296 B.

## Verification

All on the dev rig (FDA4, `2.8.0-bench`) unless stated. **Fail-first** means the same test was run
against a build with the defect and failed there.

**Sensor and T17**
- **Driver:** decoding was cross-checked against the raw registers, and a disconnected wiper was
  seen as a fault and cleared within 0.4 s of reconnection.
- **Position traces:** monotonic in both directions. An OPEN ramp of 45 samples went 0 → 1 500 mm
  with 0 rejected; a CLOSE of 98 accepted samples went 798.9 → 12.2 mm.
- **AT-WP06:** **failed first**. The idle read ignored failures, so the gate was blind at rest.
  After the fix it passed, with recovery 31 s after reconnection against a 30 s re-probe.
- **AT-WP07:** with the encoder gone, the wind override still closed M3, on the timer, 18.4 s
  after the override rose.
- **AT-WP09 (2026-09-15, re-run 2026-09-17 on this code with `--sequence`):**
  - detection half: with the draw-wire detached, rule 1 reported the stall on a CLOSE and on an
    OPEN, 6 s in each time (`param 249`: peak 0.0 mm/s against 57.6 mm/s), and nothing was
    exempted;
  - healthy half: an OPEN and a CLOSE stayed silent (96 readings each, 1 500 mm of travel);
  - the sensor was back at the open end within seconds of reattaching the wire.
- **Soak #1 failed**, usefully: rule 1 false-tripped on a CLOSE of an already-closed M3. That was
  fixed with the at-end exemption. The next boot's CLOSE of the closed window was exempted after
  83 readings, with no stall.
- **Soak #2 (2026-09-16 23:42 → 2026-09-17 11:42): PASS.**
  - 12.02 h and **14 judged strokes** (7 OPEN, 7 CLOSE, none exempt).
  - Stall faults, early stops, rejected readings and comm errors all **0**.
  - One control-mode change (the promotion at the first stroke), no reboot, no orphaned teach.
  - Encoder: 4 459 reads, 0 failed.
  - Heap, 722 samples: internal free 45–72 KB, largest block 20–31 KB, both flat
    (`log/heap_soak.py`).

  Most strokes were climate-control strokes, driven by setpoint changes and the emulated
  temperature. One was an LCD manual sequence with two reversals, counted as a single stroke
  (gh#72).
- **Release build, no encoder, 30 min (2.8.0, d8c7332):**
  - The gate shut 35 s after boot, with one log row.
  - Address 40 was probed exactly once per 30 s (60 in 1 801 s).
  - T5 made 59 + 118 reads with 0 errors and 0 busy.
  - No M3 keys, no fault flag, heap never below 68 KB.
  - Plugged back in, the encoder was found by the next probe, position was back 2 s later, and the
    mode went to POSITION at the next stroke.
- **Teach (`at_wp_teach.py`):** passed from CLOSED and from OPEN, with an abort between them.
  Orphaned teaches were aborted both at rest and mid-stroke.

**Bus**
- **Inter-frame gap:**

  | Gap | Transactions | Timeouts | CRC errors | Wind alarms |
  |---|---|---|---|---|
  | 4 ms | ~600 | 4 | 1 | yes, every session |
  | 20 ms | 779 | 0 | 0 | none |

  A 13 h soak then showed 0.087 % errors, against 0.83 % before the fix.
- **Arm A/B (gh#66):** with the encoder unplugged, T5 had **0 failures in 6 579** reads over
  18 h 33 m. With it fitted, **10 in 7 583** over 21 h 15 m (p = 0.0019). The encoder itself had
  0 failures in 8 704 reads. Both affected slaves are emulated on this rig (gh#68), so the
  *direction* of this result holds and the *rate* does not transfer to 5C88.
- **gh#69 (`at_modbus_reinit.py`):** **fail-first**, the lock compiled out crashed 5 of 5 runs. The
  fixed build passed 500, 2 000 and 3 000 re-inits under traffic with 0 skipped.
- **gh#70 (`at_modbus_ota.py`):** **fail-first**, every encoder failure followed a late DE/RE
  release: 16/16 and 13/13 during firmware uploads, worst case 665 ms. With the fix:

  | Phase | Encoder reads | Failed |
  |---|---|---|
  | Baseline | 2 166 | 0 |
  | Firmware upload | 686 | 0 |
  | Asset extraction | 112 | 0 |

  The 12-test loopback suite on 12F0 passed twice, and failed on two deliberately broken builds.

**STANDBY**
- **`at_wp_teach_standby.py`** passed all four cases:
  - A: logout releases the hold and recalibrates.
  - B: the timeout releases it 66 s after the last request, against a 60 s timeout.
  - C: an operator's own STANDBY is left alone.
  - D: a reboot ends the hold.

  **Fail-first:** a build without the hold failed A, and a build that saves the hold failed D.
- **`at_lcd_standby.py`**, with a person at the keypad, passed all four cases:
  - L: logout ends the menu's STANDBY and recalibrates.
  - E: an operator's STANDBY outlives the menu session.
  - C: a teach and the menu hold the pause together.
  - R: a reboot ends it.

  **Fail-first:** the build before the fix (088e463) failed C and R.

**OTA (`at_ota_abort.py`)**
- **Fail-first:** the old build stayed stuck at 60 % and refused the next upload until a reset.
- The fix passed firmware cut, firmware stall (the server answered 30.2 s after the last byte),
  assets cut, paired cut, and a cut closed with a reset. Each was followed by a clean paired
  install with `fw_ver` and `asset_version` matching.

**Config (`at_cfg_roundtrip.py`, 12F0).** 43 of the 47 writable keys round-trip, `deadzone_m3`
included. A wiped unit boots with 43 keys at their descriptor defaults.

**Build**
- Repeated builds gave one hash per environment.
- `build_release.ps1` refused a binary that still carried a timestamp, and one whose version field
  was wrong.
- The Modbus host tests pass 12 of 12. **Fail-first:** without the fix, 3 tests fail quickly
  instead of hanging.
- The rebuilt release image differs from the one that ran on FDA4 only in the embedded ELF hash and
  the image digest; the code is byte-identical.

## Upgrading

- **No partition change and no NVS migration.** `deadzone_m3` starts at its default, 20 mm.
- **From a source checkout, delete `firmware/sdkconfig.lolin_s3*` once after pulling.** An existing
  per-environment sdkconfig overrides the defaults file, and 2.8.0 needs
  `CONFIG_UART_ISR_IN_IRAM=y` (the build stops without it) and `CONFIG_APP_COMPILE_TIME_DATE=n`
  (`build_release.ps1` refuses without it).
- **Firmware and web assets ship as a pair, as always.** The 2.8.0 GUI relies on the new status
  keys and on the 401 answer.
- **Read 2.8.0 logs with `logparser.py` from this release** (`logparser.md` 1.19).
- **Clients of the web API:**
  - an expired admin session now answers 401, not 403;
  - an interrupted OTA upload answers 408 or 500 with a reason;
  - `/api/status` has new keys (`bus`, and the `M3_*` keys when a sensor is trusted).
- **A unit already stuck in STANDBY by gh#65 stays in STANDBY.** The fix stops new cases but does
  not clear a saved one. Check before upgrading, and switch such a unit back to AUTOMATIC.
- **Production runs 2.3.1, not 2.7.1**, so 5C88's step is 2.3.1 → 2.8.0. Regenerate the release
  comparison (`design/releaseComparison_2.3.1_vs_2.4.6.md` is the template) before promoting.
- **Keep this release's ELF.** The image embeds the ELF hash, and a rebuild in a different
  directory or from a fresh clone gives a different image. `firmware/dependencies.lock` is not
  tracked.

## Known limitations

- **A unit without an encoder shows address 40 as failing**
  ([gh#73](https://github.com/pe1mew/greenhouse-Controller/issues/73)). Every failed probe counts as a failure
  in the per-sensor counters, so on 5C88:
  - the public **Bus card** would show an `M3 · 40` row whose errors grow by about 120 an hour, with
    a failure run that never ends, both highlighted;
  - the hourly log rows would report those failures.

  A farmer could read that as a broken sensor on a unit that has none. This should be fixed before
  2.8.0 is promoted to 5C88.
- **A reversal mid-stroke is not judged** ([gh#72](https://github.com/pe1mew/greenhouse-Controller/issues/72)).
  T2's reversal gap reads as moving, so T17 sees one stroke, and both rules keep the first
  direction's verdict. That includes a wind override closing a window that is opening.
- **The fault thresholds are validated on the rig only**: a 13 s test window, not production's
  171 s rope flap. Production logging is the test for them.
- **The at-end exemption's continuity break is not tested on hardware.** AT-WP09 passed on this
  code, but neither detached-wire stroke started with the exemption's conditions met:
  - the detached wire held its last reading (1500 mm, the open end);
  - with the wire off, the closed end sensor released at rest (contract §5.3).

  So what keeps a shorted wiper judged, the end sensor dropping as the leaf leaves, is covered by
  the code and a replay of logged strokes, not by a test. An obstruction with a healthy sensor is
  not tested either.
- **No DEGRADED threshold for the bus** (gh#66). The rig's addr 1 and addr 44 figures describe
  emulated slaves that are out of spec (gh#68).
- **ROTA re-downloads both artefacts on every deferred apply**
  ([gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71)). This behaviour predates
  2.8.0.
- **Commissioning is bench-only.** A teach started part-way, a first leg that does not move, and
  failure branches other than the abort are not exercised.
- **Stale comments in `window_pos_task.h`** still describe the first design (a 2× rejection limit,
  no reads at rest). The code is correct.
