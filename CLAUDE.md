<!-- agent-ready-projects: v1.10.3 -->
# Greenhouse Controller — Claude project guide

Auto-loaded by Claude Code into every session in this repo. Keep terse; deeper material lives behind the pointer table below. **A row here holds the rule; its reasons and incidents go to [memory/before-you-start.md](memory/before-you-start.md), not here.**

## Identity

ESP32-S3 firmware for a greenhouse ventilation controller. ESP-IDF 5.5 via PlatformIO (`framework=espidf`, `espressif32@6.12.0`). FreeRTOS task-graph design. Custom partition table (dual app banks + dual LittleFS + coredump). Dutch operator manuals (boer + beheerder).

**There are two hardware rigs, and the development rig takes a SWAPPABLE Lolin ESP32-S3 module:**

- **Production rig** — at Herenboeren Willemshoeve (Soest), module **5C88**, ROTA `mainstream`. Runs 2.17.0 since ~2026-10-28 (pushed on site; `mainstream` still points at 2.3.1), with the M3 rope sensor taught 2026-10-30. Remote, behind NAT: no push path except from its own LAN on site (`192.168.1.104`).
- **Development rig** — SD and relays live, the M3 rope rig and the **real wire encoder at Modbus addr 40**, ROTA `soak`. **The T/RH (addr 1) and S200 wind (addr 44) slaves are EMULATED here.** The emulator (`192.168.20.226`, `sensor-emulator.local`) normally runs in REST, fed every 60 s by Node-RED on Shuttle2 with 5C88's live status from `rfsee.net/hbwv`: the rig acts on production's climate, and a value set on the emulator is overwritten within a minute unless that slave is in Manual. **Any bus measurement on this rig for addr 1 or 44 characterises the emulator, not the field sensors.** `FDA4` and `2344` are modules, not units: they take turns in this one rig, and only one is fitted and powered at a time.

**A dev module that does not answer is almost certainly the one not fitted — that is expected, not a crash.** Establish which module is in the rig (`unit_id`) before concluding a board has hung. **Swapping modules ends any soak**, and the SD card stays with the rig, so a swap splices two firmwares into one log file.

Fixed DHCP reservations: **`FDA4` is `192.168.20.169`**, **`2344` is `192.168.20.160`**. ROTA config is per-module and an IO0 reset erases it. `devices.json` MAC keys and access routes live in user-global memory.

## Hard constraints

### Workflow (inherits from `~/.claude/CLAUDE.md`)

