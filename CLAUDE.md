<!-- agent-ready-projects: v1.10.3 -->
# Greenhouse Controller — Claude project guide

Auto-loaded by Claude Code into every session in this repo. Keep terse; deeper material lives behind the pointer table below.

## Identity

ESP32-S3 firmware for a greenhouse ventilation controller. ESP-IDF 5.5 via PlatformIO (`framework=espidf`, `espressif32@6.12.0`). FreeRTOS task-graph design. Custom partition table (dual app banks + dual LittleFS + coredump). Dutch operator manuals (boer + beheerder).

Production deployment at Herenboeren Willemshoeve (Soest). Field units in active service: **5C88** (production, ROTA `mainstream` channel), **FDA4** (ROTA dev/test bench), and **2344** (ROTA `soak`-channel soak/bench unit, back in active soak service — previously the plant-model training soak). All three run the 2.2.x ROTA firmware. Current addresses, `devices.json` MAC keys, and access routes live in user-global memory.

## Hard constraints

### Workflow (inherits from `~/.claude/CLAUDE.md`)

- **Never `git commit / push / merge / rebase / force-push`.** Stage changes and a suggested commit message; hand off to the user.
- **Commit to `main` directly** — no `claude/...` feature branches, no PRs unless explicitly requested.
- **Branch protection rejects merge commits on `main`** — if a branch must merge, rebase + fast-forward only.
- **No `--no-verify` / hook-skip / `--no-gpg-sign`.** Fix the hook, don't bypass it.

### Releases & OTA

- **Paired-commit invariant.** Push firmware AND assets within 120 s of each other (`FW_DONE` fallback timer commits firmware alone after that, stranding the asset partition).
- **Verify post-OTA** by reading both `fw_ver` AND `asset_version` from `/api/status`. Neither alone is sufficient — a mismatch means the asset partition was left behind.
- **SemVer cadence:** feature releases bump minor (1.16.x → 1.17.0); patch is for bug-fix-only. Decide by asking: *did this add a user-visible feature, a new task, a new NVS namespace/key, or a payload-shape change?* If yes → minor, reset patch to 0. Don't let a run of patch-only commits pull the next bump into patch by inertia.
- **Greenfield flash uses `--flash_mode dio`** for the bootloader header byte. Runtime `board_build.flash_mode = qio` stays. Mixing them causes an `ets_loader.c` boot loop.
- **First flash on every new unit must erase the coredump partition** at `0x620000` (size `0x10000`). IDF reads garbage otherwise and panics on every subsequent boot.

### Honesty

- Never claim firmware was tested without a real hardware run (or soak data confirming the change held under load).
- Never claim "OTA succeeded" without a post-reboot `/api/status` query confirming both `fw_ver` and `asset_version` match.
- Never claim "no SD log gaps" without actually parsing timestamps — the HEAD endpoint is broken (see gotcha log).
- When something cannot be verified in this environment, say so explicitly.

## Before You Start

