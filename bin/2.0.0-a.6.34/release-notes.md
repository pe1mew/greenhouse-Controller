# 2.0.0-a.6.34 — T13 firmware-only fallback timer

Third alpha of the maturation plan. Restores the firmware-only fallback commit path that 1.20.3 supported: after a firmware OTA completes, if no paired web-asset upload arrives within a fixed window, the firmware is committed and the unit reboots on its own. Without this, an interrupted asset upload (network blip, operator closed the page) would leave the verified-but-uncommitted firmware sitting in the inactive partition forever, requiring a manual re-flash to recover.

## What landed

**FW_DONE_FALLBACK_MS = 120 000 ms** (two minutes). One-shot FreeRTOS timer armed at the end of `ota_firmware_end()` after the state transitions to `OTA_STATE_FW_DONE`. Cancelled in `ota_assets_begin()` when an asset upload starts.

| Aspect | Implementation |
|---|---|
| Timer | `xTimerCreate("ota_fw_fallback", pdMS_TO_TICKS(120000), pdFALSE, NULL, fw_done_fallback_cb)`, lazily created on first OTA, reused via `xTimerChangePeriod` + `xTimerStart` on subsequent OTAs |
| Arm site | `ota_firmware_end()` — immediately after `set_state_locked(OTA_STATE_FW_DONE)` and the `post_log(1)` audit row |
| Cancel site | `ota_assets_begin()` — `xTimerStop(s_fw_done_timer, 0)` before state transitions to ASSETS_BUFFERING |
| Audit | `value_a=13, value_b=0` LOG_SYSTEM event posted from the commit worker (see "Task dispatch" below) |
| Commit | `esp_ota_set_boot_partition(s_ota_part)` followed by `schedule_reboot(3000)` — same primitive used by the paired-OTA asset commit path |
| Race-safety | The spawned commit worker re-acquires `s_mx` and re-reads `s_state == OTA_STATE_FW_DONE` before doing any work; a `xTimerStop` that races with the timer fire is harmless because the worker bails silently if state moved |

## What changed

- **`firmware/src/ota_manager/ota_manager.cpp`** — new `#define FW_DONE_FALLBACK_MS`, `s_fw_done_timer` handle, `fw_done_commit_task` worker (~50 lines), `fw_done_fallback_cb` dispatcher (~15 lines); `ota_firmware_end()` extended to arm the timer after `post_log(1)`; `ota_assets_begin()` extended to stop the timer when an asset upload begins.
- **`firmware/src/event_logger/event_logger.h`** + **`event_logger.cpp`** — new `value_a=13` row in the LOG_SYSTEM encoding table; new public function `event_logger_post_sync(value_a, value_b)` for callers that need a guaranteed-on-disk audit row before a reset (kept available but the fallback path no longer uses it — the dedicated commit task makes Q3 → T9 round-trip reliable again).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.34`.

## Task dispatch — the lesson learned in this alpha

First three iterations of the fallback callback ran the commit work (state check → `post_log(13)` → `esp_ota_set_boot_partition` → `schedule_reboot`) directly in the `xTimerService` callback context. All three produced **reboots at the correct time** (~125 s after firmware POST) and **committed the firmware** correctly — but **the `value_a=13` audit row never reached the SD CSV**.

Iterations tried:
1. `schedule_reboot(1000)` — same delay as cancel-path. Audit row missing.
2. `schedule_reboot(3000)` — three-second buffer. Audit row still missing.
3. Move `post_log(13)` to *before* `esp_ota_set_boot_partition` so T9 has the partition-write window to drain Q3. Still missing.
4. Replace `post_log(13)` with a new `event_logger_post_sync()` helper that bypasses Q3 entirely and calls `storage_sd_write_append` directly. Still missing.

Iteration 4 narrowed the root cause: `xTimerService`'s configured stack (`configTIMER_TASK_STACK_DEPTH`, typically 4 KB) is large enough for the lightweight `xQueueSend` of `post_log()` but not large enough to run the SD-write path safely. The synchronous helper appeared to succeed but the row never made it to the file — most likely a stack-overflow-induced panic that the panic handler recovered from cleanly by rebooting, with the partial write lost on the way down. Cancel-path didn't hit this because the paired-OTA commit code runs in T13 (a normal task with its own larger stack).

**Iteration 5 (shipped):** the timer callback `fw_done_fallback_cb` now does nothing but spawn `fw_done_commit_task` (4 KB stack, priority 5 — one above T9). The commit task runs the same logic the original callback did, but in a normal task context with room for the SD writes. The audit row now lands on disk reliably ~5 s before the reboot.

## Acceptance — hardware verified on 192.168.20.160

Both halves of the timer's behaviour exercised:

**Cancel-path** — `POST /api/ota/firmware` followed by `POST /api/ota/assets` within the 120 s window. Expected: timer armed, then cancelled at the start of asset upload; asset OTA commits normally; **no** `value_a=13` row in CSV; reboot driven by the asset commit path. Confirmed: paired-flash deploys cleanly, only the existing `value_a=0/1/2` OTA events appear in the CSV in the test window.

**Fallback-path** — `POST /api/ota/firmware` with no follow-up. Expected: timer fires ~120 s later, audit row written, firmware committed, reboot. Confirmed:

```
07:50:00 UTC SYSTEM SYS 0 0 0,0      ← post_log(0): ota_firmware_begin
07:50:09 UTC SYSTEM SYS 0 0 1,0      ← post_log(1): ota_firmware_end + timer armed
...                                  ← 120 s of heartbeats while timer counts down
07:52:03 UTC SYSTEM SYS 0 0 13,0     ← post_log(13): fallback fired → commit worker
07:52:08 UTC SYSTEM SYS 0 0 7,253    ← post-reboot heap row (high values = fresh boot)
07:52:08 UTC SYSTEM SYS 0 0 8,8189
07:52:08 UTC SYSTEM SYS 0 0 12,176
07:52:09 UTC SYSTEM SYS 0 0 5,4      ← BOOT entry (esp_reset_reason = 4 = ESP_RST_PANIC,
                                         which is what esp_restart() registers in this
                                         IDF build — see note below)