- **Never `git commit / push / merge / rebase / force-push`.** Stage changes and a suggested commit message; hand off to the user.
- **Commit to `main` directly** — no `claude/...` feature branches, no PRs unless explicitly requested.
- **Operator-commissioned branches are rare and short-lived.** Integrate one by **rebasing it onto `main` and fast-forwarding**, never by merging `main` into it, and never put a commit on `main` that deletes the branch's files. Start the replay from the branch's last OWN commit, on a scratch branch. `git pull` after a rebase undoes the work. Method: before-you-start §7.
- **Branch protection rejects merge commits on `main`** — rebase + fast-forward only.
- **No `--no-verify` / hook-skip / `--no-gpg-sign`.** Fix the hook, don't bypass it.
- **The hooks live in `.githooks/` and are OPT-IN per clone.** Re-check with `git config core.hooksPath` (expect `.githooks`): a hook that is not installed enforces nothing.
- **Two copies of the WiFi credentials exist** (gh#56): the app's `wifi/ssid` + `wifi/psk`, and ESP-IDF's own `nvs.net80211`. **Anything that erases or provisions WiFi must handle both copies** (`nm_wifi_erase_persistent()`).

### Web GUI

- **A control the operator cannot use right now is GREYED OUT, never hidden.** Dim it, make it inert, and **say why next to it**: `.disabled-block` + `.disabled-why` in `style.css`, `button:disabled` for a lone button, `commSetAvailable()` in `app.js` as the reference. **The reason goes ABOVE the greyed element, never inside it** (gh#74): `opacity` dims a reason inside the block. **Exception, narrow:** content that does not exist at all (a row with no data, a farmer-vs-admin surface) may be absent; a feature that exists and is currently unavailable may not.

### Releases & OTA

- **Paired-commit invariant.** Push firmware AND assets within 120 s of each other (`FW_DONE` fallback timer commits firmware alone after that, stranding the asset partition).
- **Verify post-OTA** by reading both `fw_ver` AND `asset_version` from `/api/status`. Neither alone is sufficient — a mismatch means the asset partition was left behind.
- **SemVer cadence:** feature releases bump minor (1.16.x → 1.17.0); patch is for bug-fix-only. Ask: *did this add a user-visible feature, a new task, a new NVS namespace/key, or a payload-shape change?* If yes → minor, reset patch to 0. Don't let a run of patch-only commits pull the next bump into patch by inertia.
- **Greenfield flash uses `--flash_mode dio`** for the bootloader header byte. Runtime `board_build.flash_mode = qio` stays. Mixing them causes an `ets_loader.c` boot loop.
- **First flash on every new unit must erase the coredump partition** at `0x620000` (size `0x10000`). IDF reads garbage otherwise and panics on every subsequent boot.

### Honesty

- Never claim firmware was tested without a real hardware run (or soak data confirming the change held under load).
- Never claim "OTA succeeded" without a post-reboot `/api/status` query confirming both `fw_ver` and `asset_version` match.
- Never claim "no SD log gaps" without actually parsing timestamps — the HEAD endpoint is broken (see gotcha log).
- When something cannot be verified in this environment, say so explicitly.

## Before You Start

`§N` = a section of [memory/before-you-start.md](memory/before-you-start.md).

| Task | Read first |
|---|---|
| Touching a FreeRTOS task or subsystem | [memory/architecture.md](memory/architecture.md) — task graph T1-T17, subsystem map, partition layout |
| Bumping firmware version | `firmware/platformio.ini` line 93 — feature releases bump minor, not patch |
| OTA work (any) | [design/OTAimplementation.md](design/OTAimplementation.md). Internet-pull OTA (**ROTA**): TDS [design/rota_tds.md](design/rota_tds.md), plan [design/rotaImplementationPlan.md](design/rotaImplementationPlan.md), study [design/remoteOTAstudy.md](design/remoteOTAstudy.md). Server and operator guide in the separate `greenhouse-Controller-FOTA-server` repo |
| Changing partition table | [design/migrationPlan_FullESP-IDFmigration.md](design/migrationPlan_FullESP-IDFmigration.md); update [firmware/partitions.csv](firmware/partitions.csv) header comment |
| Changing T9/T14 audit log format | **Append to `log_type_t`, never insert** (`types/app_types.h`): the ordinal is in every archived row. `log/logparser.py` learns each new encoding in the same change. **The emitters are authoritative, not the comment**: grep them before claiming a subtype is free. A second emitter on an event type needs its own `param_id` and a parser branch (gh#54). §1 |
| Changing the build pipeline | [bin/build_release.ps1](bin/build_release.ps1) — the manifest placeholder dance (Steps 0 and 3.5), gh#9 |
| OTA-pushing to a unit | `python bin/ota_push.py <bin-path> --host <ip>`. **The default host is FDA4's**, so pass `--host 192.168.20.160` when 2344 is fitted; identify by `unit_id` first. 5C88 only from its own LAN. Default PIN `12345678` (admin), `1234` (farmer). **After about six reboots in an hour `ntp_synced` stays false for 5-15 min while the clock is right**: count the reboots before investigating. Keep 40 s of uptime between reboots of a fresh image, or the boot-loop guard falls back a bank |
| Publishing a release to ROTA | `python bin/rota_release.py release <version>` (→ GitHub Release → soak, **recommended**) / `publish` / `promote` (→ mainstream) / `status` — [bin/rota_release.md](bin/rota_release.md); config in `bin/.rota_release.env` |
| Preparing a production candidate (anything headed for `mainstream`) | Regenerate §2 of the [release comparison](design/releaseComparison_2.3.1_vs_2.17.0.md) against the production tag: every surface extracted at both tags, the control-path diffs read in full. **When a change gates a shared queue, table or key set, enumerate every producer/consumer with `grep` and paste the list into the release notes.** Record the heap as steady-state free + largest block at a stated uptime, **not** `heap_min_kb` (gh#81) |
| Filing / commenting / closing a GitHub issue | `python bin/gh_issue.py {list,show,create,comment,close,reopen,edit}`. **No `gh` CLI here.** `401 Bad credentials` on every call means the PAT lapsed, not a script bug. Never name where the token lives (the operator's secret store) |
| Adding or changing a config key, or its bounds | **A key is declared ONCE, in [firmware/config/cfg_desc.inc](firmware/config/cfg_desc.inc)** (gh#64); verify with `python bin/check_cfg_desc.py`. **A wrong field in `CFG_SH()` compiles and quietly writes a different setting:** believe the checker. Bounds live in `firmware/config/cfg_limits.h`, but check the consuming task's own clamp too (gh#57). **`POST /api/config` is asynchronous**: a read-back right after it returns the PREVIOUS value. Stored values are not clamped at load. §2 |
| Anything affecting greenhouse behaviour | [design/functionalRequirementsSpecification.md](design/functionalRequirementsSpecification.md); soak on the **fitted dev module** before any production push |
| Changing motor travel/dwell config, or anything T2 caches | **`travel_m3` belongs to the RIG, never to the module:** production 171 (a 176 s stroke), rig 13. **UNDER-travel is the real risk:** a rig value in production opens M3 about a tenth while the log records OPEN. Read `travel_s` after every swap, reset or reflash; likewise **`wpos_fitted_m3` = 1** on a rig module (gh#73). **In mode 2 M3's dwells are REPLACED by `min_intv_m3`**; the mode that acts is the EFFECTIVE one (`dm_m3_ctrl_mode()`). **Before touching Q4, `/api/config` or `cfg_shadow_t`, enumerate the producers: `grep -rn 'xQueueSend(Q4\|post_q4('`.** **If T6 "is out of sync", it is being refused, not confused.** §3, [design/fixMotorTimingRefresh.md](design/fixMotorTimingRefresh.md) |
| Touching the Modbus/RS485 bus (driver, or any new caller) | **Single-caller is policy:** ~215 ms of blocking must stay out of T2/T3. The receive loop never yields (TWDT); a re-init takes the bus mutex (gh#69); `MODBUS_IFG_US` (20 000 us) is **paid before `bus_unlock()`**, and a slave that reads a request as a continuation answers with **zero bytes and a timeout, never a CRC error**; the UART drives DE/RE (gh#70). **Do NOT raise `MODBUS_IFG_US` past 20 ms on rig evidence:** the rig's addr 1 and 44 are emulated and out of spec on levels (gh#68). **Diagnose from counters, not a scope:** `python bin/bus_watch.py --host <ip>` (any build) for per-interval ok/err, `GET /api/diag/windowpos` on a bench build. **A bus failing as a whole at a correct 60 Ω wants a terminator OFF** (gotcha 2026-10-30). §4, [design/addModbusMutex.md](design/addModbusMutex.md) |
| Touching T5 / sensor polling, or adding a sensor | [design/refactorSensorConfiguration.md](design/refactorSensorConfiguration.md) — **study, not implemented** |
| Fitting a window position sensor (M3) | **Without a trusted position nothing acts on it.** T17 (`firmware/src/window_pos/`, driver `drivers/windowPos/`, Modbus **address 40**) measures and reports; **mode 2 (factory default) drives M3 to a measured target** and falls back to mode 1 on its own. Admin: teach, then the characterisation run (`characterise.cpp`, `drivers/m3Char`). **Traps:** soaks count JUDGED strokes; a teach needs BOTH end sensors after arming and holds STANDBY until its session ends; **measure positions LIVE**, because status carries T17's cache. **Read first:** [design/integrateWindowPositionSensor.md](design/integrateWindowPositionSensor.md) §2a, §5b/§5c; [design/ventModelContract.md](design/ventModelContract.md). §5, §7 |
| Tuning climate/vent behaviour, or judging whether it's misbehaving | **The law lives in ONE place: `drivers/ventModel`** (`stepped` = mode 1, `graded` = mode 2 and the chosen law; both v2, each pinned by a decision digest; `pio test -e native`). T6 keeps what is around the law (inhibit mask, snapshots, actuator limits, close-before-open order, Q1). **New laws go behind [design/ventModelContract.md](design/ventModelContract.md), never into T6.** A law change re-runs the host tests, `model/vent_step_replay.py` and `model/closedloop/closed_loop.py gate-control`. On hardware: `bin/law_watch.py` beside the soak, `bin/law_steer.py` when the climate gives the law nothing to decide, `bin/law_conformance.py` afterwards. §7 |
| Analysing heap/leak behaviour from SD logs | `python log/heap_soak.py <SD .csv>` — a leak is a slope *within* a boot segment, not a step between |
| Touching SD mount state, SD logging, or T9's automount | **`s_sd_ok == false` has two meanings:** unavailable (T9 retries) and released on request; keep the `s_sd_released` latch at both automount sites (gh#61). **Decide nothing from a list-based scan:** use `storage_sd_foreach_csv()` (gh#82). **Retention is PER UNIT.** **Every SD operation goes through `drivers/sdCard`, under ONE recursive lock** (gh#90): opening files under the mount point directly brings the panic back. §6, §7 |
| Anything weird / unexpected | [memory/gotcha-log.md](memory/gotcha-log.md) — check before debugging from scratch |

## Release cycle

1. Edit code; bump `-DFIRMWARE_VERSION=\"X.Y.Z\"` in `firmware/platformio.ini`
2. `powershell -ExecutionPolicy Bypass -File bin/build_release.ps1`
3. New `## [X.Y.Z]` section at top of `changelog.md`; write `bin/X.Y.Z/release-notes.md` (use prior release's structure)
4. Hand off staged changes for the user to commit
5. OTA push to the **fitted dev module** (read `unit_id` first): `python bin/ota_push.py bin/X.Y.Z/greenhouse-controller-X.Y.Z.bin --host <ip>`
6. Verify both `fw_ver` and `asset_version` from `/api/status`
7. Soak ≥ overnight (or longer for high-risk changes)
8. Production (5C88) only after soak passes: `python bin/rota_release.py promote <ver>` (needs the operator's instruction), and it applies in its own night window, or a push from its LAN on site

## Key directories

| Path | Contents |
|---|---|
| `firmware/src/` | Production firmware source (`diag/` is dev-only, compiled out of release builds) |
| `drivers/` | Host-testable libraries, each with an `[env:native]` unity suite (`pio test -e native`) and a thin `firmware/components/<name>/` proxy: `modBus`, `windowPos`, `gpio`, `i2c`, `LCD1602_I2C`, `keyPad`, `nvs`, `littleFS`, `sdCard`, `DS1307_RTC`, `FG6485A`, `s200`, `ventModel` (the T6 law), `m3Char` (characterisation arithmetic), `m3Hold` (LCD tap-or-hold). `relay_sequence_test/` is a standalone bench sketch |
| `design/` | FRS, TSDS, implementation/migration plans, audit reports, `OTAimplementation.md` — the closest thing to ADRs |
| `bin/<version>/` | Per-release archive: bin, zip, bootloader, partitions, elf, map, release-notes; `manifest-<version>.json` (ROTA seq ledger — tracked in git) |
| `manual/` | Operator manuals (Dutch: boer + beheerder + admin) |
| `model/` | Plant model, calibration scripts, campaigns: `campaignResults_summer2026.md`, `thermalProfileCampaign.md` |
| `log/` | `logparser.py` + `logparser.md`, example log CSV + parsed output |
| `memory/` | L3/L4 layer — `MEMORY.md` index, `architecture.md`, `gotcha-log.md`, `before-you-start.md` (the long form of the rows above) |
