# Release 2.9.0

**Date:** 2026-09-17
**Built on:** 2.8.0
**Fixes:** [gh#73](https://github.com/pe1mew/greenhouse-Controller/issues/73) (a unit without an encoder shows address 40 as failing)

**Minor.** A new config key, a new gate reason in the log, and a new `/api/config` field.

**What it is.** 2.8.0 could not tell a unit with no M3 position sensor from a unit whose sensor
had stopped answering. 2.9.0 is told: an installation setting, *Position sensor fitted*, says
which it is. The default is **not fitted**, which is what a unit in the field is today, so 5C88
takes the fix by ROTA with no site visit. A unit that has a sensor, such as the dev rig's modules,
is switched on once.

Greenhouse behaviour is unchanged: M3 still runs on its travel time, with or without a sensor.
Acting on position is the next step, now 2.10.0.

## What changed

### The setting

`motor/wpos_fitted_m3`: 0 or 1, **default 0**, audit param 49, admin only. It is one row in the
config descriptor. `GET /api/config` returns it as `wpos_fitted_m3`, and `/api/config/limits`
publishes `[0, 1]`.

| | Not fitted (0, the default) | Fitted (1) |
|---|---|---|
| T17 on the bus | nothing at all | as in 2.8.0 |
| Mode row (`ALARM ch6 param 248`) | `TIMED` with the new reason 5, once at boot and at each switch-off | as in 2.8.0; a switch-on logs `TIMED [probing]`, then the verdict |
| `/api/status` | no `M3_*` keys, no `sensor_fault_position`, no address 40 in `bus` | `M3_*` keys when trusted; **`sensor_fault_position` when the sensor is absent, refused or faulted** |
| Hourly bus rows (`LOG_SYSTEM 31`) | none for address 40 | as in 2.8.0 |
| Commissioning (bench builds) | refused with `not_fitted`, except an abort | as in 2.8.0 |

### T17

- **It reads the setting at most every 500 ms**, through `dm_wpos_fitted_m3()`, a single-field
  accessor. When T4's lock is busy, the accessor reads the field without the lock instead of
  returning the default, so a busy lock cannot switch a sensor off.
- **A switch-on starts over:** no verdict, no failures counted, the bench-build latch cleared, and a
  probe at once.
- **A switch-off:**
  - shuts the gate without counting a probe failure;
  - forgets the last reading and the event baseline;
  - abandons a stroke in progress, so a later switch-on mid-stroke starts a fresh verdict.
- **At start, T17 waits for T4 to finish loading NVS** (`dm_cfg_loaded()`, 10 s at most). T4 is
  created first but loads NVS in its own task body. Without the wait, a fitted unit could read the
  zeroed shadow and log a not-fitted row at boot.

### A fault now means fitted and unusable

2.8.0 raised `sensor_fault_position` only when the sensor answered and reported a fault. An
unplugged encoder raised nothing (FDA4, 2026-09-16), because to 2.8.0 it looked like a unit without
one. FR-WP19 requires the flag, and the farmer manual already described it. With the setting on,
2.9.0 raises it for a sensor that is absent, refused (bench firmware) or faulted. With the setting
off, nothing raises it.

### Web GUI

- ***Position sensor fitted* heads M3's *Linear control* group.** While it is No, the deadzone and
  the commissioning card are greyed, with the reason above them. The reason sits outside the greyed
  block, because a dimmed parent dims its children whatever their own opacity says.
- **The greyed commissioning card names the cause of a 404** (operator request, 2026-09-17):
  - on a release build: teaching needs a bench build, and a taught sensor keeps its calibration;
  - on a bench build (`fw_ver` ends in `-bench`): a route failed to register at boot, which is a
    fault.

  2.8.0 named both possibilities.

### Log, mock and tests

- **`logparser.py`** decodes param 49 (`fitted` / `not fitted`) and gate reason 5.
  **`logparser.md` 1.20** documents both, and also lists param 48 (`deadzone_m3`), which it had
  missed since 2.8.0.
- **`webUiMock/mock_server.py`** follows the setting, off by default: address 40, the `M3_*`
  keys, the fault flag and commissioning.
- **`bin/at_wpos_fitted.py`** is this release's acceptance test, in stages.

### Docs

- the admin manual 1.22: the setting, and that it is off after an update and after a level-2 or
  level-3 reset;
- the farmer manual 1.19: no `M3 · 40` row without a sensor, and the badge only for a fitted one;
- FR-WP23 in the requirements study;
- the TSDS motor NVS row, which now also lists `deadzone_m3`;
- the plan's *Fitted or not* section;
- two gotcha-log entries.

## Who writes and who reads

**`wpos_fitted_m3`:**

- **Declared** by one row in `firmware/config/cfg_desc.inc`.
- **Written** only through `POST /api/config` (T11, the Motors tab). The IO0 level-2 and level-3
  resets put it back to 0, because they erase the `motor` namespace (T8).

  The other Q4 producers (`grep -rn 'xQueueSend(Q4\|post_q4('`) do not write it:
  - T10 posts `lat_*`, `lon_*` and `wifi/ap_enable`;
  - the LCD (T8) posts only keys from its parameter tables, which hold climate and wind keys, and
    `wifi/ap_enable`.
- **Read** by:
  - T17, through `dm_wpos_fitted_m3()`, every 500 ms at most;
  - T4: `emit_bus_kpi()` hourly, and `dm_status_snapshot()`, which feeds `/api/status`, the
    WebSocket push and T14's remote POST;
  - T11: `GET /api/config`, and the commissioning refusal in bench builds.
- **Published** by `GET /api/config` and `/api/config/limits`, **mirrored** in
  `webUiMock/mock_server.py`, and **decoded** by `logparser.py`.

**Gate reason 5 (`WPOS_GATE_NOT_FITTED`):**

- **Written** by T17 (`publish_mode()`).
- **Read** by:
  - `dm_status_snapshot()`;
  - `GET /api/diag/windowpos`, as `reason_str` `not_fitted` (bench builds);
  - `logparser.py`, as param 248 `value_b`.

## Size

|  | 2.8.0 | 2.9.0 | delta |
|---|---|---|---|
| app image | 1 391 472 | **1 392 336** | **+864 B** |
| `.flash.text` | 937 162 | 937 746 | +584 B |
| `.flash.rodata` | 309 596 | 309 884 | +288 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 112 | 42 128 | +16 B |

The bench image is 1 406 272 B (+976).

## Verification

**Builds and checks**
- The release and bench builds compile. Every warning is of a kind this change does not touch:
  - missing initializers in `web_server.cpp`'s route table;
  - `volatile` increments in `event_logger.cpp` and `status_post.cpp`;
  - two stack-usage warnings in the OTA upload handlers.

  None comes from a changed header.
- `check_cfg_desc.py`: 53 keys, 42 published, and the bounds, shadow fields and mock all agree.
  `gen_cfg_desc.py --check` still reproduces all 51 migrated rows.
- The worst-case `GET /api/config` body is 959 bytes, in a 1 536-byte buffer.

**Web GUI against the mock**
- **API, 15 checks, all passed:**
  - the limits;
  - the default of 0;
  - with the setting off: no address 40, no position, no flag, a teach refused and an abort
    allowed;
  - the setting switched on and read back;
  - with it on: address 40 and the position present;
  - with it on and the sensor not answering: the flag, and no position;
  - a refresh accepted.
- **Page logic, exercised in the browser:**
  - Off: the group is greyed, the reason is shown above it, and the card is not greyed a second
    time.
  - On, on a release build: the card is greyed with the release-build reason.
  - On, on a bench build: the reason says it is a fault.
  - Firmware version not yet known: a neutral reason.
  - A live card: shown, not greyed.
  - A firmware without the field: nothing greyed.
- **Layout not checked in the browser.** The settings pane needs an admin login, and no PIN was
  entered in the browser.

**Hardware: 2344 in the dev rig, the 2.9.0 release image, encoder connected**
- **Update:** pushed with `ota_push.py`; after the reboot, `fw_ver` and `asset_version` both
  read 2.9.0.
- **Default after the update (`at_wpos_fitted.py unfitted`): PASS.** The setting read 0. For
  300 s, none of 30 status readings showed address 40, a position or the fault flag.
- **Switched on after 331 s of uptime (`on`): PASS.**
  - The sensor appeared 0.1 s after the read-back, with no flag.
  - Address 40 had **4 transactions since boot**, all successful: the switch-on's own identify
    probe, orphan check, first idle read and restart check.
  - A T17 still reading at rest would have had about 22 by then. **So nothing reached address 40
    while the unit was not fitted.**
- **Switched off at run time, then on again (`off`): PASS.**
  - Address 40 and the position disappeared 0.1 s after the read-back, with no flag.
  - None of 30 readings over 300 s showed them.
  - Switched on again, address 40's transactions had grown by **4**, the switch-on's own.
    A T17 still reading at rest would have added about 20.
- **SD log (`log`): PASS.**
  - Each of the update's two reboots (firmware, then assets) logged one
    `TIMED [no position sensor fitted]`, 7 s after the boot row.
  - Each switch-on logged the audit row (param 49), then `TIMED [probing]`, then
    `TIMED [sensor present and trusted]`, all within a second.
  - The switch-off logged its audit row and `TIMED [no position sensor fitted]`.
  - A switch off and back on 6 s later at 14:18, made through the web interface and not by the
    test script, was followed the same way, within the same second.
  - No position rows (`SENSOR_HR ch3`) were written while the unit was not fitted.
  - **Hourly bus rows (`LOG_SYSTEM 31`):**

    | Rows at | Setting | Addresses |
    |---|---|---|
    | 14:50 | on | 1, 40, 44 |
    | 15:49 | off | 1, 44 |

    T4's "hour" is 59 minutes here, because every sensor reading ends one of its 1 s waits
    early.
- **Not run yet: the encoder unplugged with the setting on (`unplug`).** The first attempt
  timed out after 30 min because the encoder was never unplugged (124 successful reads, 0
  failed), so it produced no result.

## Upgrading

- **No partition change and no NVS migration.** The new key is absent after an update, so the unit
  boots with the default: **not fitted**.
- **A unit that has a position sensor must be switched on once:** Motors → M3 → *Linear control* →
  *Position sensor fitted* = Yes. Today that means the dev rig's modules, FDA4 and 2344.
  - Until then such a unit ignores its sensor: no position, no position faults, no position rows.
  - The same applies after an IO0 level-2 or level-3 reset.
- **5C88 needs nothing.** It has no encoder, and the default is right for it. When its encoder is
  installed, switch the setting on at the site, because there is no remote GUI path. The sensor
  also has to be taught, which needs a bench build.
- **Firmware and web assets ship as a pair, as always.** The 2.9.0 GUI reads the new field.
- **Read 2.9.0 logs with this release's `logparser.py`** (`logparser.md` 1.20).
- **Clients of the web API:**
  - `GET /api/config` has a new field, `wpos_fitted_m3`;
  - `POST /api/diag/commission` (bench builds) can answer `{"ok":false,"error":"not_fitted"}`;
  - `GET /api/diag/windowpos` (bench builds) can report `reason_str` `not_fitted`.
- **ROTA does not offer 2.9.0 to a unit that already runs a pushed 2.9.0 build, a bench build
  included.** The version compare ignores the `-bench` suffix. To test the ROTA path on such a
  module, push 2.8.0 back first.
- **Keep this release's ELF.** The image embeds the ELF hash, and a rebuild elsewhere gives a
  different image.
- **Production runs 2.3.1**, so 5C88's step would be 2.3.1 → 2.9.0. Regenerate the release
  comparison before promoting.

## Known limitations

- **The setting is the operator's word, and nothing checks it.**
  - A unit set to not fitted that has a sensor reports nothing about it.
  - A unit set to fitted without one shows a permanent fault.

  The GUI says what the setting does, and the log records every change (param 49).
- **A reversal mid-stroke is not judged**
  ([gh#72](https://github.com/pe1mew/greenhouse-Controller/issues/72)). Unchanged from 2.8.0.
- **The fault thresholds are validated on the rig only.** Unchanged.
- **The at-end exemption's continuity break is not tested on hardware.** Unchanged.
- **No DEGRADED threshold for the bus** (gh#66). The rig's addresses 1 and 44 are emulated
  slaves that are out of spec (gh#68).
- **ROTA re-downloads both artefacts on every deferred apply**
  ([gh#71](https://github.com/pe1mew/greenhouse-Controller/issues/71)).
- **Commissioning is bench-only.**
- **Older comments in `window_pos_task.h`** still describe the first design: a 2× rejection
  limit, and no reads at rest. The new gate section is current. The code is correct.