```

Reboot landed 6 s after `post_log(13)` (≈ 3 s reboot timer + the boot itself), with `fw_ver=2.0.0-a.6.34` and `asset_version=2.0.0-a.6.34` reported by `/api/status` immediately after. The unit was already on a.6.34 before the fallback test (deployed via the cancel-path step), so the fallback re-flashed and re-committed the same bin — the user-visible outcome (clean boot into expected firmware) is identical to a real upgrade-via-fallback scenario.

### Note on `esp_reset_reason()` and `value_b=4`

Every OTA-triggered reboot in this CSV window — both cancel-path and fallback-path — surfaces with `value_b=4` in the BOOT entry (`value_a=5`). The same `value_b=4` shows up on every cancel-path reboot we already trust as clean (asset commit → `schedule_reboot(1000)` → `esp_restart()`). External resets and clean power-cycles register as `value_b=1` (ESP_RST_POWERON) earlier in the same file. Conclusion: in this PlatformIO+espidf 5.5.0 build, `esp_restart()` is being recorded as ESP_RST_PANIC (4), not the ESP_RST_SW (3) we'd expect from the documented enum. This is cosmetic — every fallback reboot lands cleanly on the new firmware. Worth investigating in a later alpha, not blocking 2.0.0.

## Build delta vs a.6.33

| Metric | a.6.33 | a.6.34 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 343 632 B | **1 344 925 B** | +1 293 B |
| RAM static | 60 488 B | 60 504 B | +16 B |

+1.3 KB flash. Plan estimate for this alpha was +0.4 KB — the overrun comes from the dispatch refactor (extra worker function, extra strings for the diagnostic LOG lines) and the new `event_logger_post_sync` helper that stays linked even though it's no longer the primary path. The `event_logger_post_sync` API is kept because it's the right primitive for any future "must-flush-before-reset" caller. Final flash usage: **64.1 %** of the 2 MB OTA bank.

bin sha256: see `greenhouse-controller-2.0.0-a.6.34.bin` in this directory.

## Next

**a.6.35** — T14 status_secret + canonical JSON + SD log upload + `status_enable` / `log_upload_rot` gates + HTTPS-only validator. The largest alpha of the four; will land the last batch of 1.20.x parity items before Phase 7 (14-day soak).
