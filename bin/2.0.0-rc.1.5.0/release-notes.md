# 2.0.0-rc.1.5.0 — release notes

**Date built:** 2026-05-26
**Built on top of:** 2.0.0-rc.1.4.0 (SD-log format upgrade)
**Closes:** [gh#28](https://github.com/pe1mew/greenhouse-Controller/issues/28) (MODE_STANDBY end-to-end), [gh#29](https://github.com/pe1mew/greenhouse-Controller/issues/29) (admin manual motor control)

---

## Why a minor bump (1.4.0 → 1.5.0)

Three reasons compound:

1. **New operating mode reachable end-to-end.** `MODE_STANDBY` has been a declared-but-orphan enum value since the original 1.x design. rc.1.5.0 wires the bit, the persistence, the LCD surface, the web surface, the API endpoint, the T6 inhibit, and the audit-log row — all six landed in one release. That's a new operational primitive, not a patch.
2. **New `EG1` bits + new `cmd_source_t` value.** `EG1_BIT_STANDBY` (bit 7), `EG1_BIT_MANUAL_SESSION` (bit 8), and `SRC_OPERATOR_MANUAL` extend the shared cross-task vocabulary that every task uses. Adding to the project's IPC enum set is a minor-version-level change.
3. **New `POST /api/mode` endpoint.** Third-party integrations that script the controller (status dashboards, CI test harnesses, the operator's phone bookmarks) get one new endpoint to know about.

---

## What changed

### Change A — `MODE_STANDBY` reachable (gh#28)

The mode enum value lived in `firmware/src/types/app_types.h::op_mode_t` and the JSON serialiser knew how to print it (`status_json.cpp:42`), but no code path ever assigned it. rc.1.5.0 wires the full chain:

- **EG1 bit.** `EG1_BIT_STANDBY` (bit 7). Priority in the mode-derivation chain in `dm_status_snapshot()`: `MOTOR_ALARM → WIND_OVERRIDE → STANDBY → AUTOMATIC` — STANDBY ranks below safety so wind/alarm still win.
- **NVS persistence.** New key `system/mode_standby` (i32: 0 = automatic, 1 = standby). Loaded into EG1 at T4 boot via `nvs_load_mode()`. A power blip during a deliberate STANDBY does NOT silently re-enable climate control on reboot — the unit comes back up in STANDBY.
- **Setter API.** `dm_set_standby(bool standby, log_initiator_t initiator, uint8_t channel)`:
  - Idempotent (already-in-state calls are no-ops, no log row, no NVS write).
  - Toggles `EG1_BIT_STANDBY`.
  - Writes `system/mode_standby` via `nvs_cfg_set_i32()`.
  - Emits a `LOG_MODE_CHANGE` audit row: `initiator = caller-supplied`, `channel = surface hint (0 web / 1 LCD)`, `value_a = 1 enter | 0 leave`, `value_b = 0`.
  - On STANDBY exit, posts `CMD_RECALIBRATE` to Q1 → T2 runs synchronous `calib_close_all()` with `EG1_BIT_CALIBRATING` set for the duration. Windows return to a known CLOSED baseline before T6 resumes.
- **T6 (climate control) gate.** `EG1_BIT_STANDBY` added to the inhibit mask in `task_climate_control()` alongside `WIND_OVERRIDE` / `MOTOR_ALARM` / `SENSOR_FAULT_T`.
- **LCD surface — Scherm 3 (Mode + Session).** Page-2 mode-string set extended with `"Mode: STANDBY   "` for the new state. `#`-key dispatch added: enters `UI_MODE_TOGGLE` (1 = Automatic, 2 = Standby, * = back). Either Farmer or Admin PIN is accepted — the digit count entered at submission (4 or 8) selects the role. Per the locked design: STANDBY is a routine operational toggle, not a configuration change.
- **Web surface — Climate-tab toggle.** New "Mode" control at the top of the Climate tab. Both Farmer and Admin sessions can use it. WS-driven mirror of the live mode. Greyed out (select + Apply disabled) when `WIND_OVERRIDE`, `MOTOR_ALARM`, or `CALIBRATING` is active (those override operator intent — safety/early-boot states win the priority chain).
- **API — `POST /api/mode`.** Body `{"mode":"automatic"|"standby"}`. Farmer or Admin auth. Returns `{"ok":true,"mode":"automatic"|"standby"}` with the post-transition state. Audit-logged via `dm_set_standby()` with `LOG_BY_WEB`.

### Change B — Admin manual motor control (gh#29)

Admin can now open/close M1/M2/M3 directly from the unit's keypad without going through the web GUI. Use cases: maintenance with gloved hands, network outage, commissioning, emergency override, community demos.

- **EG1 bit.** `EG1_BIT_MANUAL_SESSION` (bit 8, RAM-only — meaningless across reboot). Set on Scherm 6 menu entry; cleared on every exit path:
  - Explicit `*=back` from `UI_MOTOR_PICK` (cleared in `handle_motor_pick`).
  - 10 s idle dismiss (new `MANUAL_MENU_IDLE_TICKS = 100u`, applies only to `UI_MOTOR_PICK` / `UI_MOTOR_ACTION`; `go_status()` clears the bit).
  - PIN-session timeout (`session_close(true)` clears the bit alongside the session log row).
  - Menu-auto-return tick.
- **New `cmd_source_t::SRC_OPERATOR_MANUAL`.** T8 becomes a Q1 producer (was previously T3 + T6 only — the C9 "manual window commands out of scope" constraint is explicitly lifted by this release).
- **T2 dwell-timer policy.** Reframed from `source != SRC_T3` (bypass for T3 only) to `source == SRC_T6` (observed only for the autonomous loop). Effect: SRC_T3 (safety) and SRC_OPERATOR_MANUAL (deliberate admin override) both bypass the dwell timer. The admin's explicit choice overrides anti-thrash protection that exists to gate autonomy.
- **New `CMD_RECALIBRATE` action.** Dispatched by T2 to the existing synchronous `calib_close_all()`. Used by gh#28's STANDBY-exit path; also available as a general "re-establish baseline" primitive.
- **LCD surface — Scherm 6 (Window states).** `#`-key dispatch added: enters `UI_MOTOR_PICK`. Admin PIN only — per the locked design, farmers are supported by the controller's autonomous logic, not by bypassing it.
  - **UI_MOTOR_PICK** — Row 0: per-channel state (`OPEN CLOS UNK ` etc.), 16 cols. Row 1: `1=M1 2=M2 3=M3*B`. Selecting a channel advances to `UI_MOTOR_ACTION` with `s_motor_pick_ch` set.
  - **UI_MOTOR_ACTION** — Row 0: `[Mx] OPEN/CLOSED/MOV>/MOV<` with bracketed motor header. Row 1: `1=Open 2=Cls *Bk`.
- **Safety gates remain authoritative.** Refusal is non-silent (a transient LCD message):
  - `EG1.MOTOR_ALARM` → blocks all manual commands.
  - `EG1.CALIBRATING` → blocks all manual commands (boot or STANDBY-exit calibration window).
  - `EG1.WIND_OVERRIDE` → blocks manual OPEN; CLOSE accepted.
- **T6 gate.** Inhibit mask extended to `STANDBY OR MANUAL_SESSION` (in addition to safety/sensor-fault bits). T6 yields to the admin's manual positions until the session exits.
- **Audit log.** Every manual command produces the existing `LOG_RELAY` audit row when T2 actually energises the relay (`log_relay_event` from `ch_start_open` / `ch_start_close`). The `cmd_source_t source = SRC_OPERATOR_MANUAL` field in the Q1 message threads admin attribution through T2's per-command log line (`process_command` now logs `"from ADM"`). No new log type and no logparser.py change required.

### Other adjustments

- `process_command()` source-string helper: `src_name(cmd_source_t)` consolidates the per-command log-line source string (`"T3"`, `"T6"`, `"ADM"`).
- Web `cfg.max_uri_handlers` bumped 32 → 32 (unchanged, but the comment updated). 30 routes now registered; 2 spare slots.
- Status-page-2 (Scherm 3) Mode string set extended to include `"Mode: STANDBY   "`.

---

## What did NOT change

- **rc.1.4.0 SD-log format** (`LOG_SENSOR_HR` triplet + `LOG_SUN` change-detect) is preserved byte-for-byte. Cadence (30 s default), encoding (T at 0.1 °C precision, wind dms + dir, window-state bitmask), and emit-site (T4's `handle_sensor_reading`) are all unchanged.
- **Rotation defaults** bumped in rc.1.4.0 (1 MB / 30 files / 5 floor / 4 MB free) are preserved.
- **LOG_SENSOR_HR ch=2 window-state bitmask** format is unchanged. Bit 15 stays reserved — STANDBY visibility happens via `LOG_MODE_CHANGE` rows, not via the bitmask. (A future rc could optionally set bit 15 on STANDBY for finer-grained per-sample mode tracking; not in scope here.)
- **Parser / plotter** — `log/logparser.py` and `temp/plot_daily.py` work on rc.1.5.0 SD files without modification. STANDBY transitions show up as the already-decoded `LOG_MODE_CHANGE` (`MODE`) rows; admin manual commands show up as `LOG_RELAY` rows just like any other relay action.
- **Window-state NVS persistence** (`t2_st_ch0..2`) handles admin manual final positions automatically via the existing `persist_ch_state(ch, CH_OPEN|CH_CLOSED)` call at travel-timer expiry. No new NVS key for "last manual position" — the existing per-channel key is the right answer.
- **rc.1.3.3 NTP-resync fix** is preserved (the `sntp_stop()` call in T10's `run_ntp_resync()`).
- **rc.1.4.0 web assets** are NOT byte-identical (we added the Climate-tab toggle to `index.html` and the `postMode()` helper + `handleStatus()` mirror to `app.js`). 4 files in the assets ZIP, sizes unchanged for `style.css` / `manifest.json`; both `index.html` and `app.js` grew slightly.

---

## Build delta vs rc.1.4.0

| File | rc.1.4.0 | rc.1.5.0 | Δ |
|---|---:|---:|---:|
| `greenhouse-controller-*.bin` | 1 353 088 B | 1 356 928 B | **+3 840 B** |
| `web-assets-*.zip` | 101 133 B | 103 864 B | **+2 731 B** |
| Bootloader | 22 528 B | 22 528 B | 0 |
| Partition table | 3 072 B | 3 072 B | 0 |
| `firmware-*.elf` (debug) | 13 051 944 B | 13 071 196 B | +19 252 B |

Link-report usage (`pio run` output, from the `.elf`):
- RAM: 18.9 % (62 016 / 327 680 B) — unchanged from rc.1.4.0.
- Flash: 64.7 % (1 356 517 / 2 097 152 B) — was 64.5 % on rc.1.4.0.

Build is clean: **zero source-tree warnings** (full `pio run` log grepped with `-vE "system_includes|deprecated|sdkconfig|pragma message|esp-idf|components/"`).

---

## SHA-256

```
3fb7c67b775fc971462f6bda588e5580a2353cccd4ed1146c5ca42ec73226c42  greenhouse-controller-2.0.0-rc.1.5.0.bin
ac85d38838b8cf1d36855f4ae8f433807ebe23bf228b08e9f45b7ad48f44df15  web-assets-2.0.0-rc.1.5.0.zip
```

---

## Deployment record

| Step | Status |
|---|---|
| Build artifacts produced | ✅ 2026-05-26 (this directory) |
| SHA-256 captured | ✅ |
| OTA-flashed to soak unit (`192.168.20.160`, unit 2344) | ⏳ **not yet** (per operator instruction "do not OTA" at the implementation request) |
| Phase 7 soak clock started | ⏳ pending OTA |

---

## Verification checks (post-OTA)

When rc.1.5.0 is eventually OTA'd, the following 8 checks confirm gh#28 + gh#29 are working end-to-end. None of them require source-code access — all are observable via the web API and the SD log.

### Smoke / cold-start

1. **Boot + status.** `GET /api/status` shows `fw_ver = 2.0.0-rc.1.5.0`, `asset_version = 2.0.0-rc.1.5.0`, bank flipped, accepted=true, `eg1 = 0`. Mode = AUTOMATIC.
2. **SD log header.** The active SD CSV file shows the post-reboot `BOOT (SW=3)` row followed by `SUN` and the usual heap/uptime cadence — same shape as rc.1.4.0's first-row block.

### gh#28 — MODE_STANDBY

3. **Web set + persist.** `POST /api/mode` body `{"mode":"standby"}` (admin or farmer session) → 200 `{"ok":true,"mode":"standby"}`. `/api/status` shows `mode.current = STANDBY` within one WS tick (~2 s). SD log gets a `MODE,WEB,0,0,1,0` row.
4. **Reboot survival.** Power-cycle the unit. After boot: `/api/status` shows `mode.current = STANDBY` immediately (no operator action needed). SD log shows the boot row followed by a `T4` log line `STANDBY restored from NVS`.
5. **Exit recalibrates.** `POST /api/mode` body `{"mode":"automatic"}` → 200. `/api/status` mode flips to `AUTOMATIC` and the `calibrating` flag appears for the duration of the calibration sweep (~3 min worst-case for M3). SD log: `MODE,WEB,0,0,0,0` followed by three `RELAY,SYS,1,0,1,...` (`MOVING_CLOSE`) rows, then three `RELAY,SYS,1,0,2,...` (`CLOSED`) rows once each motor's travel timer expires.

### gh#29 — Admin manual motor control

6. **Scherm 6 # admin-only.** On the LCD, press `D` to advance to Scherm 6 (Window states), press `#`. With no admin session active: PIN-entry screen requests 8 digits. Entering the admin PIN (or having an existing admin session) enters `UI_MOTOR_PICK`. EG1 bit 8 set (visible as `eg1` bit in `/api/status`).
7. **Dwell-timer bypass.** Issue a manual CLOSE immediately after an autonomous OPEN (e.g. while the channel is in dwell). The manual command goes through; the SD log gets `RELAY,SYS,N,0,4,...` (`MOVING_CLOSE`) within one T2 loop iteration (no dwell wait).
8. **Idle dismiss.** Enter the menu, wait 10 s without pressing any key. LCD returns to status rotation. `eg1` bit 8 clears. T6 resumes its decision tick on the next sensor cycle.

### Safety gates

(Optional, harder to stage without artificial conditions:)

- With `EG1.WIND_OVERRIDE` set (forced by an in-greenhouse gust above v_max), admin manual OPEN refused with a transient LCD message; CLOSE accepted.
- With `EG1.MOTOR_ALARM` asserted, all manual commands refused.

---

## Phase 7 soak acceptance criteria (rc.1.5.0)

Same shape as rc.1.4.0 — the 14-day clean-soak gate applies independently per release. Day-14 begins when rc.1.5.0 is OTA'd onto the soak unit.

| Criterion | Status (pre-OTA) |
|---|---|
| Zero PANIC / WDT during 14-day clean window | n/a — clock starts at OTA |
| Zero SW reboots | n/a |
| Zero motor-SSR alarms (ch 1/2/3) | inherited clean from rc.1.3.3 → rc.1.4.0 |
| Zero unexpected sensor-fault clusters | inherited clean |
| Heap not bleeding (min internal ≥ 40 KB, median ≥ 90 KB) | inherited clean |
| gh#28 — STANDBY toggle round-trip via web + LCD | ⏳ verify in week 1 of soak |
| gh#28 — STANDBY survives reboot | ⏳ verify in week 1 |
| gh#28 — STANDBY-exit calibration completes ≤ 4 min | ⏳ verify in week 1 |
| gh#29 — admin manual OPEN/CLOSE produces correct `LOG_RELAY` audit rows | ⏳ verify in week 1 |
| gh#29 — `EG1_BIT_MANUAL_SESSION` correctly clears on every exit path | ⏳ verify in week 1 |

---

## Analyst follow-on (for the operator)

| Item | Status |
|---|---|
| `firmware/issues.md` — flip gh#28 and gh#29 status flags from `(open — design locked; ready for implementation)` to `(implemented in rc.1.5.0; ready for soak)` | ✅ done in this release |
| TSDS update — EG1 table additions, T6 gate, Scherm 3 / Scherm 6 # flows, /api/mode | ✅ done in this release |
| FRS update — FR-MD0X (STANDBY) + FR-MM0X (manual motor) | ✅ done in this release |
| Changelog entry for rc.1.5.0 | ✅ done in this release |
| logparser.py / plot_daily.py change required? | ❌ no — LOG_RELAY + LOG_MODE_CHANGE rows already decode correctly |
| Update beheerderHandleiding / boerHandleiding / boerQuickRef for the new LCD flows | ✅ done in this release (boer §10.4 Standby; beheerder §10.10 Standby + §10.11 Handmatige raambediening; quick-ref Mode/Standby row + FAQ rows) |
| OTA-flash decision | ⏳ user choice — flagged as "do not OTA" at implementation request |

---

## Files in this directory

| File | Purpose |
|---|---|
| `bootloader-2.0.0-rc.1.5.0.bin` | 22 528 B — bootloader (unchanged shape vs rc.1.4.0; rebuilt for consistency) |
| `greenhouse-controller-2.0.0-rc.1.5.0.bin` | 1 356 928 B — firmware image (rc.1.5.0 main artifact) |
| `web-assets-2.0.0-rc.1.5.0.zip` | 103 864 B — STORE-only ZIP of `/data` for `POST /api/ota/assets` |
| `partitions-2.0.0-rc.1.5.0.bin` | 3 072 B — partition table (unchanged shape; rebuilt) |
| `firmware-2.0.0-rc.1.5.0.elf` | 13 071 196 B — debug ELF for `addr2line` if a crash needs investigation |
| `release-notes.md` | this file |
