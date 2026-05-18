# 2.0.0-alpha.6.22 — Phase 6.N.1 (T1 watchdog + OTA rollback gate)

## What landed

Two new pieces of architecture, both gating the rest of Phase 6.N:

| Component | What it does |
|---|---|
| `task_watchdog` (T1) | New task — minimal body: subscribes to IDF TWDT and kicks every 500 ms; calls `ota_mark_healthy()` exactly once at the 30 s uptime mark to close the rollback window |
| `ota_check_rollback()` at boot | New call in `app_main_stub.cpp` between `nvs_cfg_init()` and `pin_auth_init()` — increments NVS `system/ota_fail_cnt` on every cold boot. T1 resets the counter to 0 after `OTA_HEALTHY_MS` (30 s) of stable uptime; three boots without surviving 30 s = rollback to the previous OTA bank |

Together these complete the 3-fail rollback flow that's been dormant since Phase 6.13 (when `ota_manager.cpp` was first added). The fail counter has been at 0 in NVS the whole time because nothing wrote to it — now both the increment side (`ota_check_rollback`) and the reset side (`ota_mark_healthy` via T1) are exercised on every boot.

## Bug found and fixed under this tag — T1 stack overflow

First build of alpha.6.22 used `xTaskCreatePinnedToCore(..., 2048, ...)` with the comment "stack words". That's wrong: under ESP-IDF the stack-size parameter is in **bytes**, NOT words like vanilla FreeRTOS. 2 KB was too small for T1's body — ESP_LOGI's per-call buffer (~150 B) + nvs_cfg_set_i32 inside ota_mark_healthy (~400 B) blew the stack.

Symptom: device unresponsive within seconds of flash. HTTP probes returned `HTTP:000`, ping returned "Destination host unreachable" for 60+ s straight. Recovery: direct-flashed the saved `bin/2.0.0-alpha.6.21/firmware-2.0.0-alpha.6.21.bin` via esptool.

Diagnosis: scripted a PowerShell SerialPort reader to capture 25 s of post-flash serial output. Boot log immediately showed:

```
***ERROR*** A stack overflow in task T1-WDT has been detected.
Backtrace: 0x40378301:0x3fcea250 0x403802b1:0x3fcea270 0x403818aa:0x3fcea290
           0x40382be3:0x3fcea310 0x40381970:0x3fcea330 0x40381966:0xa5a5a5a5
           |<-CORRUPTED
Rebooting...
```

Repeated 12 times in 25 s = boot loop confirmed.

Fix: bumped to **4096 bytes**, matching T11's stack. Comment in both `app_main_stub.cpp` and `watchdog.h` updated to say "BYTES — ESP-IDF convention, NOT FreeRTOS words" so the next migrator doesn't fall into the same trap.

## Deferred to a later alpha.6.22.X

Minimal T1 ships now. The 1.20.3 T1 also did:

- **NeoPixel day/night brightness** — needs ESP-IDF RMT driver port (Adafruit_NeoPixel is Arduino-only)
- **60-second LOG_SYSTEM rows** — heap-free, PSRAM-free, largest-contiguous-block (value_a = 7 / 8 / 12)
- **30-s-offset `heap_caps_check_integrity_all`** — corruption detector
- **10-minute stack-HWM sweep** across all task handles

These will arrive together once the minimal T1 is verified stable on hardware. Same minimal-then-extend pattern used for T10 / T14 / T11.

## Acceptance — hardware verified on 192.168.20.160

After 37 s uptime (well past the 30 s healthy mark):

```
GET /api/whoami           → 200  {"ok":false,"error":"no_session"}  HTTP:401  [@ 12 s uptime]

After login (admin / 12345678):

GET /api/ota/status       → {"ok":true,"state":"idle","progress":0,"error":"",
                              "bank":"A","accepted":true}     ← T1 ran ota_mark_healthy ✓
GET /api/status           → uptime_s=103, fw_ver=2.0.0-alpha.6.22
```

The `accepted:true` is the critical signal: it means `ota_is_accepted()` read `system/ota_fail_cnt == 0` from NVS, which means T1's tick-60 callback fired and reset the counter that `ota_check_rollback()` had incremented at boot.

End-to-end OTA rollback flow now exercised on every boot:

```
boot  → ota_check_rollback()  → fail_cnt: 0 → 1   (NVS write)
T1    → tick 60 (30 s)        → ota_mark_healthy() → fail_cnt: 1 → 0   (NVS write)
```

A future panic that prevents T1 from reaching tick 60 three times in a row would correctly trigger `esp_ota_mark_app_invalid_rollback_and_reboot()` — but only with a valid previous bank present.

## Build delta vs alpha.6.21

| Metric | alpha.6.21 | alpha.6.22 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 303 385 B | **1 305 213 B** | +1 828 B |
| RAM static | 60 232 B | 60 256 B | +24 B |

**+1.8 KB flash** is the new T1 task body + the boot-time `ota_check_rollback()` call site. RAM goes up by 24 B for the new `task_t1` handle slot (already declared in `system_globals.cpp` since alpha.6.1 but unused until now) plus a few static counters.

bin sha256: `9B3859BB5AA658BB…`

## Artifacts

| File | Bytes |
|---|---|
| firmware-2.0.0-alpha.6.22.bin | 1 305 616 |
| firmware-2.0.0-alpha.6.22.elf | 12 664 276 |
| partitions.bin | 3 072 |
| bootloader.bin | 22 528 |

## Next

Phase 6.N.2 — rename `app_main_stub.cpp` → `main.cpp`, delete the three archived 1.20.3 .cpp files, final consolidation pass before 2.0.0-rc.1 (Phase 7 = 14-day soak).
