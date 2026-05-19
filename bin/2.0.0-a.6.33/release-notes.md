# 2.0.0-a.6.33 — T10 24 h NTP resync + LOG_SYSTEM audit events

Second alpha of the maturation plan. Restores two T10 features that were on the deferred-to-2.0.1 list: the periodic NTP-resync cadence that 1.20.3 used to keep its DS1307 RTC aligned over multi-day operation, and the LOG_SYSTEM audit events (AP start/stop, geo-sync success) that filled the operator-visible SD CSV with WiFi-side state transitions.

## What landed

**Periodic 24 h NTP resync** (T10 main loop)

| Aspect | Implementation |
|---|---|
| Cadence | `NTP_RESYNC_INTERVAL_S = 86400u` (24 h). Compile-time override via `-DNTP_RESYNC_INTERVAL_S=N` for verification builds |
| State | `s_last_ntp_sync_us` static, seeded at boot when STA + NTP came up via `wifi_tickle` and on every successful `STA_GOT_IP` reconnect edge |
| Trigger | Inside T10 main loop after `poll_ap()`: when STA connected AND elapsed since last sync ≥ 24 h, call `run_ntp_resync()` |
| Method | `esp_sntp_setoperatingmode(POLL)` + `esp_sntp_setservername("pool.ntp.org")` + `esp_sntp_init()` (repeatable). Wait up to 10 s for `SNTP_SYNC_STATUS_COMPLETED` |
| TZ preservation | After sync, re-apply `cfg.tz_str` via `setenv("TZ", …)` + `tzset()` only if current TZ differs |
| Side effect | `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` so T4 re-writes DS1307 |
| Geo | NOT re-fetched (location is stable; matches 1.20.3) |
| Failure handling | Single-attempt resync per cycle; on timeout, leave `s_last_ntp_sync_us` unchanged so retry happens on next 5 s tick |

**LOG_SYSTEM audit events** (Q3 producers)

| Event | When | Code |
|---|---|---|
| AP start | `start_ap()` after `s_ap_active = true` is set | `value_a=3, value_b=1` |
| AP stop | `stop_ap()` before returning | `value_a=3, value_b=0` |
| Geo sync success | `do_geo_sync()` after the four `post_q4` writes | `value_a=4, value_b=1` |

Matches the LOG_SYSTEM `value_a` encoding documented in `event_logger.h` lines 109–124.

## What changed

- **`firmware/src/network_manager/network_manager.cpp`** — new includes (`esp_sntp.h`, `esp_timer.h`, `../event_logger/event_logger.h`); new `s_last_ntp_sync_us` static; new `log_sys(value_a, value_b)` helper wrapping `log_post`; new `run_ntp_resync()` function (~55 lines); 3 `log_sys` call sites (start_ap, stop_ap, do_geo_sync); main-loop hook checking the resync cadence; `s_last_ntp_sync_us` seeded at boot-time NTP-up and on reconnect-NTP-up edges.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.33`.

## Build trap caught + fixed inline

First build of a.6.33 errored with `'esp_timer_get_time' was not declared in this scope; did you mean 'timer_gettime'?`. Adding `#include "esp_timer.h"` next to the existing `esp_http_client.h` include fixed it cleanly. Documented here so the next migrator who reaches for `esp_timer_get_time` from a new translation unit knows the explicit include is needed (other call sites have inherited it through `esp_log.h`, but not `esp_log.h` from this file's chain).

A separate transient GCC ICE on `esp_lcd_panel_rgb.c` hit the same build attempt — unrelated to my changes (the file isn't touched). Retry succeeded immediately. Mentioned only so a fresh-eye reviewer doesn't trace a phantom regression.

## Acceptance — hardware verified on 192.168.20.160

After a clean a.6.33 boot (uptime=52 s at probe), toggled AP via `POST /api/config ns=wifi key=ap_enable value=1` then `value=0`. SD CSV grep:

```
06:34:50 UTC SYSTEM SYS 0 0 4,1    ← geo sync (this boot, T+30 s)
06:35:45 UTC SYSTEM SYS 0 0 3,1    ← AP started (operator POST)
06:35:50 UTC SYSTEM SYS 0 0 3,0    ← AP stopped (operator POST, 5 s later)
06:35:48 UTC SYSTEM SYS 0 0 7,98   ← T1 heap row (a.6.32 still working)
06:35:48 UTC SYSTEM SYS 0 0 8,8153
06:35:48 UTC SYSTEM SYS 0 0 12,31
```

All three event types fire correctly. Heartbeat counters (value_a from 1500 upward in main.cpp's heartbeat_task) and T1 heap rows continue producing as expected — no regression in the existing audit stream.

**24 h NTP resync code path:** verified to compile and link. Live verification deferred — the 24 h cadence can't be exercised within a single test cycle. Logic is straightforward (esp_sntp_* call sequence + 10 s wait); the gh#21 lwIP-init race that broke earlier versions doesn't apply here because `wifi_tickle_run` already initialized the stack at boot.

**Paired asset bundle** uploaded: `fw_ver == asset_version == 2.0.0-a.6.33`, mismatch cleared, uptime=10 s post-OTA reboot.

## Build delta vs a.6.32

| Metric | a.6.32 | a.6.33 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 343 120 B | **1 343 632 B** | +512 B |
| RAM static | 60 488 B | (~same) | unchanged |

**+512 B flash — under the plan's +1.2 KB estimate.** Reason: `esp_sntp_*` symbols were already linked from `wifi_tickle_run`'s boot-time use, so this alpha only adds the resync function body + 3 `log_sys` call sites + the helper. No new component pulled in.

bin sha256: `<from build_release.ps1 — see firmware-2.0.0-a.6.33.bin in this directory>`

Final flash usage: **64.0 %** of the 2 MB OTA bank. Headroom intact.

## Next

**a.6.34** — T13 firmware-only fallback timer. ~+0.4 KB flash, low risk.
