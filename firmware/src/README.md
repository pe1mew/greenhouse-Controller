# firmware/src

The application source of the Greenhouse Ventilation Controller firmware: ESP-IDF 5.5 built through
**PlatformIO** for the **WEMOS LOLIN S3** (ESP32-S3). Board, framework and build environments are in
[`../platformio.ini`](../platformio.ini).

The firmware is a graph of FreeRTOS tasks, **T1–T17**, that talk through queues and event groups. The
task graph, subsystem map and partition layout are described in
[`../../memory/architecture.md`](../../memory/architecture.md); read it before touching a task.
Libraries that can be tested on a PC (the drivers and the ventilation control law) live in
[`../../drivers/`](../../drivers/) and reach the firmware through thin proxies in
[`../components/`](../components/).

## Layout

| Directory / file | Task | Responsibility |
|---|---|---|
| `main.cpp` | — | Entry point: early init, OTA rollback check, spawns the tasks |
| `system_globals.{h,cpp}` | — | Queues, event groups, mutexes and task handles shared between tasks |
| `watchdog/` | T1 | Task watchdog subscribers; marks a boot healthy after 30 s; heartbeat LED |
| `relay_controller/` | T2 | Per-window (M1/M2/M3) state machine: relays, travel and dwell timing |
| `safety_monitor/` | T3 | Wind safety: raises and clears the wind override, closes all windows |
| `data_manager/` | T4 | Status snapshot for `/api/status`; the configuration pipeline (NVS, bounds, audit) |
| `sensor_poll/` | T5 | The only task on the Modbus/RS485 bus besides T17: polls T/RH and wind |
| `climate_control/` | T6 | Mode and setpoint logic; calls the control law in `drivers/ventModel` |
| `keypad_scan/` | T7 | 4×4 keypad input |
| `ui_display/` | T8 | LCD and menu state machine; the IO0 BOOT-button recovery sequence |
| `event_logger/` | T9 | SD-card CSV log: rotation at 1 MB, retention, the closed-file scan T14 uploads from |
| `network_manager/` | T10 | WiFi station/AP, SNTP, geolocation |
| `web_server/` | T11 | HTTP API and WebSocket push, sessions, push-OTA endpoints |
| `ota_manager/` | T13 | Writes an OTA image and web assets, commits and reboots; rollback bookkeeping |
| `status_post/` | T14 | Periodic HTTPS status POST and SD-log upload to the status site |
| `status_post_supervisor/` | T15 | **Dormant: excluded from the build** in `CMakeLists.txt` (gh#44) |
| `ota_client/` | T16 | ROTA: the internet-pull update client (check, download, verify, apply in the window) |
| `window_pos/` | T17 | M3 window-position sensor: polls the wire encoder, gates position control, judges each drive. `commission.cpp` holds the traverse measurement and the teach, in every build since 2.11.0 (gh#77) |
| `auth/` | — | PIN authentication: salted SHA-256 hashes, per-role lockout |
| `system_id/` | — | The unit ID, from the chip's eFuse MAC |
| `types/` | — | Shared types and helpers (`app_types.h` holds `log_type_t`; `fmt_tenths.h`) |
| `diag/` | — | Development-only diagnostics that compile to nothing in a release build: `modbus_bench` (the bench build, `MODBUS_BENCH`) and `modbus_probe`, the bus-lock concurrency probe (`MODBUS_CONCURRENCY_PROBE`, the `lolin_s3_mbprobe` environment) |

T12 (an MQTT client) is declared but has no source and is disabled.
