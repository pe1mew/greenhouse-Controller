# Architecture — task graph, subsystem map, partition layout

Reference material extracted from `CLAUDE.md` (per audit-context structural lint, 2026-06-16) to keep the auto-loaded project file under budget. Reached via the "Touching a FreeRTOS task or subsystem" pointer in CLAUDE.md's Before-You-Start table.

## FreeRTOS task graph

Full task graph diagram in [`../design/rtosTaskDiagram.png`](../design/rtosTaskDiagram.png). Roles verified against `xTaskCreate*` sites in `firmware/src/main.cpp` (T1-T11, T14, T16) and on-demand spawn points (T13, T15):

| Task | Subsystem | Role |
|---|---|---|
| T1 | `watchdog` | TWDT subscribers; calls `ota_mark_healthy()` after 30 s uptime |
| T2 | `relay_controller` | Per-channel (M1/M2/M3) window state machine |
| T3 | `safety_monitor` | Motor-alarm + sensor-fault detection |
| T4 | `data_manager` | Status snapshot for `/api/status`; notifies T6 on new sensor data. Owns the config pipeline: Q4 → `cfg_key_kind()` (UNKNOWN / SHADOW / **NVS-ONLY**, 2.4.7) → NVS → shadow → audit row; `wifi/ap_enable` is NVS-only and read by T10, not the shadow. Seeds the clock from the DS1307 at boot and every 60 s while NTP-unsynced — a failed read leaves the shadow timestamp 0, the web clock blank and T6 on night thresholds (gh#55) |
| T5 | `sensor_poll` | Sensor read scheduling; HR-rate logging. **The only task that touches the Modbus/RS485 bus**, polling all sensors at one global `poll_interval_s` (default 30 s); both drivers hard-coded at fixed addresses. **Single-owner is policy, and the reason is timing — not correctness (gh#49, Phase 4).** Since 2026-09-07 the driver *does* serialise transactions with a mutex (`modbus_rtu.cpp`, verified on FDA4: the unpatched driver managed 0/1000 clean transactions under two callers, the locked one 1000/1000). So a second caller is no longer a *correctness* hazard — but a transaction can hold the bus ~215 ms (`MODBUS_TIMEOUT_MS` dominates), and blocking that long inside High-priority, WDT-subscribed **T2** or **T3** could delay a wind-override response. **Keep all bus I/O in T5**, or in the dedicated bus task of [../design/refactorSensorConfiguration.md](../design/refactorSensorConfiguration.md) §2.2. Two further traps: `modbus_init()` deletes and reinstalls the UART driver, so a re-init during another task's transaction is a use-after-delete; and **the bus does not survive boot** — `main.cpp`'s early `modbus_init()` is dead by the time T5 starts, which is why T5 re-inits (gotcha 2026-09-07). Rationale and evidence: [../design/addModbusMutex.md](../design/addModbusMutex.md). A refactor to roles + a scheduled bus is designed but **not implemented** |
| T6 | `climate_control` | Mode + setpoint logic; consumes T4 notifications |
| T7 | `keypad_scan` | Keypad input (4×4 matrix) |
| T8 | `ui_display` | LCD + UI state machine; Q5 consumer |
| T9 | `event_logger` | SD CSV writer; rotates at 1 MB per file |
| T10 | `network_manager` | WiFi STA/AP, SNTP, geo lookup, Q5 producer; `xTaskNotifyWait`-driven loop |
| T11 | `web_server` | HTTP API (`/api/status`, `/api/ota/*`, `/api/log/*`, `/api/web`, etc.) |
| T12 | MQTT client | **Optional** — declared in `system_globals.cpp` (`task_t12`), no source dir, currently disabled (handle may be NULL) |
| T13 | `ota_manager` | Spawned on demand by T11 from `ota_assets_end()`; no global handle |
| T14 | `status_post` | Periodic HTTPS POST to remote (configurable via `cfg.status_url`) |
| T15 | `status_post_supervisor` | **Dormant** — excluded from build via CMakeLists.txt |
| T16 | `ota_client` | **ROTA** internet-pull OTA client (2.2.0+): periodic manifest check → download + SHA-256 verify of both artefacts → apply via T13 inside the night window + quiet gate. Created at boot in `main.cpp` |

Also: a low-priority `heartbeat_task` is spawned at `main.cpp:1599` as a serial-only liveness indicator — not part of T1-T16 numbering.

**Shared state:** Q1-Q6 queues + EG1 event group. The bit most callers respect is `EG1_BIT_OTA_IN_PROGRESS` — defer non-essential work while set.

## Subsystem map (`firmware/src/`)

| Dir | Owns |
|---|---|
| `auth/` | PIN auth, Farmer/Admin roles |
| `climate_control/` | Mode + setpoint logic |
| `data_manager/` | Status snapshot for `/api/status` and T14 push; config pipeline (`apply_config_update`, `cfg_key_kind`, `dm_reload_all_cfg`); clock seeding (DS1307 / SNTP / manual — priority under review in gh#55) |
| `event_logger/` | T9 — SD CSV writer (gh#30 unit-id prefix) |
| `keypad_scan/` | Keypad input |
| `network_manager/` | T10 — WiFi, SNTP, geo lookup; L3 self-recovery (gh#33) |
| `ota_manager/` | T13 — dual-bank OTA + 3-fail rollback |
| `ota_client/` | T16 — ROTA internet-pull OTA client (manifest, download + verify, apply via T13) |
| `relay_controller/` | T2 — window motor state machine |
| `safety_monitor/` | Motor alarm, sensor fault detection |
| `sensor_poll/` | Sensor read scheduling |
| `status_post/` | T14 — remote status POST + daily log upload |
| `system_id/` | Unit ID derived from MAC (gh#17, since 1.18.3) |
| `types/` | Shared structs (`status_snapshot_t` etc.) |
| `ui_display/` | LCD + UI state machine |
| `watchdog/` | T1 — TWDT subscribers |
| `web_server/` | T11 — HTTP API |

## Partition table

See [`../firmware/partitions.csv`](../firmware/partitions.csv) for offsets and the header-comment notes:

- Dual app banks `app0` / `app1` (2 MB each) at `0x20000` / `0x220000`
- Dual LittleFS `lfs0` / `lfs1` (1 MB each) at `0x420000` / `0x520000`
- NVS at `0x10000`, otadata at `0xe000`, coredump at `0x620000`
- **Coupling rule:** active app bank A → mount `lfs0`; active app bank B → mount `lfs1`. T13 enforces this.
- **Separate VFS mountpoints per partition (invariant, since 1.17.9).** `drivers/littleFS/src/littlefs_storage.cpp` mounts each partition via `LittleFSFS::begin(formatOnFail, basePath, maxFiles, partitionLabel)`, where basePath is the ESP-IDF VFS mountpoint. Keep `MOUNT_A = "/lfsa"` and `MOUNT_B = "/lfsb"` **distinct** — never share one path between partitions; a third LFS partition would need its own (`/lfsc`). Up to 1.17.8a both used `"/lfs"`: VFS silently rebound the path, so T13's writes never reliably reached lfs1 and the device booted to stale assets from a prior cycle. **Symptom to recognise:** "OTA flipped the firmware version but the assets stayed old" — check basePaths first. The diagnostic stack that exposes it within one OTA round-trip (manifest.json on the LFS, the `/manifest.json` route, the `<!-- web-assets X.Y.Z -->` HTML stamp, the OTA diagnostic card) has been in place since 1.17.7.
- **Greenfield flash requires erasing the coredump partition** — see CLAUDE.md hard constraint and gotcha-log.

## See also

- [../CLAUDE.md](../CLAUDE.md) — identity, hard constraints, release cycle
- [../design/OTAimplementation.md](../design/OTAimplementation.md) — OTA reference spec (11 sections)
- [gotcha-log.md](gotcha-log.md) — append-only weird-stuff archive
