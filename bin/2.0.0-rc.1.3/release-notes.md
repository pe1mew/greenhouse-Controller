# 2.0.0-rc.1.3 — housekeeping release

Pure source-quality cleanup pass on top of rc.1.2.1. **Zero behavioural change**, **zero web-UI change**, **zero operator-facing change**. Supersedes rc.1.2.1 as the Phase 7 soak candidate; the 14-day clock restarts at day 0.

## What changed

### 1. Dead source files deleted (~375 lines)

- `firmware/src/mqtt_client/mqtt_client.cpp` + `.h` — Phase-0 stub for an unimplemented Phase 9; never spawned, never referenced.
- `firmware/src/web_server_tickle.cpp` + `.h` — explicitly "REMOVED in 2.0.0-alpha.6.16" by CMakeLists comment, but the files lingered.
- `firmware/src/util/time_compat.h` — header-only helpers; never `#include`d anywhere.

Empty `mqtt_client/` and `util/` directories removed automatically.

### 2. wifi_tickle → folded into T10 (network_manager.cpp)

The 398-line `firmware/src/wifi_tickle.cpp` was the actual boot-time WiFi STA bring-up + SNTP sync + long-lived event handler + reconnect-backoff timer. The "tickle" name was an alpha-era scaffold remnant.

**Relocated** to `network_manager.cpp` SECTION A (clearly demarcated block). Public entry-point renamed:

| Before (in wifi_tickle.h) | After (in network_manager.h) |
|---|---|
| `wifi_tickle_run(uint32_t connect_timeout_ms)` | `nm_wifi_init_blocking(uint32_t connect_timeout_ms)` |
| `wifi_tickle_status_t` | `nm_wifi_status_t` |
| `WIFI_TICKLE_OK / _OK_NO_NTP / _NO_SSID / _INIT_FAILED / _CONNECT_TIMEOUT / _DISCONNECTED` | `NM_WIFI_OK / _OK_NO_NTP / _NO_SSID / _INIT_FAILED / _CONNECT_TIMEOUT / _DISCONNECTED` |

Same blocking call from main.cpp, same event-handler lifecycle, same exponential-backoff reconnect timer (alpha.6.31). Only the file location and the public-symbol names changed.

### 3. https_tickle → deleted without replacement

The alpha-4-era boot-time **5×POST to `https://www.google.com/generate_204`** was the original demonstration of the gh#23 mbedTLS keep-alive + 1 KB buffer fix. The demonstration succeeded; rc.1+ has been running the same TLS configuration successfully against the production status server. T14's real status POSTs every `status_interval_s` provide ongoing validation. The boot-time test was log noise.

### 4. Lolin-S3 datasheets moved

`drivers/Lolin-S3/` → `documentation/hardware/Lolin-S3/`. The directory contained only a datasheet PDF, three board-photo JPGs, and a schematic PDF — no code. It was misfiled under `drivers/` (which is for actual drivers).

### 5. Worktree leftovers cleared (244 MB local disk reclaimed)

Deleted `design/.claude/worktrees/` and `design/.clone/` from disk. Both were already in `.gitignore`, so the repo state is unchanged — only the local checkout got tidier.

### 6. build_release.ps1 — bulletproof stderr-warning handling

PIO emits `-Wmissing-field-initializers` warnings to stderr for the brace-init lists in `web_server.cpp`. Under `$ErrorActionPreference='Stop'` (the script default), PowerShell upgrades those stderr writes to terminating errors despite pio's actual exit code being 0.

The rc.1.2.1 attempt to fix this via a `2>&1 | ForEach-Object { Write-Host $_ }` pipeline did not take because the merge happened inside the strict-mode envelope. rc.1.3 locally toggles `$ErrorActionPreference='Continue'` for the two pio-invocation blocks (Steps 1 and 2), restores at `finally`-time. Gates failure on `$LASTEXITCODE` alone.

## What did NOT change

- Operator-facing behaviour — none.
- Web UI / web assets — `app.js`, `index.html`, `style.css` byte-identical.
- Climate control / relay / sensor / OTA / coredump / audit-log subsystems — untouched.
- T15 supervisor — still dormant per the original rc.1 plan ("source on disk, not in CMakeLists.txt SRCS"). To be re-enabled paired with a.6.36 only if gh#23 mitigations land.
- The rc.1.1 wind-direction fix, the rc.1.2 OTA-reboot carve-off, the rc.1.2.1 log-upload buffer overrun fix — all preserved.

## Build delta vs rc.1.2.1

| Metric | rc.1.2.1 | rc.1.3 | Delta |
|---|---:|---:|---:|
| Firmware bin | 1 353 957 B | **1 351 881 B** | **−2 076 B** |
| RAM static | 60 568 B | 60 568 B | 0 |
| Source LOC in firmware/src/ | ~−625 lines | — | (dead files + tickle files) |
| Local disk | — | −244 MB | (worktree leftovers) |

The 2 KB flash saving is from the dead-path elimination (linker drops the wifi_tickle / https_tickle TUs entirely once they leave SRCS). Modest, but the source-tree clarity win matters more for the long run.

## Verification on bench (192.168.20.160)

Paired OTA deploy from rc.1.2.1 → rc.1.3 at 2026-05-20 ~10:10:

```
PRE-OTA  : fw=2.0.0-rc.1.2.1 uptime=7836s flags=[]
Firmware POST: 200 {"ok":true,"awaiting_assets":true}
Assets POST  : 202 {"ok":true,"message":"extracting"}
Wait ~30s for reboot...
POST-OTA : fw=2.0.0-rc.1.3 uptime=86s flags=[]
Coredump  : present:false
last_post : OK 2026-05-20 10:09:56  ← first status POST after rc.1.3 boot
mode.flags: []                       ← no coredump_available, calibration completed
wind.dir  : 217 deg (averaged)        ← rc.1.1 fix still in effect; LCD agrees
```

Clean reboot via the rc.1.2 reboot-worker-task carve-off (proves that fix still works), clean WiFi bring-up via `nm_wifi_init_blocking()` (proves the fold preserves boot behaviour), clean status POST through the production server (proves the rc.1.2.1 chunk-reader fix is preserved), no residual coredump, no operator-visible difference vs rc.1.2.1.

## Artefacts archived (gitignored, local-only)

```
bin/2.0.0-rc.1.3/
├── greenhouse-controller-2.0.0-rc.1.3.bin    (1 320.6 KB — OTA payload, deployed)
├── firmware-2.0.0-rc.1.3.elf                 (12 740.8 KB — symbols for coredump decode)
├── bootloader-2.0.0-rc.1.3.bin               (22 KB)
├── partitions-2.0.0-rc.1.3.bin               (3 KB)
├── web-assets-2.0.0-rc.1.3.zip               (96.9 KB)
└── release-notes.md
```

## Soak status

Day 0 of Phase 7 against rc.1.3. The codebase is now clean enough that any new fault during the soak should point at a real production issue rather than at scaffold leftovers. Same acceptance criteria as rc.1.

Day 14 = `v2.0.0` if green across the board.