| Task | Read first |
|---|---|
| Touching a FreeRTOS task or subsystem | [memory/architecture.md](memory/architecture.md) — task graph T1-T16, subsystem map, partition layout |
| Bumping firmware version | `firmware/platformio.ini` line 93 — and feature releases bump minor, not patch |
| OTA work (any) | [design/OTAimplementation.md](design/OTAimplementation.md) — full reference, 11 sections. Internet-pull OTA (**ROTA**, mainline since 2.2.x — developed on the now-merged `rota` branch): TDS [design/rota_tds.md](design/rota_tds.md), plan [design/rotaImplementationPlan.md](design/rotaImplementationPlan.md), study [design/remoteOTAstudy.md](design/remoteOTAstudy.md). Server + operator guide (`documentation/documentation.md`) in the separate `greenhouse-Controller-FOTA-server` repo |
| Changing partition table | [design/migrationPlan_FullESP-IDFmigration.md](design/migrationPlan_FullESP-IDFmigration.md); update [firmware/partitions.csv](firmware/partitions.csv) header comment |
| Changing T9/T14 audit log format | `log/logparser.py` must learn the new value_a/value_b/param encoding alongside the firmware change (and check `model/campaign-summer-2026/plot_daily.py` — it decodes ALARM/SENSOR_HR rows too) |
| Changing the build pipeline | [bin/build_release.ps1](bin/build_release.ps1) — see manifest placeholder dance (Steps 0 and 3.5) and gh#9 history |
| OTA-pushing to a unit | `python bin/ota_push.py <bin-path> --host <ip>` — current unit IPs in user-global memory; default host = 2344 soak unit; default PIN `12345678` |
| Publishing a release to ROTA (internet-pull) | `python bin/rota_release.py release <version>` (→ GitHub Release → soak, **recommended**) / `publish` (scp → soak) / `promote` (→ mainstream) / `status` — see [bin/rota_release.md](bin/rota_release.md); config in `bin/.rota_release.env`; server in the `greenhouse-Controller-FOTA-server` repo |
| Filing / commenting / closing a GitHub issue | `python bin/gh_issue.py {list,show,create,comment,close,reopen,edit}` — token in the operator's secret store, path via git-ignored `.github/gh_issue.local`. **No `gh` CLI on this machine.** |
| Anything affecting greenhouse behaviour | [design/functionalRequirementsSpecification.md](design/functionalRequirementsSpecification.md); soak on 2344 before any production push |
| Tuning climate/vent behaviour, or judging whether it's misbehaving | `python model/vent_step_replay.py <SD .csv>` — replays T6's step decision under candidate `hyst_t`/`avg_win_t` against real logs, and **refuses to project unless it first reproduces ≥90% of the logged T-demands**. Read its docstring first: five encoding/cadence traps are listed there and in [memory/gotcha-log.md](memory/gotcha-log.md) (2026-07-28) |
| Analysing heap/leak behaviour from SD logs | `python log/heap_soak.py <SD .csv>` — segments at boot markers (a firmware change steps the baseline; a leak is a slope *within* a segment, not a step between) |
| Anything weird / unexpected | [memory/gotcha-log.md](memory/gotcha-log.md) — check before debugging from scratch |

## Release cycle

1. Edit code; bump `-DFIRMWARE_VERSION=\"X.Y.Z\"` in `firmware/platformio.ini`
2. `powershell -ExecutionPolicy Bypass -File bin/build_release.ps1`
3. New `## [X.Y.Z]` section at top of `changelog.md`; write `bin/X.Y.Z/release-notes.md` (use prior release's structure)
4. Hand off staged changes for the user to commit
5. OTA push to soak 2344: `python bin/ota_push.py bin/X.Y.Z/greenhouse-controller-X.Y.Z.bin`
6. Verify both `fw_ver` and `asset_version` from `/api/status`
7. Soak ≥ overnight (or longer for high-risk changes)
8. Production (5C88) only after soak passes. 5C88 now pulls via **ROTA** on the `mainstream` channel — `python bin/rota_release.py promote <ver>` (or hand-edit `channels/mainstream.json`) and it applies in its own night window, **no site visit** (this is what ROTA added, per [design/remoteOTAstudy.md](design/remoteOTAstudy.md)). 5C88 is still behind NAT/outbound-only, so there is no *push* path — but the ROTA *pull* path now covers routine production updates.

## Key directories

| Path | Contents |
|---|---|
| `firmware/src/` | Production firmware source (~23 300 LOC, 44 files) |
| `design/` | FRS, TSDS, implementation/migration plans, audit reports, `OTAimplementation.md` — the closest thing to ADRs |
| `bin/<version>/` | Per-release archive: bin, zip, bootloader, partitions, elf, map, release-notes; `manifest-<version>.json` (ROTA seq ledger — tracked in git) |
| `manual/` | Operator manuals (Dutch: boer + beheerder + admin) |
| `model/` | Plant model, calibration scripts (`calibrate_plant_*.py`), summer-2026 campaign: results in `campaignResults_summer2026.md`, audit trail in `thermalProfileCampaign.md` |
| `log/` | `logparser.py` + `logparser.md`, example log CSV + parsed output |
| `memory/` | L3/L4 layer — `MEMORY.md` index, `architecture.md`, `gotcha-log.md` |
