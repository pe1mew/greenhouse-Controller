<!-- agent-ready-projects: v1.10.3 -->
# Greenhouse Controller — Claude project guide

Auto-loaded by Claude Code into every session in this repo. Keep terse; deeper material lives behind the pointer table below.

## Identity

ESP32-S3 firmware for a greenhouse ventilation controller. ESP-IDF 5.5 via PlatformIO (`framework=espidf`, `espressif32@6.12.0`). FreeRTOS task-graph design. Custom partition table (dual app banks + dual LittleFS + coredump). Dutch operator manuals (boer + beheerder).

Production deployment at Herenboeren Willemshoeve (Soest). Field units in active service: **5C88** (production) and **2344** (soak / bench). Current addresses and access routes live in user-global memory.

## Hard constraints

### Workflow (inherits from `~/.claude/CLAUDE.md`)

- **Never `git commit / push / merge / rebase / force-push`.** Stage changes and a suggested commit message; hand off to the user.
- **Commit to `main` directly** — no `claude/...` feature branches, no PRs unless explicitly requested.
- **Branch protection rejects merge commits on `main`** — if a branch must merge, rebase + fast-forward only.
- **No `--no-verify` / hook-skip / `--no-gpg-sign`.** Fix the hook, don't bypass it.

### Releases & OTA

- **Paired-commit invariant.** Push firmware AND assets within 120 s of each other (`FW_DONE` fallback timer commits firmware alone after that, stranding the asset partition).
- **Verify post-OTA** by reading both `fw_ver` AND `asset_version` from `/api/status`. Neither alone is sufficient — a mismatch means the asset partition was left behind.
- **SemVer cadence:** feature releases bump minor (1.16.x → 1.17.0); patch is for bug-fix-only.
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
| Touching a FreeRTOS task or subsystem | [memory/architecture.md](memory/architecture.md) — task graph T1-T15, subsystem map, partition layout |
| Bumping firmware version | `firmware/platformio.ini` line 93 — and feature releases bump minor, not patch |
| OTA work (any) | [design/OTAimplementation.md](design/OTAimplementation.md) — full reference, 11 sections |
| Changing partition table | [design/migrationPlan_FullESP-IDFmigration.md](design/migrationPlan_FullESP-IDFmigration.md); update [firmware/partitions.csv](firmware/partitions.csv) header comment |
| Changing T9/T14 audit log format | `log/logparser.py` must learn the new value_a/value_b encoding alongside the firmware change |
| Changing the build pipeline | [bin/build_release.ps1](bin/build_release.ps1) — see manifest placeholder dance (Steps 0 and 3.5) and gh#9 history |
| OTA-pushing to a unit | `python bin/ota_push.py <bin-path> --host <ip>` — current unit IPs in user-global memory; default host = 2344 soak unit; default PIN `12345678` |
| Filing / commenting / closing a GitHub issue | `python bin/gh_issue.py {list,show,create,comment,close,reopen,edit}` — token in `.github/token.local`. **No `gh` CLI on this machine.** |
| Anything affecting greenhouse behaviour | [design/functionalRequirementsSpecification.md](design/functionalRequirementsSpecification.md); soak on 2344 before any production push |
| Anything weird / unexpected | [memory/gotcha-log.md](memory/gotcha-log.md) — check before debugging from scratch |

## Release cycle

1. Edit code; bump `-DFIRMWARE_VERSION=\"X.Y.Z\"` in `firmware/platformio.ini`
2. `powershell -ExecutionPolicy Bypass -File bin/build_release.ps1`
3. New `## [X.Y.Z]` section at top of `changelog.md`; write `bin/X.Y.Z/release-notes.md` (use prior release's structure)
4. Hand off staged changes for the user to commit
5. OTA push to soak 2344: `python bin/ota_push.py bin/X.Y.Z/greenhouse-controller-X.Y.Z.bin`
6. Verify both `fw_ver` and `asset_version` from `/api/status`
7. Soak ≥ overnight (or longer for high-risk changes)
8. OTA production (5C88 etc.) only after soak passes

## Key directories

| Path | Contents |
|---|---|
| `firmware/src/` | Production firmware source (~21 600 LOC, 41 files) |
| `design/` | FRS, TSDS, implementation/migration plans, audit reports, `OTAimplementation.md` — the closest thing to ADRs |
| `bin/<version>/` | Per-release archive: bin, zip, bootloader, partitions, elf, map, release-notes |
| `manual/` | Operator manuals (Dutch: boer + beheerder + admin) |
| `model/` | Greenhouse simulation model + scenario inputs |
| `log/` | `logparser.py`, `plot_daily.py`, parsed historical CSVs |
| `memory/` | L3/L4 layer — `MEMORY.md` index, `architecture.md`, `gotcha-log.md` |
