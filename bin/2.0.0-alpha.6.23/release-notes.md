# 2.0.0-alpha.6.23 — Phase 6.N.2 (housekeeping: main.cpp rename + archive deletion)

## What landed

Pure housekeeping. Same binary behaviour as alpha.6.22 — only file moves + three string updates.

| Action | Path |
|---|---|
| Renamed (git mv preserving history) | `firmware/src/app_main_stub.cpp` → `firmware/src/main.cpp` |
| Deleted (1.20.3 Arduino-era file, would not compile under `framework = espidf`) | `firmware/src/main.cpp` (pre-rename) |
| Deleted | `firmware/src/web_server/web_server_1.20.3_original.cpp.archived` |
| Deleted | `firmware/src/status_post/status_post_1.20.3_original.cpp.archived` |
| Deleted | `firmware/src/network_manager/network_manager_1.20.3_original.cpp.archived` |

The renamed `main.cpp` now identifies as the real entry point (was named `app_main_stub.cpp` since alpha.1 because the early phases really were stubs). The internal `TAG` changed from `"GHC-STUB"` to `"GHC"`; the alpha.2.5 LCD greeting changed from `"ESP-IDF stub OK"` to `"ESP-IDF boot OK"`. File header docblock rewritten to describe the full boot sequence (8 documented steps from `log_boot_banner` through to the task-spawn order) rather than the original Phase-1 stub mandate.

Comment-only references to `app_main_stub` updated in two live source files (network_manager.cpp / relay_controller.cpp) and `platformio.ini`. The changelog and prior release-notes retain the original name verbatim as historical record.

## Build delta vs alpha.6.22

| Metric | alpha.6.22 | alpha.6.23 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 305 213 B | 1 305 213 B | **0 B** |
| RAM static | 60 256 B | 60 256 B | 0 B |

Byte-identical flash + RAM usage as expected — the renames are textual only, and the three string changes (`"GHC-STUB"` 9 B → `"GHC"` 3 B, `"ESP-IDF stub OK"` 16 B → `"ESP-IDF boot OK"` 16 B) happen to land in the same `.rodata` size class.

bin sha256: `2589802B9D506887…`

## Acceptance — hardware verified on 192.168.20.160

After 184 s uptime:

```
GET /api/ota/status   → {state:"idle", progress:0, error:"", bank:"A", accepted:true}
GET /api/status       → fw_ver=2.0.0-alpha.6.23, uptime_s=184
```

`accepted=true` confirms the T1 + `ota_check_rollback` flow added in alpha.6.22 is still functional after the file move — exactly as expected since the spawn block + boot call were not relocated, only the file containing them was renamed.

## Tree state at the end of alpha.6.23

```
firmware/src/
├── main.cpp                          ← renamed; the real entry point
├── wifi_tickle.cpp
├── https_tickle.cpp
├── system_globals.cpp
├── web_server/
│   ├── web_server.cpp                ← T11 (alpha.6.16–η, 25/25 routes)
│   └── web_server.h
├── status_post/
│   ├── status_post.cpp               ← T14 minimal (alpha.6.15)
│   ├── status_json.cpp
│   ├── status_json.h
│   └── status_post.h
├── network_manager/
│   ├── network_manager.cpp           ← T10 minimal (alpha.6.14)
│   └── network_manager.h
├── watchdog/                         ← new in alpha.6.22
│   ├── watchdog.cpp
│   └── watchdog.h
├── auth/, climate_control/, data_manager/, event_logger/,
   keypad_scan/, ota_manager/, relay_controller/, safety_monitor/,
   sensor_poll/, system_id/, types/, ui_display/
```

Zero `.cpp.archived` files remain. The migration plan's Phase 6.N item "delete archived 1.20.3 .cpp files" is satisfied.

## Phase 6.N retrospective

Phase 6.N has two completed sub-phases:

| Sub-phase | Tag | Scope |
|---|---|---|
| 6.N.1 | alpha.6.22 | T1 minimal watchdog + `ota_check_rollback` boot wiring + 2 KB→4 KB stack bug fix |
| 6.N.2 | **alpha.6.23** | main.cpp rename + 4 archive deletions (this tag) |

The deferred Phase 6.N.1.X (T1 full instrumentation — NeoPixel + LOG_SYSTEM heap rows + heap-integrity check + stack-HWM sweep) carries forward to a future alpha.6.23.X or into 2.0.0-rc.X stabilisation if it doesn't surface before Phase 7 starts.

## Next

Phase 7 — 14-day soak on bench unit → 2.0.0-rc.1.

Acceptance criteria (from the migration plan):
- Zero unplanned reboots (no `ESP_RST_PANIC` / `ESP_RST_INT_WDT` / `ESP_RST_TASK_WDT`)
- Zero T15 planned reboots from heap-drop accumulator (gh#23 acceptance signal — but T15 isn't ported yet, so this is partial)
- Web GUI fully functional — verified by daily smoke test
- SD log shows steady free-heap (124-126 KB), steady largest-block (77-83 KB)
- Climate-control responsiveness identical to 1.20.3
- Flash usage ≤ 1.30 MB (currently 1.305 MB — slightly over budget; will revisit)

The 1.30 MB target may need a small bump given the cumulative phase-6 work. Currently at 62.2 % of the 2 MB OTA bank with ~750 KB of headroom, so not architecturally at risk.
