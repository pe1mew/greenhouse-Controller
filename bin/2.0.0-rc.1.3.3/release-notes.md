# 2.0.0-rc.1.3.3 — T10 NTP-resync missing sntp_stop fix

Patch release on top of rc.1.3.2. **Single-line C++ change** in `firmware/src/network_manager/network_manager.cpp` plus the version bump. Closes a lwIP-asserted panic in the periodic 24-hour NTP-resync path of T10 (Network Manager).

Supersedes rc.1.3.2 as the Phase 7 soak candidate. The 14-day soak clock restarts at day 0.

## What rc.1.3.2 told us

After ~74 hours of stable rc.1.3.2 operation on the test bench, the unit panicked at **2026-05-22 13:23:09** with `ESP_RST_PANIC` (4). The captured 45 KB coredump decoded cleanly via `esp-coredump` against the matching ELF.

> *Crashed task: `tiT` (lwIP tcpip thread)*
>
> *Panic reason: assert failed: sntp_setoperatingmode at lwip/src/apps/sntp/sntp.c:748*
> *(Operating mode must not be set while SNTP client is running)*

Full triage in `bin/2.0.0-rc.1.3.2/post-mortem/POST_MORTEM.md`.

## Root cause

`run_ntp_resync()` (added in `a.6.33` — periodic 24-hour NTP refresh against `pool.ntp.org`) calls `esp_sntp_setoperatingmode(POLL)` then `esp_sntp_init()`. `esp_sntp_init()` allocates lwIP's internal `sntp_pcb`. The function returns without a matching `esp_sntp_stop()`.

On the next 24-hour cycle, `esp_sntp_setoperatingmode()` runs while `sntp_pcb` is still allocated. lwIP asserts: *"Operating mode must not be set while SNTP client is running"*. The assertion fires inside the tcpip thread → `panic_abort` → `ESP_RST_PANIC`.

The neighbouring boot-time `nm_sntp_quick_sync()` uses the high-level `esp_netif_sntp_init / esp_netif_sntp_deinit` pair correctly. The resync path used the low-level API but missed the matching stop. A doxygen comment in the function header incorrectly claimed `esp_sntp_setoperatingmode` is idempotent — it is not.

## The fix

`firmware/src/network_manager/network_manager.cpp::run_ntp_resync()` — call `esp_sntp_stop()` at the top:

```c
static void run_ntp_resync(void)
{
    ESP_LOGI(TAG, "[T10] Starting periodic NTP resync (24 h cadence)");

    /* rc.1.3.3 — defensive stop before reconfiguring. ... */
    esp_sntp_stop();   /* ← new */

    /* existing IN_PROGRESS guard, setoperatingmode, setservername, init ... */
}
```

`esp_sntp_stop()` on an already-stopped client is a no-op in lwIP, so this is safe on every entry — including the very first call after boot, when `sntp_pcb` is already NULL from `nm_sntp_quick_sync()`'s `esp_netif_sntp_deinit()`.

The function-header doxygen has been rewritten to strike the misleading idempotency claim and explain the defensive stop.

## What did NOT change

- `firmware.bin` size: 1 352 464 B (rc.1.3.2 = 1 352 432 B). **+32 B** — solely the new `esp_sntp_stop()` call site.
- `firmware-2.0.0-rc.1.3.3.elf` differs only in that call site and the version string section.
- No NVS schema changes, no event-type changes, no API surface changes, no LittleFS asset changes other than the embedded version stamp.
- All prior fixes (rc.1.1, rc.1.2, rc.1.2.1, rc.1.3, rc.1.3.1, rc.1.3.2) preserved verbatim.

## Build delta vs rc.1.3.2

| Metric | rc.1.3.2 | rc.1.3.3 | Δ |
|---|---:|---:|---:|
| Firmware bin | 1 352 432 B | 1 352 464 B | +32 B |
| Web assets ZIP | 101 045 B | 101 133 B | +88 B (version-stamp only) |
| RAM static | 60 568 B | 60 568 B | 0 |

## SHA-256

```
92225f61a05d38b365d03d50b14d89dd40e0718fbdcc040697d8e694483c3e39  greenhouse-controller-2.0.0-rc.1.3.3.bin
2dcbba8de22a0bf7436dfe8a481794adef77105f814516988b0840cc28276031  web-assets-2.0.0-rc.1.3.3.zip
```

## Deployment record

Flashed to the test bench (unit 0x2344, 192.168.20.160) via the dashboard's OTA tab at **2026-05-22 ~14:34** local time. Combined firmware + assets push; T13 reboot-worker fired after asset extraction completed. Post-boot verification:

- `fw_ver = 2.0.0-rc.1.3.3` ✓
- `asset_version = 2.0.0-rc.1.3.3` ✓ (no MISMATCH badge)
- `uptime_s` reset cleanly
- The pre-panic rc.1.3.2 coredump was erased via `POST /api/coredump/erase` after the boot succeeded
- `mode.flags = []` (no `coredump_available`, no overrides, no alarms)

## Verification under soak

The 24-hour NTP-resync trigger fires at roughly +24 h, +48 h, +72 h, … after boot. Under rc.1.3.2 the second trigger asserted within milliseconds of execution. Under rc.1.3.3 the resync should complete silently — successful resyncs do not emit a log row (only failures emit `SYSTEM value_a=2, value_b=0`).

Acceptance for the rc.1.3.3 Phase 7 soak (≥ 14 continuous days from 2026-05-22 14:34):

- Zero `ESP_RST_PANIC`.
- Zero `ESP_RST_TASK_WDT`.
- Zero coredump captures.
- Three or more successful 24-hour NTP-resync windows traversed (boot + 24h, +48h, +72h, …) with no `SYSTEM value_a=2` failure rows.

## Files

| File | Description |
|---|---|
| `greenhouse-controller-2.0.0-rc.1.3.3.bin` | Firmware image — flash via OTA `/api/ota/firmware` |
| `firmware-2.0.0-rc.1.3.3.elf` | Symbol-bearing ELF — keep for future coredump decode |
| `bootloader-2.0.0-rc.1.3.3.bin` | Bootloader (byte-identical to rc.1.3.2 — no boot-stage change) |
| `partitions-2.0.0-rc.1.3.3.bin` | Partition table (byte-identical to rc.1.3.2) |
| `web-assets-2.0.0-rc.1.3.3.zip` | STORE-only ZIP for `/api/ota/assets` (version-stamp delta only) |

Per `.gitignore`, the binaries above are not tracked in git — they're rebuildable from the rc.1.3.3 source commit via `bin/build_release.ps1`.
