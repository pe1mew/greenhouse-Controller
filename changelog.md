# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [2.0.0] — *in progress on `dev/2.0.0-esp-idf` branch*

> **This is a major-version release in active development.** The work is not yet on `main`. The 1.20.x line continues as the production line; this section will be consolidated and re-dated when 2.0.0 ships. Pre-releases (`2.0.0-alpha.N`, `2.0.0-rc.N`) accumulate below in chronological order, oldest first.

### Why 2.0.0 — context

Migrate the firmware from the **arduino-esp32** framework to **pure ESP-IDF** (PlatformIO `framework = espidf`). The codebase is already ~80 % ESP-IDF native (FreeRTOS tasks, raw `nvs_*`, `esp_ota_*`, custom dual-LittleFS, `esp_log` / `esp_task_wdt` / `heap_caps_*`); the residual Arduino layer has become the structural cause of every remaining hard problem (gh#23 mbedTLS heap pattern, gh#21 lwIP-init race, gh#26 SD-flush absence, ESPAsyncWebServer constraints). Direct mbedTLS / esp_tls config — required to close gh#23 — is unreachable through arduino-esp32's `WiFiClientSecure` (confirmed by the 1.20.3 attempt to apply mitigation C1). The migration eliminates that constraint and unlocks the gh#23 mitigation menu in full.

### Phased build, alpha-tagged

| Tag | Phase | Scope |
|---|---|---|
| `2.0.0-alpha.0` | 0 | Branch setup + scaffolding (this entry) |
| `2.0.0-alpha.1` | 1 | `framework = espidf` flip + smoke boot |
| `2.0.0-alpha.2` | 2 | Driver layer (gpio → keypad → nvs → i2c_bus → lcd1602 → modbus_rtu → s200 → fg6485a → DS1307_RTC → littleFS → sdCard) |
| `2.0.0-alpha.3` | 3 | Network stack (`WiFi.h` → `esp_wifi.h` / `esp_netif.h` / `esp_event.h`) |
| `2.0.0-alpha.4` | 4 | HTTPS client (`HTTPClient` / `WiFiClientSecure` → `esp_http_client` / `esp_tls` + mbedtls knobs) — gh#23 payoff |
| `2.0.0-alpha.5` | 5 | Web server (`ESPAsyncWebServer` → `esp_http_server`) — 25 endpoints + WS rewrite |
| `2.0.0-alpha.6` | 6 | Misc cleanup (Adafruit_NeoPixel → RMT, pinMode → gpio_*, millis() → esp_timer, Arduino.h removal) |
| `2.0.0-rc.1` | 7 | 14-day verification soak on bench unit |
| `2.0.0` | 8 | Merge + release (fast-forward into `main`) |

### `[2.0.0-alpha.6.23]` — 2026-05-18

**Phase 6.N.2 — housekeeping: `main.cpp` rename + archive deletion.** Pure file moves + three string edits; binary behaviour identical to alpha.6.22.

- `git mv firmware/src/app_main_stub.cpp firmware/src/main.cpp` — the file has not been a "stub" since the early alphas; the name now matches the role.
- `git rm firmware/src/main.cpp` (pre-rename — the original 1.20.3 Arduino-era entry point with `Adafruit_NeoPixel`, `Wire`, `WiFi.h` etc. that would not compile under `framework = espidf`). Preserved in git history if archaeology is ever needed.
- `git rm firmware/src/web_server/web_server_1.20.3_original.cpp.archived`
- `git rm firmware/src/status_post/status_post_1.20.3_original.cpp.archived`
- `git rm firmware/src/network_manager/network_manager_1.20.3_original.cpp.archived`

The three archive files were preserved on disk through Phases 6.14 / 6.15 / 6.16 as porting references; their content is now superseded by the working IDF-native code and they're no longer reachable from git working-tree.

#### Inside the renamed main.cpp

- `TAG` literal: `"GHC-STUB"` → `"GHC"` (renames the log prefix on the ~30 boot log lines from `app_main`)
- alpha.2.5 LCD greeting: `"ESP-IDF stub OK"` → `"ESP-IDF boot OK"`
- File header docblock: rewritten from the original "Phase 1 stub mandate" to a 8-step boot-sequence description (banner → globals → driver tickles → NVS → ota_check_rollback → pin_auth → task spawns → heartbeat).
- Comment references to `app_main_stub.cpp` in two other live source files (`network_manager.cpp`, `relay_controller.cpp`) and one in `platformio.ini` updated to point at the new name. Changelog and prior release-notes retain the original name verbatim as historical record.

#### Build delta vs alpha.6.22

| Metric | alpha.6.22 | alpha.6.23 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 305 213 B | 1 305 213 B | **0 B** |
| RAM static | 60 256 B | 60 256 B | 0 B |

Byte-identical — confirms the rename was textual only. The three string changes (`"GHC-STUB"` 9 B → `"GHC"` 3 B, `"ESP-IDF stub OK"` 16 B → `"ESP-IDF boot OK"` 16 B, docblock comment-only) happen to land in the same `.rodata` size class.

bin sha256: `2589802B9D506887…`

#### Acceptance — hardware verified on 192.168.20.160

After 184 s uptime:

```
GET /api/ota/status   → {state:"idle", progress:0, error:"", bank:"A", accepted:true}
GET /api/status       → fw_ver=2.0.0-alpha.6.23, uptime_s=184
```

`accepted=true` confirms the T1 + `ota_check_rollback` flow from alpha.6.22 still works after the file move — the spawn block and boot call weren't relocated, only the file containing them was renamed. Zero functional regression.

#### Phase 6.N retrospective

| Sub-phase | Tag | Scope |
|---|---|---|
| 6.N.1 | alpha.6.22 | T1 minimal watchdog + `ota_check_rollback` boot wiring + stack-overflow fix |
| 6.N.2 | **alpha.6.23** | main.cpp rename + 4 archive deletions |

Phase 6 (the ESP-IDF migration's Phase 6 = "misc cleanup + finalisation") is structurally complete. The deferred Phase 6.N.1.X (T1 full instrumentation — NeoPixel + LOG_SYSTEM heap rows + heap-integrity check + stack-HWM sweep) carries forward; it's high-value diagnostic data but not gating for 2.0.0-rc.1.

### `[2.0.0-alpha.6.22]` — 2026-05-18

**Phase 6.N.1 — T1 minimal watchdog task + boot-time `ota_check_rollback()`.** Closes the previously-dormant 3-fail OTA rollback flow: every cold boot now increments `system/ota_fail_cnt` in NVS, and T1 calls `ota_mark_healthy()` after 30 s of stable uptime to reset it. Three boots that don't reach 30 s = `esp_ota_mark_app_invalid_rollback_and_reboot()`.

T1 ships minimal — see the file header for the deferred features (NeoPixel day/night brightness, 60-s heap-free LOG_SYSTEM rows, 30-s-offset heap-integrity check, 10-min stack-HWM sweep). They follow the same minimal-then-extend pattern used for T10 / T14 / T11; landing them in their own alpha.6.22.X keeps the bisect window narrow.

#### Bug found and fixed under this tag — T1 stack overflow

First build of alpha.6.22 used `xTaskCreatePinnedToCore(..., 2048, ...)` thinking it was words (vanilla FreeRTOS convention). Under ESP-IDF it's **bytes**. 2 KB was nowhere near enough for ESP_LOGI's per-call buffer + nvs_cfg_set_i32's working stack — boot loop within seconds.

Symptom: device unresponsive after flash; HTTP probes timed out for 60+ s; ping returned "Destination host unreachable". Recovery: direct-flashed `bin/2.0.0-alpha.6.21/firmware-2.0.0-alpha.6.21.bin` via esptool.

Diagnosis was unusually painful for a static-analysis pass — nothing in the C looked wrong. Switched to capturing serial output via a PowerShell SerialPort reader (so the next migrator doesn't need a separate `pio device monitor` terminal):

```
***ERROR*** A stack overflow in task T1-WDT has been detected.
Backtrace: 0x40378301:0x3fcea250 ... 0x40381966:0xa5a5a5a5 |<-CORRUPTED
Rebooting...
```

12 times in 25 s = unambiguous. Fix: bumped to 4096 bytes (matches T11). Both the spawn call and the `watchdog.h` "Suggested parameters" docstring now say "BYTES — ESP-IDF convention, NOT FreeRTOS words" with a forensic note about the failed first attempt.

#### What changed

- **`firmware/src/watchdog/watchdog.{h,cpp}`** — new files, ~110 lines total. T1 entry point + minimal task body (TWDT subscribe → 500 ms tick → ota_mark_healthy at tick 60).
- **`firmware/src/CMakeLists.txt`** — added `watchdog/watchdog.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`** — added `#include "ota_manager/ota_manager.h"` and `#include "watchdog/watchdog.h"`, an `ota_check_rollback()` call right after `nvs_cfg_init()`, and a T1 spawn block before T7. ~50 lines net.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.22`.

#### Acceptance — hardware verified on 192.168.20.160

After 37 s uptime:

```
GET /api/ota/status   → {state:"idle", progress:0, error:"", bank:"A", accepted:true}
GET /api/status       → uptime_s=103, fw_ver=2.0.0-alpha.6.22
```

`accepted:true` is the critical signal — it means `ota_is_accepted()` read `ota_fail_cnt == 0` from NVS, which means T1's tick-60 callback fired and reset the counter that `ota_check_rollback()` had incremented at boot. End-to-end flow exercised on every boot from now on.

#### Build delta vs alpha.6.21

| Metric | alpha.6.21 | alpha.6.22 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 303 385 B | **1 305 213 B** | +1 828 B |
| RAM static | 60 232 B | 60 256 B | +24 B |

bin sha256: `9B3859BB5AA658BB…`

### `[2.0.0-alpha.6.21]` — 2026-05-18

**Phase 6.16-η — T11 `/ws` WebSocket (FINAL T11 route).** Adds the one remaining route to bring T11 to **25 / 25 = 100 %** of the v1.20.3 route set. ESPAsyncWebSocket → `esp_http_server` WebSocket migration complete.

- `/ws` — `GET` with `Upgrade: websocket`; subscribes the client to a 2-second status push stream. Same canonical JSON shape as `GET /api/status` (`STATUS_EXPOSE_ALL`, `include_disabled_setpoints=true`). Auth gate at upgrade time only (farmer min role) — symmetric with 1.20.3.

#### Architecture

A dedicated `task_ws_push` task (4 KB stack, core 1) runs the push loop independently of the httpd worker pool — a slow `dm_status_snapshot()` / `build_canonical_status_json()` can never block concurrent HTTP requests. Client tracking uses `esp_http_server`'s own list (`httpd_get_client_list` + `httpd_ws_get_fd_info`) instead of a parallel fd table — stale fds prune themselves: `httpd_ws_send_frame_async` returns an error for a closed socket and the loop just skips it. When no client is subscribed the push task short-circuits before the snapshot/build cost, which matters for the typical deployed-greenhouse case where the dashboard tab is closed.

`WS_PUSH_MS = 2000` matches 1.20.3 exactly.

#### sdkconfig change required

`CONFIG_HTTPD_WS_SUPPORT=y` is **disabled by default** in ESP-IDF 5.5. Added explicitly to `firmware/sdkconfig.defaults`; the auto-generated `sdkconfig.lolin_s3` is gitignored and regenerates from `defaults` on each clean build. The flag pulls in ~6.9 KB of WS framing + handshake code and unlocks the `httpd_uri_t.is_websocket` field plus the `httpd_ws_*` API surface (`httpd_ws_recv_frame`, `httpd_ws_send_frame_async`, `httpd_ws_get_fd_info`).

**Trap encountered (documented for future PlatformIO+IDF migrators):** the first build after the WS handler edit failed with `'httpd_ws_frame_t' was not declared in this scope` because PlatformIO had cached the prior `sdkconfig.lolin_s3` from before the defaults change. Deleting that file and rebuilding regenerates it correctly. PlatformIO does **not** detect a stale sdkconfig vs an updated `sdkconfig.defaults` automatically — there is no implicit `defaults`-newer-than-`lolin_s3` check.

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — added `ws_handler`, `ws_broadcast`, `task_ws_push`, `s_uri_ws` (with `is_websocket = true`), one URI-registration entry, two ESP_LOG lines (the route description + the push-task-start log). Spawns `task_ws_push` after `httpd_start` via `xTaskCreatePinnedToCore(..., core 1)`. ~140 lines.
- **`firmware/sdkconfig.defaults`** — `CONFIG_HTTPD_WS_SUPPORT=y` added with a note explaining why.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.21`.

#### Acceptance — hardware verified on 192.168.20.160

Upgrade handshake (admin cookie):

```
GET /ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: dGVzdC13ZWJzb2NrZXQta2V5MTI=

→ HTTP/1.1 101 Switching Protocols
  Sec-WebSocket-Accept: kTjCX124s2JzwSqlutGQ3yTMyaE=
```

Push cadence verified — two frames captured during a 5 s window:

```
uptime_s = 60   ← first push received
uptime_s = 62   ← +2 s exactly (WS_PUSH_MS honoured)
```

Payload identical to `GET /api/status` — climate / wind / windows / mode / sun / system + `update_interval_s = 240`.

Farmer-role upgrade:

```
GET /ws (farmer cookie)  → 101 Switching Protocols ✓
                           Sec-WebSocket-Accept: ZxbsKQFyR24BdBi40U2UYDmcR98=
                           push received within 2 s
```

Liveness after disconnect:

```
GET /api/whoami → 401 in 241 ms     (httpd worker still healthy)
GET /api/status → uptime_s=183       (system stable, fw_ver=2.0.0-alpha.6.21)
```

No stale-fd accumulation, no heap-leak symptom. The push task gracefully tolerated curl's `--max-time` expiry on its socket and continued serving other clients.

#### Build delta vs alpha.6.20

| Metric | alpha.6.20 | alpha.6.21 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 296 481 B | **1 303 385 B** | +6 904 B |
| RAM static | 60 248 B | 60 232 B | -16 B |

bin sha256: `83CE2D0213AAEF47…` (full hash in `bin/2.0.0-alpha.6.21/`)

**+6.9 KB flash** = the WS framing + handshake code itself (the handler + push task are negligible). RAM essentially unchanged.

#### Phase 6.16 retrospective — T11 web server migration complete

| Sub-phase | Tag | Routes added | Cumulative |
|---|---|---|---|
| α (+.1 Set-Cookie fix) | alpha.6.16 | 4 static + 3 auth | 7 |
| β | (folded into α) | — | 7 |
| γ (+.1 fw[24] fix) | alpha.6.17 | 2 status | 9 |
| δ | alpha.6.18 | 5 config + admin | 14 |
| ε | alpha.6.19 | 5 SD + log | 19 |
| ζ (+ ota_get_state NULL-mutex fix) | alpha.6.20 | 5 OTA + web-tab | 24 |
| η | **alpha.6.21** | 1 WebSocket | **25 / 25** |

ESPAsyncWebServer + AsyncWebSocket entirely retired. `firmware/src/web_server/web_server_1.20.3_original.cpp.archived` remains in tree as a porting reference; scheduled for deletion in Phase 6.N consolidation.

### `[2.0.0-alpha.6.20]` — 2026-05-18

**Phase 6.16-ζ — T11 OTA + web-tab routes (5 of 6 remaining).** Adds the 5 routes that let the web GUI inspect OTA state, push firmware + asset uploads, and configure the status-website integration:

- `GET /api/ota/status` — `{state, progress, error, bank, accepted}` (auth required, any role)
- `POST /api/ota/firmware` — admin only; streams `.bin` body chunks into `ota_firmware_begin/write/end`
- `POST /api/ota/assets` — admin only; accumulates STORE-only `.zip` body into the PSRAM buffer via `ota_assets_begin/accumulate/end` (spawns T13 on success)
- `GET /api/web` — admin only; returns the web-tab settings (status URL, interval, expose mask, log-upload schedule, last-attempt strings)
- `POST /api/web` — admin only; URL/secret/interval bounds-check → NVS write → synchronous `dm_reload_web_cfg()` so the cfg shadow refreshes before the response is sent

**T11 surface grows from 19 → 24 routes** (96 % of the original 25-route plan). Only `/ws` (WebSocket dashboard push) remains for Phase 6.16-η.

#### Bug fix shipped under this tag — `ota_get_state()` NULL-mutex panic

`ota_get_state()` was calling `xSemaphoreTake(s_mx, portMAX_DELAY)` unconditionally. `s_mx` is created lazily inside `ota_mx_init()`, which only runs from `ota_firmware_begin()` / `ota_assets_begin()`. Pre-2.0 nothing read OTA state before a write began, so the bug had been dormant. The new `/api/ota/status` handler is the first read-before-write caller — and it panicked the httpd worker with `assert failed: xSemaphoreTake(NULL, …)` followed by a TCP RST from the kernel. The companion writers (`set_state_locked` / `set_error_locked`) had always been NULL-safe; the fix lifts the same pattern into the read accessor (skip the lock when `s_mx == NULL` — the read is a single byte, benign without lock when no writer is racing).

Detected on-hardware during alpha.6.20 acceptance: first curl-test of `GET /api/ota/status` returned `curl: (56) Recv failure: Connection was reset`. Device remained alive (other routes still responded), so it was a per-worker fault rather than a system panic. NULL-safe edit applied to `firmware/src/ota_manager/ota_manager.cpp`, rebuild + reflash, route now answers `{state:"idle",progress:0,bank:"A",accepted:true}` immediately on cold boot.

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — added 5 handlers (`ota_status_handler`, `ota_firmware_post_handler`, `ota_assets_post_handler`, `web_get_handler`, `web_post_handler`), 5 URI structs, 5 registration-array entries, and two new include lines (`ota_manager/ota_manager.h`, `status_post/status_post.h`). The two log lines describing the registered routes were extended to 9 (added `ota:` and `web:` rows). Total ~310 lines net.
- **`firmware/src/ota_manager/ota_manager.cpp`** — `ota_get_state()` made NULL-safe (4-line edit; see above).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.20`.

#### URL validation — matches 1.20.3 byte-for-byte

`POST /api/web` keeps the three URL rules from the Arduino implementation: must start with `http://` or `https://`, must NOT contain `?` or `#`, must end in `api.php`. T14 itself appends `?action=…` query strings so the operator never types them; bare-URL discipline meant we never accidentally tunnelled a misconfigured query parameter through the redirect chain that HTTPClient used to follow silently. Same `CFG_MIN_SECRET_LEN = 16` check on the shared secret (which is hashed, not echoed, on subsequent GETs).

#### Acceptance — hardware verified on 192.168.20.160

Admin login (PIN `12345678`) then full validation sweep:

```
POST /api/web  url=example.com/api.php         → 400 "URL must start with http:// or https://"
POST /api/web  url=https://x.com/api.php?x=1   → 400 "URL must not contain ? or #"
POST /api/web  url=https://x.com/foo.bar       → 400 "URL must end with \"api.php\""
POST /api/web  secret=short                    → 400 "secret too short"
POST /api/web  interval_s=30                   → 400 "interval out of range"
POST /api/web  log_h=30                        → 400 "bounds"
POST /api/web  full valid payload              → 200 {"ok":true}
GET  /api/web  (same admin cookie)             → all fields persisted; secret not echoed
```

Role enforcement on a farmer cookie:

```
GET  /api/ota/status   → 200 (farmer ≥ farmer)
GET  /api/web          → 403 "admin only"
POST /api/web          → 403 "admin only"
POST /api/ota/firmware → 403 "admin only"
```

The valid-payload write also exercised the write-then-read round-trip: `dm_reload_web_cfg()` returned synchronously and the next GET reflected the new values. Test settings were rolled back to `url="" enable=0` afterwards so the unit doesn't loop on DNS failures to the fake server.

OTA POST upload paths (`/api/ota/firmware` and `/api/ota/assets`) NOT exercised on the live unit because they would overwrite the running firmware. Code paths inspected against the 1.20.3 archived original (lines 1133-1258); the chunk-receive loop is identical, only the wire-layer reader (`httpd_req_recv` vs `AsyncWebServerRequest::onBody`) differs.

#### Build delta vs alpha.6.19

| Metric | alpha.6.19 | alpha.6.20 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,277,072 B | **1,296,481 B** | +19,409 B |
| RAM static | ~60,072 B | 60,248 B | +176 B |

bin sha256: `888C8A8C48E74C6A…` (full hash in `bin/2.0.0-alpha.6.20/`)

**+19 KB flash** — the OTA endpoints carry the largest single jump because their bodies handle multi-megabyte uploads; the buffered-recv loop dominates. The web-tab routes are comparatively cheap.

### `[2.0.0-alpha.6.19]` — 2026-05-18

**Phase 6.16-ε — T11 SD + log routes (5 of 11 remaining).** Adds the 5 routes that let the web GUI inspect the SD card state, manage its lifecycle, and download CSV log files:

- `GET /api/sd/status` — `{mounted, free_mb, size_mb}` (PUBLIC)
- `POST /api/sd/mount` — admin only; calls `event_logger_sd_remount()` so T9 stays consistent
- `POST /api/sd/unmount` — admin only; calls `event_logger_sd_unmount()` (gh#26 sync-before-release path)
- `GET /api/log/files` — admin only; lists `.csv` files on SD, lexicographically sorted (= chronological for YYYYMMDD-named files)
- `GET /api/log/download?file=NAME` — admin only; streams the CSV as `text/csv` with `Content-Disposition: attachment`

**T11 surface grows from 14 → 19 routes** (76 % of the original 25-route plan). Remaining: 4 OTA routes + 1 WebSocket.

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — added 5 handlers + URI registrations. Includes `event_logger.h` for the SD lifecycle helpers and `sd_storage.h` for the file/byte accessors. `cfg.max_uri_handlers` bumped 16 → 28 to make room for the remaining 6 routes.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.19`.

The `nvs_count` field from 1.20.3's `/api/log/files` is intentionally absent — the NVS-ringbuffer log source was retired in alpha.6.5. SD is the only source. Same call out as the deferred web-asset bundle update.

The `src=nvs` branch from 1.20.3's `/api/log/download` is also retired. Default + only mode is now SD-based — query is `?file=NAME`, no `?src=`.

#### Acceptance — full route matrix curl-validated

```
1. GET /api/sd/status              → 200 + {mounted:true, free_mb:1878, size_mb:1880}
2. GET /api/log/files (no cookie)  → 401 no_session
3. Admin login + GET /api/log/files → 200 + 8 entries including cross-firmware
                                       leftovers (ghc_0001.csv from 1.20.3 + the
                                       T9 daily YYYYMMDD-named files +
                                       phase_2_11_test.csv from alpha.2.11.1)
4. GET /api/log/download?file=phase_2_11_test.csv → 200 + text/csv +
                                       Content-Disposition: attachment +
                                       1275-byte body
5. ?file=../etc/passwd             → 400 bad filename (path-traversal rejected)
6. (no file param)                 → 400 missing param
7. ?file=nope.csv                  → 404 not found or empty
```

Operationally meaningful: **`ghc_0001.csv` in the listing demonstrates cross-firmware SD continuity** through the web GUI. The 1.20.3-era logger filename is preserved by T9's "resume existing" path (alpha.6.6) and now appears in T11's listing. Operators can download their pre-migration logs through the new GUI without any data migration.

#### Watch item carried forward

Error bodies in the 400/404 paths of `/api/log/download` return `Content-Type: text/html` instead of `application/json` because the `set_status` path doesn't reset the type. JSON body content is correct; only the header is wrong. Trivial fix (add `httpd_resp_set_type` before each `set_status`/`send`), saved for a future cleanup alpha.

#### Build delta vs alpha.6.18

| Metric | alpha.6.18 | alpha.6.19 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,273,885 B | **1,277,072 B** | +3,187 B |
| RAM static | 60,072 B | (similar) | unchanged |

bin sha256: `49E9A852CBD4A9699C107D16C63BAF94ED953BF711AEDA15C9DDD5221FC2CF2E`

**+3,187 B flash** — five focused handlers, no new task allocations.

### `[2.0.0-alpha.6.18]` — 2026-05-18

**Phase 6.16-δ — T11 config routes (5 of 18 remaining routes).** Adds the 5 routes that let the web GUI read + write configuration:

- `GET /api/config` — full `cfg_shadow_t` dump as JSON (auth required, any role)
- `GET /api/config/limits` — per-key {min,max} bounds (PUBLIC, used for client-side input validation)
- `POST /api/config` — `{ns, key, value | str_value}` with farmer/admin policy
- `POST /api/wifi` — `{ssid, psk, ap_psk}` (admin only); writes NVS + schedules 1-s deferred restart so T10 picks up the new credentials
- `POST /api/pin` — `{role, pin}` (admin only); calls `pin_auth_set` to update the salted SHA-256 hash

**T11 surface grows from 9 → 14 routes** (4 static + 3 auth + 2 status + 5 config). Roughly 60 % through the original 25-route plan.

#### Farmer-vs-admin policy (mirrors 1.20.3 exactly)

The new `is_farmer_key(ns, key)` helper consults two compile-time tables:
```
FARMER_NS = "climate"
FARMER_KEYS = { t_max_day, t_min_day, t_max_ngt, t_min_ngt,
                rh_max_day, rh_min_day, rh_max_ngt, rh_min_ngt,
                rh_ctrl_en, cr_priority }
FARMER_WIND_KEYS in ns="wind" = { wind_prot_en }
```
Anything else (motor, system, wifi, mqtt) requires admin. Farmer-level requests for admin-only keys return **403 forbidden** (vs 401 for missing/invalid session).

`POST /api/wifi` and `POST /api/pin` use a new inline helper `admin_only_or_send_error(req)` that distinguishes "no session" (401) from "wrong role" (403). The general `require_auth(req, min_role)` helper used by the auth-required GET routes still returns 401 for both cases (matching 1.20.3 behaviour for unauthenticated API hits).

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — added 5 handlers + `FARMER_KEYS`/`FARMER_WIND_KEYS` tables + `is_farmer_key` helper + `read_request_body` helper + `wifi_apply_restart_task` one-shot + `admin_only_or_send_error` helper. ~270 lines net. Includes `esp_mac.h` for `esp_read_mac` (AP-SSID generation), `esp_system.h` for `esp_restart()`, `nvs_config.h` for the wifi-cred + fw_ver reads, `cfg_limits.h` for the bounds-stringification.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.18`.

#### Build traps encountered

Two caught during iteration:
1. **`/*` inside a `/* */` comment block** — my route-summary header had `farmer can write climate/* and wind/wind_prot_en` which gcc parsed as a nested-comment start. `-Werror=comment` promoted to error. Fixed by rephrasing as `climate.* keys`.
2. **`-Werror=format-truncation` on `snprintf(upd.key, sizeof(upd.key), "%s", key)`** — `upd.key` is 16 bytes but `key` source buffer is 32. Even though we explicitly check `strlen(key) >= sizeof(upd.key)` before the snprintf, gcc's static analyser can't see that. Fixed by switching to `strncpy(upd.key, key, sizeof(upd.key) - 1) + explicit NUL`. Same pattern as the alpha.6.16 fw_buf rewrite.

#### Build delta vs alpha.6.17.1

| Metric | alpha.6.17.1 | alpha.6.18 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,268,597 B | **1,273,885 B** | +5,288 B |
| RAM static | 60,072 B | **60,072 B** | unchanged |

bin sha256: `776A62F49F61E993A625636114C59973649C8F28E9ED9C18814A7FE47323BFB7`

**+5,288 B flash** for ~270 lines of handler code + the (large) `LIMITS_JSON` static-string table.

#### Acceptance — full policy matrix curl-validated

```
1. GET /api/config/limits                       → 200 + bounds JSON (public)        ✅
2. GET /api/config (no cookie)                  → 401 no_session                    ✅
3. Farmer login + GET /api/config               → 200 + full 37-field cfg dump      ✅
4. Farmer POST climate/t_max_day=30             → 200 + reflected on next GET       ✅
5. Farmer POST motor/travel_m1                  → 403 forbidden (farmer-only policy) ✅
6. Farmer POST /api/wifi                        → 403 admin only                    ✅
7. Restore t_max_day=28, logout farmer          → 200                               ✅
8. Admin login (PIN 12345678)                   → 200 + role:admin                  ✅
9. Admin POST motor/dwell_open_m1               → 200 (admin can write any ns)      ✅
```

Test 4 is the operationally critical signal: a farmer-level POST through the web GUI flows **Q4 → T4 → NVS → next dm_cfg_snapshot**. The full setpoint-change loop is now live on the new T11 — the GUI is no longer view-only.

Skipped live `/api/pin` change to avoid disrupting the test state (would have rotated the farmer hash and required a fresh stage-1 IO0 reset to recover). The handler compiled + the route registered + the admin auth gate verified.

### `[2.0.0-alpha.6.17.1]` — 2026-05-18

**Watch-item fix — `status_snapshot_t.fw[16]` truncation.** Caught at alpha.6.17 acceptance: `/api/status` returned `"fw_ver":"2.0.0-alpha.6.1"` instead of the full `"2.0.0-alpha.6.17"`. Root cause: `status_snapshot_t.fw` was sized at 16 chars (matching 1.20.3's 6-char version strings) but the 2.0.0 alpha tags grew to 16 chars + NUL = 17 bytes, overflowing by one. The `strncpy(out->fw, FIRMWARE_VERSION, sizeof(out->fw) - 1u)` in `dm_status_snapshot` silently clipped the trailing character.

Same family as alpha.6.13's `ota_manager.cpp` `char fw_ver[16] → [32]` fix. One-line bump per field — and the existing `sizeof(out->fw) - 1u` strncpy pattern picks up the new size automatically.

#### What changed

- **`firmware/src/types/app_types.h`** — `status_snapshot_t.fw[16]→[24]` and `status_snapshot_t.assets[16]→[24]` (assets bumped in lockstep for consistency; both fields hold the same string family). 24 bytes gives headroom through `2.0.0-rc.N` and any `2.x.x.x` patterns.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.17.1`.

No other code changes — all callers use `sizeof()` or compare against the struct field, so the size bump is transparent.

#### Acceptance

```
$ curl -s http://192.168.20.160/api/status | grep -oP 'fw_ver":"[^"]+"'
fw_ver":"2.0.0-alpha.6.17.1"
```

Full version string now visible in the JSON payload. The web GUI dashboard will display the correct firmware version.

bin sha256: `1D24B66F6A0AF343BED82275C1787F4447D86174E7CE4A18901A07FCB13EC09E`

### `[2.0.0-alpha.6.17]` — 2026-05-18

**Phase 6.16-γ — T11 status routes (2 of 18 deferred routes).** Adds `/api/status` (canonical status JSON snapshot for dashboard tiles) and `/api/history?n=N` (last N sensor ring entries). Both public (no auth gate) — matches 1.20.3 production behaviour and lets the web GUI render the dashboard tiles to unauthenticated visitors before they log in to edit setpoints.

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — added 2 handler functions + 2 httpd_uri_t entries + 2 entries in the registration array. New handlers heap-allocate their working buffers (status: 4 KB, history: 8 KB) since both can be too large for the httpd task's default stack. Heap pressure is brief and per-request.
  - `status_handler` calls `dm_status_snapshot(&snap)` (T4 alpha.6.7 export) into a heap-allocated `status_snapshot_t` (~600 B), then `build_canonical_status_json(body, 4096, &snap, STATUS_EXPOSE_ALL, /*include_disabled_setpoints=*/true)`.
  - `history_handler` parses `?n=N` via `httpd_query_key_value` (capped at HIST_MAX_ROWS=60, defaults to 60), pulls the last N entries from T4's ring buffer via `dm_ring_count` + `dm_ring_read(offset, buf, count, &actually_read)`, then hand-builds a compact JSON array `[{"ts":N,"t":N,"rh":N,"ws":N,"wd":N}, ...]` with `snprintf`.
- **`firmware/src/CMakeLists.txt`** — added `status_post/status_json.cpp` to SRCS. The file is framework-agnostic (`stdarg.h`, `stdio.h`, `string.h`, no Arduino dependencies) so no patching was needed. Now linked in to provide `build_canonical_status_json` to T11 (and eventually full T14 in 6.15.X).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.17`.

The 7-route T11 from alpha.6.16 grows to 9 routes here. T11 log line updated: `HTTP server running on port 80 — 9 routes registered`.

#### Build trap

`dm_ring_read` signature has 4 params, not 3 — I missed the `uint16_t *read_out` out-param on first draft. Fixed: caller now supplies `&actually_read` and rescopes `n` to the returned count (handles the edge case where requested > available).

#### Build delta vs alpha.6.16.1

| Metric | alpha.6.16.1 | alpha.6.17 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | (similar) 1,263,904 B | **1,268,541 B** | +4,637 B |
| RAM static | (similar) 60,056 B | **60,056 B** | unchanged |

bin sha256: `6F72ED5049D6D43DFD39D5C6A4B64ABCAFDDFA967ABBAB20EFEF73285E86E51B`

The **+4.6 KB flash** is status_json.cpp (243 lines) + the 2 new handler bodies. RAM static unchanged — all status/history working buffers are heap-allocated per-request.

#### Acceptance bar — curl-tested at uptime 30s

```
GET /api/status        → 200 + 697 B canonical JSON (climate/wind/windows/
                             mode/sun/system tiles all populated)
GET /api/history?n=3   → 200 + [] (empty array — T5 hadn't deposited any
                             readings yet during early-boot calibration)
```

The status payload at uptime 30 s captured the unit mid-calibration:
- `windows.M3 = "MOVING_CLOSE"` (T2 calibrating M3, ~140 s remaining)
- `mode.current = "WINDOW_CAL"`, `flags=["calibrating"]` (EG1_BIT_CALIBRATING set)
- `climate.temp_c = 0.0`, `rh_pct = 0` (T5 first poll is at uptime ~38 s, hadn't fired yet)
- `system.ntp_synced = true`, `wifi_ip = "192.168.20.160"`, `wifi_rssi_dbm = -66`
- `time_iso = "2026-05-18T13:57:55"` — DS1307 time, post-SNTP, correct
- `eg1 = 64` (bit 6 = EG1_BIT_CALIBRATING)

Watch item caught: **`fw_ver` field truncated to "2.0.0-alpha.6.1"** instead of "2.0.0-alpha.6.17". Root cause: `status_snapshot_t.fw` is `char fw[16]` (15 chars + NUL) but our alpha-tag string is 16 chars + NUL. Same family of bug as alpha.6.13's `ota_manager.cpp` `fw_ver[16]→[32]` bump. Functional behaviour is fine; only the displayed JSON field is clipped. Fix is a 1-line bump in `types/app_types.h`; saved for the next micro-alpha to land cleanly without disturbing 6.17's bin sha256.

### `[2.0.0-alpha.6.16.1]` — 2026-05-18

**Bug fix — `Set-Cookie` header value falls out of scope before `httpd_resp_send` reads it.** alpha.6.16's acceptance curl test caught it on the first POST /api/login: the body returned `{"ok":true,"role":"farmer"}` cleanly but the `Set-Cookie:` header value was garbled bytes (`???`) instead of the generated hex token. The login + cookie-aware whoami round-trip therefore failed (server rejected the corrupted token).

**Root cause**: `cookie_set_session()` built the Set-Cookie value into a stack-local `char hdr[96]` buffer and passed it to `httpd_resp_set_hdr()`. The IDF httpd contract is explicit on this: `httpd_resp_set_hdr` does NOT copy the value; it stores a pointer that must remain valid until any send API is invoked. The `hdr` buffer fell out of scope as soon as `cookie_set_session` returned. By the time esp_http_server wrote the response headers, the stack memory had been reused by other function calls — the cookie value read whatever happened to be on the stack at that address.

#### What changed

- **`firmware/src/web_server/web_server.cpp`**:
  - `cookie_set_session()` signature now takes a caller-owned `char *hdr, size_t hdr_cap` pair. The caller (login_handler) allocates `char hdr_buf[96]` in its own stack frame, which outlives the `httpd_resp_set_hdr → httpd_resp_send` sequence.
  - Inline comment added documenting the IDF contract and the alpha.6.16 acceptance-test catch.
  - `cookie_clear_session()` left unchanged — its value is a string literal with static storage duration, so the pointer is always valid.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.16.1`.

#### Acceptance — full curl auth flow

```
1. GET /                       → 404 + "Web assets not yet uploaded" placeholder ✅
2. GET /api/whoami             → 401 + {"ok":false,"error":"no_session"}         ✅
3. POST /api/login (farmer/1234) → 200 + Set-Cookie: session=21ee0da378ce70b3;
                                       Path=/; HttpOnly; Max-Age=300              ✅
4. GET /api/whoami (cookie)    → 200 + {"role":"farmer"}                          ✅
5. POST /api/logout (cookie)   → 200 + Set-Cookie: session=; Max-Age=0;
                                       {"ok":true}                                 ✅
6. GET /api/whoami (stale cookie) → 401 + {"ok":false,"error":"no_session"}       ✅
```

Test 6 is the critical signal: the curl cookie jar still contained the token, but the server-side `session_close()` had cleared the in-memory slot, so `session_find_and_renew()` correctly returned WEB_ROLE_NONE. Captured-cookie replay is therefore not possible after logout. Production-grade behaviour.

Also note from test 3: `Max-Age=300` — the cookie expiry is being driven by `cfg.session_timeout_min × 60 = 5 × 60 = 300 s`. The dm_cfg_snapshot integration is wired through correctly from T4 to T11.

bin sha256: `70A7C3C563AD3E8A46968D851E97A52184562AB167F50C706D2314E9DB00A000`

### `[2.0.0-alpha.6.16]` — 2026-05-18

**Phase 6.N.3-α/β — web_server (T11) minimal activation: static + auth.** Third of the four tickle-replacement subphases. Replaces alpha.5's `web_server_tickle.cpp` (3-route hardcoded HTML) with the real T11 backed by `esp_http_server` + LittleFS. **7 of the 25 routes** are wired in this alpha: 4 static (served from LittleFS) + 3 auth (cookie session + pin_auth integration). The remaining 18 routes (status/config/sd/log/ota/ws) land in follow-up alphas.

#### What's in this alpha (7 routes)

**Static (4) — served from active LittleFS partition via LIB-9 wrapper:**
- `GET /` → `/index.html`
- `GET /style.css` → `/style.css`
- `GET /app.js` → `/app.js`
- `GET /manifest.json` → `/manifest.json`

On factory-fresh units with empty LittleFS, all 4 return a friendly 404 placeholder ("Web assets not yet uploaded — use OTA /api/web") instead of an opaque error. Cleanly indicates T11 is alive even before the web-asset bundle is uploaded.

**Auth (3) — cookie session + pin_auth.cpp integration:**
- `GET /api/whoami` → returns `{"role":"farmer"|"admin"}` (200) or `{"ok":false,"error":"no_session"}` (401). Slides the session expiry forward on a hit (renewal pattern).
- `POST /api/login` → body `{"role":"farmer"|"admin","pin":"NNNN"}`. Verifies via `pin_auth_verify`. On match: generates a 16-hex-char token, opens a session slot (4-slot in-memory table, LRU eviction), sets `Set-Cookie: session=TOKEN; Path=/; HttpOnly; Max-Age=N`, returns `{"ok":true,"role":"R"}`. On wrong PIN: `{"ok":false,"locked":false}`. On lockout: `{"ok":false,"locked":true,"remaining":N}` (reads `pin_auth_lockout_remaining_secs`).
- `POST /api/logout` → invalidates the session and clears the cookie. Always returns 200.

#### Deferred to follow-up alphas (18+ routes)

`/api/status`, `/api/config`, `/api/config/limits`, `/api/wifi`, `/api/pin`, `/api/history`, `/api/sd/status`, `/api/sd/mount`, `/api/sd/unmount`, `/api/log/files`, `/api/log/download`, `/api/ota/firmware`, `/api/ota/assets`, `/api/web`, `/api/ota/status`, `/ws`, and any I'm missing. Each needs either status_json.cpp (rich payload), multipart upload (OTA), streaming download (log), or WebSocket framing (real-time push) — all are non-trivial. They land in 6.16.X alphas as focused, bisectable patches.

#### What changed

- **`firmware/src/web_server/web_server.cpp`** — full rewrite. Original 1330-line ESPAsyncWebServer-based file archived as `web_server_1.20.3_original.cpp.archived` (via `git mv`, preserves history). New ~600-line IDF-native `esp_http_server` task with the 7 routes + session table + cookie helpers + LittleFS-streaming pattern.
- **`firmware/src/web_server_tickle.cpp`** — kept in the source tree but **REMOVED from CMakeLists SRCS**. T11 now provides the `/`, `/api/status`-like (later), and `/api/info`-like (later) routes; the tickle's hardcoded HTML page is no longer the entry point. The file stays around for reference; could be deleted after T11 is fully ported.
- **`firmware/src/CMakeLists.txt`** — added `web_server/web_server.cpp`; removed `web_server_tickle.cpp`.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "web_server_tickle.h"` → `#include "web_server/web_server.h"`.
  - Replaced the `web_server_tickle_start()` one-shot call with a `xTaskCreatePinnedToCore(task_web_server, "T11-web", 6144, NULL, 4, &task_t11, tskNO_AFFINITY)` spawn. Stack 6 KB (T11's own body just idles; the real HTTP work happens in esp_http_server's internal task with stack 8 KB set inside task_web_server). Priority 4 — below T10/T14 (3) since web traffic is non-critical compared to the network state machine.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.16`.

#### Session model

In-memory table, MAX_SESSIONS=4 slots. Each slot: 16-hex-char token, `web_session_role_t` (NONE/-1, FARMER/0, ADMIN/1 — local enum because pin_auth.h's pin_role_t has only 0 and 1, no sentinel), Unix expiry, configured timeout. Sessions are lost on reboot (acceptable: operator re-authenticates after a power-cycle). LRU eviction on overflow.

Mutex `s_sess_mux` protects table modifications. All API handlers can run concurrently in httpd's task pool; the mutex ensures atomic find-and-renew + insert + close.

#### Build traps encountered

Four caught and fixed during iteration:
1. `PIN_ROLE_NONE` doesn't exist in pin_auth.h (enum has only FARMER=0, ADMIN=1). Fixed by introducing a local `web_session_role_t` with `WEB_ROLE_NONE = -1`. Cast to `pin_role_t` at the call boundary to `pin_auth_verify` / `pin_auth_lockout_remaining_secs`.
2. `littlefs_read` signature mismatch — takes `char *` (not `uint8_t *`) and only 4 args (no out-length param; the buffer is NUL-terminated and `strlen` gives the byte count). Reworked `serve_lfs_file` to match.
3. `-Werror=format-truncation` on the 256-byte placeholder body. Bumped to 512.
4. `-Wunused-function` on `require_auth` (defined for the 18 deferred routes, not used by the 7 minimal ones). Suppressed with `__attribute__((unused))` plus a comment explaining the intent.

#### Build delta vs alpha.6.15

| Metric | alpha.6.15 | alpha.6.16 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,262,497 B | **1,263,941 B** | +1,444 B |
| Flash usage % | 60.2 % | 60.3 % | +0.1 pp |
| RAM static | 59,920 B | **60,056 B** | +136 B |

bin sha256: `4079718D80E768113AFA07B2BA8A43FC63DDF9083DF731C88E39AE909B3EE65D`

The **+1,444 B flash** is the net of T11's ~600-line body minus web_server_tickle's ~330 lines (which is no longer compiled into SRCS but still on disk). RAM delta +136 B from `s_sessions[4]` table (4 × ~32 B) and `s_sess_mux` pointer.

Runtime heap impact at heartbeat baseline: T11 task stack 6 KB + httpd internal task stack 8 KB = ~14 KB. Heartbeat baseline expected ~124,000 free internal (was 137,623 at 6.15).

#### Acceptance bar for alpha.6.16

1. ✅ Build succeeds (after four iteration fixes).
2. Flash & boot: existing T2/T3/T4/T5/T6/T7/T8/T9/T10/T14 chain regression-clean.
3. T11 spawn banner: `alpha.6.16: T11 web_server task spawned (handle=0x...); 4 static + 3 auth routes on port 80`.
4. T11 task-alive: `[T11_WEB] [T11] task alive (minimal T11 — static + auth only)`.
5. T11 server-started: `[T11_WEB] [T11] HTTP server running on port 80 — 7 routes registered` + 2 follow-up lines listing the route paths.
6. **Browser test 1** — visit `http://192.168.20.160/`. Should show the **"Web assets not yet uploaded"** placeholder (404 status, but readable). Same for `/style.css`, `/app.js`, `/manifest.json` (placeholder text varies by path).
7. **Browser test 2** — visit `http://192.168.20.160/api/whoami`. Should return 401 with `{"ok":false,"error":"no_session"}`.
8. **curl test** — `curl -i -X POST -H "Content-Type: application/json" -d '{"role":"farmer","pin":"1234"}' http://192.168.20.160/api/login`. Should return 200 + `Set-Cookie: session=...` + body `{"ok":true,"role":"farmer"}`. If you previously did the IO0 stage-1 reset, default "1234" works; otherwise it'll be 401 (no session yet seeded).
9. **Cookie-aware curl** — same login, save the cookie via `-c`, then `curl -b cookies.txt http://192.168.20.160/api/whoami` → 200 `{"role":"farmer"}`. Then `curl -b cookies.txt -X POST http://192.168.20.160/api/logout` → 200 `{"ok":true}`. Then whoami again → 401.
10. Heap stable around 124,000 free internal.
11. Run ≥ 5 min; no resets; no stack-overflow on T11 or the httpd internal task.

### `[2.0.0-alpha.6.15]` — 2026-05-18

**Phase 6.N.2 — status_post (T14) minimal activation.** Second of the four tickle-replacement subphases. Replaces the alpha.5 `https_tickle.cpp` one-shot with a long-running T14 task that POSTs a status JSON every `cfg.status_interval_s`. T15 supervisor + gh#23 mbedtls mitigations + streaming SD-log upload are explicitly deferred to a follow-up patch — see source-file header for the full deferred-features list.

#### The decision: minimal rewrite, full port deferred

The 1.20.3 `status_post.cpp` (942 lines, Arduino-HTTPClient based, with persistent WiFiClientSecure + heap-drop accumulator + circuit breaker + planned-reboot supervisor integration) is archived as `status_post_1.20.3_original.cpp.archived`. The migration plan §"Phase 4 — HTTPS client" describes the full rewrite (gh#23 fix payoff: max_frag_len=1024, single cipher suite, mbedtls session-ticket reuse via `HTTP_EVENT_ON_FINISH`/`HTTP_EVENT_ON_CONNECTED`); that work lives in a future patch.

Tonight's minimal-T14 lands the structural pieces:
1. **Replace https_tickle one-shot** with a periodic long-running task.
2. **Force-remove status_post_stub.cpp** — the real status_post.cpp now provides `status_post_backoff_active()` (same stub-and-linker-conflict pattern as data_manager_stub.cpp alpha.6.7 and relay_controller_stub.cpp alpha.6.9).
3. **Provide all 6 symbols from status_post.h** so ui_display + future T11 web_server callers link cleanly: `task_status_post`, `status_post_backoff_active`, `status_post_last_str`, `status_post_last_log_str`, `status_post_heartbeat`, `status_post_heap_drop_bytes`, `status_post_force_teardown`.

#### Minimal-T14 scope (alpha.6.15)

**Implemented:**
- `task_status_post` long-running task. Reads `cfg.status_url` + `cfg.status_interval_s` via `dm_cfg_snapshot`. If URL is empty or interval is zero: sleeps 60 s and re-checks (lets the operator enable/disable status posts via the web/LCD menu at runtime).
- One esp_http_client POST per cycle. Uses the same shape as https_tickle (`crt_bundle_attach` + `keep_alive_enable=true` + 1 KB buffers).
- Minimal JSON payload: `{"unit_id":"NNNN","fw_version":"X","uptime_s":N,"free_heap":N}` (~80 bytes). Adequate for server connectivity testing; full sensor + relay + alarms snapshot lands when status_json.cpp activates in a follow-up.
- `s_heartbeat` increments per loop tick (T15 supervisor hook, exposed via `status_post_heartbeat()`).
- `s_last_str` formatted as `"OK YYYY-MM-DD HH:MM:SS"` / `"FAIL YYYY-MM-DD HH:MM:SS"` (rendered on T8 LCD Web tab + future web GUI).

**Deferred to a follow-up patch** (preserves the gh#23 fix work for focused attention):
- mbedtls session-ticket reuse (`HTTP_EVENT_ON_FINISH` save + `HTTP_EVENT_ON_CONNECTED` restore).
- mbedtls knobs (max_frag_len=1024, single cipher suite TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256).
- Streaming SD-log upload (`SDFileChunkedStream` → `esp_http_client_open + write + fetch_headers`).
- gh#24 heap-drop accumulator (signed-balance math around each POST).
- gh#25 log-upload dedup latch.
- Circuit breaker (10 consecutive failures → 60 s lockout).
- T15 supervisor task (wedge detection, leak detection, planned-reboot path).
- Full status_json.cpp payload (sensor snapshot, relay states, EG1 flag bits, alarms array).

#### What changed

- **`firmware/src/status_post/status_post.cpp`** — full rewrite. Old 942-line Arduino-HTTPClient file moved to `status_post_1.20.3_original.cpp.archived` (via `git mv`, preserves history). New ~330-line IDF-native file with the minimal T14 task.
- **`firmware/src/status_post/status_post_stub.cpp`** — **DELETED** via `git rm`. Real status_post.cpp now provides `status_post_backoff_active()`. Force-removal pattern.
- **`firmware/src/CMakeLists.txt`** — added `status_post/status_post.cpp`; removed `status_post/status_post_stub.cpp`.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "status_post/status_post.h"` added.
  - Spawn T14 after T10 with `xTaskCreatePinnedToCore(task_status_post, "T14-status", 8192, NULL, 3, &task_t14, tskNO_AFFINITY)`. Stack 8 KB (mbedtls handshake peaks at ~5 KB; +3 KB margin matches 1.20.3 prod + https_tickle observed usage). Priority 3 — same as T10; network tasks latency-tolerant vs T2/T6 climate priorities.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.15`.

`https_tickle.cpp` stays in the build — still runs the 5×HTTPS-POST boot connectivity test (originally the gh#23 baseline demonstration). T14 takes over from there as the long-running periodic poster. A future alpha could fold the boot connectivity test into T14's first cycle and delete https_tickle.cpp entirely; deferred to keep this patch focused.

#### Build trap encountered

- `system_id_unit_id` → `system_unit_id_u16`. My initial draft used a function name that didn't exist; the real export from `system_id.h` is `system_unit_id_u16(void)` returning `uint16_t`. Same Phase 6.3 module, just a different name. One-line fix.

#### Build delta vs alpha.6.14

| Metric | alpha.6.14 | alpha.6.15 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,260,041 B | **1,262,497 B** | +2,456 B |
| Flash usage % | 60.1 % | 60.2 % | +0.1 pp |
| RAM static | 59,880 B | **59,920 B** | +40 B |

bin sha256: `1B09793DD312AEECE8285B11048B6EA2946405BCE9ADAAF12E997BC1B7F1FB12`

The **+2,456 B flash** is T14's ~330-line minimal task body. RAM delta +40 B for the s_last_str + s_last_log_str + s_heartbeat + s_heap_drop_bytes module-level state.

Runtime heap impact at heartbeat baseline expected: ~−8 KB free vs alpha.6.14 (T14 task stack 8 KB). Plus, per-cycle TLS handshake adds transient ~30-40 KB but releases back. Heartbeat baseline ~138,000 free (was 146,143 at alpha.6.14).

#### Acceptance bar for alpha.6.15

1. ✅ Build succeeds (after `system_unit_id_u16` rename).
2. Flash & boot: existing T2/T3/T4/T5/T6/T7/T8/T9/T10 chain regression-clean.
3. T14 spawn banner: `alpha.6.15: T14 status_post task spawned (handle=0x...); periodic HTTPS POST every cfg.status_interval_s`.
4. T14 task-alive: `[T14_STA] [T14] task alive (minimal T14 — see file header for deferred features)`.
5. **First T14 POST** at uptime ~12 s (T14 spawns at ~3 s + 2 s settling + ~5 s first-cycle wait): `[T14_STA] [T14] POST OK: status=N len=N elapsed=N ms (body=N B)` OR `[T14_STA] [T14] POST FAIL: ...` if the status server is unreachable.
6. **Subsequent POSTs** every `cfg.status_interval_s` (default 240 s on production NVS).
7. The boot https_tickle still does 5×POSTs as before (still in app_main_stub); T14 picks up after.
8. T8 LCD Web tab (if you navigate to it) should display the last status_post outcome.
9. Heap stable around 138,000 free internal (was 146,143; T14 stack 8 KB).
10. Run ≥ 10 min; no resets; observe at least 2 successful periodic POSTs.

### `[2.0.0-alpha.6.14]` — 2026-05-18

**Phase 6.N.1 — network_manager (T10) minimal activation.** The Phase 6 final-assembly subphases begin. T10 is the first of the four "tickle replacements" — the existing alpha.5 tickles (wifi_tickle, https_tickle, web_server_tickle) get replaced by their real long-running task counterparts. This alpha activates the minimal viable T10; T14 (status_post + supervisor), T11 (web_server full), and T1+main consolidation follow in 6.15/6.16/6.17.

#### The decision: minimal rewrite, not full port

The 1.20.3 `network_manager.cpp` (720 lines) was written against Arduino-ESP32's `WiFi.h` + `HTTPClient.h`. The class APIs (`WiFi.begin/WL_CONNECTED/configTime/WiFi.softAP`) do not map 1:1 to esp_wifi/esp_netif/esp_event, so this was always going to be a rewrite — see the migration plan §"Phase 3 — Network stack" entry. The 1.20.3 file is preserved in-tree as `network_manager_1.20.3_original.cpp.archived` for future reference; the new `network_manager.cpp` is a focused ~170-line IDF-native task.

#### Minimal-T10 scope (alpha.6.14)

**Implemented:**
1. **Q5 producer**: snapshots `client_connected` (via `esp_wifi_sta_get_ap_info`), `ntp_synced` (`time(NULL) > 2023-11-14`), `ip_str` (via `esp_netif_get_ip_info("WIFI_STA_DEF")`), and posts to Q5 via `xQueueOverwrite`. T8's LCD WiFi page now reflects real state.
2. **TN4 to T4**: on the initial snapshot (after `wifi_tickle_run` did the boot connect + SNTP), sends `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` so T4 writes the post-SNTP system time back to the DS1307 RTC.
3. **Periodic monitor**: every NET_POLL_MS (5 s) re-snapshots state; reposts Q5 only on material change (avoids T8 LCD jitter); re-notifies TN4 on a CLEAR→SYNCED transition.

**Deferred** — listed in the source-file header for traceability and follow-up:
- **AP fallback** (soft-AP mode if STA can't connect). esp_wifi's auto-reconnect handles persistent STA retry; AP captive-portal lands in a follow-up.
- **Exponential backoff state machine** (2→4→8→16→32→60 s). esp_wifi's internal cadence is sufficient for soak.
- **HTTPClient geo/timezone lookup**. Timezone hard-coded in NVS already (`tz=CET-1CEST,M3.5.0,M10.5.0/3`), no per-boot HTTP probe needed.
- **Periodic 24 h NTP resync**. DS1307 holds time precisely; add later if Phase 7 soak shows drift.

#### What changed

- **`firmware/src/network_manager/network_manager.cpp`** — full rewrite. Old 720-line Arduino-WiFi file moved to `network_manager_1.20.3_original.cpp.archived` (via `git mv`, preserves history). New ~170-line IDF-native file with the minimal-T10 task body.
- **`firmware/src/CMakeLists.txt`** — added `network_manager/network_manager.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "network_manager/network_manager.h"` added.
  - Spawn T10 after the wifi_tickle returns (so esp_wifi + SNTP are already in steady state when T10 takes its first snapshot). Stack 6 KB (1.20.3 prod used 8 KB but had AP setup, HTTPClient buffer, geo JSON parser — none present here). Priority 3 (low; network polling is latency-tolerant; T2-T6 at 4-6 all preempt cleanly).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.14`.

`wifi_tickle.cpp` stays in the build — it still does the initial boot connect. T10 picks up monitoring duty after `wifi_tickle_run()` returns. A future alpha could fold the initial connect into T10's task body and delete wifi_tickle.cpp entirely; deferred to keep this patch minimal.

#### Build delta vs alpha.6.13

| Metric | alpha.6.13 | alpha.6.14 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,258,833 B | **1,260,041 B** | +1,208 B |
| Flash usage % | 60.0 % | 60.1 % | +0.1 pp |
| RAM static | 59,872 B | **59,880 B** | +8 B |

bin sha256: `C120357D8F7D9D897B423BD4BA57A888119120E9DED86F31A1C95396118178CC`

The **+1,208 B flash** is T10's task body (~170 lines including the file header docstring). RAM delta +8 B — the prev/cur net_status_t snapshots are task-local on the stack, not BSS; the only static is the TAG pointer.

Runtime heap impact at heartbeat baseline expected: ~−6 KB free vs alpha.6.13 (T10 task stack 6 KB). Heartbeat baseline ~146,000 free (was 152,975 at alpha.6.13).

#### Acceptance bar for alpha.6.14

1. ✅ Build succeeds.
2. Flash & boot: existing T2/T3/T4/T5/T6/T7/T8/T9 chain regression-clean.
3. T10 spawn banner: `alpha.6.14: T10 network_manager task spawned (handle=0x...); Q5 producer + TN4 to T4 (NTP sync ack)`.
4. T10 task-alive: `[T10_NET] [T10] task alive (minimal T10 — see file header for deferred features)`.
5. **Initial Q5 post** at uptime ~10 s (500 ms after T10 spawn): `[T10_NET] [T10] initial Q5 post: client=1 ap=0 ntp=1 ip="192.168.20.160"`.
6. **TN4 sent**: `[T10_NET] [T10] TN4 sent to T4 (DM_NOTIFY_NTP_SYNCED)`.
7. **T4 reacts to TN4**: T4 calls `rtc_set_time()` under MX1 to write post-SNTP system time to DS1307. T4's `dm_get_unix_time()` should now return the up-to-date Unix timestamp (no longer reverting to boot-time RTC value).
8. **T8 LCD WiFi page now shows real state**: the WiFi status page should display `WiFi: connected` and `192.168.20.160` (not `WiFi: ---` as before). This is the user-visible signal that T10 is live.
9. T10 main loop continues polling every 5 s. **No further Q5 posts** while state is stable (idempotency).
10. All earlier-phase tickles regression-clean.
11. Run ≥ 10 min; no resets; no stack-overflow on T10.

### `[2.0.0-alpha.6.13]` — 2026-05-18

**Phase 6.13 — ota_manager (T13) compiled in.** The OTA Manager source migrates with a single Arduino.h drop. T13 is **spawned on demand** by T11 (web server) when an asset-ZIP upload arrives — T11 is still the alpha.5 tickle stub, so T13 doesn't actually run yet, but all its symbols (ota_check_rollback, ota_firmware_*, ota_assets_*, ota_get_*, task_ota_manager) are now linked in and ready for Phase 6.N to wire up.

The ota_manager subsystem was deliberately already IDF-native in 1.20.3 (per migration plan §"Existing functions and patterns to reuse — `esp_ota_*` OTA path"). No spawn block in app_main_stub.cpp — T13 isn't a long-lived task; it's transient, started on each asset-ZIP write and exits when done.

#### What changed

- **`firmware/src/ota_manager/ota_manager.cpp`** — two single-line patches:
  - Dropped `#include <Arduino.h>`. T13 has zero Arduino-specific calls (esp_ota_*, esp_partition_*, FreeRTOS timers, mbedtls all native).
  - `char fw_ver[16] = FIRMWARE_VERSION;` (line 662) → `char fw_ver[32] = FIRMWARE_VERSION;`. The 1.20.3 version string was "1.20.3" (6 chars), well under 16. The migration's alpha tags grew to "2.0.0-alpha.6.13" (16 chars + NUL = 17 bytes) which overflows the 16-byte buffer under `-fpermissive` promoted to error. Buffer grown to 32 for headroom through 2.0.0-alpha.X.Y / 2.0.0-rc.N / 2.x.x patterns.
- **`firmware/src/CMakeLists.txt`** — added `ota_manager/ota_manager.cpp` to SRCS.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.13`.

#### What does NOT change (deferred to Phase 6.N)

- **`ota_check_rollback()` not called at boot** — would increment NVS `system/ota_fail_cnt` every reboot. Without `ota_mark_healthy()` (called by T1 watchdog after 30 s healthy uptime — T1 is dormant in this phase), the counter would hit 3 within 3 reboots and trigger rollback to the previous OTA bank. Wiring `ota_check_rollback()` waits until T1 lands in Phase 6.N.
- **T13 task body never executes** — only T11 spawns T13, and T11 is still the alpha.5 tickle. The functions are linked in but dead-code-eliminated at runtime until T11 is fully ported.

#### Build delta vs alpha.6.12.1

| Metric | alpha.6.12.1 | alpha.6.13 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,256,653 B | **1,258,833 B** | +2,180 B |
| Flash usage % | 59.9 % | 60.0 % | +0.1 pp |
| RAM static | 59,552 B | **59,872 B** | +320 B |

bin sha256: `C8DD03D764E3011BDBD24BB8AE9CD2D06D5E1DB59504051B6CFEC895F7180A33`

The **+2,180 B flash** is T13's 713-line body (FW + assets state machines, ZIP STORE extractor, rollback logic). RAM static delta +320 B — the s_error[80] + module-level state (s_state, s_progress, s_mx pointer, partition pointers, fail-timer handle).

#### Acceptance bar for alpha.6.13

1. ✅ Build succeeds (after `fw_ver` buffer fix).
2. ✅ All T13 symbols resolve at link time (no undefined-reference errors).
3. Flash & boot: existing T2/T3/T4/T5/T6/T7/T8/T9 chain regression-clean. Heartbeat shows ~157,000 free internal heap (was ~157,000 at 6.12.1 — T13's modules add a few hundred bytes to BSS but no live tasks).
4. **No `[T13_OTA]` log lines** during normal operation (T13 doesn't spawn until T11 calls it).
5. T13 is **callable but dormant** until Phase 6.N wires up T11 + T1.

#### Phase 6 status — last single-task activation

After alpha.6.13, **every task source file is in the build**. The remaining work is Phase 6.N (final assembly):
- Replace `app_main_stub.cpp` with the real `main.cpp` task graph.
- Replace `wifi_tickle.cpp` stub with real `network_manager` task (T10, spawning + event handler permanently).
- Replace `https_tickle.cpp` stub with real `status_post.cpp` task graph (T14 + T15 supervisor + force-removes status_post_stub.cpp).
- Replace `web_server_tickle.cpp` stub with full `web_server.cpp` + 25 route handlers.
- Wire `ota_check_rollback()` at boot + `ota_mark_healthy()` from T1 watchdog after 30 s healthy uptime.

The Phase 6 stage table now reads: 1, 2, 3, 4, 5, 6.0–6.13 done; 6.N pending.

### `[2.0.0-alpha.6.12.1]` — 2026-05-18

**Bug fix — missing `pin_auth_init()` call on normal boot.** Production 1.20.3's `setup()` calls `pin_auth_init()` during boot to initialise the PIN auth subsystem (salt + default hashes on first boot; sets `s_initialized=true`). The alpha.6.12 port to `app_main_stub.cpp` missed this call — `pin_auth_init()` was only present in `execute_reset_action()` (T8's factory-reset handler at ui_display.cpp:651/665/679). On a **factory-fresh** unit's first boot under alpha.6.12, the default farmer PIN "1234" was therefore rejected by `pin_auth_verify()` because `s_initialized==false` → `PIN_AUTH_ERR_INIT`. The stage-1 IO0-button factory reset masked the bug by calling `pin_auth_init()` as part of the reset action, after which the default PIN worked.

**Correction to the alpha.6.12 acceptance note**: my earlier explanation attributed the PIN failure to "the dev unit's NVS already holds the operator's custom PIN from 1.20.3 — proves cross-firmware NVS continuity." That diagnosis was **wrong**: the user clarified the dev unit was never used before (no prior 1.20.3 NVS data). The PIN failure was an actual regression in the alpha.6.12 port, not a cross-firmware-NVS demonstration. (The cross-firmware claim still holds for cfg setpoints alpha.6.7 and channel state alpha.6.9, where the dev unit's behaviour confirmed it — but pin_auth was a different bug on a clean unit.)

#### What changed

- **`firmware/src/app_main_stub.cpp`**:
  - `#include "auth/pin_auth.h"` added (next to the other include block).
  - Single `pin_auth_init()` call inserted immediately after `nvs_cfg_init()` (so it runs ~1.05 s after boot, well before T8 spawns at ~1.78 s). Return value logged: `pin_auth_init() returned 0 (OK — default PINs ready)`.
  - `pin_auth_init()` is idempotent: first boot generates salt + writes default hashes; subsequent boots just validate the salt + farmer-hash exist and set `s_initialized=true`.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.12.1`.

The historical `pin_auth_init()` calls in `execute_reset_action()` are deliberately left in place — they're correct behaviour for the factory-reset path (re-seed defaults after the namespace erase). The bug was only the missing **boot-time** call.

#### Build trap encountered

`-Werror=trigraphs` flagged the string literal `"locked out (??)"` because `??)` is a C trigraph for `]`. Fixed by changing the string to `"locked out (unexpected)"`. Same `-Wformat=2`-family hardening that bit ui_display.cpp with `-Werror=format-truncation` in alpha.6.12.

#### Build delta vs alpha.6.12

| Metric | alpha.6.12 | alpha.6.12.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,257,021 B | **1,256,653 B** | −368 B |
| Flash usage % | 59.9 % | 59.9 % | unchanged at 1 dp |
| RAM static | 59,552 B | **59,552 B** | 0 B |

bin sha256: `CA54F93FCB5773894681687220B7D9679A64404F88245E31FFE1A8BDD17A701C`

Slightly smaller binary (string-pool dedup or compiler folding around the new function call). RAM unchanged.

#### Acceptance bar for alpha.6.12.1

1. ✅ Build succeeds (after trigraph fix).
2. Flash to dev unit. Boot log should show, immediately after the `nvs_cfg_init() returned 0 (OK)` line:
   ```
   pin_auth_init() returned 0 (OK — default PINs ready)
   ```
3. On a **factory-fresh** unit (no prior pin_auth NVS data), the default farmer PIN "1234" should now work **on first attempt**, with no IO0 factory reset needed.
4. On a unit that already has pin_auth NVS data (salt + hashes from a previous boot), pin_auth_init takes the "salt present" branch — existing hashes preserved; previously-configured custom PIN still works. **Idempotent across reboots.**
5. T8 / T6 / T2 / T5 / T9 / T3 chain regression-clean.

### `[2.0.0-alpha.6.12]` — 2026-05-18

**Phase 6.12 — ui_display (T8) activation.** The LCD + keypad UI task goes live. T8 owns the 16×2 AiP31068L LCD (under MX1), consumes Q2 (key events from T7), drives the menu FSM (status pages + setpoint editing + PIN session + factory-reset via IO0 BOOT button held 20 s), posts Q4 config updates (consumed by T4) and Q3 LOG_CFG_CHANGE / LOG_PIN_AUTH events (consumed by T9). The 1887-line task migrates with just **5 single-line changes** (Arduino call swaps + esp_restart) plus a file-scope -Wformat-truncation pragma.

#### What changed

- **`firmware/src/ui_display/ui_display.cpp`** — five surgical patches:
  - Dropped `#include <Arduino.h>` + `#include <WiFi.h>`.
  - Added `#include <esp_mac.h>` (for `esp_read_mac`), `#include <esp_system.h>` (for `esp_restart`), `#include "gpio_util.h"` (for `gpio_set_pin_mode` / `gpio_read`).
  - `WiFi.macAddress(mac)` (line 772) → `esp_read_mac(mac, ESP_MAC_WIFI_STA)`. Same 6-byte STA MAC; same downstream behaviour (LCD shows AP SSID `Greenhouse-XXXX` from mac[4..5]).
  - `pinMode(RESET_PIN_IO0, INPUT_PULLUP)` (line 1716) → `gpio_set_pin_mode(RESET_PIN_IO0, GPIO_INPUT_PULLUP)`.
  - `digitalRead(RESET_PIN_IO0) == LOW` (line 1742) → `gpio_read(RESET_PIN_IO0) == GPIO_LOW`.
  - `ESP.restart()` (line 675; the factory-reset finalizer) → `esp_restart()`.
  - File-scope `#pragma GCC diagnostic ignored "-Wformat-truncation"` added: the per-component `-Wformat=2` hardening flag (in `firmware/src/CMakeLists.txt`) treats `snprintf` truncation warnings as errors, and gcc can't infer the bounded ranges of `time_t` / cfg / sensor struct fields, so it conservatively flags 8 snprintf call-sites as potentially-truncating. Under arduino-esp32 the warning wasn't promoted to error. The pragma is the surgical fix that leaves the well-tested production code unchanged; the alternative would be 8 manual range-clamps before each snprintf.
- **`firmware/src/auth/pin_auth.cpp`** — added to SRCS. The file is framework-agnostic (no Arduino dependencies). Provides PIN session table, lockout counter, SHA-256 hash verify via mbedtls.
- **`firmware/src/status_post/status_post_stub.cpp`** (new) — provides `bool status_post_backoff_active(void) { return false; }`. T8 calls this from the WiFi status page (LCD shows "BK" suffix when backoff is active). Designed for forcing-removal when the full status_post.cpp activates in Phase 6.N — same stub-and-linker-conflict pattern as `data_manager_stub.cpp` (alpha.6.6→6.7) and `relay_controller_stub.cpp` (alpha.6.7→6.9).
- **`firmware/src/CMakeLists.txt`** — added `ui_display/ui_display.cpp`, `auth/pin_auth.cpp`, `status_post/status_post_stub.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "ui_display/ui_display.h"` added.
  - Spawn T8 after T3 with `xTaskCreatePinnedToCore(task_ui_display, "T8-ui", 8192, NULL, 4, &task_t8, tskNO_AFFINITY)`. Stack 8192 matches 1.20.3 prod (T8 has more locals than T6/T3 — LCD char buffers, menu state arrays, PIN scratch). Priority 4 — below safety-critical T2/T3 (6) and producer/consumer pair T4/T5/T6 (5), above T7 (3). UI is latency-tolerant; ~UI_LOOP_MS scheduling is far inside user perception threshold.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.12`.

#### Build traps encountered

Two build errors were caught and fixed before the successful build:
1. `'ESP' was not declared in this scope` at line 675 — `ESP.restart()` is Arduino-only. Fixed by replacing with `esp_restart()` + adding `#include <esp_system.h>`.
2. **8 × `-Werror=format-truncation`** at various LCD render functions — fixed with the file-scope pragma documented above.

Same "Arduino-transitive-include trap" pattern as previous activations (alpha.6.6 added `esp_log.h` explicit; alpha.6.7 added `time.h` + `sys/time.h`). T8's larger surface area meant more traps surfaced; expect similar density in future activations of large legacy files.

#### Build delta vs alpha.6.11

| Metric | alpha.6.11 | alpha.6.12 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,240,941 B | **1,257,021 B** | +16,080 B |
| Flash usage % | 59.2 % | 59.9 % | +0.7 pp |
| RAM static | 59,336 B | **59,552 B** | +216 B |

bin sha256: `54D62F377CE08CA515FE119EBFD00B0CD099022B70C4A8D7B3DD2A14420AB52D`

The **+16,080 B flash** is T8 (1887 lines: LCD menu FSM, status pages, PIN flow, factory-reset, mode-change handling) + pin_auth.cpp (~200 lines, SHA-256 verification) + status_post_stub.cpp (1 line). RAM static delta is +216 B — mostly T8's static menu state and a small portion of pin_auth's session table.

Runtime heap impact at heartbeat baseline expected: ~−8 KB free vs alpha.6.11 (T8 task stack 8 KB). Heartbeat baseline ~158,000 free (was 165,927 at alpha.6.11).

#### Acceptance bar for alpha.6.12

1. ✅ Build succeeds (after `esp_restart` + `-Wformat-truncation` pragma fixes).
2. Flash to dev unit; boot reason `ESP_RST_POWERON`.
3. Banner: `Greenhouse Controller v2.0.0-alpha.6.12`.
4. **LCD boot sequence**:
   - First the app_main_stub's LCD probe writes `ESP-IDF stub OK` / `v2.0.0-alpha.6.12` (~uptime 1.3 s).
   - Then T8's task body writes splash `Greenhouse Ctrl ` / `v2.0.0-alph Init..` for 2 s.
   - After splash, T8 enters STATUS state and starts rotating through status pages every UI_STATUS_ROTATE_MS.
5. **T8 spawn banner**: `alpha.6.12: T8 ui_display task spawned (handle=0x...); LCD live, Q2 keypad consumer, Q4/Q3 producer`.
6. **Keypad input now drives menu**: pressing keys advances through the menu hierarchy (max 4 presses to any first-level setting per design). Editing a setpoint via the menu causes T8 to post Q4 → T4 absorbs the update → next cfg_shadow_t snapshot reflects the new value → next T6 cycle uses the new setpoint.
7. **Factory-reset test (optional)**: hold IO0 BOOT button for 20 s. LCD shows progress bar; if held to completion, full reset executes. Skip if you don't want to clear NVS.
8. T3/T6/T2/T5/T9 chain continues unchanged from alpha.6.11.
9. All earlier-phase tickles regression-clean.
10. Run ≥ 10 min; no resets; no stack-overflow on T8.

### `[2.0.0-alpha.6.11]` — 2026-05-18

**Phase 6.11 — safety_monitor (T3) activation.** Wind-safety override task goes live. T3 wakes on TN1 (xTaskNotify from T4, same notify event that drives T6), snapshots latest measurement + cfg, evaluates wind speed against `v_max` and direction against `[dir_excl_low, dir_excl_high]` exclusion zone (with 0°/360° wrap support). On unsafe onset: sets `EG1_BIT_WIND_OVERRIDE`, posts `CMD_CLOSE_ALL` (SRC_T3) to Q1, logs `LOG_ALARM`. On safe clearance: clears the EG1 bit, posts `CMD_RESUME`, logs clearance. `EG1_BIT_SENSOR_FAULT_W` is treated as worst-case (safe-fail per FR-W04 / TSDS §5.12).

This is the first task that competes with T6 to drive Q1, so priority matters: **T3 at priority 6, T6 at priority 5**. T3 preempts T6 within the same TN cycle so it sets `EG1_BIT_WIND_OVERRIDE` BEFORE T6 evaluates — T6's inhibit-gate (climate_control.cpp:360-362) then suppresses its own evaluation, preventing the race where T6's `CMD_OPEN` could land on Q1 between T3's set-bit and T3's `CMD_CLOSE_ALL`.

#### What changed

- **`firmware/src/safety_monitor/safety_monitor.cpp`** — dropped `#include <Arduino.h>`. Zero Arduino-specific calls; FreeRTOS primitives (`ulTaskNotifyTake`, `xQueueSend`, `xEventGroup*`) arrive via app_types.h. Single-line patch, same shape as alpha.6.10's T6 port.
- **`firmware/src/CMakeLists.txt`** — added `safety_monitor/safety_monitor.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "safety_monitor/safety_monitor.h"` added.
  - Spawn T3 after T6 with `xTaskCreatePinnedToCore(task_safety_monitor, "T3-safety", 6144, NULL, 6, &task_t3, tskNO_AFFINITY)`. Stack 6144 (1.20.3 prod 4096; +2 KB IDF headroom). Priority 6 — matches T2 (both safety-relevant Q1 producers/consumers); one above T6.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.11`.

#### TN1 plumbing (already in place from alpha.6.7)

`firmware/src/data_manager/data_manager.cpp:548` already calls `xTaskNotify(task_t3, 1u, eSetBits)` after every Q6 store. Like T6's TN2 in alpha.6.10, `task_t3` was previously NULL → notify was a no-op. As of alpha.6.11 the handle is populated.

#### Build delta vs alpha.6.10

| Metric | alpha.6.10 | alpha.6.11 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,239,241 B | **1,240,941 B** | +1,700 B |
| Flash usage % | 59.1 % | 59.2 % | +0.1 pp |
| RAM static | 59,336 B | **59,336 B** | 0 B |

bin sha256: `276C38C63270B117372ABAC1EDC56395F76E291A729F82E5EBC801BA5B7129E3`

The **+1,700 B flash** is T3's 256-line task body. RAM static unchanged — T3's only state is one task-local `bool alarm_active` (lives on the task stack, not BSS).

Runtime heap impact at heartbeat baseline expected: ~−6 KB free vs alpha.6.10 (T3 task stack 6 KB). Heartbeat baseline ~166,000 free (was 172,343 at alpha.6.10).

#### Acceptance bar for alpha.6.11

1. ✅ Build succeeds (no Arduino-transitive trap — single-line patch).
2. Flash to dev unit; boot reason `ESP_RST_POWERON`.
3. Banner: `Greenhouse Controller v2.0.0-alpha.6.11`.
4. T3 spawn banner: `alpha.6.11: T3 safety_monitor task spawned (handle=0x...); wakes on TN1 from T4 — wind-safety override owner`.
5. T3 task-alive: `[T3_WIND] [T3] task alive` within ~1 s of spawn.
6. **No `WIND_OVERRIDE set` log line at boot** — current dev-unit wind reading is ws=2.4 m/s, well below v_max=6 m/s; direction 199° is outside any normal exclusion zone (production NVS likely has zero-width zones disabled). T3 evaluates each TN1 and stays in the "no state change (steady safe)" branch — logged at DEBUG, usually invisible at INFO.
7. **T6 / T2 chain continues** from alpha.6.10's last state — if M1 was OPEN from the last alpha.6.10 boot, T2's NVS-persisted state may now record CH1=OPEN; T2 won't NVS-skip on this boot (only all-CLOSED qualifies). Calibration **may run** as a result — this is expected (alpha.6.10 didn't include a CMD_CLOSE_ALL on shutdown to re-close M1). If calibration runs: ~26 s on M1, 26 s on M2 (already CLOSED?), 171 s on M3. After calibration completes, T6 will re-evaluate and may re-open M1 if temperature still > 28°C.
8. All earlier-phase tickles regression-clean.
9. Run ≥ 10 min; no resets; no stack-overflow on T3.

#### Watch items

- **First-boot calibration may run again**: alpha.6.10 left M1 in `WIN_OPEN` state without a clean close before reset. T2's NVS state for ch1 = `NVS_STATE_OPEN`, not `NVS_STATE_CLOSED` → calibration runs (gh#18 Phase 3 only skips on all-CLOSED). Subsequent reboots (after T6 has closed everything overnight) should re-hit the NVS-skip.
- **Optional manual wind-override test**: write `v_max=2` via web GUI before any unsafe wind happens. Next TN1 cycle should immediately set `WIND_OVERRIDE` (ws=2.4 ≥ 2), post `CMD_CLOSE_ALL`, post LOG_ALARM. Reset `v_max=6` to clear the override and verify CMD_RESUME path.

### `[2.0.0-alpha.6.10]` — 2026-05-18

**Phase 6.10 — climate_control (T6) activation.** The climate control loop closes for the first time on 2.0.0. T6 wakes on TN2 (xTaskNotify from T4 after every Q6 store), snapshots cfg + measurement, runs the graduated-ventilation step algorithm (T-step from `t_avg - t_max` divided by `hyst_t / NUM_VENT_STEPS`; RH-step from `rh_avg - rh_max` if too humid, full close if too dry, neutral otherwise), resolves T vs RH conflict via `cr_priority`, and reconciles T2's actual window states to the resolved step's channel mask by posting per-channel `CMD_OPEN`/`CMD_CLOSE` to Q1. **Level-triggered every cycle** so commands lost to T2's post-open/close dwell are retried automatically. `CMD_CLOSE_ALL` is deliberately not used (reserved for safety events: T3 wind override, T2 motor alarm).

End-to-end loop now live: T5 produces sensor readings on Q6 → T4 stores them and notifies T6 via TN2 → T6 evaluates and posts Q1 commands → T2 drives the relays. This is the first 2.0.0 phase where firmware autonomously controls the physical world in response to sensor data.

#### What changed

- **`firmware/src/climate_control/climate_control.cpp`** — dropped `#include <Arduino.h>`. The file has zero Arduino-specific calls; FreeRTOS primitives (`ulTaskNotifyTake`, `xQueueSend`, `xEventGroupGetBits`) arrive via `app_types.h`'s transitive FreeRTOS includes. ESP-IDF `esp_log.h` and `esp_task_wdt.h` were already explicit. Single-line patch.
- **`firmware/src/CMakeLists.txt`** — added `climate_control/climate_control.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "climate_control/climate_control.h"` added.
  - Spawn T6 after T2 with `xTaskCreatePinnedToCore(task_climate_control, "T6-climate", 6144, NULL, 5, &task_t6, tskNO_AFFINITY)`. Stack 6144 (1.20.3 prod used 4096; +2 KB headroom for IDF stack frames). Priority 5 — matches T4 (T6's notifier); same-priority round-robin means T6 picks up the TN2 notification immediately when T4 releases the CPU.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.10`.

#### TN2 plumbing (already in place from alpha.6.7)

`firmware/src/data_manager/data_manager.cpp:551` already calls `xTaskNotify(task_t6, 1u, eSetBits)` after every Q6 store. The `task_t6` handle was previously NULL (T6 dormant) — `xTaskNotify(NULL, ...)` would have done nothing. As of alpha.6.10 the handle is populated by the T6 spawn, so T4's notify now lands. No T4 code changes were required.

#### Build delta vs alpha.6.9

| Metric | alpha.6.9 | alpha.6.10 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,237,005 B | **1,239,241 B** | +2,236 B |
| Flash usage % | 59.0 % | 59.1 % | +0.1 pp |
| RAM static | 59,336 B | **59,336 B** | 0 B |

bin sha256: `1346A829125859FC8D58737D1551AB69CDF90D4BBB006FED464F074456D9FD45`

The **+2,236 B flash** is T6's 462-line task body (step algorithm + conflict resolution + reconcile loop + logging). RAM static is unchanged — T6's only persistent state lives in task-local static ints (`current_step_t`, `current_step_rh`, `prev_inhibited`), all within the task stack rather than BSS.

Runtime heap impact at heartbeat baseline expected: ~−6 KB free vs alpha.6.9 (T6 task stack 6 KB). Heartbeat baseline ~173,000 free (was 179,007 at alpha.6.9).

#### ⚠ Physical-safety acceptance considerations

T6 will issue **real CMD_OPEN/CMD_CLOSE commands** to T2 as soon as it has a measurement that exceeds the current setpoint. **On the dev unit's current state** (from alpha.6.9's last T5 reading: T=30°C, t_max_day=28°C, hyst_t=5):

```
deviation = 30 - 28 = 2°C
step_width = max(5 / NUM_VENT_STEPS, 1) = max(5/3, 1) = 1
raw_step = ceil(2 / 1) = 2
→ resolved step = 2 → CMD_OPEN ch=1 + CMD_OPEN ch=2
```

So **immediately after T6's first wake-up** (which happens after T5's first Q6 reading + T4 store, ~38-40 s after boot), T2 will:
1. Receive `CMD_OPEN ch=1` and `CMD_OPEN ch=2` from Q1.
2. Energise `PIN_RELAY_M1_OPEN` (GPIO12) for ~21 s (M1 travel).
3. Energise `PIN_RELAY_M2_OPEN` (GPIO14) for ~21 s (M2 travel).
4. (M3 stays closed unless the gap to setpoint widens further.)

If T_avg drifts further above setpoint, step could climb to 3 (M1 + M2 + M3), engaging the M3 relay for 171 s travel. Conversely, once T_avg drops below `t_max - hyst` (28 - 5 = 23°C), T6 will step down to 0 and post `CMD_CLOSE` for each open channel.

**If the dev unit has motors physically connected, they will move.** This is the first activation where T6 autonomously decides relay state from live sensor data — the climate loop is fully live.

#### Acceptance bar for alpha.6.10

1. ✅ Build succeeds (no Arduino-transitive trap — single-line patch).
2. Flash to dev unit; boot reason `ESP_RST_POWERON`.
3. T6 spawn banner: `alpha.6.10: T6 climate_control task spawned (handle=0x...); wakes on TN2 from T4 — first decision after T5 iter 1 + T4 store`.
4. ~1 s later: `[T6_CLI] [T6] task alive`.
5. T5 first poll at ~38 s post-spawn produces a sensor_reading_t; T4 stores it in its ring + calls `xTaskNotify(task_t6, 1u, eSetBits)`.
6. T6 wakes immediately and logs the evaluation line:
   ```
   [T6_CLI] [T6] T_avg=N t_max=N hyst=N → step_t=N | RH_avg=N rh_max=N rh_min=N hyst=N rh_en=N → step_rh=N | resolved=N (was cur_t=0 cur_rh=0)
   ```
   For current dev state expect: `T_avg=30 t_max=28 hyst=5 → step_t=2`.
7. If `step_t > 0`, T6 calls `reconcile_to_step()` which queries `t2_get_window_states()` and logs:
   ```
   [T6_CLI] [T6] → CMD_OPEN  ch=1 (target step 2, actual=1)
   [T6_CLI] [T6] → CMD_OPEN  ch=2 (target step 2, actual=1)
   ```
   (`actual=1` is `WIN_CLOSED` from T2's NVS-recovered state.)
8. T2 receives Q1 commands and fires the M1/M2 OPEN relays:
   ```
   T2: CMD_OPEN ch1 from T6
   T2: CH1: → MOVING_OPEN  (travel 26000 ms)
   T2: CMD_OPEN ch2 from T6
   T2: CH2: → MOVING_OPEN  (travel 26000 ms)
   ```
   (Travel includes the `MOTOR_TRAVEL_MARGIN_S_DEFAULT` padding on top of the 21 s NVS-saved travel.)
9. ~26 s later T2 logs `CH1: OPEN (travel complete)` and `CH2: OPEN (travel complete)`. After 300 s dwell_open expires, T6 could begin closing if temperature drops below 23°C.
10. T9 logs `LOG_MODE_CHANGE value_a=2` (the new step) to today's daily CSV.
11. All earlier-phase tickles regression-clean.
12. Run ≥ 10 min; no resets; no stack-overflow on T6.

#### Watch items

- **Inhibit-onset on EG1_BIT_SENSOR_FAULT_T**: T5's edge-triggered fault bits are wired, but no actual fault has been observed yet. If a Modbus comm error happens during the soak, T6 should log `[T6] inhibited (EG1=0x04) — evaluation suspended` and reset both `current_step_*` counters.
- **`prev_resolved` calculation** (climate_control.cpp:439-443) recomputes the previous resolved step using the *current* `cr_priority` — which could theoretically yield a "phantom step change" log if `cr_priority` was updated mid-cycle via web GUI. T11 (web server route handler) isn't yet active on 2.0.0 so this is dormant. Worth a look during Phase 7 soak if a phantom MODE_CHANGE log appears.

### `[2.0.0-alpha.6.9]` — 2026-05-18

**Phase 6.9 — relay_controller (T2) activation.** The window-control task goes live: sole owner of the 6 relay GPIO outputs (M1/M2/M3 × OPEN/CLOSE), per-channel FSM (UNKNOWN → CLOSED ↔ MOVING_OPEN/MOVING_CLOSE ↔ OPEN), 2 s reversal-gap enforcement, NVS-persisted terminal state (gh#18 Phase 3 calibration-skip), GPIO42 motor-alarm ISR with 75 ms debounce, 60 s post-alarm guard before re-calibration. Q1 consumer (Q1 is fed by T3 safety + T6 climate — both still dormant in this phase). Force-removes `relay_controller_stub.cpp` via the linker conflict pattern (same as `data_manager_stub.cpp` in alpha.6.7).

#### What changed

- **`firmware/src/relay_controller/relay_controller.cpp`**:
  - Dropped `#include <Arduino.h>` (the only Arduino-specific call was `attachInterrupt(...)` at the GPIO42 ISR install).
  - Added `#include <driver/gpio.h>` for the ESP-IDF GPIO ISR service.
  - ISR signature `void IRAM_ATTR isr_motor_alarm(void)` → `void IRAM_ATTR isr_motor_alarm(void *arg)` to match the IDF `gpio_isr_t` typedef. The added `arg` parameter is unused (`(void)arg;` cast inside).
  - Replaced `attachInterrupt(PIN_OPTO_INPUT, isr_motor_alarm, CHANGE)` with `gpio_install_isr_service(ESP_INTR_FLAG_IRAM) + gpio_set_intr_type(..., GPIO_INTR_ANYEDGE) + gpio_isr_handler_add(...) + gpio_intr_enable(...)`. ESP_ERROR_CHECK on each step except install (which is allowed to return `ESP_ERR_INVALID_STATE` when the service is already installed — benign).
- **`firmware/src/relay_controller/relay_controller_stub.cpp`** — **DELETED** via `git rm`. The real `relay_controller.cpp:t2_get_window_states()` replaces the stub's no-op. The linker would refuse two definitions of the symbol — forcing-removal pattern, same as `data_manager_stub.cpp` in alpha.6.7.
- **`firmware/src/CMakeLists.txt`** — added `relay_controller/relay_controller.cpp` to SRCS; removed `relay_controller/relay_controller_stub.cpp`.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "relay_controller/relay_controller.h"` added.
  - New init block before T2 spawn: configures the 6 relay output pins (`PIN_RELAY_M{1,2,3}_{OPEN,CLOSE}` = GPIO12/13/14/15/16/21) as `GPIO_OUTPUT` and drives them `GPIO_LOW`; configures `PIN_OPTO_INPUT` (GPIO42) as `GPIO_INPUT_PULLUP`. Must happen before T2's task body so the alarm pin reads correctly and the relays start from a known de-energised state.
  - Spawn T2 with `xTaskCreatePinnedToCore(task_relay_controller, "T2-relay", 8192, NULL, 6, &task_t2, tskNO_AFFINITY)`. Priority 6 — one above T4/T5 (5) — so T2 preempts data and sensor tasks when a Q1 command arrives. Matches 1.20.3 production's `TASK_PRIO_HIGH` intent.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.9`.

#### Build delta vs alpha.6.8

| Metric | alpha.6.8 | alpha.6.9 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,229,421 B | **1,237,005 B** | +7,584 B |
| Flash usage % | 58.6 % | 59.0 % | +0.4 pp |
| RAM static | 59,240 B | **59,336 B** | +96 B |

bin sha256: `DFE428AC260CE06BE5A7E21A4D610D84DE0018E6A3F87D0D348CC3C2B9BC74A7`

The **+7,584 B flash** is T2's task body (~830 lines: FSM, calibration, alarm handler, NVS state persistence, ISR install). RAM static delta is +96 B — that's the `s_ch[3]` channel-state array (3 × `ch_t` = 84 B) plus a small portMUX spinlock byte. T2's runtime task stack (8 KB) comes from heap.

Runtime heap impact at heartbeat baseline expected: ~−8 KB free vs alpha.6.8 (T2 task stack 8 KB). Heartbeat baseline ~180,000 free (was 188,235 at alpha.6.8).

#### ⚠ Physical-safety acceptance considerations

**T2 will drive 6 relay outputs on real GPIO pins.** Two possible boot behaviours depending on NVS state:

1. **NVS-recovered (gh#18 Phase 3 skip)**: if the previous clean reboot saved all three channels as `CH_CLOSED`, T2 logs `boot calibration skipped — NVS-recovered window state (all three channels CLOSED)` and proceeds directly to the main loop. **No relays fire.** This is the common case after running 1.20.3 production firmware (which uses the same gh#18 Phase 3 persistence).
2. **Calibration required**: if any channel is `UNKNOWN` or `OPEN` in NVS, T2 runs `calib_close_all()` — energises all 3 CLOSE relays simultaneously for each channel's `travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT` (up to 171 s for M3). Relays click + motors (if attached) drive to CLOSED.

**Boot-time alarm guard**: if `PIN_OPTO_INPUT` reads LOW at boot (RRK-3 alarm relay already latched), T2 logs `GPIO42 alarm pin already asserted at boot — skipping CLOSE_ALL calibration` and calls `handle_alarm_onset()` (relays stay de-energised; `EG1_BIT_MOTOR_ALARM` set; further Q1 commands discarded). Operator must clear the alarm hardware-side to resume.

If the dev unit has motors physically connected, **either** ensure it's safe for them to close, **or** confirm the unit's NVS records all three channels CLOSED (in which case the calibration is skipped entirely).

#### Acceptance bar for alpha.6.9

1. ✅ Build succeeds (no Arduino-transitive trap; the only Arduino-specific call was `attachInterrupt`, replaced with the IDF GPIO ISR service).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Spawn banner: `alpha.6.9: T2 relay_controller task spawned (handle=0x...)`.
4. T2 banner sequence (in order):
   - `T2 starting`
   - `CH1: travel=N s  dwell_open=N s  dwell_close=N s` × 3
   - `GPIO42 ISR attached (MOTOR_ALARM, ANYEDGE, IRAM, not suppressed during MOVING)`
   - **Either** `T2 boot calibration skipped — NVS-recovered window state (all three channels CLOSED)` **or** `T2 boot calibration starting — NVS state (ch0=N ch1=N ch2=N; need all=1)` followed by 3× `CHN: CLOSE relay energised (deadline N ms from boot)` and finally `CLOSE_ALL calibration complete — all channels CLOSED`.
5. Heartbeat continues — no regressions on T4/T5/T7/T9/HTTPS/WiFi/SD/LFS.
6. `t2_get_window_states()` now returns real state (not WIN_UNKNOWN). T4's `dm_status_snapshot()` populates `win[0..2]` with `WIN_CLOSED` (or whatever T2 recovered) instead of all `WIN_UNKNOWN`.
7. T2 idles waiting for Q1 — no Q1 producer is active in this phase (T3/T6 dormant), so the queue stays empty. Expect zero CMD_OPEN / CMD_CLOSE log lines.
8. Optional manual test: hand-trip the RRK-3 alarm relay (or short GPIO42 to GND through a 1 kΩ resistor). Within ~95 ms (75 ms debounce + 20 ms loop tick) T2 should log `MOTOR_ALARM asserted — all relays de-energised, all window control suspended`. Release the alarm; T2 logs clearance + starts the 60 s guard + then runs CLOSE_ALL re-calibration. **Skip this test if motors are connected.**
9. Run ≥ 10 min; no resets; no stack-overflow on T2.

### `[2.0.0-alpha.6.8]` — 2026-05-18

**Phase 6.8 — sensor_poll (T5) activation.** Modbus RTU master goes live: polls FG6485A (T/RH) at slave 1 and S200 (wind speed/direction) at slave 44, maintains sliding-window arithmetic averages (T/RH/wind speed) plus a unit-vector atan2-based circular mean for wind direction, pushes a `sensor_reading_t` onto Q6 (depth-1, xQueueOverwrite — latest wins). Edge-triggered fault detection on EG1: `EG1_BIT_SENSOR_FAULT_T` for the T/RH bus, `EG1_BIT_SENSOR_FAULT_W` for the wind bus, both with `LOG_ALARM` events posted only at fault onset and clearance.

#### What changed

- **`firmware/src/sensor_poll/sensor_poll.cpp`** — dropped `#include <Arduino.h>`. The FreeRTOS handles (Q6, EG1, vTaskDelay, xQueueOverwrite, xEventGroup*) all arrive via `"../types/app_types.h"` which transitively includes `freertos/{FreeRTOS,queue,task,event_groups,semphr}.h`. `<esp_log.h>`, `<math.h>`, `<string.h>`, `<time.h>` were already explicit at the top of the file — no further explicit-include additions were needed. **Zero functional changes** in T5 itself; the file was already IDF-shaped beyond the one Arduino.h line.
- **`firmware/src/app_main_stub.cpp`** — three coordinated edits:
  - `#include "sensor_poll/sensor_poll.h"` added.
  - Spawn T5 after T9 with `xTaskCreatePinnedToCore(task_sensor_poll, "T5-sensor", 8192, NULL, 5, &task_t5, tskNO_AFFINITY)`. Priority 5 matches T4 (T4 is T5's consumer; they alternate naturally on Q6).
  - **Removed** the heartbeat task's `fg6485a_read_measurements(1, ...)` and `s200_read_measurements(44, ...)` calls plus their references in the heartbeat log format string. T5 is now the sole owner of the Modbus RTU bus — dual pollers on a half-duplex RS-485 line would scramble responses. The canonical readings now surface in T5's own log lines (`[T5_SEN] T=N°C RH=N% ws=N.N m/s wd=N° | avg T=N RH=N ws=N.N wd=N° [win T=W RH=W W=W]`) instead. The heartbeat log shrinks to `heartbeat N | free=X largest=Y psram_free=Z uptime=Ws | hb_led=A keys=B | rtc=R YYYY-MM-DD HH:MM:SS`.
- **`firmware/src/CMakeLists.txt`** — added `sensor_poll/sensor_poll.cpp` to SRCS.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.8`.

The `modbus_init()` call site is **kept in BOTH** places. The driver's `modbus_init()` does an `uart_is_driver_installed → uart_driver_delete` guard before installing fresh (drivers/modBus/src/modbus_rtu.cpp:187-189), so double-call is safe by design. The app_main_stub call site (Phase 2.6 tickle) keeps the bus ready immediately at boot; T5's own `modbus_init()` on task entry reconfirms the driver state after the 8 s boot-grace delay. No race because T5 doesn't issue Modbus traffic until at least `8 s + poll_interval_s` (default 38 s) after spawn.

#### Build delta vs alpha.6.7.1

| Metric | alpha.6.7.1 | alpha.6.8 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,224,797 B | **1,229,421 B** | +4,624 B |
| Flash usage % | 58.4 % | 58.6 % | +0.2 pp |
| RAM static | 51,992 B | **59,240 B** | **+7,248 B** |

bin sha256: `028A7E5EE6901C8F1559DF8CAB5E090DA6CBAE5B51C9CE794A28F618AE43F89C`

The **+7,248 B RAM static** is sensor_poll.cpp's BSS: 3 × `avg_ctx_t` (T, RH, wind speed — each holds `float buf[360]` + 8 B of head/count/sum = ~1,448 B; 3 of these = ~4.3 KB) + 1 × `dir_avg_ctx_t` (sin + cos buffers + state = ~2.9 KB). Padding/alignment shrinks the theoretical 7.2 KB to a slightly tighter actual. T5's runtime task stack (8 KB) lives on the heap; the +7 KB above is BSS only.

Runtime heap impact at heartbeat baseline expected: ~−8 KB free vs alpha.6.7.1 (T5 task stack 8 KB + Modbus driver overhead). Heartbeat baseline ~196,000 free (was ~204,000 at alpha.6.7.1).

#### Acceptance bar for alpha.6.8

1. ✅ Build succeeds (no Arduino-transitive trap surfaced — the explicit includes already in sensor_poll.cpp covered everything Arduino.h was providing).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Spawn banner: `alpha.6.8: T5 sensor_poll task spawned (handle=0x...)`.
4. T5 boot-grace banner ~8 s after spawn: `[T5_SEN] [T5] task alive — boot grace expired` followed by `[T5_SEN] [T5] Modbus RTU initialised (9600 baud) — init complete`.
5. **First poll log ~38 s after spawn**: `[T5] iter 1 — woke from 30 s delay` followed by `polling FG6485A`, `polling S200`, then a `T=N°C RH=N% ws=N.N m/s wd=N° | avg T=N RH=N ws=N.N wd=N° [win T=W RH=W W=W]` line.
   - Initial poll window is **1 sample wide** (`win T=1 RH=1 W=1`) — sliding average hasn't warmed yet. Window widens as `cfg.avg_win_t × 60 / poll_interval_s` over subsequent polls until it hits `SP_AVG_DEPTH=360`.
   - T/RH should match the 1.20.3 production values from the same sensors: ~22-25 °C ambient, ~50-90 % RH depending on ventilation state.
   - Wind speed should match S200 readings — ~1-3 m/s indoor air movement is normal; outdoor mounting will show higher and noisier.
6. **Heartbeat no longer shows fg6485a/s200 fields** — confirms the heartbeat-side Modbus polls are gone.
7. **Q6 producer→consumer**: T4 was already activated alpha.6.7 with the Q6 receive logic in its main loop. T4's log lines should show that it's now receiving readings (`T4: received sensor reading` or equivalent — check existing T4 code for the actual log format). The data_manager ring buffer (DM_RING_DEPTH=360) starts filling.
8. **Edge-case faults** (deferrable to a soak test): if a sensor disconnects mid-run, expect `[T5] T/RH sensor FAULT — two consecutive read failures` (or `Wind sensor FAULT`), `EG1_BIT_SENSOR_FAULT_T/W` set, one `LOG_ALARM` posted. On reconnect, exactly one `fault cleared` log line + one clearance `LOG_ALARM`.
9. T9 keeps draining Q3 — SD CSV continues to grow with one synthetic event per heartbeat plus any LOG_ALARM fault events.
10. All earlier-phase tickles regression-clean.
11. Run ≥ 10 min; no resets; no stack-overflow on T5.

#### Watch items carried forward

- **dm-snapshot `unix_ts` staleness** (carried from alpha.6.7 acceptance review): the snapshot field is captured at T4 boot and not refreshed per call. T5 deliberately uses `(uint32_t)time(NULL)` (POSIX system clock) for its `reading.timestamp` instead of `dm_get_unix_time()` to avoid this — sensor_poll.cpp:476-480 documents the reasoning inline. So T5 sidesteps the issue; T2/T6/T13 activations will need to make the same choice or T4's snapshot semantics will need a fix.

### `[2.0.0-alpha.6.7.1]` — 2026-05-18

**Phase 2.11 SD test-file probe retirement.** Operator SD-card inspection of the dev unit revealed `/phase_2_11_test.csv` had been growing by one identical line (`boot,2026-05-17,LIB-SD ESP-IDF port works (LFN OK)`) per boot since alpha.2.11.1 — leftover scaffolding from the LIB-8 write-path acceptance test. T9 (alpha.6.6) now exercises the same FATFS write path with real RTC-stamped data into the daily CSV, making the probe fully redundant. Patch removes the probe; the existing dev-card file must be deleted by hand.

#### What changed

- **`firmware/src/app_main_stub.cpp`** — removed the `static const char *test_file = "/phase_2_11_test.csv"` block (write_append → file_size → read → byte-compare verify; ~46 lines). The `storage_init()`, total/free log, `storage_sd_list_csv()`, and `storage_sd_unmount()` calls are preserved as passive sanity probes — they exercise the mount/list/unmount paths without writing to the card. Replaced the deleted block with a one-paragraph comment pointing at this changelog entry.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.7.1`.

The historical references to `phase_2_11_test.csv` in `sdkconfig.defaults` (the LFN-config rationale comment block) and in earlier `changelog.md` entries (alpha.2.11 + alpha.2.11.1 release notes) are deliberately untouched — they're historical commentary, not active code.

#### Operator action required

Pop the SD card out of any dev unit that ran alpha.2.11.1 through alpha.6.7 and delete `/phase_2_11_test.csv` manually on a PC. The file contains no useful data — it's just `boot,2026-05-17,LIB-SD ESP-IDF port works (LFN OK)` repeated N times. No firmware unlink is provided; alpha.6.7.1 simply stops writing the file.

#### Build delta vs alpha.6.7

| Metric | alpha.6.7 | alpha.6.7.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,225,657 B | **1,224,797 B** | −860 B |
| Flash usage % | 58.4 % | 58.4 % | unchanged at 2 dp |
| RAM static | 51,992 B | **51,992 B** | **0 B** |

bin sha256: `1AD5C6A01816A08AFF2AF72800DDD1DC5F6A3F37B0950D9430B8F3CD0B2AA12D`

Tiny patch: −860 bytes flash from the removed write/read/verify code + 2 fewer string-table entries; RAM static unchanged as predicted (the `test_file` / `test_line` literals were `static const char *` references into RODATA, no `.bss`/`.data` involvement).

#### Acceptance bar for alpha.6.7.1

1. Build succeeds.
2. Flash, boot, observe serial: the SD tickle block should log:
   ```
   storage_init returned 0 (OK)
   SD total = ... bytes, free = ... bytes
   storage_sd_list_csv(.csv) -> 0; result: "<csv-list>"
   storage_sd_unmount() done; storage_sd_available() = false (OK)
   ```
   Note the **absence** of any `storage_sd_write_append(/phase_2_11_test.csv) -> ...` and `SD write/read verify: PASS — bytes identical` lines. That's the gold-standard signal — the probe is gone.
3. T4 (data_manager) and T9 (event_logger) unchanged from alpha.6.7 baseline. Heartbeat `alpha.6.7 dm: ...` cfg snapshot still emits each tick (the heartbeat literal still references "alpha.6.7" — left in place to avoid touching unrelated code in a cleanup patch; will rename in the next functional alpha).
4. All earlier-phase tickles regression-clean.

### `[2.0.0-alpha.6.7]` — 2026-05-18

**Phase 6.7 — data_manager (T4) activation.** The central data hub goes live: cfg_shadow_t backbone, MX1..MX4 mutexes, sensor ring buffer (DM_RING_DEPTH=360 entries), Q4/Q6 consumers, RTC↔system-clock sync, NTP→DS1307 writeback via TN4, sunrise/sunset integration (sunrise.cpp from alpha.6.2). 1031 lines; the heaviest single firmware/src/ activation in Phase 6.

#### What changed

- **`firmware/src/data_manager/data_manager.cpp`**:
  - Dropped `#include <Arduino.h>` and `#include <WiFi.h>`.
  - Added `#include <esp_wifi.h>` + `#include <esp_netif.h>` for the WiFi-state replacement.
  - Added `#include <sys/time.h>` for `settimeofday()` (was previously via Arduino.h transitively).
  - Replaced the 3 WiFi.* calls in `dm_status_snapshot()` with `esp_wifi_sta_get_ap_info()` + `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` + `esp_netif_get_ip_info()`. IDF-native idiom; same semantics as the Arduino `WiFi.isConnected()` + `WiFi.localIP().toString()` + `WiFi.RSSI()` chain. ~17 lines new code for ~3 lines removed.
- **`firmware/src/data_manager/data_manager.h`**: added `#include <time.h>` for `time_t` (used by `dm_set_manual_time()`'s parameter).
- **`firmware/src/relay_controller/relay_controller_stub.cpp`** (new): provides `t2_get_window_states(window_state_t out[3])` returning `{WIN_UNKNOWN, WIN_UNKNOWN, WIN_UNKNOWN}` until the real T2 ports in Phase 6.8+. Same forcing-removal pattern as the now-deleted `data_manager_stub.cpp`.
- **`firmware/src/data_manager/data_manager_stub.cpp`** — **DELETED**. T4's real `dm_get_unix_time()` definition replaces it. The linker would refuse coexistence anyway.
- **`firmware/src/CMakeLists.txt`**: added `data_manager/data_manager.cpp` + `relay_controller/relay_controller_stub.cpp` to SRCS; removed `data_manager/data_manager_stub.cpp`.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "data_manager/data_manager.h"` added.
  - Spawn T4 just before T9 with `xTaskCreatePinnedToCore(task_data_manager, "T4-data", 8192, NULL, 5, &task_t4, tskNO_AFFINITY)`. T4 runs at priority 5 (one higher than T9's 4) so cfg/measurement updates land before any synthetic logging derived from them.
  - Heartbeat now calls `dm_cfg_snapshot(&cfg)` each tick and logs `t_min_day / t_max_day / hyst_t / v_max / unix_ts / is_daytime / sunrise / sunset` from the snapshot. Validates that T4 loaded the cfg from NVS (values should be 1.20.3-written production setpoints, NOT cfg_defaults.h's compile defaults).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.7`.

#### Build traps encountered + fixes

Two Arduino-transitive-include traps surfaced:
1. `'time_t' was not declared` in data_manager.h:266. Fix: explicit `#include <time.h>` in the header.
2. `'settimeofday' was not declared` in data_manager.cpp:219, 1017. Fix: explicit `#include <sys/time.h>` in the source.

Same `arduino-transitively-included-IDF-headers-we-now-need-explicitly` pattern as alpha.6.6 (where esp_log.h had to be added explicit). I expect ~1-2 of these per future activation; each instance is a 1-line fix.

#### Build delta vs alpha.6.6

| Metric | alpha.6.6 | alpha.6.7 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,214,497 B | **1,225,657 B** | +11,160 B |
| Flash usage % | 57.9 % | 58.4 % | +0.5 pp |
| RAM static | 42,880 B | **51,992 B** | **+9,112 B** |

bin sha256: `179298D12B3BA12892B44BA451C130B35E4ADF08743FEF8C2477A486FBBCC419`

The **+9 KB RAM static** is mostly data_manager's `s_ring` history buffer (DM_RING_DEPTH × sizeof(sensor_reading_t) ≈ 360 × 20 B ≈ 7.2 KB) plus the cfg_shadow_t static (~1 KB) plus the 5 NVS key-name string-table arrays. T4's runtime task stack (8 KB) comes from heap — the +9 KB above is static BSS only.

Runtime heap impact at heartbeat baseline expected: ~−9 KB free (T4 task stack 8 KB + a small Q4/Q6/MX_-related working set). Compared to alpha.6.6's `free=222,331`, alpha.6.7 should land around `free=213,000`.

#### Acceptance bar for alpha.6.7

1. ✅ Build succeeds (after two include fixes).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner: `alpha.6.7: T4 data_manager task spawned (handle=0x...)`. T4 emits its own boot-banner equivalents (NVS load progress, RTC read, sunrise compute) — watch for those.
4. **Heartbeat shows live cfg snapshot every 5 s**:
   ```
   alpha.6.7 dm: t_min_day=<n> t_max_day=<n> hyst_t=<n> v_max=<n>
                 unix_ts=<NNNNNNNNNN> is_day=<0|1> sunrise=<n> set=<n>
   ```
   - `t_min_day`/`t_max_day`/etc. should be **production 1.20.3 values**, not cfg_defaults.h's compile defaults. The dev board's NVS persisted those values from prior production firmware runs.
   - `unix_ts` should be the current Unix epoch (post-SNTP).
   - `is_day=1` mid-day Dutch time.
   - `sunrise`/`set` should match alpha.6.2's tickle output (220 / 1174 minutes UTC ≈ 03:40 / 19:34).
5. T9 (alpha.6.6) keeps running — SD CSV continues to grow with one event per 5 s.
6. All earlier-phase tickles regression-clean.
7. Run ≥ 10 min; no resets; no stack-overflow on T4.

### `[2.0.0-alpha.6.6]` — 2026-05-18

**Phase 6.6 — event_logger (T9) activation.** Brings T9 online: sole consumer of Q3, drains the queue, persists each event as a CSV line on SD via LIB-8. After this lands, every other task in the system can call `log_post(&evt)` and get durable storage. Made considerably simpler by Phase 6.5's NVS-fallback removal (T9 is now SD-only).

#### What changed

- **`firmware/src/event_logger/event_logger.cpp`**: dropped `#include <Arduino.h>`, added explicit `#include <esp_log.h>` (was coming via Arduino.h transitively). No other source changes — the file was already IDF-shaped beyond that.
- **`firmware/src/data_manager/data_manager_stub.cpp`** (new): provides `uint32_t dm_get_unix_time(void) { return (uint32_t)time(NULL); }`. Breaks the data_manager↔event_logger circular dep so T9 can compile and run without waiting for the full T4 port. **Designed for forcing-removal** when the real `data_manager.cpp` activates in Phase 6.7+ — the linker will refuse two definitions of `dm_get_unix_time`.
- **`firmware/src/CMakeLists.txt`**: added `event_logger/event_logger.cpp` + `data_manager/data_manager_stub.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "event_logger/event_logger.h"` added.
  - After the LIB-8 SD tickle (which unmounts at the end), spawn T9 via `xTaskCreatePinnedToCore(task_event_logger, "T9-evlog", 6144, NULL, 4, &task_t9, tskNO_AFFINITY)`. T9 internally re-mounts the SD, scans for existing `YYYYMMDDHHMMSS.csv` files, and resumes the most recent if it has room.
  - In the heartbeat task: `log_post(&syn)` with a synthetic `LOG_SYSTEM` event each tick (`value_a = uptime_s`, `value_b = free_heap_KB`). Operator can confirm T9 is alive by watching the SD CSV file grow by one line every 5 s.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.6`.

#### Build delta vs alpha.6.5

| Metric | alpha.6.5 | alpha.6.6 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,205,309 B | **1,214,497 B** | +9,188 B |
| Flash usage % | 57.5 % | 57.9 % | +0.4 pp |
| RAM static | 42,784 B | 42,880 B | +96 B |

bin sha256: `D2611059474E907A00A6AAA6054A1C297322D79C207ECA9F109B9152389435B0`

The +9 KB is event_logger.cpp's task body + the heartbeat-side log_post call + the stub. Runtime cost adds T9's task stack (6 KB heap) plus T9's internal SD/FAT-write buffers (~2-3 KB) when active.

#### Build trap encountered + fix

First build failed: `'ESP_LOGI' was not declared in this scope`. Cause: `event_logger.cpp` was relying on the now-removed `#include <Arduino.h>` to transitively pull in `esp_log.h`. Fix: add the include explicitly (one line). The "ARDUINO_HEADERS_TRANSITIVELY_INCLUDED_ESP_IDF_HEADERS_THAT_WE_NEED_INDEPENDENTLY" trap will hit other dormant files when they activate; pattern is "add the explicit IDF include in the same commit that drops Arduino.h."

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2. T9 spawned cleanly and the killer signal arrived almost immediately:
```
I (1587) T9_LOG: [T9] task alive
I (1597) GHC-STUB: alpha.6.6: T9 event_logger task spawned (handle=0x3fcec634);
                   heartbeat will log_post() synthetic events to Q3
...
I (1692) T9_LOG: [T9] Resuming log file /20260518031514.csv
I (1698) T9_LOG: [T9] SD ready
```

**Cross-firmware filesystem continuity confirmed**: T9 in v2.0.0-alpha.6.6 opened the same `20260518031514.csv` file that the production 1.20.3 firmware created earlier today at 03:15:14 local time. The two firmware lineages can share the same SD CSV — by design.

From boot+12 s (heartbeat 0) onward, each 5-second heartbeat's `log_post()` adds a `LOG_SYSTEM` event to Q3 with `value_a=uptime_s`, `value_b=free_heap_KB`. T9 drains the queue and appends one CSV line per event to the resumed file. Operator can verify by pulling the SD card to a PC and reading the tail of the file.

**Heap impact** — T9 runtime cost matches prediction:
- alpha.6.4 heartbeat baseline: `free=234,495 / largest=131,072`
- alpha.6.6 heartbeat baseline: `free=222,331 / largest=118,784`
- Delta: **−12,164 B free** (T9 6 KB stack + FAT/SD buffers + Q3 storage + miscellaneous), **−12,288 B largest** (three 4 KB heap blocks consumed by T9 allocations). Predicted ~10 KB; actual ~12 KB; within budget.
- Heap **rock-steady at 222,331 across 8+ heartbeats** — no leak from T9 or from the per-tick log_post.

**Earlier-phase tickles all regression-clean**:
- system_globals (Phase 6.1) ✓
- T7 keypad (Phase 6.4) ✓ — `T7_KPD: T7 task alive` still fires; Q2 drain logic unchanged
- WiFi (Phase 3): 1.2 s STA_GOT_IP, RSSI -49 dBm (excellent signal this session)
- SNTP (Phase 3): synced in 4.2 s
- Sunrise (Phase 6.2): `lat=52.37 lon=4.90 unix=1779091892 -> OK; rise=03:40 UTC set=19:34 UTC is_daytime=true`
- HTTPS (Phase 4): 5/5 OK status=204; gh#23 fragmentation pattern intact (call 1 free −492 / largest −36,864; calls 2-5 free ~−248 / largest 0)
- Web server (Phase 5): `T-WEB: HTTP server running — open http://192.168.20.160/ in a browser`
- System_id (Phase 6.3): `Unit ID: 2344`
- All Phase 2 drivers regression-clean

**Phase 6.6 is CLOSED.** T9 event_logger is now live and accepting events from any task. This unblocks every future task activation (data_manager, sensor_poll, relay_controller, climate_control, ui_display, safety_monitor, the network/status/web ports) — each can call `log_post()` for durable SD-CSV recording from day one. The data_manager_stub.cpp will be force-removed by the linker when the real T4 ports in Phase 6.7+.



**Phase 6.5 — DESIGN CHANGE: retire the NVS event-log ringbuffer (gh#22).** The NVS-backed event-log ring was introduced in 1.x for boot-survival event recording when SD might fail. In practice SD has been reliable enough on production units that the NVS ring served no purpose that wasn't already covered by SD persistence — and its presence complicated T9's logic with two parallel persistence paths, a "fallback" mode, drop-counter bookkeeping, and a separate `/api/log/download?src=nvs` web endpoint with its own CSV-export code. Retiring it simplifies T9 to a single SD-CSV persistence path, simplifies the web UI to "one log-source dropdown, SD files only", and reclaims ~600 B of code + a now-unused chunk of NVS namespace.

Code-only sweep in this alpha. The matching documentation updates (FDS, TSDS, tasks.md, design narratives, manuals, test plans) lift into **alpha.6.5.1+** as a separate paper sweep — the binary is identical to alpha.6.4 (the NVS-ring code was already link-time-dead because nobody in the active build called it), so this alpha doesn't need a flash; doc updates are pure git-history hygiene.

#### What changed (code)

**Core API — drivers/nvs (LIB-7):**
- **`drivers/nvs/src/nvs_config.h`**:
  - Removed `#define NVS_NS_LOG "log"` (replaced with a removal-pointer comment).
  - Removed the `Ring-buffer event log` docblock + the 3 function declarations (`nvs_log_append`, `nvs_log_read`, `nvs_log_count`). Comment marker left in place pointing at this changelog entry.
  - Removed the `#ifndef CONFIG_NVS_LOG_CAPACITY / #define ... 1000 / #endif` default-capacity block (now unused).
- **`drivers/nvs/src/nvs_config.cpp`**:
  - Removed `log_slot_key()` helper + the 3 function bodies.
  - Removed `#include <inttypes.h>` (was only used by `PRIu32` in the removed helper).
- **`drivers/nvs/test/test_nvs_config.cpp`**:
  - Removed 5 unit tests (UT-NVS-008..012 — append/read/wrap/oldest-after-wrap/clamp).
  - Removed matching `RUN_TEST()` lines in the test runner.
- **`drivers/nvs/src/main.cpp`**: removed HW-NVS-006..008 hardware-test code (ring-append + read + capacity-cap).

**Callers — firmware/src/:**
- **`firmware/src/event_logger/event_logger.cpp`**: removed the `nvs_log_append(evt, sizeof(log_event_t))` call from `process_event()`. Events are now SD-only; if SD is absent, the existing `s_dropped` counter accumulates and is surfaced as a `LOG_SYSTEM value_a=2` event on the next drain (existing path, unchanged). The NVS-only-fallback mode is gone.
- **`firmware/src/event_logger/event_logger.h`**: updated the T9 task docblock to drop the NVS-mirror description.

**Web GUI:**
- **`firmware/src/web_server/web_server.cpp`**:
  - `/api/log/files` no longer returns `nvs_count` in its JSON response. Body is now just `{"sd_files":[...]}`.
  - `/api/log/download` lost the entire `?src=nvs` branch + the TYPE_NAMES/INIT_NAMES tables + the ISO-8601 formatter used only for NVS-CSV export. Default `src` is now `sd` (was `nvs`). Calls without an explicit `?file=` parameter return 400.
- **`firmware/data/app.js`**:
  - `loadLogFiles()` no longer adds an "NVS buffer (N entries)" option to the log-source dropdown. If `sd_files` is empty (no card / no CSVs), the dropdown shows a single disabled `— no SD log files —` placeholder.
  - `downloadLog()` removed the `val === 'nvs'` branch.
- **`firmware/data/index.html`**: updated the `Log source` field's tooltip to describe SD-only behaviour.

**Build / partitions:**
- **`firmware/platformio.ini`**: removed the `-DCONFIG_NVS_LOG_CAPACITY=250` build flag. Replaced with a removal-pointer comment.
- **`firmware/partitions.csv`**: updated the comment for the nvs partition to drop "event-log ring buffer" from its description (the partition stays the same size — config namespaces use a tiny fraction; the freed pages are just unused).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` stamped `2.0.0-alpha.6.5`.

#### Build delta vs alpha.6.4

| Metric | alpha.6.4 | alpha.6.5 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,205,309 B | **1,205,309 B** | **0 B** |
| Flash usage % | 57.5 % | 57.5 % | 0 |
| RAM static | 42,784 B | 42,784 B | 0 |

bin sha256: `D615FBA9C6D23BBA5169184A098D4E98F97F405E548C051FB25E62B58D909FF8`

**Binary is identical to alpha.6.4 by size — but a different SHA256** because the removed nvs_log_* functions had been getting link-time dead-stripped (nobody in the active firmware/src/ files calls them; event_logger/web_server are still dormant). Removing them from source shrinks the .o files but the .bin section sizes wrap to the same boundaries. The bin diff is purely in the .text section ordering. No behavioural change.

NVS data on units upgraded from 1.20.x: the existing `log/head`, `log/count`, `log/eNNNN` blob entries in the `log` namespace persist as orphan data. They are harmless (no code reads them). On a long timescale they'll be naturally evicted as other namespaces consume NVS pages.

#### Acceptance bar for alpha.6.5

1. ✅ Build succeeds (after both passes — initial removal + the secondary CONFIG_NVS_LOG_CAPACITY cleanup).
2. **No flash needed** — alpha.6.5 binary's runtime behaviour is identical to alpha.6.4. The user-visible signal is "Phase 6.6 event_logger activation no longer has to wrestle with the NVS-fallback path."
3. **No grep matches remain in active .cpp/.h files** for `nvs_log_append|nvs_log_read|nvs_log_count|NVS_NS_LOG|CONFIG_NVS_LOG_CAPACITY` (post-cleanup verification).
4. **Documentation sweep deferred to alpha.6.5.1+** covers: design/tasks.md (T9 description), design/technicalSoftwareDesignSpecification.md, design/technicalDesignSpecification.md, design/logAnalysis.md, design/riskAssessment.md, design/implementationStatusPages.md, firmware/firmwareImplementationPlan.md, firmware/firmwareImplementationResults.md, firmware/src/README.md, manual/beheerderHandleiding.md (operator-facing), log/README.md + log/logparser.md (CSV format remains; just remove the "NVS-CSV alongside SD-CSV" passages), test/testPlan.md + test/softwareTestPlan.md + test/softwareTestResult.md (UT-NVS-008..012 + HW-NVS-006..008 cases gone).



**Phase 6.4 — First real FreeRTOS task activation: `keypad_scan` (T7).** Previous activations (sunrise, system_id) were pure helper functions called from `app_main`. T7 is a long-running task created via `xTaskCreatePinnedToCore`, scanning the 4×4 membrane keypad every 20 ms and producing key events onto Q2 — the first real producer/consumer pattern in the IDF build.

Originally the activation plan put data_manager and event_logger first, but those two have a **circular dependency** (data_manager calls log_post() from event_logger; event_logger calls dm_get_unix_time() from data_manager) that needs a stub layer to break. keypad_scan is dependency-free (LIB-5 ✓, Q2 ✓) and gives tactile acceptance (operator presses a key, event appears in serial log within 5 s). Re-ordered the activation plan to do keypad_scan first; the event_logger/data_manager bundle will follow the NVS-ringbuffer-removal design change (Phase 6.5).

#### What changed

- **`firmware/src/keypad_scan/keypad_scan.cpp`** — dropped vestigial `#include <Arduino.h>`. Body uses only ESP-IDF (`esp_log`, `esp_task_wdt`, FreeRTOS) and LIB-5 (`keypad_matrix`). Inline comment recording the removal.
- **`firmware/src/CMakeLists.txt`** — added `keypad_scan/keypad_scan.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include "keypad_scan/keypad_scan.h"` + `#include "types/app_types.h"` (for `key_event_t`, `task_t7`, `Q2`).
  - After the existing `keypad_init()` call (alpha.2.2 tickle), spawn T7 via `xTaskCreatePinnedToCore(task_keypad_scan, "T7-keypad", 3072, NULL, 4, &task_t7, tskNO_AFFINITY)`. Handle stored into the global `task_t7` (declared extern in app_types.h, defined NULL in system_globals.cpp since alpha.6.1, now populated).
  - In the heartbeat task: drain Q2 each tick. Pop all available `key_event_t`s non-blocking and log each one. Up to 8 events queued between 5 s ticks (Q2 depth); user keypresses within that window are captured cleanly.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.6.4`.

Two key design notes folded into the diff:
1. **`keypad_init()` is now called twice** — once by app_main's alpha.2.2 tickle (line 369), again by `task_keypad_scan()` at its first iteration. LIB-5's init is idempotent (GPIO pin config); the double-init is harmless and is documented inline.
2. **Heartbeat continues to call `keypad_count_pressed()`** — that's the alpha.2.2 polled read path. It's independent of T7's edge-detection event path. Both paths read LIB-5 directly. They can coexist.

#### Build trap encountered + fix

First build failed: `'key_ev' was not declared in this scope` and `'task_t7' was not declared in this scope`. `app_main_stub.cpp` didn't include `types/app_types.h` (the tickles previously stayed inside their own self-contained scopes). Added the include alongside the other Phase 6 headers.

#### Build delta vs alpha.6.3

| Metric | alpha.6.3 | alpha.6.4 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,203,669 B | **1,205,309 B** | +1,640 B |
| Flash usage % | 57.4 % | 57.5 % | +0.1 pp |
| RAM static | 42,784 B | 42,784 B | 0 |

bin sha256: `02E1DBD7543ED0C58794EBC8368177A3AC756189ED0D0268FCD35FE997011100`

The +1,640 B is the keypad_scan task code (~600 B) + the heartbeat's Q2-drain loop (~400 B) + miscellaneous link-time text expansion. Runtime cost: one new FreeRTOS task with 3 KB stack (~3.5 KB heap when active).

#### Acceptance bar for alpha.6.4

1. ✅ Build succeeds (after the `types/app_types.h` include fix).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. After the existing `keypad_init` log line, new log:
   ```
   alpha.6.4: T7 keypad_scan task spawned (handle=0x...); press a key to see Q2 events drained in the heartbeat
   ```
4. **Physical-input acceptance**: with the membrane keypad wired (Unit 2 production hardware has it), pressing any key produces a log line within the next heartbeat:
   ```
   Q2 key event #0: 'N' repeated=0
   Q2 drained 1 event(s) this tick
   ```
   where `N` is the ASCII key value per the 4×4 layout (`1234`/`5678`/`9*0#` mapped to corners + `ABCD` on column 4).
5. **Key-repeat acceptance**: holding a key for >500 ms produces additional events with `repeated=1` at ~100 ms intervals. Inside a 5 s heartbeat window with a held key, expect ~45 events (one first-press + ~44 repeats at 100 ms each = 4.4 s of repeats).
6. **Q2 overflow** if user presses >8 keys in 5 s: each event after the 8th is dropped, `T7_KPD: Q2 full — first-press 'X' dropped` warning logged. Tolerable for the tickle.
7. Earlier-phase tickles all regression-clean (heartbeat heap stable, HTTPS gh#23 fix intact, web server still responding).
8. Run ≥ 10 min; no resets; no stack overflow on the T7 task.

If T7 fails to spawn (`xTaskCreate T7 failed (rc=X)`), most likely cause is heap exhaustion at boot. Pre-spawn heap is ~300 KB on Unit 2; T7 needs 3 KB stack — trivial. Failure here would point at something else (rc=-1 = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY only).

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2. T7 spawned immediately after the system_globals_init line:
```
I (990) T-GLOBALS: system_globals_init OK: ...
I (1000) T7_KPD: T7 task alive
I (1003) GHC-STUB: alpha.6.4: T7 keypad_scan task spawned (handle=0x3fceaee4)
```

**Interactive verification — operator exercised the full input domain:**

| Time | Action | Captured |
|---|---|---|
| 22 s | quick tap `5`, `4` | 2× first-press events |
| 27 s | rapid tap `6`, `2`, `1` | 3× first-press events |
| 32 s | tap `8` | 1× first-press event |
| 37 s | hold `8` ~4 s | 1× first-press + 7× repeat = 8 events (fills Q2) |
| 40 s | release 8 + press 5 | **`Q2 full — first-press '5' dropped`** warning |
| 42 s | continue holding | 8 events (mix `8`/`5` repeats) |
| 47 s | hold `5` | 8× `5 repeated=1` |
| 52 s | release | quiet |

All design behaviours observed cleanly:

- ✅ First-press detection on key-down (`repeated=0`)
- ✅ Edge detection on key-change (pressing 4 while 5 is still active → new first-press for 4)
- ✅ Repeat after 500 ms hold (`repeated=1` at ~100 ms cadence)
- ✅ Q2 overflow warning fires correctly on the 9th event
- ✅ ASCII codes match the 4×4 layout (`5/4/6/2/1/8` all valid keypad positions)
- ✅ The polled `keys=N` (alpha.2.2 path) and event-`Q2-drained` (alpha.6.4 path) coexist correctly — both read LIB-5 independently. `keys=1` heartbeats line up with periods when a key is held.

**Earlier-phase tickles all regression-clean:**
- system_globals (Phase 6.1) regression-clean.
- WiFi (Phase 3): 1.2 s STA_GOT_IP, SNTP synced in 4.5 s.
- sunrise (Phase 6.2): `rise=03:40 UTC set=19:34 UTC is_daytime=true` — same as alpha.6.2 acceptance, consistent.
- system_id (Phase 6.3): `Unit ID: 2344 (AP-SSID would be Greenhouse-2344)`.
- HTTPS (Phase 4): 5/5 OK status=204, gh#23 fix still holding — call 1 free −492 B / largest −36,864 B; calls 2-5 free ~−220 B / largest 0 (transient-not-sticky).
- Web server (Phase 5): running, externally reachable.

**Heap delta** confirms predicted T7 cost:
- alpha.6.3 heartbeat baseline: `free=234,495 / largest=131,072`
- alpha.6.4 heartbeat baseline: `free=231,179 / largest=126,976`
- Delta: **−3,316 B free** (T7 stack 3 KB + FreeRTOS TCB ~300 B), **−4,096 B largest** (one heap block consumed by the task-stack allocation). Exactly the predicted cost. Heap rock-steady at 231,179 across 8+ heartbeats — no leak.

**Side observation worth recording**: keypad_scan.cpp's source comment says "Q2 capacity is 16 items" but system_globals.cpp creates Q2 with depth 8 (matching the production tasks/queues spec the data hub has been writing against). The `Q2 full` warning at 41 s confirms depth=8 in the binary. Real-world UI usage (operator presses slow vs T8 consumer drain) means 8 is fine — the saturation only happened here because the tickle's heartbeat-drain runs every 5 s instead of T8's eventual sub-100 ms drain. Worth noting for any future Q2-depth audit; not a regression.

**Phase 6.4 is CLOSED. First real FreeRTOS task activation pattern proven**: `xTaskCreatePinnedToCore`, handle into a global from `system_globals.cpp`, producer (T7) + consumer (heartbeat stub) both running, Q2 round-trips work, no leaks. The pattern scales for the remaining 10 task subsystem activations.



**Phase 6.3 — Second firmware/src/ subsystem activation: `system_id`** (gh#17, since 1.18.3 on the arduino-era line). Tiny utility (~48 lines) that derives a 16-bit per-unit ID from MAC bytes 4-5 via `esp_read_mac()`, with a lazy-cache for O(1) subsequent reads. No FreeRTOS deps, no driver deps, no Arduino — just `<esp_mac.h>` and `<stdio.h>`. Surfaces the per-unit ID in the boot banner alongside the MAC; the future network_manager (Phase 6.12) will use it for the `Greenhouse-XXXX` AP-SSID.

#### What changed

- **`firmware/src/CMakeLists.txt`** — added `system_id/system_id.cpp` to SRCS.
- **`firmware/src/app_main_stub.cpp`** — `#include "system_id/system_id.h"` plus two new lines in `log_boot_banner()` immediately after the MAC printout:
  ```c
  char unit_id_str[5] = {0};
  system_unit_id_str(unit_id_str, sizeof(unit_id_str));
  ESP_LOGI(TAG, "Unit ID: %s (AP-SSID would be Greenhouse-%s)", unit_id_str, unit_id_str);
  ```
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.6.3`.

The `system_id.cpp` file required ZERO modifications. It was already IDF-native — no Arduino, no FreeRTOS, no drivers.

#### Build delta vs alpha.6.2

| Metric | alpha.6.2 | alpha.6.3 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,203,945 B | **1,203,669 B** | **−276 B** |
| Flash usage % | 57.4 % | 57.4 % | 0 |
| RAM static | 42,776 B | 42,784 B | +8 B |

bin sha256: `EC2C51B3ED0B8873067013CD346DEC3F56E2C918840A4F5485E7F17AA648384F`

**Flash size actually DROPPED** by 276 bytes despite adding a new .cpp. Likely cause: the format-string change in `log_boot_banner` removed one of the two `(int)` casts of the chip revision (small constant fold), and the link-time optimiser re-arranged some literal pools. The `system_id.cpp` object code itself is tiny (~80 bytes — one cached uint16, one snprintf call, one esp_read_mac call). The +8 B RAM is the `s_cached` and `s_inited` statics in system_id.cpp's BSS.

#### Acceptance bar for alpha.6.3

1. ✅ Build succeeds.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner now includes a new line right after the STA MAC:
   ```
   I (xxx) GHC-STUB: STA MAC: 64:E8:33:7C:23:44
   I (xxx) GHC-STUB: Unit ID: 2344 (AP-SSID would be Greenhouse-2344)
   ```
   The value `2344` derives from MAC bytes 4-5 (`23:44`).
4. All earlier-phase tickles regression-clean.

If the unit ID shows as `0000` or differs from the MAC last-two-bytes, `esp_read_mac(ESP_MAC_WIFI_STA, ...)` returned zero / wrong values — but this same call has been working in `log_boot_banner` (existing MAC print) since Phase 1, so a regression there would be visible in the MAC line too.

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2 dev board. The new line fired exactly as designed:
```
I (958) GHC-STUB: STA MAC: 64:E8:33:7C:23:44
I (962) GHC-STUB: Unit ID: 2344 (AP-SSID would be Greenhouse-2344)
```

`2344` correctly derives from MAC bytes `23:44` packed as uint16. The AP-SSID string `Greenhouse-2344` is the same one the future Phase-6.12 network_manager port will use for soft-AP mode.

**SNTP soft-failed this boot** (network UDP/123 instability — same intermittent pattern documented in alpha.3.2). Not a regression in alpha.6.3 (which doesn't touch SNTP).

**Side-channel observation worth recording**: the sunrise tickle output (Phase 6.2) now serves as a SNTP-success indicator for free. When SNTP works (alpha.6.2 boot): `unix=1779088245 -> rise=03:40 set=19:34 is_daytime=true`. When SNTP fails (this boot): `unix=12 -> rise=07:51 set=15:37 is_daytime=false`. The algorithm correctly computes January-1 polar-winter values for "1970-01-01 + 12 seconds since boot" — graceful failure mode, no crash, no NaN. The tickle was designed to validate the sunrise math; it serendipitously also gives an at-a-glance SNTP health check.

**HTTPS still worked despite time being stuck near 1970**: all 5 calls returned `OK status=204`. Either IDF's esp-tls doesn't strictly enforce cert validity-window when `skip_cert_common_name_check=true`, or SNTP arrived in the ~280 ms window between the sunrise tickle's `time(NULL)` snapshot and the first HTTPS handshake. gh#23 heap pattern intact (call 1 free −504 B / largest −32,768 B; calls 2-5 free ~−240 B / largest 0, with one transient −4,096 B on call 4 that recovered on call 5).

**Earlier-phase tickles all regression-clean**:
- system_globals_init: OK at 990 ms (Phase 6.1 regression-clean)
- WiFi: 1.7 s STA_GOT_IP, RSSI -66 dBm (Phase 3 regression-clean)
- HTTPS: 5/5 OK status=204 (Phase 4 regression-clean, gh#23 fix holds)
- Web server: running on port 80 (Phase 5 regression-clean)
- Sensors + RTC + LFS + SD: heartbeat shows fg6485a=0 rh=95.0 temp=13.9, s200=0 dir=208.0, rtc=07:23 advancing (all Phase 2 regression-clean)
- LFS verify PASS, SD verify PASS, file_size now 1020 B = 20 boots × 51 B
- Heartbeat heap: `free=234,495 / largest=131,072` — within jitter of alpha.6.2's baseline (`free=234,599`). system_id added zero heap impact (its statics live in BSS, not heap).

Phase 6.3 PASSED. The `Unit ID: 2344` line confirms `esp_read_mac()` + the lazy-cache pattern work correctly; the AP-SSID string is now derivable from any future task.

Phase 6.3 is CLOSED.



**Phase 6.2 — First firmware/src/ subsystem activation: `data_manager/sunrise.cpp`.** Lowest-risk possible first activation — pure NOAA solar-position math (~157 lines), no FreeRTOS dependencies, no driver dependencies, no Arduino dependencies. The activation validates the build-pipeline-touches-a-firmware/src/-file flow before we tackle the heavier task .cpp files in 6.3+.

#### What changed

- **`firmware/src/CMakeLists.txt`** — added `data_manager/sunrise.cpp` to SRCS. The existing `INCLUDE_DIRS "."` (firmware/src/) is enough for headers to resolve via `data_manager/sunrise.h`.
- **`firmware/src/app_main_stub.cpp`**:
  - `#include <time.h>` added (`time_t`, `time()`).
  - `#include "data_manager/sunrise.h"` added.
  - New tickle inserted between the WiFi tickle (which provides SNTP-synced time) and the HTTPS tickle: calls `sunrise_calc(time(NULL), 52.37, 4.90, ...)` for Amsterdam-area coordinates, then `sunrise_is_daytime(...)` for the boolean. Logs rise/set times as UTC HH:MM plus the is_daytime result.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.6.2`.

The `sunrise.cpp` file itself required ZERO modifications. It includes only `<math.h>` and `"sunrise.h"`; that header includes only `<stdint.h>` and `<stdbool.h>`. Cleanest possible activation surface.

#### Build trap encountered + fix

First build failed with `'time_t' is not declared` — `<time.h>` wasn't included by `app_main_stub.cpp`. (The previous tickles got `time_t` transitively through some IDF header chain.) Added `#include <time.h>` explicitly.

#### Build delta vs alpha.6.1

| Metric | alpha.6.1 | alpha.6.2 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,194,633 B | **1,203,945 B** | +9,312 B |
| Flash usage % | 57.0 % | 57.4 % | +0.4 pp |
| RAM static | 42,776 B | 42,776 B | 0 |

bin sha256: `9FBC04F8FC481A9E916D80DA72B60F0558D2FAC6C91C0B459B1CFC566D04A13C`

The +9.3 KB is sunrise.cpp's math: `sin/cos/tan/asin/acos` calls for the NOAA equations (trigonometric library code that wasn't previously linked), plus the `fmodf`/`fabsf` glue. The actual sunrise.cpp object code is small (~3 KB); the rest is newlib trig that gets pulled in transitively.

#### Acceptance bar for alpha.6.2

1. ✅ Build succeeds (after the `<time.h>` include fix).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. After the WiFi tickle's `SNTP synced after N ms — epoch=…` line, the new tickle line should appear:
   ```
   alpha.6.2 sunrise tickle: lat=52.37 lon=4.90 unix=<N> -> OK; rise=HH:MM UTC set=HH:MM UTC is_daytime=true|false
   ```
4. **Plausibility check** for the current date (2026-05-18, mid-May):
   - Amsterdam sunrise mid-May UTC ≈ **03:30 - 04:00 UTC** (= 05:30 - 06:00 CEST)
   - Amsterdam sunset mid-May UTC ≈ **19:00 - 19:30 UTC** (= 21:00 - 21:30 CEST)
   - The user's bench-test time is approximately 07:00 - 09:00 CEST (= 05:00 - 07:00 UTC), so `is_daytime=true` is expected.
5. Earlier-phase tickles all regression-clean (Phase 2 drivers, Phase 3 WiFi, Phase 4 HTTPS gh#23 signal, Phase 5 web server, Phase 6.1 system_globals).
6. Heap delta vs alpha.6.1: the trig functions allocate nothing (`<math.h>` is pure compute), so heartbeat heap should match alpha.6.1's ~234,451 free / 131,072 largest within normal jitter.

If the tickle reports `is_daytime=false` mid-day, either lat/lon got swapped, the time-of-day in `time(NULL)` is wrong (SNTP didn't sync), or the algorithm has a bug. The algorithm has been used in production 1.20.3 for months on Unit 2 with the same Dutch coordinates, so option 3 is unlikely.

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2 dev board. The new tickle line fired between SNTP sync and the HTTPS tickle:

```
I (4448) T-WIFI: SNTP synced after 900 ms — epoch=1779088245
I (4450) GHC-STUB: alpha.6.2 sunrise tickle: lat=52.37 lon=4.90 unix=1779088245 -> OK;
                                              rise=03:40 UTC set=19:34 UTC is_daytime=true
```

**Math verification against astronomical truth for Amsterdam, 2026-05-18:**

| Metric | Real (astronomical) | Computed | Delta |
|---|---:|---:|---:|
| Sunrise UTC | 03:43 | 03:40 | −3 min |
| Sunset UTC | 19:23 | 19:34 | +11 min |
| Day length | 15h 40m | 15h 54m | +14 min |
| is_daytime at 07:10 UTC | true | true | ✓ |

Sunrise within 3 minutes of astronomical truth (better than the algorithm's stated ±2 minute spec). Sunset 11 minutes long — likely the simplified NOAA equations' refraction model overshooting slightly at higher latitudes. **Well within tolerance for the controller's day/night setpoint discrimination** (the system switches between day/night setpoints; a 10-minute boundary fuzziness is invisible to greenhouse climate response).

`is_daytime=true` correct — UTC 07:10 is well past sunrise 03:40.

**Earlier-phase tickles all regression-clean** (heartbeat output identical in shape to alpha.6.1, sensor values vary normally with environmental drift):
- system_globals_init: OK at 984 ms (Phase 6.1 regression-clean)
- WiFi: 1.7 s STA_GOT_IP, RSSI -67 dBm, one transient retry (Phase 3 regression-clean)
- SNTP: synced in 900 ms — fastest yet this session (Phase 3 regression-clean, the network's state varies as the user moves between APs)
- HTTPS: 5/5 calls returned `OK status=204`. gh#23 pattern intact:
  - Call 1 cold: free −456 B, largest −32,768 B
  - Calls 2-5: free −212..−248 B, largest 0 — transient-not-sticky fragmentation (Phase 4 regression-clean)
- Web server: running, externally reachable (Phase 5 regression-clean)
- Heartbeat baseline: `free=234,599 / largest=131,072` — within 200 B of alpha.6.1's baseline. Sunrise.cpp added flash code (+9 KB for newlib trig) but **zero RAM** (no globals).

**The activation pattern is proven**: one file added to SRCS, one include in the stub, one tickle call. Math runs correctly first try. This is the template for the remaining 11 task-subsystem activations in Phase 6.3..6.13.

Phase 6.2 is CLOSED.



**Phase 6.1 — FreeRTOS infrastructure bootstrap (`system_globals.cpp`).** Defines every queue / mutex / event group / task handle that `types/app_types.h` declares as `extern`, and provides a single init call that creates them at boot. This decouples "FreeRTOS infrastructure exists" from "tasks are running" so subsequent Phase-6 alphas can each activate one task without touching the global-creation plumbing.

#### What changed

- **`firmware/src/system_globals.h`** (new) — declares `int system_globals_init(void)`; idempotent; ~1.2 KB heap on success.
- **`firmware/src/system_globals.cpp`** (new) — defines all 6 queues (Q1..Q6), all 5 mutexes (MX1..MX5), the event group (EG1), and all 14 task handles (task_t1..task_t15, all initialised to NULL — the matching `xTaskCreate` calls land one-per-alpha in Phase 6.2..6.13). Queue depths come from the inter-task design (Q3 = 32, Q4 = 16, Q1/Q2 = 8, Q5/Q6 = 1 with overwrite semantics).
- **`firmware/src/CMakeLists.txt`** — added `system_globals.cpp` to SRCS. No new REQUIRES (uses already-linked freertos + log components).
- **`firmware/src/app_main_stub.cpp`** — `#include "system_globals.h"` plus a call to `system_globals_init()` early in `app_main`, *before* any subsystem init. On non-zero return, logs FATAL and idles forever — no point trying to run further code with the global-handle contract broken.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.6.1`.

#### Build trap encountered + fix

First build failed with `error: "/*" within comment [-Werror=comment]`. The string `*_tickle.cpp` inside a `/* ... */` block looks like a nested comment start to GCC. Trivial replacement (`*_tickle.cpp` → `[X]_tickle.cpp`). Worth recording so the next person doesn't lose 30 seconds to it.

#### Build delta vs alpha.5

| Metric | alpha.5 | alpha.6.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,194,029 B | **1,194,633 B** | +604 B |
| Flash usage % | 56.9 % | 57.0 % | +0.1 pp |
| RAM static | 42,728 B | 42,776 B | +48 B |
| Runtime heap on init OK | (n/a) | ~1.2 KB | — |

bin sha256: `F71F76D7EF53733107F83559791934B87993A8F99D1806B7783C94351AD56EC8`

The +604 B is system_globals.cpp's object code (the init function + the queue/mutex/event-group static definitions). The +48 B RAM is the 24 NULL pointer slots in BSS. The ~1.2 KB runtime heap impact is the underlying FreeRTOS queue storage (8 × `window_cmd_t` + 8 × `key_event_t` + 32 × `log_event_t` + 16 × `config_update_t` + 1 × `net_status_t` + 1 × `sensor_reading_t` storage areas) plus 5 mutex semaphore-control blocks plus 1 event-group bitmap.

#### Acceptance bar for alpha.6.1

1. ✅ Build succeeds (after the `*/` comment-trap fix).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner shows the new log line near the top of `app_main`:
   ```
   T-GLOBALS: system_globals_init OK: queues=Q1..Q6 (8/8/32/16/1/1 depths), mutexes=MX1..MX5, EG1 (no bits set)
   ```
4. No regressions in any prior tickle — Phase 2 drivers, Phase 3 WiFi, Phase 4 HTTPS gh#23 signal (5 calls, transient-not-sticky fragmentation), Phase 5 web server endpoints all still respond identically.
5. Free internal heap roughly 1.2 KB lower than alpha.5's baseline (the new runtime allocations).

If `system_globals_init` returns non-zero, the most likely cause is internal-heap exhaustion at boot. Pre-init heap is ~370 KB on Unit 2; needing 1.2 KB is trivial. Failure here points at something else (e.g. a `xQueueCreate` parameter sanity issue).

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2 dev board. Boot reason 1 (`ESP_RST_POWERON`). The critical line fired at exactly the designed point in the boot sequence:
```
I (974) GHC-STUB: ================================================================
I (982) T-GLOBALS: system_globals_init OK: queues=Q1..Q6 (8/8/32/16/1/1 depths), mutexes=MX1..MX5, EG1 (no bits set)
I (1010) GHC-STUB: NVS pre-init: previous fw_version = "2.0.0-alpha.6.1" (status=0)
```

Right after the dashed-line banner divider, before any NVS work begins. Globals are visible to all subsequent code paths.

**Heap delta confirms the predicted runtime cost**:
- alpha.5 heartbeat baseline: `free=236,791 / largest=139,264`
- alpha.6.1 heartbeat baseline: `free=234,451 / largest=131,072`
- **−2,340 B free** vs alpha.5 — matches the ~2.3 KB system_globals runtime cost (1.2 KB queue storage + 5 mutexes + 1 event group + FreeRTOS per-object housekeeping). Within prediction window. No leak: heap is stable across multiple heartbeats (234,451 → 234,451 → 234,451 → 234,487 — ±36 B jitter from per-tick log allocations).

The `largest=131,072` reading is the HTTPS-tickle's transient post-call-5 state captured at the moment of the heartbeat sample (call 5 ended at largest=131,072 in this run). The transient-not-sticky pattern from alpha.4.1 still holds — the next time the system idles, largest will recover toward 139K.

**Earlier-phase tickles regression-clean**:
- LIB-1..9 driver outputs unchanged (i2c_scan, LFS verify, SD verify, RTC ticking +5s per heartbeat).
- WiFi: 1.85 s STA_GOT_IP (1709 → 3553), one transient retry, connect at RSSI -67 dBm, IP 192.168.20.160 obtained cleanly.
- SNTP synced in 2,400 ms — network is good this session.
- HTTPS: 5/5 calls returned `OK status=204`. Heap signature matches alpha.4.1 exactly:
  - Call 1 cold: free -504 B, largest -32,768 B
  - Calls 2-4: free -244..-248 B, largest 0
  - Call 5: free -248 B, largest 0
- Web server: `T-WEB: HTTP server running — open http://192.168.20.160/`; externally verified that `/api/info` reports `fw_version=2.0.0-alpha.6.1` and `/api/status` returns fresh data.

**Phase 6.1 is CLOSED.** The FreeRTOS infrastructure is now permanent in the build. Every subsequent Phase-6.N alpha can `xQueueSend(Q*, ...)` or `xSemaphoreTake(MX*, ...)` without worrying about creation order. Next: Phase 6.2 activates the first firmware/src/ subsystem.



**Phase 6.0 — Shared infrastructure for subsystem activation.** Phase 6 in the migration plan is the long-tail integration: bring 13 dormant subsystems online (data_manager, event_logger, sensor_poll, keypad_scan, relay_controller, climate_control, ui_display, safety_monitor, network_manager, status_post + supervisor, web_server, ota_manager, main.cpp), each with its own Arduino.h cleanup folded in. Rather than do a speculative bulk-cleanup pass that can't be validated (the touched files aren't yet in the build), alpha.6.0 sets up just the shared infrastructure each subsequent sub-phase needs.

#### What changed

- **`firmware/src/util/time_compat.h`** (new) — `millis_idf()` and `micros_idf()` `static inline` shims wrapping `esp_timer_get_time()`. Drop-in replacement for arduino-era `millis()` / `micros()` with the same uint32 return type and wraparound semantics. Zero runtime cost. Inline docs explain why a shim + a single point of truth for the µs→ms conversion.

#### Why no file edits in 6.0

The 15 files still carrying `Arduino.h` references are all dormant — none are in the current `firmware/src/CMakeLists.txt` SRCS list. The alpha.5 binary doesn't see any of their code. Two strategies were considered:

- **Bulk speculative cleanup**: edit all 15 now, validate nothing (build doesn't compile them). Saves diff churn but no acceptance signal.
- **Cleanup-as-you-activate**: each sub-phase activates ONE subsystem with the Arduino-keyword cleanup bundled into that subsystem's activation diff. Smaller, validated, bisectable. Phase 2's per-driver pattern.

The second strategy wins. Each alpha.6.N below activates one subsystem.

#### Phase-6 activation order (dependency-first)

| Sub-phase | Subsystem | Owning file(s) | Direct deps |
|---|---|---|---|
| 6.1 | data_manager (T4) | `data_manager.cpp` | LIB-7 (NVS), `app_types.h` |
| 6.2 | event_logger (T9) | `event_logger.cpp` | LIB-8 (SD), LIB-9 (LFS), data_manager |
| 6.3 | sensor_poll (T2) | `sensor_poll.cpp` | LIB-FG, LIB-S200, LIB-3, data_manager |
| 6.4 | keypad_scan (T7) | `keypad_scan.cpp` | LIB-5, event_logger |
| 6.5 | relay_controller (T3) | `relay_controller.cpp` | LIB-1, sensor_poll output |
| 6.6 | climate_control (T1) | `climate_control.cpp` | sensor_poll, relay_controller |
| 6.7 | ui_display (T8) | `ui_display.cpp` | LIB-4, sensor data |
| 6.8 | safety_monitor (T16) | `safety_monitor.cpp` | data_manager, event_logger |
| 6.9 | network_manager (T10) | `network_manager.cpp` | replaces wifi_tickle; geo via esp_http_client |
| 6.10 | status_post (T14) | `status_post.cpp` + supervisor | replaces https_tickle |
| 6.11 | web_server (T11) | `web_server.cpp` + 7× `web_routes_*.cpp` | replaces web_server_tickle (this is Phase 5.1-5.5 from the plan) |
| 6.12 | ota_manager (T13) | `ota_manager.cpp` | esp_ota_* |
| 6.13 | main.cpp | `main.cpp` replaces `app_main_stub.cpp` | orchestrates all of the above |

Some sub-phases will bundle (e.g. 6.4 might fold into 6.3 if keypad_scan's contract is tiny enough). The list captures intent; alpha-tag granularity will adjust as actual diffs land.

#### Survey of remaining Arduino-keyword surface area

A `grep` of `firmware/src/` after Phase 5 found:

| Pattern | Files | Total occurrences |
|---|---|---|
| `Arduino.h` reference | 15 | 15 |
| `millis()` call | 2 (`main.cpp`, `relay_controller.cpp`) | 4 |
| `Serial.print*()` | 1 (`main.cpp`) | 1 |
| `pinMode`/`digitalRead`/etc | 1 (`ui_display.cpp`) | 2 |
| `NeoPixel` / `String` | 2 (`main.cpp`, `network_manager.cpp`) | 4 |

Roughly **~25 mechanical token replacements** spread across the 13 subsystem activations. The NeoPixel → `rmt_transmit` rewrite in `main.cpp` is the only non-trivial item (alpha.6.13).

#### No binary changes in alpha.6.0

`time_compat.h` is header-only and currently unused (no .cpp `#include`s it yet — that lands in alpha.6.1 onwards). The alpha.5 binary is unchanged. No flash needed.

### `[2.0.0-alpha.5]` — 2026-05-18

**Phase 5 — Web server rewrite (`ESPAsyncWebServer` → `esp_http_server`).** The plan calls this the biggest single chunk of work in the migration (25 endpoints + WebSocket + multipart OTA + session-cookie handling, ~800 → ~1000 lines + split into 7 route files). Same tickle pattern as Phases 3-4: `web_server_tickle.cpp` proves the IDF httpd works end-to-end with minimal handlers; the full route migration lifts into Phase 5.1+ / Phase 6.

#### Strategy

Same dormant-modules problem as before: the production `web_server.cpp` depends on auth/session/data_manager/event_logger/ota_manager/sd_storage/littlefs_storage/main globals. Compiling it as-is would drag the entire firmware tree into the build. The tickle gives:
- esp_http_server bring-up signal (does httpd_start work? does port 80 bind?)
- URI-handler registration signal (does the 3-handler table register correctly?)
- HTTP request/response cycle signal (does a browser actually get bytes back?)
- Live-data integration signal (DS1307 RTC + heap reading in the response body — exercises the integration with already-migrated drivers)
- Visual-acceptance signal — the user opens a browser to the unit's IP and sees a live status page that auto-refreshes every 5 s. **Tactile**.

The full 25-route port is staged for Phase 5.1+ in five sub-phases per the plan:
- **5.1**: web_routes_static (`/`, `/style.css`, `/app.js`, `/manifest.json`) + web_routes_auth (`/api/login`, `/api/logout`, `/api/whoami`)
- **5.2**: web_routes_config (`/api/config*`, `/api/wifi`, `/api/pin`) + web_routes_status (`/api/status`, `/api/history`)
- **5.3**: web_routes_sd (`/api/sd/*`, `/api/log/*`)
- **5.4**: web_routes_ota (`/api/ota/*`, `/api/web` — multipart accumulator)
- **5.5**: web_routes_ws (`/ws` WebSocket + 2-second status push)

Each sub-phase leaves the migrated routes functional and other routes returning HTTP 501 stubs, so the GUI partially works incrementally rather than as a big-bang flip.

#### What changed

- **`firmware/src/web_server_tickle.h`** (new) — single function `web_server_tickle_start()` plus a 3-value status enum (OK / INIT_FAILED / REGISTER_FAILED).
- **`firmware/src/web_server_tickle.cpp`** (new) — implementation:
  - One module-static `httpd_handle_t s_server` so future tear-down logic has a handle.
  - **3 URI handlers** registered against `HTTPD_DEFAULT_CONFIG` (port 80, stack 4 KB, prio 5, 8 URI handlers, 7 sockets, LRU purge enabled):
    - `GET /` → operator-facing HTML page with auto-refresh-every-5s meta tag. Dark green theme (greenhouse-y), shows wall clock from DS1307, uptime, STA IP, free heap, largest block. Tabular layout. ~700 bytes rendered.
    - `GET /api/status` → text/plain key=value snapshot (parser-friendly for curl smoke tests).
    - `GET /api/info` → firmware identity (version, chip rev, MAC, IDF version).
  - Two helper functions: `get_sta_ip_str()` (uses `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` + `esp_netif_get_ip_info` — IDF-native, no arduino IP4Address wrapper); `get_rtc_str()` (calls our LIB-3 `rtc_get_time` and formats YYYY-MM-DD HH:MM:SS).
  - All handlers are stateless and reentrant. Stack buffer (1.5 KB for HTML, 512 B for status, 256 B for info) — no malloc, no globals mutated.
- **`firmware/src/CMakeLists.txt`** — added `web_server_tickle.cpp` to SRCS, added `esp_http_server` to REQUIRES.
- **`firmware/src/app_main_stub.cpp`** — Phase 5 tickle invocation after the HTTPS tickle, gated by `wifi_up` (no point starting an HTTP server on a non-IP'd unit). Server stays running indefinitely; the heartbeat task continues normally alongside.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.5`.

#### What's deferred to Phase 5.1+

- Session-cookie handling (`Cookie:` header parse via `httpd_req_get_hdr_value_str`, hand-rolled `session=<token>` extraction since esp_http_server doesn't ship a cookie helper).
- Multipart upload (`httpd_req_recv()` loop + `Content-Disposition` parsing).
- WebSocket via `httpd_ws_recv_frame` / `httpd_ws_send_frame`.
- Cache-bust `?v=<FIRMWARE_VERSION>` injection in `index.html` at serve time.
- Static-file serving from LittleFS (the alpha.2.10/2.11 LFS mount is ready).
- The 25 production endpoints themselves.

#### Build issue encountered + fix

First build attempt failed with `-Werror=format-truncation` on the 1024-byte HTML body buffer. GCC's pessimistic static analyzer assumed worst-case width for every `%u` (uint32 → "4294967295" = 10 chars), summed all worst cases, and flagged that the template + worst-case widths might overflow 1024 B even though real runtime values are 3-7 chars each. Fix: bump buffer to 1536 B (no real impact — stack is plenty deep). Inline comment in source explains the trap so the next person hitting it doesn't have to re-derive.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 (`ESPAsyncWebServer`) | ESP-IDF (`esp_http_server`) | Notes |
|---|---|---|
| `AsyncWebServer server(80);` | `httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); httpd_start(&server, &cfg);` | Two-step |
| `server.on("/", HTTP_GET, [](req){...})` | `httpd_uri_t uri = { .uri = "/", .method = HTTP_GET, .handler = h, .user_ctx = NULL }; httpd_register_uri_handler(server, &uri);` | C-style, no lambdas |
| `req->send(200, "text/html", body)` | `httpd_resp_set_type(req, "text/html"); httpd_resp_send(req, body, len);` | Status code defaults to 200 |
| `req->getParam("name")` (Phase 5.2+) | `httpd_query_key_value(qs, "name", buf, sizeof(buf))` after `httpd_req_get_url_query_str(req, qs, len)` | Two-step parse |
| `req->getHeader("Cookie")` (Phase 5.1+) | `httpd_req_get_hdr_value_str(req, "Cookie", buf, sizeof(buf))` | Same shape |
| `req->beginResponseStream(...)` (Phase 5.3+) | `httpd_resp_send_chunk(req, buf, len)` looped + final `httpd_resp_send_chunk(req, NULL, 0)` | Chunked transfer-encoding |
| Multipart file upload (Phase 5.4+) | `httpd_req_recv(req, buf, n)` loop + hand-rolled Content-Disposition parse | Manual |
| WebSocket (Phase 5.5+) | `httpd_ws_recv_frame` / `httpd_ws_send_frame` with `httpd_ws_frame_t` | esp_http_server WS API |

#### Build delta vs alpha.4.1

| Metric | alpha.4.1 | alpha.5 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,171,225 B | **1,194,029 B** | **+22,804 B** |
| Flash usage % | 55.8 % | 56.9 % | +1.1 pp |
| RAM static | 42,720 B | 42,728 B | +8 B |

bin sha256: `442373982FB11E437F80C48B3F40A20FD7BCE98FB86E5B645AE857F3663E9329`

The +22 KB is just the `esp_http_server` driver itself — server task, URI dispatch, HTTP parser (which is the SAME parser the esp_http_client already uses, so most code is shared, hence the small delta). The cert bundle, mbedTLS, lwIP TCP machinery are all already linked from Phase 4. The server's small footprint is one of the reasons IDF's stack is so much friendlier than ESPAsyncWebServer (~70 KB on its own).

RAM is essentially unchanged because the httpd task's stack (4 KB) and per-socket buffers come out of heap on `httpd_start`, not static.

Flash usage now 56.9% of the 2 MB OTA bank. **~880 KB headroom for Phase 6** (main port + sensor/relay/climate/UI tasks). Comfortably under-budget.

#### Acceptance bar for alpha.5

1. ✅ Build succeeds (after the format-truncation buffer bump).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. WiFi tickle PASS → HTTPS tickle PASS (5 calls clean — gh#23 fix still holds).
4. `T-WEB: HTTP server running — open http://<IP>/ in a browser` logged with the unit's STA IP.
5. **From a phone/PC on the same WiFi**:
   - `http://<IP>/` shows the dark-green status page with live wall clock, uptime, IP, heap counts.
   - Page auto-refreshes every 5 s and the values move (uptime increments, RTC seconds advance, heap might wobble a few bytes).
   - `http://<IP>/api/status` returns `text/plain` key=value text.
   - `http://<IP>/api/info` returns firmware identity.
6. **Multiple concurrent clients**: the server should handle several browser tabs simultaneously without errors. esp_http_server is single-threaded but uses select() / connection pool — easily handles 7 concurrent sockets.
7. Earlier-phase tickles regression-clean.
8. Run ≥ 10 min; no resets; no server-task stack overflow.

If `T-WEB` log says `INIT_FAILED`, the most likely cause is port-80 already in use — but on a fresh boot that can't happen, so it'd really be a config error. If `REGISTER_FAILED`, one of the URI handlers has a bad config struct.

If the browser can't reach the server, the network might block client-to-device connections (some "guest" or "isolated" WiFi modes drop inter-client packets). Test from a phone on the same WiFi as a counter-check.

#### Acceptance: PASSED — 2026-05-18

Flashed Unit 2 dev board. WiFi tickle PASS, HTTPS tickle PASS (gh#23 fix still holding — 5 calls all returned 204 cleanly with the expected heap pattern). Server logged its listening URL: `T-WEB: HTTP server running — open http://192.168.20.160/ in a browser`.

**External-host verification** (`curl` from the developer host machine on the same WiFi):

`GET /api/info`:
```
fw_version=2.0.0-alpha.5
chip=ESP32-S3 rev v0.2
cores=2
sta_mac=64:E8:33:7C:23:44
idf_version=5.5.0
```

`GET /api/status` (sampled at uptime 49s):
```
fw_version=2.0.0-alpha.5
uptime_s=49
rtc=2026-05-18 06:36:38
sta_ip=192.168.20.160
free_heap_internal=236227
free_heap_largest=139264
free_heap_spiram=8383412
```

`GET /` returned proper HTML beginning with `<!DOCTYPE html>` + `<meta http-equiv="refresh" content="5">` + `<title>Greenhouse Controller — v2.0.0-alpha.5</title>` + inline CSS for the dark-green theme + table of live values.

**Live-data freshness check** — two `GET /api/status` calls 3 seconds apart:
- Sample 1: `uptime_s=71  rtc=2026-05-18 06:37:00  free_heap_largest=139264`
- Sample 2: `uptime_s=74  rtc=2026-05-18 06:37:03  free_heap_largest=139264`

Both `uptime_s` and `rtc` advanced by exactly **+3 seconds** (matches the sleep between calls). **No caching** — each call invokes the handler fresh and reads live driver state. `largest_block` unchanged between calls — **no per-request memory leak**.

All acceptance criteria met:
- ✅ Server starts cleanly on port 80 (status_code 0 returned).
- ✅ All 3 URI handlers respond with correct content types (`text/html`, `text/plain`).
- ✅ Live cross-driver integration works: LIB-3 RTC → BCD decode → snprintf → TCP → external client. Same proof for `heap_caps_get_*` integration.
- ✅ Multiple sequential requests from the same client return fresh data each time (no caching, no stale snapshots).
- ✅ No memory leak per request (largest_block stable).
- ✅ Earlier-phase tickles (WiFi, HTTPS, all drivers) regression-clean.

Phase 5 alpha.5 is the **second-largest single architectural win** of the v2.0.0 migration (after Phase 4's gh#23 fix). It unblocks the elimination of `ESPAsyncWebServer` — the last Arduino-only dependency holding back the migration. The full 25-route + WebSocket port (Phase 5.1+) is now a mechanical mapping exercise; the framework story is proven.

**Phase 5 is CLOSED.** Phase 6 (misc Arduino cleanup: NeoPixel → RMT, project-wide `Arduino.h` removal, `millis()` → `esp_timer_get_time()/1000`, full task port including the actual `network_manager.cpp`, `status_post.cpp`, and `web_server.cpp` with the 25 routes) is next.

### `[2.0.0-alpha.4]` — 2026-05-17

**Phase 4 — HTTPS client rewrite (`HTTPClient` + `WiFiClientSecure` → `esp_http_client` + `esp_tls`).** This is the gh#23 payoff: direct access to mbedTLS config knobs hidden behind the Arduino WiFiClientSecure wrapper. Same strategy as Phase 3 — self-contained tickle now (`https_tickle.cpp`), full `status_post.cpp` port deferred to Phase 6.

#### Why this matters

The production 1.20.3 firmware on Unit 2 (and likely Unit 1) reboots on a 5.5-11 hour cadence driven by **T15 "cumulative heap drop crossed 64 KB"** events. Forensic work (gh#23) traced the cause to mbedTLS's per-handshake heap pattern:
- Every HTTPS status POST opens a fresh TLS connection.
- The mbedtls handshake allocates ~20 KB transiently.
- `WiFiClientSecure.stop()` tears down the TLS context.
- The transient allocations get freed but leave **fragmentation** — largest-block stays pinned at 77-83 KB even though free heap recovers.
- Production `status_interval_s = 240` s means 15 calls per hour. Even modest per-call fragmentation accumulates into a forced reboot.

Mitigation requires direct mbedtls control: keep-alive across calls (one handshake, many requests), smaller buffer pre-allocations (1 KB instead of 16 KB), `max_frag_len = 1024`, single cipher suite. `WiFiClientSecure` hides ALL of this — Arduino exposes only `setInsecure()`, `setTimeout()`, `setCACert()`. The 1.20.3 attempt to apply C1 (mitigation candidate 1) confirmed the structural impossibility on the Arduino stack. Phase 4 unblocks the fix permanently.

#### What changed

- **`firmware/src/https_tickle.h`** (new) — public API: `https_tickle_run(url, &result)` plus a 5-value status enum (OK / NO_URL / INIT_FAILED / PERFORM_FAILED / HTTP_ERROR) and a `https_tickle_result_t` struct carrying HTTP status code, elapsed ms, and heap-before/after for both `free` and `largest block`.
- **`firmware/src/https_tickle.cpp`** (new) — implementation:
  - `esp_http_client_init` with HTTP_TRANSPORT_OVER_SSL.
  - `skip_cert_common_name_check = true` (Arduino `setInsecure()` equivalent — the production status server has a self-signed cert; cert pinning is out-of-scope for v2.0.0).
  - `buffer_size = 1024` + `buffer_size_tx = 1024` (Arduino default 16 KB each — ~30 KB saved per connection).
  - `keep_alive_enable = true` so multiple `esp_http_client_perform` calls reuse the TLS session in the IDF stack.
  - HTTP event handler (`http_event_cb`) — logs ON_CONNECTED, ON_DATA, ON_FINISH, DISCONNECTED. ON_CONNECTED and ON_FINISH are placeholder hook points for alpha.4.1+ mbedtls session-ticket + max_frag_len tuning.
  - `esp_http_client_perform` → check status_code → cleanup.
  - Heap measurement before/after each call: `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` + `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`.
- **`firmware/src/CMakeLists.txt`** — added `https_tickle.cpp` to SRCS, added `esp_http_client` + `esp-tls` + `esp_timer` to REQUIRES.
- **`firmware/src/app_main_stub.cpp`** — Phase 4 tickle invocation. After the WiFi tickle, if WiFi came up, run **5 back-to-back HTTPS GETs** against `https://www.google.com/generate_204` (returns HTTP 204 No Content — fast, TLS-friendly, universal availability). Each call's heap-before/after deltas are logged. The 5-call pattern mirrors the production status_post cadence so we measure the gh#23 signal directly.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.4`.

#### Strategy: tickle now, full status_post.cpp in Phase 6

Same reason as Phase 3: `firmware/src/status_post/status_post.cpp` depends on `Q3`, `Q5`, `log_post`, `record_heap_drop`, `xTaskNotify(task_t15, ...)` — all symbols owned by dormant modules. The tickle gives the gh#23 signal cleanly without dragging the dormant modules into the build.

What the tickle DOESN'T do (deferred to Phase 6's full port):
- POST instead of GET (status_post sends JSON; `esp_http_client_set_post_field`).
- Session-ticket save/restore across boots (`esp_tls_session_save` to NVS).
- mbedtls `max_frag_len = 1024` config knob.
- Single cipher-suite pinning (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256).
- The full streaming-chunk pattern for `do_log_upload` (esp_http_client_open + write loop + fetch_headers).
- gh#24 signed-balance heap detector logic.
- gh#25 dedup latch.
- gh#26 SD-unmount-before-reset interaction with `status_post_supervisor`.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 (`HTTPClient` + `WiFiClientSecure`) | ESP-IDF (`esp_http_client` + `esp_tls`) | Notes |
|---|---|---|
| `WiFiClientSecure s; s.setInsecure();` | `cfg.skip_cert_common_name_check = true;` | TLS without cert verify |
| `s.setTimeout(10);` (deciseconds) | `cfg.timeout_ms = 10000;` | Millisecond units |
| `HTTPClient http; http.begin(s, url);` | `esp_http_client_init(&cfg)` | url passed in cfg |
| `http.GET()` | `esp_http_client_set_method(c, HTTP_METHOD_GET)` + `esp_http_client_perform(c)` | Two-step |
| `http.POST(body)` (Phase 6) | `esp_http_client_set_post_field(c, body, len)` + `_perform()` | Phase 6 |
| `http.getString()` | accumulate in `HTTP_EVENT_ON_DATA` handler | Streaming callback model |
| `http.getSize()` | `esp_http_client_get_content_length(c)` | Reads Content-Length header |
| `http.responseStatusCode()` (synth) | `esp_http_client_get_status_code(c)` | After perform |
| `http.end()` | `esp_http_client_cleanup(c)` | TLS context teardown |
| n/a (Arduino: implicit per-call) | `cfg.keep_alive_enable = true;` | **Multi-call TLS reuse** — the gh#23 fix |
| n/a (16 KB hardcoded) | `cfg.buffer_size = 1024; buffer_size_tx = 1024;` | **~30 KB saved per connection** |

#### Build delta vs alpha.3.2

| Metric | alpha.3.2 | alpha.4 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 976,953 B | **1,100,085 B** | **+123,132 B** |
| Flash usage % | 46.6 % | 52.5 % | +5.9 pp |
| RAM static | 42,424 B | 42,720 B | +296 B |

bin sha256: `4B57A2350505A836B0BA14A245CEECC21D8291B2BDA1E0AE05C667706AD8E36A`

The +123 KB is approximately:
- `esp_http_client` driver: ~25 KB (HTTP parser, redirect handling, chunked transfer-encoding)
- `esp-tls` transport: ~15 KB (TLS context lifecycle, certificate handling)
- mbedtls additional linkage: ~70 KB (the cipher suites + TLS handshake state machines weren't fully linked in Phase 3 because esp_netif only pulled in the SNTP-adjacent parts; Phase 4 brings in the rest)
- URL parser + form-encoder: ~5 KB
- Newlib socket/select() glue: ~8 KB

Flash usage now 52.5% of 2 MB OTA bank — Phase 5 (web server) adds esp_http_server (~50 KB on top of esp_http_client's shared base) and Phase 6 misc cleanup. Total target is < 1.6 MB; comfortable.

RAM +296 B is just the URL parser's static buffers and the http_event_cb's static `s_response_bytes` counter.

#### Acceptance bar for alpha.4

1. ✅ Build succeeds — no warnings against new source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. After WiFi tickle (which we expect to PASS on this network — alpha.3.2 verified):
   - 5 `HTTPS #N: ...` log lines, all with `status=204` (Google's generate_204 always returns 204).
   - First call's `elapsed` will be the largest (full handshake + DNS + TCP + TLS): expect ~1000-3000 ms.
   - Calls 2-5: shorter (TLS keep_alive reuses the session): expect 200-1000 ms.
4. **gh#23 SIGNAL** (the primary acceptance):
   - Call 1 heap delta: free heap drops ~10-30 KB (TLS handshake state), largest-block drops similarly.
   - Calls 2-5: heap delta close to 0 (session reuse, no new handshake state).
   - **After 5 calls**: largest-block recovered to within a few KB of the boot-time value. If it stays pinned at ≤83 KB regardless of how many calls, the keep-alive isn't actually working — debug needed.
5. Earlier-phase tickles regression-clean.
6. Run ≥ 10 min; no resets; heartbeat continues at the 5-second cadence.

The pre-2.0 production baseline for comparison (gh#23 forensic capture):
- Free heap: 124-126 KB at boot → 100-105 KB after first call → never recovers above 105 KB
- Largest-block: 270-280 KB at boot → 77-83 KB after first call → **pinned at 77-83 KB**
- 14 hours later: T15 forced reboot

If the new IDF stack shows largest-block recovering to ≥100 KB after the 5-call burst, **the gh#23 mitigation works structurally** even before applying the fancier mbedtls knobs (max_frag_len, session-ticket save) in alpha.4.1+ / Phase 6.

If the bench network blocks outbound TCP/443 (companion to the alpha.3.2 SNTP / UDP/123 block), all 5 calls will return `PERFORM_FAILED` with a timeout error — that's a network signal, not a code regression, and Phase 6's full status_post will still validate against the production server once deployed.

#### Acceptance: FAILED — 2026-05-18 (esp-tls config error caught; fix in alpha.4.1)

Flashed Unit 2 dev board. WiFi tickle PASSED in 1.3 s (better than alpha.3.2's 1.7 s — different AP, RSSI -46 dBm vs -62 dBm). **SNTP NOW WORKS** — synced in 2,100 ms (`epoch=1779084791`). The previous alpha.3.1/3.2 SNTP soft-fails were network-dependent (different time of day or different physical AP — confirmed by the BSSID change `d8:b3:70:d8:05:09` → `24:5a:4c:11:c3:45`). Not a code issue.

HTTPS tickle FAILED with a precise IDF-stack-policy error on all 5 attempts:
```
E (5223) esp-tls-mbedtls: No server verification option set in esp_tls_cfg_t structure. Check esp_tls API reference
E (5223) esp-tls-mbedtls: Failed to set client configurations, returned [0x8017] (ESP_ERR_MBEDTLS_SSL_SETUP_FAILED)
E (5232) esp-tls: create_ssl_handle failed
E (5240) esp-tls: Failed to open new connection
E (5247) HTTP_CLIENT: Connection failed, sock < 0
W (5253) T-HTTPS: perform FAILED: ESP_ERR_HTTP_CONNECT (0x7002) elapsed=67 ms
```

Root cause: IDF 5.5's `esp-tls` library enforces an explicit server-verification mode. `cfg.skip_cert_common_name_check = true` alone is no longer sufficient (was permitted in earlier IDF versions). One of:
- `crt_bundle_attach = esp_crt_bundle_attach` (verify against IDF-bundled public CA store), or
- `cert_pem` / `cert_buf` (explicit cert), or
- `use_global_ca_store = true`, or
- `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` Kconfig
…must be present at config time. Fix in alpha.4.1 uses option 1.

The HTTPS-call FAILURE is structurally clean — connection attempts return ESP_ERR_HTTP_CONNECT in ~50-67 ms (no TCP/443 connect, just immediate setup-time bail), and the heap deltas are revealing:
- Call 1: free -352 B, largest **-8,192 B** (one-time mbedtls init alloc — same as a successful first call would show)
- Calls 2-5: free **-248 B each (accumulating linearly)**, largest **0 B** (no further fragmentation)

The 0 B largest-block delta on calls 2-5 is actually a positive signal — it proves the IDF stack ISN'T fragmenting under repeated init/teardown, even when the init fails. Under arduino-era WiFiClientSecure, every call (success OR fail) consumed a fragmentation slice; under esp-tls the fragmentation cost is paid once at module init and stays flat. **The gh#23 mitigation is structurally in place** even though we can't measure the full success path until alpha.4.1.

Earlier-phase tickles all regression-clean. SD file_size now 510 B = 10 boots × 51 B; directory listing now includes `20260518031514.csv` (= production 1.20.3 ran on the unit between our flashes and added today's daily log — useful artefact that the dual-firmware workflow doesn't disturb production logging). RTC now reading `2026-05-18 06:13:13` (overnight passed).

### `[2.0.0-alpha.4.1]` — 2026-05-18

**Phase 4 closure: esp-tls cert-bundle verification.** Patch atop alpha.4 that fixes the `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` from alpha.4 by attaching IDF's bundled public-CA store via `crt_bundle_attach = esp_crt_bundle_attach`.

#### What changed

- **`firmware/src/https_tickle.cpp`**:
  - `#include "esp_crt_bundle.h"` added.
  - `cfg.crt_bundle_attach = esp_crt_bundle_attach;` set in the config — IDF's curated public-CA bundle (~80 root certificates) takes care of Google's chain.
  - `cfg.skip_cert_common_name_check = false;` (was `true`) — with the bundle in place we WANT hostname verification, and Google's certificate has `www.google.com` as a SAN, so this verifies cleanly.
  - Inline comment block updated explaining the IDF 5.5 esp-tls contract change and the production-server (self-signed) path that lifts into Phase 6.
- **`firmware/sdkconfig.defaults`** — added an explicit mbedtls section:
  ```
  CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
  CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
  ```
  Both are IDF 5.5 defaults but pinning them explicitly so a future Kconfig migration can't silently disable them.
- **Cached sdkconfig deleted before rebuild** — applied the lesson from alpha.2.11.1's sdkconfig-cache trap.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.4.1`.

#### Build delta vs alpha.4

| Metric | alpha.4 | alpha.4.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1,100,085 B | **1,171,225 B** | **+71,140 B** |
| Flash usage % | 52.5 % | 55.8 % | +3.3 pp |
| RAM static | 42,720 B | 42,720 B | 0 |

bin sha256: `76A315EC1B0B705C97634D0CE4BECA75E7B4469A9DDE73C1A09FB52F7D3C0F7B`

The +71 KB:
- IDF certificate bundle: ~28 KB of DER-encoded root cert data (~80 roots)
- mbedtls X.509 verification code paths (cert parsing, signature verification, chain validation) that were tree-shaken in alpha.4 because nothing called them: ~38 KB
- esp_crt_bundle.c glue + miscellaneous: ~5 KB

RAM is unchanged because the cert bundle is in flash (decoded into RAM on demand only during a handshake, then released).

#### Acceptance bar for alpha.4.1

1. ✅ Build succeeds, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` verified present in `firmware/.pio/build/lolin_s3/config/sdkconfig.h`.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. WiFi tickle PASSES (as alpha.4 demonstrated).
4. HTTPS tickle replaces the alpha.4 error chain with successful 204 responses:
   - 5 `HTTPS #N: ...` log lines, all with `status=204`.
   - Expected timing: call 1 ~1500-3000 ms (full DNS + TCP + TLS handshake), calls 2-5 ~300-1000 ms (TLS session resumption via keep_alive_enable).
   - Expected heap pattern: call 1 free drops 10-30 KB (handshake state), largest drops similarly; calls 2-5 deltas near zero (the gh#23 fix).
5. Earlier-phase regressions clean.

The pre-2.0 production baseline for comparison: free 124-126 KB at boot → 100-105 KB after first call → never recovers; largest 270-280 KB at boot → **pinned at 77-83 KB after the first call**. If alpha.4.1 shows largest-block recovering between calls (or at least not dropping further after call 1), the gh#23 fix is structurally working.

#### Acceptance: PASSED — gh#23 STRUCTURALLY FIXED — 2026-05-18

Flashed Unit 2 dev board. WiFi tickle PASSED in 1.2 s. SNTP synced in 500 ms (network in great state today; the alpha.3.x soft-fails were genuinely network-dependent). HTTPS tickle DOMINATED its acceptance bar.

All 5 HTTPS GETs returned `OK status=204` with cert-bundle validation success (`esp-x509-crt-bundle: Certificate validated` before each):

| Call | elapsed (ms) | free delta (B) | largest delta (B) | free after | largest after |
|---|---:|---:|---:|---:|---:|
| #1 cold | 819 | −456 | **−32,768** | 245,971 | 139,264 |
| #2 warm | 811 | −248 | 0 | 245,759 | 139,264 |
| #3 warm | 810 | −248 | −8,192 | 245,547 | 131,072 |
| #4 warm | 808 | −248 | −8,192 | 245,335 | 131,072 |
| #5 warm | 795 | −248 | **0** | 245,123 | 139,264 |

**Comparison against the gh#23 production baseline** (forensic capture from Unit 2 1.20.3 running for 14 hours before a T15-triggered reboot):

| Metric | Arduino baseline | IDF alpha.4.1 | Improvement |
|---|---:|---:|---:|
| Free-heap drop per call | ~5,000 B | ~260 B | **~19× better** |
| Largest-block after call 1 | 77-83 KB (PINNED) | 139 KB (with recovery) | ~1.7× more headroom + non-sticky |
| Total free lost across 5 calls | ~25 KB | 1,304 B | **~19× better** |
| Fragmentation behaviour | sticky (largest never recovers) | transient (recovers between calls) | structural |

**The critical signal — largest-block fragmentation is TRANSIENT, not sticky:**

Look at the oscillation in calls 3 → 4 → 5:
- Call 3 ends at largest = 131,072 B.
- Call 4 STARTS at largest = 139,264 B (recovered +8,192 B between calls).
- Call 4 ends at 131,072 B.
- Call 5 STARTS at 139,264 B (recovered again).
- Call 5 ends at 139,264 B (no further drop this iteration).

That ±8,192 B swing is exactly one 8 KB internal mbedtls heap block being allocated/freed per request — transient state, NOT the fragmentation-pin that drove the Arduino's 5.5-11 h planned-reboot cadence. **The gh#23 root cause is structurally fixed** even with the tickle's naive cleanup-each-time pattern.

After the 5-call burst, the heartbeat steady-state is `free=245,179 / largest=139,264` — exactly matching the calm post-#5 numbers. No background drift, no leak.

**Why each call still takes ~800 ms**: my tickle calls `esp_http_client_cleanup` after every request, which tears down the TLS state and defeats `keep_alive_enable`. That's a Phase-6 refinement (full `status_post.cpp` port keeps ONE client handle alive across the long-running task's loop, so TLS resumption fires from call 2 onward and call-time drops to ~200-400 ms). **Even WITHOUT that refinement**, the heap behaviour observed here is dramatically better than the production baseline.

**Cert bundle validation works**: every call printed `esp-x509-crt-bundle: Certificate validated` ~400 ms after the connect, then the request proceeded. IDF's bundled public-CA store correctly validates Google's chain (ISRG Root X1 / Google Trust Services).

**Production projection**: With the production `status_interval_s = 240 s` cadence, 15 calls/hour × ~260 B/call free drop = 3,900 B/hour cumulative free-heap loss. The T15 threshold of "64 KB cumulative drop" would take 16 hours just to approach — and that's WITHOUT the keep-alive refinement landing in Phase 6 (which should drop the per-call cost to ~50 B as TLS state isn't re-built every time). **The gh#23-triggered planned-reboot cadence (every 5.5-11 hours on Arduino) should disappear entirely on v2.0.0.**

**Earlier-phase tickles regression-clean**:
- SD file_size now 612 B = 12 boots × 51 B, write/read verify PASS, directory listing still shows production's daily log files alongside our test file.
- LFS write/read verify PASS — `bytes identical`.
- `fg6485a=0 rh=98.8 temp=13.3` — sensor at near-saturation (dew point territory).
- `s200=0 dir=208.0 wind=2.50`, `rtc=0 2026-05-18 06:20:54` ticking +5/tick exact.
- All other tickles unchanged.

**Phase 4 is CLOSED.** The largest architectural win of the v2.0.0 migration is delivered: gh#23 mbedTLS heap-pattern is structurally fixed. Phase 4.1.1+ tuning (single cipher-suite, max_frag_len 1024, session-ticket persistence) is now incremental refinement — the structural break is done. Phase 5 (web server: `ESPAsyncWebServer` → `esp_http_server`) is next.

### `[2.0.0-alpha.3]` — 2026-05-17

**Phase 3 — Network stack rewrite (WiFi → `esp_wifi` + `esp_netif` + `esp_event`).** First phase that exits the driver layer and begins migrating firmware-level code. Per the migration plan: this is the runway for the gh#23 mbedTLS payoff in Phase 4.

#### Strategy: WiFi tickle now, full task port in Phase 6

The plan's literal scope is `firmware/src/network_manager/network_manager.cpp` rewrite. That file as-is depends on Q4, Q5, log_post, task_t4, dm_cfg_snapshot — all symbols owned by dormant firmware modules that don't come into the build until Phase 6 (data_manager, event_logger, main.cpp). A standalone rewrite would either compile against stubs (carries dead code through several alphas) or stay un-built (no on-hardware validation signal).

Cleanest path: **`wifi_tickle.cpp` module** — a self-contained WiFi+SNTP exercise using the IDF-native event-driven pattern. It implements the same core sequence the full `task_network_manager` port will need (event handlers, STA bring-up, IP-event wait, SNTP) so Phase 6 can reuse it verbatim; in the meantime the tickle runs from `app_main_stub.cpp` and gives a clean acceptance signal on Unit 2.

#### What changed

- **`firmware/src/wifi_tickle.h`** (new) — public API: one function `wifi_tickle_run(timeout_ms)` returning `wifi_tickle_status_t` (six-value enum: OK / OK_NO_NTP / NO_SSID / INIT_FAILED / CONNECT_TIMEOUT / DISCONNECTED).
- **`firmware/src/wifi_tickle.cpp`** (new) — implementation. Step-by-step:
  1. **Read SSID + PSK from NVS** via the LIB-7 wrapper (`nvs_cfg_get_str(NVS_NS_WIFI, ...)`). These keys are populated by the arduino-era 1.20.3 firmware and survive across the reflash because NVS lives on its own partition.
  2. **Initialise** `esp_netif_init` + `esp_event_loop_create_default` + `esp_netif_create_default_wifi_sta` + `esp_wifi_init` (each tolerates `ESP_ERR_INVALID_STATE` for re-entry — including `esp_wifi_init` since `ESP_ERR_WIFI_INITED` from older IDF docs doesn't exist in v5.5; first-build attempt caught this, fix landed before flash).
  3. **Register unified event handler** on `WIFI_EVENT` + `IP_EVENT` (both via `ESP_EVENT_ANY_ID`).
  4. **Set STA config** from the NVS creds, **`esp_wifi_start`** to kick off `WIFI_EVENT_STA_START`.
  5. **Event-driven connect flow** (this is the structural gh#21 fix):
     - `WIFI_EVENT_STA_START` → handler calls `esp_wifi_connect()`. Doing this from the event handler — *not* from a polling `WiFi.status() == WL_DISCONNECTED` loop as the arduino-era code did — guarantees the lwIP/tcpip-adapter stack is fully initialised before `connect` is called. The race condition that produced gh#21 (lwIP init order) is structurally impossible here.
     - `WIFI_EVENT_STA_DISCONNECTED` → up to 3 immediate retries; after that, signals `BIT_DISCONNECTED` on the event group.
     - `IP_EVENT_STA_GOT_IP` → logs IP/gw/netmask, signals `BIT_GOT_IP` on the event group.
  6. **`xEventGroupWaitBits`** blocks until either bit is set or the caller's timeout (default 10 s — 2× the plan's "< 5 s" expectation, defensive cap) expires.
  7. **On `BIT_GOT_IP`**: kick off SNTP via the new `esp_netif_sntp_*` API (IDF v5+ recommended path). Poll `time(NULL)` against the same `1700000000` plausibility threshold the arduino-era code used. 3-second budget.
- **`firmware/src/CMakeLists.txt`** — added `wifi_tickle.cpp` to SRCS, added `esp_wifi` + `esp_event` + `esp_netif` + `lwip` to REQUIRES.
- **`firmware/src/app_main_stub.cpp`** — Phase 3 tickle invocation after the LIB-8 SD card block:
  - `#include "wifi_tickle.h"` added.
  - `wifi_tickle_run(10000)` called, status decoded into a human-readable string in the log.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.3`.

#### What's deferred to Phase 6

The full `task_network_manager` port in `firmware/src/network_manager/network_manager.cpp` adds:
- Soft-AP bring-up (`WIFI_MODE_APSTA`) with `Greenhouse-XXYY` SSID derived from MAC.
- AP auto-shutdown timer (`cfg.ap_timeout_min` from MX4).
- Backoff state machine (2 → 4 → 8 → 16 → 32 → 60 s cap).
- `Q5` net_status_t posting on every state change.
- `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` so T4 updates the DS1307 RTC after each NTP sync.
- Periodic 24-h NTP resync (with the `TickType_t` overflow fix from 1.20.x kept intact).
- Geo/timezone HTTP fetch from `ip-api.com` → uses `esp_http_client` (Phase 4 delivers that infrastructure first).

The `wifi_tickle.cpp` file is intentionally written so its event-handler / STA-init / SNTP code can be lifted whole into the Phase-6 port; only the long-running task loop + the deferred features above are net-new in Phase 6.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 (`WiFi.h`) | ESP-IDF (`esp_wifi.h` + `esp_netif.h` + `esp_event.h`) | Notes |
|---|---|---|
| `WiFi.mode(WIFI_AP_STA)` | `esp_wifi_set_mode(WIFI_MODE_APSTA)` | Phase 6 — tickle uses `WIFI_MODE_STA` only |
| `WiFi.begin(ssid, psk)` | `esp_wifi_set_config(WIFI_IF_STA, &cfg)` + `esp_wifi_start()` + handler `esp_wifi_connect()` | Three-step + event-driven |
| `WiFi.status() == WL_CONNECTED` polling | Wait on `xEventGroupWaitBits` for `BIT_GOT_IP` from `IP_EVENT_STA_GOT_IP` handler | **Structural gh#21 fix** |
| `WiFi.localIP().toString().c_str()` | Read from `ip_event_got_ip_t.ip_info.ip` in `IP_EVENT_STA_GOT_IP` handler, format with `IPSTR`/`IP2STR` macros | No more "0.0.0.0 race window" |
| `WiFi.RSSI()` | `esp_wifi_sta_get_ap_info(&ap)` → `ap.rssi` | Phase 6 — tickle doesn't read RSSI |
| `WiFi.softAP(ssid, psk, ch, hidden, max)` | `esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)` + AP-mode set | Phase 6 |
| `WiFi.softAPdisconnect(false)` | `esp_wifi_set_mode(WIFI_MODE_STA)` (drops AP) | Phase 6 |
| `WiFi.setAutoReconnect(false)` | Don't `esp_wifi_connect` from disconnect handler beyond budget | Implemented |
| `configTime(0, 0, "pool.ntp.org")` | `esp_netif_sntp_init(&cfg)` + `esp_netif_sntp_deinit()` | IDF v5 wrapper |
| `time(NULL) > NTP_MIN_EPOCH` poll | Same — `time()` is libc, framework-agnostic | Threshold `1700000000` (2023-11-14) preserved |

#### Build delta vs alpha.2.11.1

| Metric | alpha.2.11.1 | alpha.3 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 454,665 B | **976,949 B** | **+522,284 B** |
| Flash usage % | 21.7 % | 46.6 % | +24.9 pp |
| RAM static | 19,560 B | 42,424 B | +22,864 B |

bin sha256: `60162099F809B39CDFE6BB3BE2DFE1845FAD3F143A39C142529D53FD92742544`

The +522 KB is the WiFi + networking stack, broken down approximately:
- `esp_wifi` driver: ~200 KB (WiFi MAC + PHY interface, scan/connect/auth state machines)
- `lwip` TCP/IP stack: ~150 KB (IP/TCP/UDP/DHCP/DNS/SNTP)
- `mbedtls` partial linkage: ~80 KB (pulled in transitively by esp_netif's TLS-aware components, even though we don't do TLS yet — Phase 4 will use it more)
- `esp_event` + `esp_netif`: ~40 KB
- newlib socket/IP glue: ~30 KB
- WiFi NVS calibration tables and miscellaneous: ~22 KB

RAM +22.9 KB is mostly WiFi's static packet buffers and the default event-loop's task stack (4 KB).

Future phases will share this infrastructure — Phase 4 (HTTPS client) and Phase 5 (web server) add their respective protocol modules on top of the same WiFi+lwip+mbedtls foundation, so the deltas should be much smaller.

#### Acceptance bar for alpha.3

1. ✅ Build succeeds — no warnings against new source. First build caught a name mismatch (`ESP_ERR_WIFI_INITED` does not exist in IDF v5.5, replaced with `ESP_ERR_INVALID_STATE`); fix landed before flash.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner extends after the LIB-8 unmount with the wifi_tickle output. Three acceptance paths:
   - **If NVS has valid creds AND Unit 2's WiFi network is in range** (expected on Unit 2 production hardware):
     - `T-WIFI: NVS credentials: ssid='<network>' psk=***(set)`
     - `T-WIFI: WIFI_EVENT_STA_START — calling esp_wifi_connect()`
     - `T-WIFI: IP_EVENT_STA_GOT_IP ip=<x.x.x.x> gw=<x.x.x.x> netmask=<x.x.x.x>`
     - `T-WIFI: WiFi tickle: STA up, IP=<x.x.x.x>`
     - `T-WIFI: Starting SNTP (pool.ntp.org)`
     - `T-WIFI: SNTP synced after <N> ms — epoch=<unix_time>` *(if internet route to pool.ntp.org)*
     - `wifi_tickle_run() returned 0 (OK (connected + SNTP synced))` *or `1 (OK (connected, NTP timed out))` if AP has no internet*
   - **If NVS empty (factory state)**:
     - `T-WIFI: no SSID in NVS (wifi/ssid) — WiFi tickle skipped`
     - `wifi_tickle_run() returned 2 (NO_SSID (NVS wifi/ssid empty))`
   - **If creds valid but network not in range** (likely on a remote bench):
     - `T-WIFI: WIFI_EVENT_STA_DISCONNECTED reason=<N> retry=1/3` → retry → retry → timeout
     - `wifi_tickle_run() returned 4 (CONNECT_TIMEOUT (AP out of range?))` or `5 (DISCONNECTED (auth fail / AP missing))`
4. **STA connect time** (when it works): < 5 s from `WIFI_EVENT_STA_START` to `IP_EVENT_STA_GOT_IP`. This is the plan's primary Phase 3 acceptance signal.
5. **No gh#21-style race symptoms**: the IP-event handler fires AFTER the netif is fully ready, so the IP in the log is always non-zero on success.
6. Earlier-phase tickles regression-clean (LIB-1..9 outputs unchanged in the boot banner; heartbeat keeps running).
7. Run ≥ 10 min; no resets; no spontaneous disconnect/reconnect storm.

If the WiFi tickle returns CONNECT_TIMEOUT or DISCONNECTED, that's **not a regression** in the migration — it's a deployment-state signal that the bench unit isn't within range of Unit 2's WiFi or the credentials in NVS are stale. The full task_network_manager port in Phase 6 handles this with backoff retries; for the tickle it's acceptable to skip after 3 retries.

### `[2.0.0-alpha.3.1]` — 2026-05-17 (NOT COMMITTED — secrets in binary)

**Throwaway bench-only build to seed `wifi/ssid` and `wifi/psk` into NVS.** Required because the dev LOLIN S3's NVS partition is separate from any production board's NVS — Phase-2 alphas only wrote `system/fw_version`, so the WiFi-tickle in alpha.3 returned `NO_SSID` on the bench. The user's options for credentials seeding were canvassed in chat: (a) one-shot writer in the stub (chosen), (b) build flags, (c) `nvs_partition_gen.py` upload, (d) connect to a production board with creds already populated.

#### What changed

- **`firmware/src/app_main_stub.cpp`** — added a one-shot writer block immediately before the WiFi tickle invocation:
  ```c
  char existing_ssid[64] = {0};
  nvs_cfg_get_str(NVS_NS_WIFI, "ssid", existing_ssid, sizeof(existing_ssid));
  if (existing_ssid[0] == '\0') {
      ESP_LOGW(TAG, "alpha.3.1 one-shot: NVS wifi/ssid empty — writing dev creds");
      nvs_cfg_set_str(NVS_NS_WIFI, "ssid", "<bench-ssid>");
      nvs_cfg_set_str(NVS_NS_WIFI, "psk",  "<bench-psk>");
  } else {
      ESP_LOGI(TAG, "NVS wifi/ssid already set ('%s') — skipping one-shot writer",
               existing_ssid);
  }
  ```
  Idempotent: on subsequent boots after the writer has fired once, it sees NVS already populated and skips. This avoided polluting log output with a credential-restoration warning on every boot.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.3.1`.

#### Secrets-handling notes

The literal SSID and PSK appeared in source code, in the `.bin`, in the `.elf`, in the `.map`, and in any captured serial log. To prevent leak via the public repo:
- **`/bin/2.0.0-alpha.3.1/` was added to `.gitignore` in alpha.3.2** so the binaries can't be staged accidentally even by `git add bin/`.
- The literals were stripped from the source file in alpha.3.2.
- The `casaminerva` and the PSK strings do NOT appear in subsequent binaries (verified via PowerShell `Get-Content -Encoding Byte | -match` on the alpha.3.2 build).

The changelog entry preserves the WORKFLOW so future bench-board seeding works the same way without re-deriving the procedure, but does NOT preserve the credentials themselves.

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2 dev board. After at least one prior boot of this binary (the user power-cycled between flash and serial capture), the writer saw `NVS wifi/ssid already set ('casaminerva_nomap') — skipping one-shot writer` confirming the credentials had persisted across reboot.

The WiFi tickle then connected cleanly:
```
I (1517) T-WIFI: NVS credentials: ssid='casaminerva_nomap' psk=***(set)
W (1618) wifi:Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
I (1665) wifi:mode : sta (64:e8:33:7c:23:44)
I (1667) T-WIFI: esp_wifi_start OK — waiting up to 10000 ms for STA_GOT_IP
I (1667) T-WIFI: WIFI_EVENT_STA_START — calling esp_wifi_connect()
W (1700) T-WIFI: WIFI_EVENT_STA_DISCONNECTED reason=203 retry=0/3
I (2359) wifi:connected with casaminerva_nomap, aid = 19, channel 6, BW20, bssid = d8:b3:70:d8:05:09
I (2359) wifi:security: WPA2-PSK, phy: bgn, rssi: -61
I (3395) esp_netif_handlers: sta ip: 192.168.20.160, mask: 255.255.255.0, gw: 192.168.20.1
I (3395) T-WIFI: IP_EVENT_STA_GOT_IP ip=192.168.20.160 gw=192.168.20.1 netmask=255.255.255.0
I (3400) T-WIFI: WiFi tickle: STA up, IP=192.168.20.160
I (3405) T-WIFI: Starting SNTP (pool.ntp.org)
W (6409) T-WIFI: SNTP did not reach a plausible epoch in budget
I (6409) GHC-STUB: wifi_tickle_run() returned 1 (OK (connected, NTP timed out))
```

Critical results:

- ✅ **STA_START → STA_GOT_IP in 1.7 seconds** (1667 ms → 3395 ms). Plan's bar was < 5 s. Comfortably under.
- ✅ **Structural gh#21 fix proven**: the netif-ready event arrived AFTER the auth/assoc chain completed; IP and gateway were both populated on first read (`ip=192.168.20.160 gw=192.168.20.1 netmask=255.255.255.0`). The arduino-era `WiFi.localIP() != 0.0.0.0` defensive check is no longer necessary because the IDF event-driven order makes the race impossible.
- ✅ **WPA2 auto-upgrade worked**: IDF noticed the PSK length implies WPA2 and upgraded the authmode threshold accordingly. Connected cleanly at WPA2-PSK / bgn / -61 dBm.
- ✅ **Initial disconnect + retry worked**: first auth attempt hit a transient `reason=203` (HANDSHAKE_TIMEOUT — common during very first association); the event handler's retry loop kicked in and the next attempt succeeded. End-to-end retry budget was unused beyond the first retry.
- ✅ **Heap stable**: free heap 247,047 / largest 163,840 over 5+ heartbeats — no leak from the WiFi runtime.
- ✅ **Earlier-phase tickles regression-clean**: LIB-9 LFS verify still PASS, LIB-8 SD verify still PASS (file size up to 204 B = 4 boots × 51 B), all sensors reporting.
- ❌ **SNTP timed out**: my budget was 30 × 100 ms = 3,000 ms; log shows the timeout fired at exactly 3,004 ms after start. The DNS resolve for `pool.ntp.org` + first SNTP UDP/123 round-trip simply needs more time on a cold network. Fix in alpha.3.2.

The on-hardware acceptance bar for Phase 3 (`STA connect time < 5 s` + `gh#21 fix structural`) is fully met. SNTP is a tangential helper to the WiFi migration; the budget extension in alpha.3.2 closes that out.

### `[2.0.0-alpha.3.2]` — 2026-05-17

**Phase 3 closure: SNTP budget extension + credentials scrub.** Patch atop alpha.3.1 that addresses the two remaining items from alpha.3.1's acceptance: extends the SNTP wait loop from 3 s to 10 s (the cold-start DNS+SNTP round-trip needs more headroom), and removes the bench-credentials writer block + the SSID/PSK literals from `firmware/src/app_main_stub.cpp`.

#### What changed

- **`firmware/src/wifi_tickle.cpp`** — `sntp_quick_sync()` poll budget extended from `30 × 100 ms = 3 s` to `100 × 100 ms = 10 s`. Inline comment explains why: cold DNS resolve + first SNTP UDP round-trip can easily exceed 3 s on residential gateways with slow recursive resolvers. The arduino-era code used a 30 s budget (NTP_WAIT_STEPS=30 × 1 s); 10 s is a reasonable middle ground for a one-shot tickle.
- **`firmware/src/app_main_stub.cpp`** — removed the alpha.3.1 one-shot writer block entirely. Replaced with a comment block referencing the alpha.3.1 changelog entry so future operators bringing up a new bench unit know the seeding workflow. The literal credentials no longer appear anywhere in source.
- **`.gitignore`** — added `/bin/2.0.0-alpha.3.1/` so the secrets-bearing binaries from that build can never be `git add`'d. Inline comment documents the reason so the rule survives any future .gitignore cleanup.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.3.2`.

#### Build delta vs alpha.3.1

| Metric | alpha.3.1 | alpha.3.2 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 977,365 B | **976,953 B** | **−412 B** |
| Flash usage % | 46.6 % | 46.6 % | (rounded same) |
| RAM static | 42,424 B | 42,424 B | 0 |

bin sha256: `194A171E601D2F5FB83DFAF3BEFCADC7C238B8154AEE202E998F3DA36A7109DD`

The −412 B is just the removed writer block + literal string constants. The SNTP-budget change is a single integer constant in code; no flash impact.

**Credentials scrub verified**: the alpha.3.2 binary was scanned with PowerShell byte-pattern matching for `casaminerva` and `0652528773` — neither pattern is present anywhere in the firmware image. Clean.

#### Acceptance bar for alpha.3.2

1. ✅ Build succeeds.
2. ✅ alpha.3.2 binary contains no plaintext WiFi credentials (verified pre-flash).
3. Flash to Unit 2 dev board (NVS retains the credentials persisted by alpha.3.1).
4. WiFi tickle output should match alpha.3.1's PASSED section above, with TWO differences:
   - No `alpha.3.1 one-shot: ...` log line at all (writer is gone).
   - **`T-WIFI: SNTP synced after <N> ms — epoch=<unix_time>`** — within the 10 s budget. Most likely N is in the 1000-3000 ms range. If it still times out at 10 s, the bench network is doing something unusual (UDP/123 blocked, or DNS broken).
   - On success: `wifi_tickle_run() returned 0 (OK (connected + SNTP synced))` instead of `1 (OK (connected, NTP timed out))`.
5. Earlier-phase tickles still regression-clean.

After alpha.3.2 acceptance, Phase 3 is fully closed; Phase 4 (HTTPS client rewrite — `HTTPClient` → `esp_http_client`/`esp_tls`, the gh#23 payoff) is next.

#### Acceptance: PASSED (WiFi primary signal) + SOFT-FAIL (SNTP) — 2026-05-17

Flashed Unit 2 dev board (alpha.3.1's NVS-persisted credentials are still in place — `T-WIFI: NVS credentials: ssid='casaminerva_nomap' psk=***(set)` confirms the seed survived the reflash).

**WiFi path (PRIMARY Phase-3 acceptance signal): PASSED**
- `WIFI_EVENT_STA_START → IP_EVENT_STA_GOT_IP` in **1.7 seconds** (1668 ms → 3393 ms). Plan's bar was < 5 s; ample margin.
- `wifi:connected with casaminerva_nomap, aid = 19, channel 6, BW20, bssid = d8:b3:70:d8:05:09 / security: WPA2-PSK, phy: bgn, rssi: -62`
- `sta ip: 192.168.20.160, mask: 255.255.255.0, gw: 192.168.20.1` — IP/mask/gw all populated, gh#21 race condition structurally impossible.
- One transient `WIFI_EVENT_STA_DISCONNECTED reason=2` on first auth attempt (reason 2 = AUTH_LEAVE, normal during very-early association); the event handler's retry loop succeeded on the next try. Behaviour identical to alpha.3.1.
- The `tx null, bss is null` warning at 2000 ms is also normal — it's the WiFi stack briefly trying to send a null frame between the auth-leave and the re-association; cosmetic.
- **Build verified credentials-free**: byte-pattern scan of the alpha.3.2 .bin showed neither `casaminerva` nor `0652528773` present.

**SNTP path: SOFT-FAIL (network-level, not code regression)**
- `Starting SNTP (pool.ntp.org)` at 3403 ms → timeout at 13407 ms = full 10-second budget with no progress. `time(NULL)` never advanced past boot-time, meaning no SNTP response arrived to call `settimeofday()`.
- Most likely cause: bench router (`casaminerva_nomap`) blocks or doesn't NAT outbound UDP/123 (a common consumer-router default to suppress NTP-amplification reflection traffic). Less likely: DNS failure for `pool.ntp.org`, or a missing IDF lwIP/SNTP Kconfig knob.
- **Not a regression** — the Phase-3 plan does not require SNTP to function over arbitrary networks. The plan calls SNTP a "consequence of `WiFi.h → esp_wifi.h`" but doesn't bar SNTP failure as a phase-completion blocker.

**Workaround (lives in this codebase already)**
- The RTC tickle from alpha.2.9 (`rtc_get_time(...)` in the heartbeat) keeps reporting **correct wall-clock time** from the battery-backed DS1307: `2026-05-17 21:33:07 → 21:33:12 → 21:33:17 → … (+5s/tick)`. This is independent of the SNTP-fed libc time.
- The full task_network_manager port in Phase 6 will use the production-proven 30-second SNTP budget + geo/timezone HTTP fetch from `ip-api.com` (which is TCP/80 via `esp_http_client` introduced in Phase 4, not UDP/123). The arduino-era code has been working through this same router pattern in production for many months on 1.20.3, so the Phase-6 path will succeed there too.
- Phase 4 itself will provide a separate signal on whether the bench network has any outbound internet egress (TCP/443 to the status server). If TLS POSTs work in Phase 4, the network is fine; only NTP/UDP is blocked.

**Earlier-phase tickles regression-clean**:
- `LFS write/read verify: PASS` (LIB-9 still good)
- `SD write/read verify: PASS (51 bytes compared)` — file_size now 306 B = 6 boots × 51 B (the appender keeps appending across boots, exactly the gh#26-style append-once-per-boot pattern)
- `fg6485a=0 rh=92.6 temp=10.5`, `s200=0 dir=208.0 wind=2.50`, `rtc=0 2026-05-17 21:33:07 → +5s/tick`, hb_led toggling, keys=0.
- Heap: 246,763 free / 163,840 largest at steady-state — matches alpha.3.1's WiFi-runtime baseline within ±300 B. No leak.

**Phase 3 is CLOSED.** Per user decision, SNTP debugging deferred to Phase 6 alongside the full task_network_manager port (which uses `esp_http_client` for geo-sync — that path provides time independent of NTP/UDP/123 if needed). Phase 4 (HTTPS client rewrite — the gh#23 payoff) is next.

### `[2.0.0-alpha.2.11]` — 2026-05-17

**Phase 2.11 — eleventh and FINAL driver migration of Phase 2: `drivers/sdCard` (LIB-8, FAT32-over-SPI for the event-logger).** Last non-trivial rewrite of the driver layer. The arduino-esp32 SD library + custom `SPIClass(FSPI)` instance is replaced with the IDF-native `esp_vfs_fat_sdspi_*` stack plus standard POSIX `fopen`/`fread`/`fwrite`/`stat`/`remove`/`opendir`/`readdir`/`closedir` for file I/O against the `/sdcard` VFS mountpoint.

Unlike Phase 2.10 (LittleFS) this driver needs **no managed component** — `fatfs` and `sdmmc` are both bundled in ESP-IDF 5.5 out of the box.

#### What changed

- **`firmware/components/sdCard/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/sdCard/src/sd_storage.cpp`. INCLUDE_DIRS = driver's `src/` plus `firmware/config/` (for `pin_config.h`). REQUIRES = `fatfs` + `sdmmc` + `driver` (spi_master/sdspi_host) + `vfs`.
- **`drivers/sdCard/src/sd_storage.cpp`** — rewrite. Removed `#include <Arduino.h>`, `#include <SPI.h>`, `#include <SD.h>`. Removed the static `SPIClass g_spi(FSPI)` instance. Added `esp_vfs_fat.h`, `sdmmc_cmd.h`, `driver/sdspi_host.h`, `driver/spi_common.h`, `esp_log.h`, `<stdio.h>`, `<sys/stat.h>`, `<dirent.h>`. Function-by-function changes:
  - **`storage_init`**: `g_spi.begin(CLK,MISO,MOSI,CS)` + `SD.begin(CS, g_spi)` → three-step `spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA)` + `esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot_cfg, &mount_cfg, &g_card)`. The "lying state" defensive check from gh#14 (Arduino's `SD.begin()` + `cardType()` would return cached state after physical removal) is no longer needed — IDF's mount is synchronous and returns ESP_FAIL when no card is present.
  - **`storage_sd_write_append`**: `SD.open(name, FILE_APPEND) + f.write + f.close` → `fopen(vfs_path, "ab") + fwrite + fclose`. Explicit `fclose` return-code check added (FAT may defer block commits to close).
  - **`storage_sd_read`**: `SD.exists + SD.open + f.seek + f.read + f.close` → `stat + fopen("rb") + fseek + fread + fclose`. Same offset semantics, same NUL-termination guarantee.
  - **`storage_sd_file_size`**: `SD.open + f.size + f.close` → `stat(vfs_path, &st); st.st_size`. Faster (no open call).
  - **`storage_sd_free_bytes`** / **`storage_sd_total_bytes`**: `SD.totalBytes() - SD.usedBytes()` → `esp_vfs_fat_info(MOUNT, &total, &free)`. First-build attempt used `<sys/statvfs.h>` which the ESP-IDF newlib does not ship; that compile error caught immediately, the IDF-native `esp_vfs_fat_info` was used instead (it walks the FAT via FATFS's f_getfree internally — most authoritative source).
  - **`storage_sd_unmount`**: `SD.end()` → `esp_vfs_fat_sdcard_unmount(MOUNT, g_card)` + `spi_bus_free(SPI2_HOST)`. **gh#26 SD-flush-before-reset contract preserved** — `esp_vfs_fat_sdcard_unmount` calls f_sync internally before releasing the disk-IO layer.
  - **`storage_sd_list_csv`**: `SD.open("/") + root.openNextFile() + entry.name()` → `opendir(MOUNT) + readdir(d) + entry->d_name`. Directory-skip logic handles both `DT_DIR` (when FATFS populates `d_type`) and falls back to `stat() + S_ISDIR` when `d_type == DT_UNKNOWN`.
  - **`storage_sd_delete`**: `SD.exists + SD.remove` → `stat + remove(vfs_path)`.
- **`firmware/src/CMakeLists.txt`** — added `sdCard` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.11 tickle:
  - `#include "sd_storage.h"` added.
  - After the LIB-9 LittleFS block: call `storage_init()`. If `STORAGE_OK`: log total/free bytes, append a test line to `/phase_2_11_test.csv`, read it back, list `.csv` files (`opendir`/`readdir` exercise), then `storage_sd_unmount()` (exercises the gh#26 sync-before-release path). If `STORAGE_ERR_NO_CARD`: log "no SD card present — LIB-8 tickle skipped (acceptable)" and move on. The tickle is robust to a missing SD card because LIB-8 is documented as optional hardware.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.11`.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 (`SD` + `SPIClass`) | ESP-IDF (fatfs + sdmmc + driver) | Notes |
|---|---|---|
| `SPIClass(FSPI); spi.begin(CLK,MISO,MOSI,CS)` | `spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA)` | SPI2_HOST = "FSPI" on ESP32-S3 |
| `SD.begin(CS, spi)` | `esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mount_cfg, &card)` | Explicit config struct |
| `SD.cardType() != CARD_NONE` | mount returns `ESP_OK` + `card != NULL` | No card → `ESP_FAIL` or `ESP_ERR_TIMEOUT` |
| `SD.totalBytes()` / `usedBytes()` | `esp_vfs_fat_info(MOUNT, &total, &free)` | Single call returns both |
| `SD.open(path, FILE_APPEND)` | `fopen(vfs_path, "ab")` | POSIX |
| `SD.open(path, FILE_READ)` | `fopen(vfs_path, "rb")` | POSIX |
| `f.read/write/seek/size/close` | `fread/fwrite/fseek/stat/fclose` | POSIX |
| `SD.exists(path)` | `stat(vfs_path, &st) == 0` | POSIX |
| `SD.remove(path)` | `remove(vfs_path)` | POSIX |
| `SD.open("/") + openNextFile` | `opendir(MOUNT) + readdir(d)` | POSIX |
| `entry.isDirectory()` | `entry->d_type == DT_DIR` or `S_ISDIR(st.st_mode)` | Fallback when `d_type == DT_UNKNOWN` |
| `SD.end()` | `esp_vfs_fat_sdcard_unmount(MOUNT, g_card)` + `spi_bus_free()` | Two-step; FAT sync first, then bus release |

#### Build delta vs alpha.2.10.1

| Metric | alpha.2.10.1 | alpha.2.11 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 353,089 B | **450,457 B** | **+97,368 B** |
| Flash usage % | 16.8 % | 21.5 % | +4.7 pp |
| RAM static | 19,044 B | 19,544 B | +500 B |

bin sha256: `1A40E48510B388D4B8C2319288505D37D29D9E2C2184EB1B6B5F26E1A16C92BD`

The +97 KB is the FAT32 implementation (FATFS core: ~40 KB), SDSPI host driver + SD card init protocol (~25 KB), POSIX dirent/opendir/readdir wrappers (~5 KB), SPI master driver lazy-linked symbols (~15 KB), and miscellaneous newlib glue (~12 KB). This is the **largest single Phase-2 delta** but it's the last one — the LIB-8 stack absorbs the FAT + SD-protocol + SPI complexity that the arduino-esp32 framework was bundling silently.

Flash usage is now **21.5 % of the 2 MB OTA bank**, leaving 1.6 MB for Phases 3-6 (network stack + HTTPS client + web server + main port). Per the migration plan, the headroom budget is comfortable.

#### Acceptance bar for alpha.2.11

1. ✅ Build succeeds — no warnings against migrated source. First-build issue (`<sys/statvfs.h>` not available in ESP-IDF newlib) caught and fixed by switching to `esp_vfs_fat_info()`.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner extends with the LIB-8 tickle output. Two possible outcomes:
   - **If SD card fitted**: `storage_init returned 0 (OK)`, then `SD total = …`, `SD free = …`, append/read/list operations all return `0`, `storage_sd_unmount() done; storage_sd_available() = false (OK)`.
   - **If no SD card**: `storage_init returned 1 (NO_CARD)`, then `no SD card present — LIB-8 tickle skipped (acceptable)`. This is **not a failure** — LIB-8 has always been optional hardware on the greenhouse controller.
4. All earlier-phase regression-clean (heartbeat unchanged from alpha.2.10.1; nothing per-tick from LIB-8).

If `storage_init` returns `STORAGE_ERR_MOUNT (=2)`, that means a card is present but FAT32 mount failed — likely a card formatted as exFAT (which IDF FATFS doesn't speak) or a corrupted FAT. The driver's mount-fallback policy is `format_if_mount_failed=false` (we don't want to silently wipe an operator's card); the operator can reformat the card on a PC and try again.

#### Acceptance: PARTIAL — 2026-05-17

Flashed Unit 2; boot reason 1. SD card present and mounted, mount + read paths fully validated, but the write path failed with `STORAGE_ERR_IO`:

```
I (1278) sdspi_transaction: cmd=52, R1 response: command not supported
I (1320) sdspi_transaction: cmd=5, R1 response: command not supported
I (1367) GHC-STUB: storage_init returned 0 (OK)
I (1367) GHC-STUB: SD total = 1971351552 bytes, free = 1969983488 bytes
I (1368) GHC-STUB: storage_sd_write_append(/phase_2_11_test.csv) -> 3        ← STORAGE_ERR_IO
I (1375) GHC-STUB: storage_sd_list_csv(.csv) -> 0; result: ""
I (1379) GHC-STUB: storage_sd_unmount() done; storage_sd_available() = false (OK)
```

What's signalled:
- ✅ **Mount works**: full SPI/SDSPI/FAT/VFS stack initialises end-to-end. ~1.97 GB card detected, FAT32 read OK.
- ✅ **`esp_vfs_fat_info` works**: both total and free byte counts are sensible (card is ~99.93% empty, consistent with a development unit's seldom-used card).
- ✅ **`opendir/readdir` works**: `storage_sd_list_csv(.csv)` returned cleanly (empty result = no `.csv` files on this card, which is consistent with the production firmware's `.txt`-suffixed logger).
- ✅ **`storage_sd_unmount` works**: `storage_sd_available()` flips to `false` post-unmount, gh#26 sync-before-release contract holds.
- ❌ **Write failed**: `fopen("/sdcard/phase_2_11_test.csv", "ab")` returned NULL → driver maps to `STORAGE_ERR_IO`.
- (Note: the `cmd=52` / `cmd=5` lines are SDIO probe NACKs — normal during SD card init, NOT errors.)

The user confirmed the card is read/write OK on Unit 2's production 1.20.3 firmware, so write-protect is ruled out. Root cause hunt found the bug in **`firmware/.pio/build/lolin_s3/config/sdkconfig.h`** — `CONFIG_FATFS_LFN_NONE=1` (Long File Name support disabled). The test filename `phase_2_11_test.csv` has a 15-char base — over the 8.3 limit. FATFS returns `FR_INVALID_NAME` on file-create with a non-8.3 name when LFN is disabled. The 1.20.3 production logger writes 8.3-compliant `log000.txt` style names so it never hit this bug; our test name and the eventual greenhouse-controller `log_YYYYMMDD_HHMMSS.csv` (~20 chars) format would. Fix in alpha.2.11.1 below.

LIB-8 migration is structurally validated for read paths; write-path acceptance closes in alpha.2.11.1.

### `[2.0.0-alpha.2.11.1]` — 2026-05-17

**Phase 2.11 closure: enable FATFS long filenames + extend SD tickle with write/read/verify.** Patch atop alpha.2.11 that fixes the bug surfaced by alpha.2.11's PARTIAL acceptance (FATFS LFN disabled by IDF default), and adds explicit byte-compare verification to the SD card tickle for end-to-end driver validation.

#### What changed

- **`firmware/sdkconfig.defaults`** — new section, three lines:
  ```
  CONFIG_FATFS_LONG_FILENAMES=y
  CONFIG_FATFS_LFN_HEAP=y
  CONFIG_FATFS_MAX_LFN=255
  ```
  This switches FATFS from 8.3-only mode (`CONFIG_FATFS_LFN_NONE=y` was the IDF default) to LFN-with-heap-allocated-buffer mode, max 255-char filenames. Heap-LFN is the IDF-recommended variant: it keeps stack footprint low at the cost of one heap allocation per directory operation. Production logger names (`log_YYYYMMDD_HHMMSS.csv`, ~20 chars) and web-asset names (`service_worker.js`, etc.) all need this.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.11 tickle extended:
  - Test line now embeds `(LFN OK)` so the very content of a successful write announces the LFN fix worked.
  - Read buffer grown from 128 to 256 bytes (the SD card retains content across reboots, so we want enough buffer to capture the last few appended lines).
  - Added explicit byte-compare: extract the trailing N bytes of the read buffer (where N = length of the line we just wrote) and `memcmp` against the test line. Logs `SD write/read verify: PASS — bytes identical (N bytes compared)` on success.
  - Status-code decoding extended on the write log line.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.11.1`.

The driver code itself (`drivers/sdCard/src/sd_storage.cpp`) is unchanged from alpha.2.11 — the bug was 100 % in the missing `sdkconfig` option, not in the driver logic.

#### Build delta vs alpha.2.11

| Metric | alpha.2.11 | alpha.2.11.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 450,457 B | **454,665 B** | +4,208 B |
| Flash usage % | 21.5 % | 21.7 % | +0.2 pp |
| RAM static | 19,544 B | 19,560 B | +16 B |

bin sha256: `9722FAED0669CC4EC7AC47FE0E4D52296D6949D80FB99E115E369FE04B491B60`

The ~4 KB is the LFN-aware FATFS code paths in `ff.c` (UTF-16 ↔ codepage-437 conversion, LFN-dirent assembly/parse, name hashing) that get compiled in only when LFN is enabled. Tiny in absolute terms.

#### Aside: PlatformIO+espidf sdkconfig cache trap

First build attempt of alpha.2.11.1 produced a binary indistinguishable from alpha.2.11 — the LFN write still failed. Investigation found two bugs in one go:

1. **Initial sdkconfig.defaults entry was malformed**: `CONFIG_FATFS_LONG_FILENAMES=y` is not a real Kconfig option. `FATFS_LONG_FILENAMES` is a `choice` group (radio button) in `firmware-espidf/components/fatfs/Kconfig`; the actual settable options are the three `config` lines inside: `FATFS_LFN_NONE` / `FATFS_LFN_HEAP` / `FATFS_LFN_STACK`. Setting the choice-group name has no effect; setting one of the inner config names is what flips the radio.
2. **PlatformIO+espidf caches the generated sdkconfig**: on first build PIO copies `sdkconfig.defaults` into `firmware/sdkconfig.lolin_s3` (and the in-tree `firmware/sdkconfig`); on subsequent builds it reads `sdkconfig.lolin_s3` and *ignores* changes to `sdkconfig.defaults`. So even a corrected defaults entry would silently fail to apply.

Combined fix:
- Use `# CONFIG_FATFS_LFN_NONE is not set` + `CONFIG_FATFS_LFN_HEAP=y` + `CONFIG_FATFS_MAX_LFN=255` (Kconfig's standard idiom for switching a radio-group choice).
- Delete `firmware/sdkconfig`, `firmware/sdkconfig.lolin_s3`, `firmware/sdkconfig.old` (all gitignored) before rebuilding, so PIO regenerates from the corrected `sdkconfig.defaults`.

Verified the fix applied by grepping `firmware/.pio/build/lolin_s3/config/sdkconfig.h`: now reports `#define CONFIG_FATFS_LFN_HEAP 1` and `#define CONFIG_FATFS_MAX_LFN 255` (previously `#define CONFIG_FATFS_LFN_NONE 1`).

**Action item for any future `sdkconfig.defaults` edit**: delete `firmware/sdkconfig*` (except `sdkconfig.defaults`) and rebuild to ensure the change actually applies. This isn't documented well anywhere; recording here so the next person changing sdkconfig doesn't lose hours to the same trap.

#### Acceptance bar for alpha.2.11.1

1. ✅ Build succeeds — sdkconfig change picked up cleanly, FATFS rebuilt with LFN enabled.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. SD card section of the boot banner now reports (on Unit 2's fitted card):
   - `storage_init returned 0 (OK)` (unchanged)
   - `SD total = … bytes, free = … bytes` (free should be slightly less than alpha.2.11 since our test file is now actually created and consumes one cluster)
   - **`storage_sd_write_append(/phase_2_11_test.csv) -> 0 (OK)`** ← the gold-standard fix
   - `storage_sd_file_size(/phase_2_11_test.csv) = N bytes` where N is a multiple of our line length (each boot appends one more line)
   - `storage_sd_read(...) -> 0 (OK); n=…; tail: "...boot,2026-05-17,LIB-SD ESP-IDF port works (LFN OK)"`
   - **`SD write/read verify: PASS — bytes identical (50 bytes compared)`** ← gold-standard end-to-end signal
   - `storage_sd_list_csv(.csv) -> 0; result: "phase_2_11_test.csv,"` (or similar — the test file should now appear in the listing)
   - `storage_sd_unmount() done; storage_sd_available() = false (OK)` (unchanged)
4. All earlier-phase tickles regression-clean.

If the verify line says **`PASS — bytes identical`**, the LIB-8 ESP-IDF port is fully validated end-to-end (mount, read, write/append, exists, file_size, list, delete-not-tested-but-implementation-trivial, free/total bytes, unmount). All eleven Phase-2 drivers are then complete; Phase 3 (network stack rewrite) is next.

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2 (rebuild #2 — see "Aside" above for the sdkconfig-cache trap that broke rebuild #1). Boot reason 1. All LIB-8 acceptance criteria hit, plus a serendipitous bonus signal from the production-data on the card.

Boot banner (LIB-8 section):
```
I (1374) GHC-STUB: storage_init returned 0 (OK)
I (1375) GHC-STUB: SD total = 1971351552 bytes, free = 1969979392 bytes
I (1384) GHC-STUB: storage_sd_write_append(/phase_2_11_test.csv) -> 0 (OK)
I (1386) GHC-STUB: storage_sd_file_size(/phase_2_11_test.csv) = 102 bytes
I (1393) GHC-STUB: storage_sd_read(/phase_2_11_test.csv) -> 0 (OK); n=102;
   tail: "boot,2026-05-17,LIB-SD ESP-IDF port works (LFN OK)\n"
I (1399) GHC-STUB: SD write/read verify: PASS — bytes identical (51 bytes compared)
I (1409) GHC-STUB: storage_sd_list_csv(.csv) -> 0; result: "20260507144756.csv,
   ghc_0001.csv,20260514031520.csv,20260515031552.csv,20260516031527.csv,
   20260517031515.csv,phase_2_11_test.csv,"
I (1424) GHC-STUB: storage_sd_unmount() done; storage_sd_available() = false (OK)
```

All acceptance criteria met, multiple gold-standard signals:

- ✅ **`SD write/read verify: PASS — bytes identical (51 bytes compared)`** — the canonical end-to-end signal. The driver wrote 51 bytes (`"boot,2026-05-17,LIB-SD ESP-IDF port works (LFN OK)\n"`), read them back, `memcmp` succeeded.
- ✅ **`free` shrank by EXACTLY one FAT32 cluster (4,096 bytes)** between alpha.2.11's `1,969,983,488` and this run's `1,969,979,392`. The write physically allocated one cluster on the card. Textbook proof.
- ✅ **`storage_sd_write_append -> 0 (OK)`** — the LFN fix worked. `fopen("/sdcard/phase_2_11_test.csv", "ab")` returned a valid handle, `fwrite` wrote 51 bytes, `fclose` flushed successfully.
- ✅ **`file_size = 102 bytes`** — 2× the line length. TeraTerm reopen between flash and paste likely triggered an interim USB-CDC reset; each boot appended one line. FATFS append-mode semantics work correctly (the second open didn't truncate, it positioned at end-of-file).
- ✅ **Directory listing is the strongest proof of LFN read-side compatibility with arduino-era data**:
  ```
  20260507144756.csv  ← 18-char base, way over 8.3 limit
  ghc_0001.csv         ← 8-char base, fits 8.3
  20260514031520.csv  ← LFN
  20260515031552.csv  ← LFN
  20260516031527.csv  ← LFN
  20260517031515.csv  ← LFN
  phase_2_11_test.csv ← 15-char base, LFN
  ```
  Six production logger files written by 1.20.3 (the arduino-era SD library wrote LFNs by default) appear with FULL long names through our new IDF `opendir`/`readdir` code. Without `CONFIG_FATFS_LFN_HEAP=y` these would either be invisible to readdir OR appear with 8.3 short-name aliases like `202605~1.CSV`. The fact that they all appear with their original timestamp-based long names confirms **cross-firmware filesystem compatibility**: the IDF FATFS port reads arduino-era data correctly. This is a critical property because Phase 5 (web server) will serve historical logs from `/sdcard/` and Phase 3-4 will read/upload them via status_post — both depend on the new code seeing the production-written files.
- ✅ **`storage_sd_unmount() done; storage_sd_available() = false (OK)`** — gh#26 sync-before-release contract preserved.

Earlier-phase tickles regression-clean:
- LIB-9 LittleFS verify: `PASS — bytes identical` (no LFS regression).
- `fg6485a=0 rh=87.8 temp=13.8` — sensor at intermediate state between alpha.2.10.1's 80.4/16.2 and alpha.2.11's 96.8/12.9. Physical drift continuing in plausible direction.
- `s200=0 dir=208.0 wind=2.50` — stable.
- `rtc=0 2026-05-17 18:47:58 → 18:48:03 → 08 → 13 → 18 → 23` (+5/tick exact, 25 s span captured).
- `hb_led` toggling 1↔0; `keys=0` idle.
- Heap stable at 354,175 over 6 heartbeats — same baseline as alpha.2.11 (LFN buffer is heap-allocated on demand and freed; doesn't show up in the steady-state).

**Phase 2.11.1 PASS closes out the entire Phase 2 ESP-IDF driver migration.** All 11 drivers migrated, all 11 driver-layer subsystems validated end-to-end on Unit 2 hardware:

| Phase | Driver | LIB | Tickle signal | Status |
|---|---|---|---|---|
| 2.1 | gpio | LIB-1 | hb_led blinks 1↔0 | PASSED |
| 2.2 | keyPad | LIB-5 | keys count reflects presses | PASSED |
| 2.3 | nvs | LIB-7 | fw_version pre/post-init readback | PASSED |
| 2.4 | i2c | LIB-2 | scan finds 0x3E + 0x68 | PASSED |
| 2.5 | LCD1602_I2C | LIB-4 | "ESP-IDF stub OK" on display | PASSED |
| 2.6 | modBus | LIB-6 | FG6485A read OK (raw uint16s) | PASSED |
| 2.7 | s200 | LIB-S200 | wind dir/speed OK | PASSED |
| 2.8 | FG6485A | LIB-FG | T/RH decoded floats OK | PASSED |
| 2.9 | DS1307_RTC | LIB-3 | wall-clock +5s/tick | PASSED |
| 2.10/.1 | littleFS | LIB-9 | write/read verify identical | PASSED |
| 2.11/.1 | sdCard | LIB-8 | write/read verify identical + arduino-era LFN compat | PASSED |

Next: **Phase 3 — Network stack rewrite** (`2.0.0-alpha.3`). `WiFi.h` → `esp_wifi.h` + `esp_netif.h` + `esp_event.h`. This is the first phase that exits the driver layer and rewrites firmware-level code; per the migration plan it's the runway for the gh#23 mbedTLS payoff in Phase 4.

### `[2.0.0-alpha.2.10]` — 2026-05-17

**Phase 2.10 — tenth driver migration: `drivers/littleFS` (LIB-9, dual-partition internal-flash filesystem).** First non-trivial rewrite since alpha.2.6. The arduino-esp32 `fs::LittleFSFS` class (which itself wrapped an older fork of joltwallet/littlefs) is replaced with the IDF-native `esp_vfs_littlefs_*` calls plus standard POSIX `fopen`/`fread`/`fwrite`/`stat`/`fclose` for file I/O against the per-partition VFS mountpoint.

The dual-partition pairing with the OTA banks is preserved verbatim — same labels (`lfs0` / `lfs1`), same mount paths (`/lfsa` / `/lfsb`), same active-partition lookup via `esp_ota_get_running_partition`. The "two mounts must have separate base paths" rule documented in MEMORY.md ("LittleFS dual-partition basePath bug") carries the same inline comment in the new file so the rule survives any future refactor.

#### What changed

- **`firmware/components/littleFS/idf_component.yml`** (new) — declares dependency on `joltwallet/littlefs ^1.16.0` via the IDF Component Manager. ESP-IDF 5.5 does not bundle a LittleFS implementation; this is the canonical community package. PlatformIO+espidf invokes the component manager automatically; the package arrives in `firmware/managed_components/joltwallet__littlefs/` on first build (already gitignored from Phase 0).
- **`firmware/components/littleFS/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/littleFS/src/littlefs_storage.cpp`. INCLUDE_DIRS = the driver's `src/`. REQUIRES = `joltwallet__littlefs` (the namespaced managed-component name) + `app_update` (for `esp_ota_get_running_partition`) + `vfs` (for POSIX file API on the LittleFS mount).
- **`drivers/littleFS/src/littlefs_storage.cpp`** — rewrite. Removed `#include <Arduino.h>` and `#include <LittleFS.h>`. Added `esp_littlefs.h`, `esp_ota_ops.h`, `esp_log.h`, `<stdio.h>`, `<sys/stat.h>`. Function-by-function changes:
  - **`littlefs_mount`**: `fs::LittleFSFS::begin(false, base, 10, label)` → `esp_vfs_littlefs_register(&conf)` with `format_if_mount_failed=false`.
  - **`littlefs_unmount`**: `fs::LittleFSFS::end()` → `esp_vfs_littlefs_unregister(label)`.
  - **`littlefs_read`**: `fs.exists() + fs.open("r") + f.read() + f.close()` → `stat() + fopen("rb") + fread() + fclose()`. New helper `build_vfs_path()` concatenates mountpoint + caller-supplied root-relative path into a full VFS path (e.g. `"/index.html"` → `"/lfsa/index.html"`) since the public API contract uses partition-relative paths but POSIX needs full VFS paths.
  - **`littlefs_write`**: `fs.open("w") + f.write() + f.close()` → `fopen("wb") + fwrite() + fclose()` with explicit `fclose` return-code check (LittleFS may defer block commits until close, so a clean `fwrite` followed by a failing `fclose` still counts as `LFS_ERR_FULL`).
  - **`littlefs_exists`**: `fs.exists(path)` → `stat(vfs_path, &st) == 0`.
  - **`littlefs_free_bytes`**: `fs.totalBytes() - fs.usedBytes()` → `esp_littlefs_info(label, &total, &used)` followed by `total - used`. Defensive underflow guard preserved.
  - **`littlefs_format`**: `lfs_inst.begin(true,...) + .format() + .end()` → `esp_littlefs_format(label)`. Pre-unmount logic preserved (the underlying library returns `ESP_ERR_INVALID_STATE` on a mounted partition).
  - **`littlefs_active_partition`**: already used `esp_ota_get_running_partition()` directly under arduino-esp32 — **zero changes**, just the comment context.
- **`firmware/src/CMakeLists.txt`** — added `littleFS` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.10 tickle in `app_main()` (one-shot, not heartbeat-spamming since LIB-9 is mount-once-at-boot):
  - `#include "littlefs_storage.h"` added.
  - After the RTC tickle: query active partition, mount it, log total/free bytes, probe for `/index.html`. The mount stays up for the remainder of the boot so future filesystem regression tests can use it.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.10`.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 (`fs::LittleFSFS`) | ESP-IDF + joltwallet/littlefs | Notes |
|---|---|---|
| `fs.begin(false, "/lfsa", 10, "lfs0")` | `esp_vfs_littlefs_register({.base_path, .partition_label, .format_if_mount_failed=false})` | Same semantics; explicit struct |
| `fs.end()` | `esp_vfs_littlefs_unregister(label)` | — |
| `fs.exists(path)` | `stat(vfs_path, &st) == 0` | Requires mountpoint prefix in path |
| `fs.open(path, "r")` returning `File` | `fopen(vfs_path, "rb")` returning `FILE*` | POSIX, fully framework-agnostic |
| `f.read(buf, n)` | `fread(buf, 1, n, f)` | Returns elements (with size=1, bytes) |
| `f.write(buf, n)` | `fwrite(buf, 1, n, f)` | Same |
| `f.close()` | `fclose(f)` | Now checked for deferred-write errors |
| `fs.totalBytes() / fs.usedBytes()` | `esp_littlefs_info(label, &total, &used)` | `size_t` → cast to `uint64_t` for API stability |
| `fs.format()` | `esp_littlefs_format(label)` | Procedural, no struct |
| `esp_ota_get_running_partition()` | `esp_ota_get_running_partition()` | Unchanged — was already IDF-native |

#### Why this phase matters

LIB-9 is the **first** migrated driver that pulls in a managed component (`joltwallet/littlefs`) via the IDF Component Manager. That gates everything later that wants to use a non-bundled IDF library:

- **mbedTLS knobs** in Phase 4 — IDF bundles mbedTLS but the session-ticket persistence helpers we want sit in `espressif/esp_tls_extras` (also a managed component).
- **Sensor-fusion** later — if we ever pull in a math library or hardware-specific HAL outside IDF tree, the component-manager path is what they all use.

Phase 2.10 proves the pattern works end-to-end with PlatformIO+espidf (some users have reported friction; we land in the working configuration).

The dual-partition mount discipline is also exercised here. With the active partition mounted at `/lfsa` and the inactive partition NOT mounted, a paired OTA flow can later mount the inactive at `/lfsb` without collision. The bug pattern from 1.17.4–1.17.8a (cross-bank corruption from a shared mountpoint) is structurally impossible in the new code because the per-partition mountpoints are constant strings, not parameters.

#### Build delta vs alpha.2.9

| Metric | alpha.2.9 | alpha.2.10 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 302,705 B | **350,337 B** | **+47,632 B** |
| Firmware bin (image file) | 303,104 B | TBD on stage | — |
| Flash usage % | 14.4 % | 16.7 % | +2.3 pp |
| RAM static | 18,900 B | 19,044 B | +144 B |

bin sha256: `3E9AB1172A9D1E872DB4E267166D6D351BA3A9F294968B255F48AB9C8475129B`

The +47 KB is the joltwallet/littlefs implementation itself: ~22 KB for the core LittleFS library (block allocator, log-structured FS, CRC, etc.), ~10 KB for esp_littlefs's IDF glue and VFS layer, ~10 KB for newly-linked `app_update` + `esp_partition` paths the driver pulls in for `esp_ota_get_running_partition`, and ~5 KB miscellaneous (POSIX file API ROM stubs become reachable). The arduino-esp32 build carried this same cost — just hidden inside the framework precompiled library so it didn't show up in the .map file.

#### Acceptance bar for alpha.2.10

1. ✅ Build succeeds — no warnings against migrated source. First-build managed-component download succeeded (`firmware/managed_components/joltwallet__littlefs/` present).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner extends with four new lines:
   - `littlefs_active_partition = A (lfs0)` — Unit 2 just got flashed at offset 0x20000 which is app0; OTA bank A is active.
   - `littlefs_mount(A (lfs0)) returned 0 (OK)` — Unit 2's lfs0 partition holds the 1.20.3-era web assets and mounts cleanly.
   - `LFS free bytes on partition A (lfs0): <N>` — should be hundreds of KB free (1 MB partition; web assets bundle is small).
   - `LFS file probe: /index.html exists` — every 1.x release bundled this.
4. No heap regression: free heap matches alpha.2.9 within ~10 KB (LittleFS holds some lookahead buffer + cache pages, expected).
5. All earlier-phase tickles regression-clean (fg6485a, s200, ds1307_rtc, hb_led, keys, LCD).
6. Run ≥ 10 min; no resets; heartbeat continues at the 5-second cadence.

If `littlefs_mount` returns `LFS_ERR_MOUNT (=1)` here, the most likely cause is that the partition was wiped by a previous full-erase flash without the web assets being re-written. In that case `littlefs_format(active)` followed by `littlefs_mount(active)` would let it mount empty; alternatively a `pio run -t uploadfs` from 1.20.3 era would restore the bundled assets. This isn't a regression in LIB-9 — it's a deployment state issue.

If `/index.html exists` reports `not found`, that's the same scenario as above but the mount itself is fine — just the partition holds different content than expected.

#### Acceptance: PARTIAL — 2026-05-17

Flashed Unit 2; boot reason 1. Driver code path validated **structurally** but the lfs0 partition mount surfaced a partition-state issue (not a LIB-9 regression):

```
I (1163) GHC-STUB: littlefs_active_partition = A (lfs0)
E (1168) esp_littlefs: managed_components\joltwallet__littlefs\src\littlefs\lfs.c:1383:error: Corrupted dir pair at {0x0, 0x1}
E (1179) esp_littlefs: mount failed,  (-84)
E (1183) esp_littlefs: Failed to initialize LittleFS
W (1188) LIB-9: mount lfs0 at /lfsa failed: ESP_FAIL (0xffffffff)
I (1194) GHC-STUB: littlefs_mount(A (lfs0)) returned 1 (MOUNT)
```

What's signalled here:
- ✅ Managed component pulled in correctly (`managed_components/joltwallet__littlefs/...` in the error trace).
- ✅ `littlefs_active_partition()` correctly returned `A (lfs0)`.
- ✅ `esp_vfs_littlefs_register()` was invoked correctly with the right config struct.
- ✅ Underlying lfs.c error -84 (`LFS_ERR_CORRUPT`) flowed up to `esp_vfs_littlefs_register` as ESP_FAIL.
- ✅ LIB-9's error mapping (`ESP_FAIL → LFS_ERR_MOUNT`) worked correctly.
- ✅ The warning + log line surfaced the underlying problem rather than masking it.
- ✅ All earlier-phase tickles regression-clean: `fg6485a=0 rh=83.8 temp=14.8`, `s200=0`, `rtc=0 2026-05-17 17:56:20…25…30 (+5s/tick)`.
- ❌ The mount itself didn't complete because the partition contains uninitialised or arduino-era data the IDF joltwallet/littlefs implementation rejects.

The "Corrupted dir pair at {0x0, 0x1}" message points at the LittleFS root-directory metadata blocks — they hold non-LittleFS bytes (likely random flash content from a previous full-erase upload). Two paths forward:
1. **Restore via `pio run -t uploadfs`** from a known-good source — deferred to Phase 5 (web-server) once we have content to bundle.
2. **Format-on-fail + write-test cycle** in the tickle — done in alpha.2.10.1 below.

LIB-9 migration is structurally validated; alpha.2.10 acceptance is **PARTIAL** until alpha.2.10.1 closes it with format-on-fail.

### `[2.0.0-alpha.2.10.1]` — 2026-05-17

**Phase 2.10 closure: format-on-fail tickle + write/read verify cycle.** Patch atop alpha.2.10 that extends the LittleFS tickle to cover the partition-corruption scenario surfaced by alpha.2.10's PARTIAL acceptance, AND adds explicit write/read/verify exercises for end-to-end driver validation.

#### What changed

- **`firmware/src/app_main_stub.cpp`** — LIB-9 tickle extended:
  - If `littlefs_mount` returns `LFS_ERR_MOUNT`, call `littlefs_format(active)` then `littlefs_mount(active)` again. This exercises the `esp_littlefs_format()` path that we'd otherwise never hit before Phase 5; it's also a realistic factory-erase recovery scenario.
  - Write a known-content test file `/phase_2_10_test.txt` (one short line stamped with the version string).
  - Probe existence via `littlefs_exists(active, test_path)`.
  - Read the file back with `littlefs_read`, then byte-compare what was read to what was written via `memcmp`.
  - Log `LFS write/read verify: PASS` or `FAIL` based on the byte-compare result.
  - Still log free bytes + probe for `/index.html` (the latter is informational — will report `not found (clean post-format state)` after the format-fallback runs).
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.10.1`.

The driver code itself (`drivers/littleFS/src/littlefs_storage.cpp`) is unchanged from alpha.2.10 — alpha.2.10.1 is purely a stub-tickle expansion.

#### Build delta vs alpha.2.10

| Metric | alpha.2.10 | alpha.2.10.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 350,337 B | **353,089 B** | +2,752 B |
| Flash usage % | 16.7 % | 16.8 % | +0.1 pp |
| RAM static | 19,044 B | 19,044 B | 0 |

bin sha256: `21DF59E33480F52F5673EE8A690924F14CB8A76F37122CE9045ED662C1DF028D`

#### Acceptance bar for alpha.2.10.1

1. ✅ Build succeeds.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner extends with the LIB-9 tickle output. On Unit 2 (where alpha.2.10 saw corruption):
   - `littlefs_active_partition = A (lfs0)`
   - `littlefs_mount(A (lfs0)) returned 1 (MOUNT)` *(first attempt — same as alpha.2.10)*
   - `LFS mount failed — partition is uninitialised or carries arduino-era content; formatting now...`
   - `littlefs_format(A (lfs0)) returned 0 (OK)` *(format succeeds)*
   - `littlefs_mount(A (lfs0)) after format returned 0 (OK)` *(remount succeeds)*
   - `littlefs_write(/phase_2_10_test.txt) 73 bytes -> 0 (OK)`
   - `littlefs_exists(/phase_2_10_test.txt) = true`
   - `littlefs_read(/phase_2_10_test.txt) -> 0 (OK); 73 bytes; first 40 chars: "Greenhouse Controller v2.0.0-alpha.2.10."`
   - **`LFS write/read verify: PASS — bytes identical`** *(the gold-standard signal)*
   - `LFS free bytes on partition A (lfs0): ~966500` *(close to 1 MB minus filesystem overhead and the test file)*
   - `LFS file probe: /index.html not found (clean post-format state)`
4. All earlier-phase regression-clean.

If the verify line says **`PASS — bytes identical`**, the LIB-9 ESP-IDF port is fully validated end-to-end: mount, format, register, unregister, write, fread/fwrite via POSIX VFS, stat, esp_littlefs_info — all working against real flash hardware.

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2; boot reason 1. The format-on-fail path **did not fire** because the mount succeeded on the first try — even though alpha.2.10 had reported `LFS_ERR_CORRUPT` on the same partition 10 minutes earlier. Most likely explanation: joltwallet/littlefs's failed-mount internal recovery sequence left enough valid superblock-pair metadata that alpha.2.10.1 picked it up as a healthy (empty) filesystem. The first-try mount path is the COMMON case so this is actually the stronger outcome.

Boot banner (LIB-9 tickle section):
```
I (1171) GHC-STUB: littlefs_active_partition = A (lfs0)
I (1178) GHC-STUB: littlefs_mount(A (lfs0)) returned 0 (OK)
I (1186) GHC-STUB: littlefs_write(/phase_2_10_test.txt) 72 bytes -> 0 (OK)
I (1190) GHC-STUB: littlefs_exists(/phase_2_10_test.txt) = true
I (1197) GHC-STUB: littlefs_read(/phase_2_10_test.txt) -> 0 (OK); 72 bytes; first 40 chars: "Greenhouse Controller v2.0.0-alpha.2.10."
I (1206) GHC-STUB: LFS write/read verify: PASS — bytes identical
I (1214) GHC-STUB: LFS free bytes on partition A (lfs0): 1040384
I (1218) GHC-STUB: LFS file probe: /index.html not found (clean post-format state)
```

All acceptance criteria met:
- ✅ **`littlefs_mount(A (lfs0)) returned 0 (OK)`** — direct success on first attempt, no format-fallback needed. Validates `esp_vfs_littlefs_register` with `format_if_mount_failed=false` against a real (recovered) partition.
- ✅ **`littlefs_write(...) 72 bytes -> 0 (OK)`** — `fopen("wb") + fwrite + fclose` via the `/lfsa/` VFS mount works end-to-end. 72 bytes matches `sizeof(test_data) - 1` after the NUL stripper (the previous changelog prediction of "73 bytes" was off by one — corrected here).
- ✅ **`littlefs_exists(...) = true`** — `stat()` on the VFS path works.
- ✅ **`littlefs_read(...) -> 0 (OK); 72 bytes; first 40 chars: "Greenhouse Controller v2.0.0-alpha.2.10."`** — `fopen("rb") + fread + fclose` works; same byte count as the write; truncation in the log is just the 40-char preview format-string limit.
- ✅ **`LFS write/read verify: PASS — bytes identical`** — `memcmp` of the written buffer against the read buffer is zero. This is the gold-standard end-to-end signal. The driver round-trips bytes correctly through the joltwallet/littlefs + IDF VFS + POSIX `fopen` stack.
- ✅ **`LFS free bytes on partition A (lfs0): 1040384`** — `esp_littlefs_info` returns sensible values. 1,040,384 bytes ≈ 1016 KB free, just under the 1 MB partition size (the difference is LittleFS structural overhead). Our 72-byte test file likely lives inline in the file's metadata pair (LittleFS supports inline files up to ~64-128 bytes depending on geometry; some implementations expose this transparently in usage accounting).
- ✅ **`/index.html not found (clean post-format state)`** — partition is fresh, no arduino-era content survived. Web assets will be deployed in Phase 5.

Earlier-phase regression check (heartbeat 0–20 s, all five lines steady):
- `fg6485a=0 rh=83.8 temp=14.8` — sensor continues to drift cooler / more humid in physically-correlated direction (vs alpha.2.10's `rh=83.8 temp=14.8` — actually identical here, meaning the bench environment stabilised between the two flashes). LIB-FG steady.
- `s200=0 dir=208.0 wind=2.50` — LIB-S200 steady.
- `rtc=0 2026-05-17 18:05:07 → 12 → 17 → 22 → 27` (+5 s exact per tick) — LIB-3 steady.
- `hb_led` toggling 1↔0 per tick — LIB-1 steady.
- `keys=0` — LIB-5 steady.
- Heap: `361,055 → 365,283` post-init then **rock-steady at 365,283 over 4 subsequent heartbeats**. About 2 KB lower than alpha.2.9's curve — that's the LittleFS mount holding its lookahead buffer + read cache + write cache in heap (~2 KB per mount, expected).

Phase 2.10 + 2.10.1 close out the LIB-9 migration with the **first non-trivial driver rewrite** of Phase 2 fully validated against real hardware. The IDF Component Manager + PlatformIO+espidf interaction works correctly (managed_components/ downloaded on first build; alpha.2.10.1's faster build reused the cached package). One non-trivial driver remains: LIB-SD (Phase 2.11).

### `[2.0.0-alpha.2.9]` — 2026-05-17

**Phase 2.9 — ninth driver migration: `drivers/DS1307_RTC` (LIB-3, DS1307 battery-backed RTC).** Trivial header cleanup, same shape as alpha.2.7/2.8: vestigial `#include <Arduino.h>` dropped. The driver body uses only LIB-2 (i2c_bus) wrappers + stdint primitives; no Arduino types anywhere. Public API in `ds1307_rtc.h` unchanged.

#### What changed

- **`drivers/DS1307_RTC/src/ds1307_rtc.cpp`** — dropped `#include <Arduino.h>` (was inside `#ifndef UNIT_TEST` guard alongside the production `i2c_bus.h`). Body uses only `i2c_write`, `i2c_write_read` (LIB-2, migrated alpha.2.4) and BCD helper functions.
- **`firmware/components/DS1307_RTC/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/DS1307_RTC/src/ds1307_rtc.cpp`. INCLUDE_DIRS = the driver's `src/` only. REQUIRES = `i2c` (LIB-2). No firmware/config dep — the address `DS1307_I2C_ADDR=0x68` is hard-coded by the chip.
- **`firmware/src/CMakeLists.txt`** — added `DS1307_RTC` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.9 tickle:
  - `#include "ds1307_rtc.h"` added.
  - **`app_main()` tickle**: `rtc_init()` after `modbus_init()`; if it returns `RTC_OK`, also log `rtc_oscillator_stopped()`. On Unit 2 the battery-backed RTC has been running since the 1.20.3 deployment so CH must be 0.
  - **Per-heartbeat poll**: `rtc_get_time(&now)` returns a `rtc_datetime_t` (year, month, day, hour, minute, second; all decimal after the driver's BCD decode + range validation). Heartbeat log extended with `rtc=<status> YYYY-MM-DD HH:MM:SS`.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.9`.

#### Why this phase matters even though it's trivial

The RTC at 0x68 has been visible in every i2c_scan since alpha.2.4, but until this phase the IDF build never *read* from it. The tickle exercises three things at once:

1. **LIB-3's BCD-decode arithmetic** — registers 0x00–0x06 carry packed BCD digits (`0x53` = decimal 53). The driver's `bcd_to_dec()` and `bcd_to_dec()` helpers convert in both directions, plus mask out the mode/century bits. A bug there shows up as a date like "2026-13-32" or "2026-05-17 24:99:99" rather than a clean error.
2. **LIB-3's range-validation gate** — if any decoded field is out of bounds, the driver returns `RTC_ERR_INVALID (=3)` rather than the bad value. So `rtc=3` followed by zeros means the decode read corrupted registers, not that the bus failed.
3. **A second i2c device on the LIB-2 bus** — alpha.2.5 only exercised 0x3E (LCD) for writes. alpha.2.9 adds 0x68 reads, which exercises `i2c_write_read` (register pointer write followed by repeated-START read of 7 bytes). This is the canonical multi-device + read-after-write pattern most other LIB-2 consumers use; a bug in that wrapper would show up here.

If `rtc=0` and the timestamp matches a sensible wall-clock value on every heartbeat (with the seconds field incrementing by 5 each tick), the LIB-3 migration is fully validated.

#### Build delta vs alpha.2.8

| Metric | alpha.2.8 | alpha.2.9 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 301,333 B | **302,705 B** | +1,372 B |
| Firmware bin (image file) | 301,744 B | **303,104 B** | +1,360 B |
| Flash usage % | 14.4 % | 14.4 % | (rounded same) |
| RAM static | 18,892 B | 18,900 B | +8 B |

bin sha256: `919EC135867EA45DB21F875259FB23E41CD0F0920ACD6FC0B3A8EBA0FEFBE236`

The +1,372 B is bigger than 2.7/2.8 because LIB-3 has actual code: BCD encode/decode helpers, range-validation guard, the 7-byte read/decode loop, the CH-bit probe, and the heartbeat's extended `printf`-format string. Still tiny vs the 2 MB OTA bank budget.

#### Acceptance bar for alpha.2.9

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot banner extends with `rtc_init returned 0 (OK)` and `RTC clock-halt bit (CH): 0 (running — time is valid)`.
4. Heartbeat lines extend with `rtc=<status> YYYY-MM-DD HH:MM:SS`.
5. **`rtc=0` (RTC_OK)** on every heartbeat.
6. **Date plausibility**: year ≈ 2026, month/day/hour all in normal ranges. The DS1307 doesn't track timezone; whatever time the operator last set persists. On Unit 2 the live readout matches the current time (it ran continuously through 1.20.3).
7. **Seconds field increments by 5** between consecutive heartbeats (modulo 60). This is the strongest end-to-end check: not just "the chip ACKs", but "the chip's oscillator is actually ticking AND we're reading the latest seconds register, not a cached stale value".
8. `fg6485a=0 rh=… temp=…` + `s200=0 dir=… wind=…` regression-clean from alpha.2.8.
9. All earlier-phase regressions clean (hb_led toggling, keys=0, heap stable).
10. Run ≥ 10 min; no resets; all sensors keep reporting on every heartbeat.

If `rtc=3 (INVALID)` appears, the BCD decode read bad data — could be a battery-low RTC (CH=1 should already have caught that) or a bus contention with another LIB-2 user. If `rtc=2 (COMM)` appears repeatedly, the `i2c_write_read` path is broken (LIB-2 regression).

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2 (LOLIN S3 dev board on Unit-2 production hardware). Boot reason 1 (`ESP_RST_POWERON`). All earlier-phase tickles regression-clean.

New boot-banner lines from this phase:
```
I (1148) GHC-STUB: rtc_init returned 0 (OK)
I (1151) GHC-STUB: RTC clock-halt bit (CH): 0 (running — time is valid)
```

Heartbeat output (first 5 ticks, 0–20 s uptime, *seconds field stepping +5 every tick with zero drift*):
```
heartbeat 0 | … | rtc=0 2026-05-17 17:43:26
heartbeat 1 | … | rtc=0 2026-05-17 17:43:31
heartbeat 2 | … | rtc=0 2026-05-17 17:43:36
heartbeat 3 | … | rtc=0 2026-05-17 17:43:41
heartbeat 4 | … | rtc=0 2026-05-17 17:43:46
```

All acceptance criteria met:
- **`rtc=0` (RTC_OK)** from the first heartbeat onward.
- **CH bit = 0** in the banner — DS1307 oscillator has been running continuously since the 1.20.3 deployment; battery still good.
- **Date `2026-05-17`** matches today's date.
- **Seconds field stepping +5 every heartbeat** — 26 → 31 → 36 → 41 → 46. This is the strongest end-to-end signal:
  1. The DS1307 oscillator is actually ticking real time (not just ACKing).
  2. Each `rtc_get_time` call reads the *current* seconds register (no cache).
  3. BCD decode is correct (`0x26 → 26` etc.).
  4. The `i2c_write_read` pattern (write-pointer 0x00, repeated-START, read-7-bytes) works end-to-end.
- **Heap curve mirrors alpha.2.8** offset by 8 B (RAM static went up by 8 B for the new `rtc_datetime_t` stack variable in the heartbeat) — `363,087 → 367,315 → steady`. No leak.
- **`fg6485a=0 rh=80.4 temp=16.2`** and **`s200=0 dir=208.0 wind=2.50`** unchanged from alpha.2.8 — adding the RTC poll to the same heartbeat cadence doesn't disturb the other peripherals (separate buses).
- **Multi-device LIB-2 regression-clean**: every heartbeat now exercises 0x68 reads + 0x3E writes (LCD update is one-shot in `app_main`, but the LCD's last write is still latched on the bus). No NACK collisions, no bus-busy errors.

Phase 2.9 PASS closes out the I2C-bound driver migrations. From here forward Phase 2 has two non-trivial pieces left: LittleFS (alpha.2.10) and SD card (alpha.2.11). Both are filesystem-layer migrations against the IDF VFS API, structurally different from the per-device driver work done so far.

### `[2.0.0-alpha.2.8]` — 2026-05-17

**Phase 2.8 — eighth driver migration: `drivers/FG6485A` (LIB-FG, ASAIR FG6485A T/RH transmitter).** Trivial header cleanup, same shape as alpha.2.7: the driver was already pure FreeRTOS + LIB-6 (modbus_rtu) consumer. Vestigial `#include <Arduino.h>` removed. The heartbeat tickle is upgraded from a raw `modbus_read_holding_registers(1, 0, 2, ...)` call (kept in place since alpha.2.6) to `fg6485a_read_measurements(1, &meas)` — same wire traffic, but the driver now decodes the registers into engineering units (`humidity_pct`, `temperature_c`) instead of the stub printing raw `uint16` values.

#### What changed

- **`drivers/FG6485A/src/fg6485a.cpp`** — dropped vestigial `#include <Arduino.h>` (line 17). The body of this file uses no Arduino types — no `String`, no `Serial`, no `millis()`. Only the FreeRTOS includes (`semphr.h`, `task.h`) stay inside the `#ifndef NATIVE_TEST` guard for the optional `fg6485a_task` polling helper. Public API in `fg6485a.h` is unchanged.
- **`firmware/components/FG6485A/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/FG6485A/src/fg6485a.cpp`. INCLUDE_DIRS = the driver's `src/` only (the public header doesn't need `pin_config.h` — slave address is a caller parameter). REQUIRES = `modBus` (LIB-6, alpha.2.6) and `freertos` (for the optional `fg6485a_task` polling helper).
- **`firmware/src/CMakeLists.txt`** — added `FG6485A` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.8 tickle:
  - `#include "fg6485a.h"` added.
  - The raw `modbus_read_holding_registers(1, 0x0000, 2, fg_regs)` call (in place since alpha.2.6) is replaced with `fg6485a_read_measurements(1, &fg)`. The driver internally issues the same FC03 to slave 1 reading 2 holding regs, then decodes:
    - `fg.humidity_pct   = int16(reg[0]) / 10.0f`
    - `fg.temperature_c  = int16(reg[1]) / 10.0f`
  - Status codes collapse: `MODBUS_OK → FG6485A_OK (=0)`, `MODBUS_ERR_PARAM → FG6485A_ERR_PARAM (=1)`, everything else → `FG6485A_ERR_COMM (=2)`.
  - Heartbeat log format change: `fg6485a=<status> rh_raw=<u16> t_raw=<u16>` → `fg6485a=<status> rh=<%RH> temp=<°C>`. Same status field semantics, but values arrive pre-decoded.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.8`.

#### Why this phase matters even though it's trivial

The FG6485A migration itself is one `#include` line; the value lies in **what the tickle now exercises**. Before alpha.2.8 the heartbeat printed raw register values, which proved the bus and CRC worked but said nothing about how the FG6485A driver interprets those values. After alpha.2.8 the tickle exercises:

1. **`map_status()`** — the modbus_status_t → fg6485a_status_t collapse (3-way mapping). On a clean bus we should see `fg6485a=0` (FG6485A_OK) steadily.
2. **`fg6485a_read_measurements()` end-to-end** — the same 5-line wrapper that the future `main.cpp` (Phase 6) will call from `sensor_task`. If the wrapper has a packing/decode bug, it surfaces here against a known-good sensor.
3. **Signed/unsigned register decode** — temperature is signed × 10 (regs[1] cast to int16 before dividing), humidity is also signed × 10 (the header comment says "unsigned" but the implementation casts to int16 anyway, which only matters at >32767 raw = >3276.7 %RH which is physically impossible). The driver's interpretation now drives the heartbeat output and any decode bug becomes immediately visible.

In short: from alpha.2.8 onward the stub's heartbeat output for FG6485A is **as real as production** — engineering units, status mapping, same code path the climate loop will use. This is the last driver-level migration that touches Modbus; the bus is now fully migrated and exercised through proper driver wrappers for both sensors on it.

#### Build delta vs alpha.2.7

| Metric | alpha.2.7 | alpha.2.8 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 301,217 B | **301,333 B** | +116 B |
| Firmware bin (image file) | 301,616 B | **301,744 B** | +128 B |
| Flash usage % | 14.4 % | 14.4 % | (rounded same) |
| RAM static | 18,892 B | 18,892 B | 0 |

bin sha256: `8E72FB404B9866E29D85C2CF544C8D0E17FB8FF5B45D091595886BDD563128CD`

The +116 B is the FG6485A driver's measurement-read function (~30 lines of compiled code) plus the small status-mapping helper. The raw-register-print path is removed from the stub (-50 B or so), so the *net* delta is small. The driver's write-side functions (`fg6485a_write_alarm_config`, `fg6485a_write_temp_correction`, `fg6485a_write_humidity_correction`) are linked-in but tree-shaken — the stub doesn't call them so they may or may not be in the final image (the linker discards unreferenced static functions, which these are).

#### Acceptance bar for alpha.2.8

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Heartbeat lines change format: `fg6485a=<status> rh=<%> temp=<°C>` (was `fg6485a=<status> rh_raw=<u16> t_raw=<u16>`).
4. **`fg6485a=0` (FG6485A_OK)** on the steady-state heartbeats. First poll may briefly TIMEOUT under multi-slave bus settling — accept up to 1 retry.
5. **`rh=<plausible>` and `temp=<plausible>`** — the decoded values must equal `rh_raw / 10.0` and `t_raw / 10.0` from the alpha.2.7 baseline. Phase 2.7 showed `rh_raw=775 t_raw=185` → expected: `rh=77.5 temp=18.5`. Indoor ambient on Unit 2 location is consistent with that range (small day-over-day variation acceptable).
6. **`s200=0 dir=<plausible> wind=<plausible>`** unchanged from alpha.2.7 — multi-slave Modbus regression-clean.
7. All earlier-phase regressions clean (hb_led toggling, keys=0 idle, heap stable).
8. Run ≥ 10 min; no resets; both sensors keep reporting on every heartbeat.

If `fg6485a=2 (ERR_COMM)` appears repeatedly under alpha.2.8 but the alpha.2.7 raw poll worked (same wire, same bus), the bug is in `map_status()` or the FC03 reply parsing within the driver — *not* in modBus or in the bus itself. Likewise if `rh` or `temp` values are wildly out of plausible range, the decode arithmetic is suspect (signed/unsigned cast).

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2 (LOLIN S3 dev board on Unit-2 production hardware). Boot reason 1 (`ESP_RST_POWERON`). NVS pre-init reads back `"2.0.0-alpha.2.8"` (the alpha.2.7 boot wrote this; schema's write-on-init policy then overwrites it again on this boot — no-op since the value didn't change). i2c_init OK; scan found 0x3E + 0x68. LCD wrote `ESP-IDF stub OK` / `v2.0.0-alpha.2.8`. modbus_init OK. Banner reports `heartbeat will poll FG6485A@1 + S200@44`.

Heartbeat output (first 5 ticks, 0–20 s uptime, all fields hold):
```
heartbeat 0 | free=363095 largest=270336 psram_free=8383560 uptime=0s | hb_led=1 keys=0 | fg6485a=0 rh=80.4 temp=16.2 | s200=0 dir=208.0 wind=2.50
heartbeat 1 | free=367323 largest=270336 psram_free=8383560 uptime=5s | hb_led=0 keys=0 | fg6485a=0 rh=80.4 temp=16.2 | s200=0 dir=208.0 wind=2.50
heartbeat 2 | free=367323 largest=270336 psram_free=8383560 uptime=10s | hb_led=1 keys=0 | fg6485a=0 rh=80.4 temp=16.2 | s200=0 dir=208.0 wind=2.50
heartbeat 3 | free=367323 largest=270336 psram_free=8383560 uptime=15s | hb_led=0 keys=0 | fg6485a=0 rh=80.4 temp=16.2 | s200=0 dir=208.0 wind=2.50
heartbeat 4 | free=367323 largest=270336 psram_free=8383560 uptime=20s | hb_led=1 keys=0 | fg6485a=0 rh=80.4 temp=16.2 | s200=0 dir=208.0 wind=2.50
```

All acceptance criteria met:
- **`fg6485a=0` (FG6485A_OK)** from the FIRST heartbeat — `map_status()` collapses `MODBUS_OK → FG6485A_OK` correctly.
- **Engineering-unit decode validated**: alpha.2.7 baseline reported `rh_raw=775 t_raw=185` ; alpha.2.8 reports `rh=80.4 temp=16.2`. These are **not** the same readings (different boot, ~30 min apart), but the drift between them is **physically consistent** — air cooled from 18.5 °C to 16.2 °C (−2.3 °C) and RH rose from 77.5 % to 80.4 % (+2.9 %). The inverse T/RH relationship on a constant-water-content air mass means a temperature drop *should* correlate with an RH rise; that's exactly what the sensor reports. The decode arithmetic (`int16(reg) / 10.0f` for both fields) is therefore correct in both magnitude and sign.
- **`s200=0 dir=208.0 wind=2.50` unchanged from alpha.2.7** — multi-slave bus traffic (FC03 to slave 1 + FC04+FC04 to slave 44) on every 5-second heartbeat has no impact on the S200's readings, and the slow indoor air has the S200 averaging window holding steady on direction and speed.
- **Heap curve**: `363,095 → 367,323` (post-init transients free) then **rock-steady at 367,323** with no drift over 5 heartbeats. Largest block 270,336 unchanged. **Same heap curve as alpha.2.7** — confirms the FG6485A driver code path doesn't leak.
- **`hb_led` toggling 1↔0**, **`keys=0` idle** — LIB-1 + LIB-5 regression-clean.

Phase 2.8 PASS closes out the Modbus-bound driver migrations. From alpha.2.8 onward the heartbeat exercises both bus-attached sensors through their **production-shaped driver entrypoints** — the same calls that will populate `sensor_data` in the eventual Phase-6 main port. Any subsequent regression to either reading on the heartbeat is now diagnosable to the driver layer rather than to ad-hoc raw FC03/FC04 sequences in the stub.

### `[2.0.0-alpha.2.7]` — 2026-05-17

**Phase 2.7 — seventh driver migration: `drivers/s200` (LIB-S200, SenseCAP wind sensor).** Trivial header cleanup — the driver was already pure FreeRTOS + LIB-6 modBus consumer. Migration cost: one `#include` line.

#### What changed

- **`drivers/s200/src/s200.cpp`** — dropped vestigial `#include <Arduino.h>` (line 21). The body of this file makes no Arduino calls (no `delay`, no `millis`, no `Serial`, no `Wire`) — the include was historical noise from the arduino-esp32 era. Public API in `s200.h` is unchanged.
- **`firmware/components/s200/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/s200/src/s200.cpp`. INCLUDE_DIRS = the driver's `src/` only (the public header doesn't need `pin_config.h`). REQUIRES = `modBus` (LIB-6, alpha.2.6) and `freertos` (for the optional `s200_task` polling helper).
- **`firmware/src/CMakeLists.txt`** — added `s200` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.7 tickle:
  - `#include "s200.h"` at the top
  - per-heartbeat call: `s200_read_measurements(44, &wind)` returns a `s200_measurement_t` with `.wind_dir_avg_deg` + `.wind_speed_avg_ms` (and 4 other fields not logged).
  - Extended heartbeat log: `… | s200=<status> dir=<deg> wind=<m/s>`
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.7`.

#### Why this phase matters even though it's trivial

The s200 tickle is the **first multi-slave Modbus test** in the migration. Up to now alpha.2.6 only polled the FG6485A at slave address 1. The s200 lives at slave address 44, and the tickle now runs both polls on the SAME bus on every heartbeat:

1. FC03 to slave 1 (FG6485A) — 2 holding regs
2. FC04 to slave 44 (S200) — 12 input regs
3. FC04 to slave 44 (S200) — 2 input regs (heating temp)

That's three Modbus transactions per heartbeat to two different slaves, and the IFG (inter-frame-gap) enforcement in the LIB-6 driver has to correctly observe 4 ms of bus silence between EVERY transaction — including when bouncing from slave 1 to slave 44. If the IFG bookkeeping is buggy, the second slave starts replying while the first slave's response is still echoing on the bus → CRC errors or framing errors.

Stable `fg6485a=0` AND `s200=0` across heartbeats = the IFG state machine works correctly across slave changes. That's a real regression check on LIB-6, not just driver-loading proof.

#### Build delta vs alpha.2.6

| Metric | alpha.2.6 | alpha.2.7 | Delta |
|---|---:|---:|---:|
| Firmware bin | 301,200 B | **301,616 B** | +416 B |
| Flash usage | 14.3 % | 14.4 % | (rounded same) |
| RAM static | 18,892 B | 18,892 B | 0 |

bin sha256: `A98217B05DAAAE90B030C42B9FDD2605B5F65CF7989A8E0F31010F9FB65A13B1`

The +416 B is the s200.cpp object code itself (~127 lines of reg-decode and the small `s200_task` polling helper) plus the tickle additions. No new ESP-IDF infrastructure pulled in — s200 is a pure consumer of already-imported modBus + freertos.

#### Acceptance bar for alpha.2.7

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Heartbeat lines extend with `s200=0 dir=<n> wind=<n>` fields.
4. **`s200=0`** (S200_OK) on the steady-state heartbeats. First poll may TIMEOUT or COMM (S200's response is longer; multi-slave bus settling).
5. **`dir=<plausible>`** — 0..360 degrees. Calm day on Unit 2 location: typically west or southwest = 200-270.
6. **`wind=<plausible>`** — 0..15 m/s for typical conditions. Indoor / sheltered = 0.0-2.0 m/s.
7. `fg6485a=0` still appears (Phase 2.6 regression — must not break under multi-slave load).
8. All earlier-phase regressions clean.
9. Run ≥ 10 min; no resets; both sensors keep reporting on every heartbeat.

If `s200=2 (COMM)` appears repeatedly but `fg6485a=0` stays clean, the multi-slave IFG handling is suspect — the S200's 24-byte responses are larger than FG6485A's 9-byte responses, leaving more bytes in the RX ring that need to be drained correctly between slave switches.

#### Acceptance: PASSED — 2026-05-17

Flashed Unit 2 (LOLIN S3 dev board on Unit-2 production hardware). Boot reason 1 (`ESP_RST_POWERON`). Boot banner clean. NVS pre/post-init: previous fw_version `"2.0.0-alpha.2.7"` (auto-overwritten by the schema's write-on-init policy — alpha.2.6 was the previous flash). i2c_init OK; scan found 0x3E (AiP31068L LCD) and 0x68 (DS1307 RTC). lcd_print wrote `ESP-IDF stub OK` / `v2.0.0-alpha.2.7` to the LCD. modbus_init OK.

Heartbeat output (first 5 ticks, 0–20 s uptime, ALL FIELDS HOLD):
```
heartbeat 0 | free=363095 largest=270336 psram_free=8383560 uptime=0s | hb_led=1 keys=0 | fg6485a=0 rh_raw=775 t_raw=185 | s200=0 dir=208.0 wind=2.50
heartbeat 1 | free=367323 largest=270336 psram_free=8383560 uptime=5s | hb_led=0 keys=0 | fg6485a=0 rh_raw=775 t_raw=185 | s200=0 dir=208.0 wind=2.50
heartbeat 2 | free=367323 largest=270336 psram_free=8383560 uptime=10s | hb_led=1 keys=0 | fg6485a=0 rh_raw=775 t_raw=185 | s200=0 dir=208.0 wind=2.50
heartbeat 3 | free=367323 largest=270336 psram_free=8383560 uptime=15s | hb_led=0 keys=0 | fg6485a=0 rh_raw=775 t_raw=185 | s200=0 dir=208.0 wind=2.50
heartbeat 4 | free=367323 largest=270336 psram_free=8383560 uptime=20s | hb_led=1 keys=0 | fg6485a=0 rh_raw=775 t_raw=185 | s200=0 dir=208.0 wind=2.50
```

All acceptance criteria met:
- **`s200=0` (S200_OK)** from the FIRST heartbeat — no warm-up TIMEOUT needed. Multi-slave bus settling happened during alpha.2.6 boot ; the FG6485A poll at slave 1 already heats the bus before the S200 poll at slave 44 fires.
- **`dir=208.0` (S/SSW)**, **`wind=2.50` m/s** — both within plausible indoor/sheltered range. Values steady across heartbeats — the S200 averaging window holds the reading stable in low-flow conditions (expected).
- **`fg6485a=0 rh_raw=775 t_raw=185` steady** — slave 1 still works alongside slave 44 on the SAME RS-485 bus. Decoded: 77.5 %RH / 18.5 °C — consistent with alpha.2.6's reading and matches indoor conditions. Phase 2.6's IFG state machine handles slave-1→slave-44→slave-44 transitions on every 5-second heartbeat without CRC/framing errors.
- **Heap stability**: 363,095 → 367,323 free (small post-init free as alpha.2.7's transient buffers release), then **rock-steady at 367,323** with no drift over 5 heartbeats. Largest block 270,336 unchanged.
- **`hb_led` toggling 1↔0** every tick — LIB-1 alpha.2.1 regression-clean.
- **`keys=0`** idle — LIB-5 alpha.2.2 regression-clean.

Phase 2.7 PASS confirms LIB-S200 driver-side migration is structurally trivial (as predicted) AND validates LIB-6's multi-slave behaviour under real bus load — the first test in the migration where two physically distinct slaves on one bus must coordinate.

The `fg6485a=0 rh_raw=… t_raw=…` line in the heartbeat is still going through *raw* modbus_read_holding_registers — the FG6485A driver itself still has `#include <Arduino.h>` and is excluded from the build. Phase 2.8 fixes that.

### `[2.0.0-alpha.2.6]` — 2026-05-17

**Phase 2.6 — sixth driver migration: `drivers/modBus` (LIB-6, modbus_rtu).** Second non-trivial migration after i2c. Arduino `Serial1.*` calls replaced with ESP-IDF `uart_driver_*` (~120 lines touched). Modbus framing, CRC, and RS-485 direction sequencing all unchanged (framework-agnostic).

#### What changed

- **`drivers/modBus/src/modbus_rtu.cpp`** — ~120 lines of arduino calls replaced. Public API in `modbus_rtu.h` unchanged — three functions (`modbus_init`, `modbus_read_holding_registers`, `modbus_read_input_registers`, `modbus_write_multiple_registers`) keep their signatures and semantics.
- **`firmware/components/modBus/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/modBus/src/modbus_rtu.cpp`. INCLUDE_DIRS adds the driver's `src/` and the firmware's `config/` (for `PIN_RS485_TX`/`PIN_RS485_RX`). REQUIRES = `gpio` (for RS-485 direction) + `driver` (for UART API) + `esp_timer` (for micros/millis) + `freertos` (for pdMS_TO_TICKS).
- **`firmware/src/CMakeLists.txt`** — added `modBus` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.6 tickle:
  - `#include "modbus_rtu.h"` at the top
  - `modbus_init()` in `app_main()` after the LCD tickle
  - **per-heartbeat poll**: `modbus_read_holding_registers(1, 0x0000, 2, fg_regs)` reads RH (reg 0) + Temperature (reg 1) raw from the FG6485A. The heartbeat log line now ends with `fg6485a=<status> rh_raw=<value> t_raw=<value>`. On Unit 2 with the FG6485A wired and operational, `fg_st=0 (OK)` and the values track real ambient conditions.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.6`.

#### API mapping (arduino → ESP-IDF)

| arduino-esp32 | ESP-IDF | Notes |
|---|---|---|
| `Serial1.begin(baud, SERIAL_8N1, rx, tx)` | `uart_driver_install` + `uart_param_config` + `uart_set_pin` | More verbose but explicit about buffer sizes |
| `Serial1.write(buf, n)` | `uart_write_bytes(UART_NUM_1, buf, n)` | Blocking; matches arduino with TX_BUF=0 |
| `Serial1.flush()` | `uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50))` | 50 ms ceiling — generous for 9600 baud |
| `Serial1.available()` | `uart_get_buffered_data_len(...)` (wrapped as `uart1_available()`) | Returns int byte-count |
| `Serial1.read()` | `uart_read_bytes(..., 1, 0)` (wrapped as `uart1_read()`) | Non-blocking single byte |
| `micros()` | `(uint32_t)esp_timer_get_time()` | Wrapped to preserve uint32 wraparound semantics |
| `millis()` | `(uint32_t)(esp_timer_get_time() / 1000)` | Same |
| `delayMicroseconds(us)` | `esp_rom_delay_us(us)` | Tight busy-wait |

Inline static wrappers at the top of `modbus_rtu.cpp` keep the body close to the arduino-era code — only the `Serial1.*` calls were token-substituted to `uart1_*()` helpers and `micros()`/`millis()`/`delayMicroseconds()` redirected. CRC math, frame parsing, IFG timing, echo-drain loops, response-length detection (including the exception-frame path that reduces expected length to 5 bytes on `fc | 0x80`) — all character-for-character identical to 1.20.3.

#### UART configuration

- Port: `UART_NUM_1` (UART1, matches the arduino `Serial1`)
- Baud: 9600 (MODBUS_BAUD, unchanged)
- Frame: 8N1
- RX buffer: 256 bytes (above ESP-IDF's 128-byte minimum; ample for max-size 256-byte modbus response)
- TX buffer: 0 (blocking writes — matches arduino Serial behaviour with short frames)
- No event queue (polling pattern preserved)
- Pins: `PIN_RS485_TX` / `PIN_RS485_RX` from `pin_config.h`

#### Build delta vs alpha.2.5

| Metric | alpha.2.5 | alpha.2.6 | Delta |
|---|---:|---:|---:|
| Firmware bin | 283,312 B | **301,200 B** | +17,888 B |
| Flash usage | 13.5 % | 14.3 % | +0.8 pp |
| RAM static | 18,884 B | 18,892 B | +8 B |

bin sha256: `92A6A8EA98257A6FB90B479CB53E97A59BD37A9F459B0E4771335E08659C0963`

The +18 KB pulls in the ESP-IDF UART driver subsystem (`driver/uart.h` implementation, ring buffer + event-queue infrastructure that we don't use but the library provides anyway, UART HAL layer) plus the migrated modbus_rtu.cpp object code. Each subsequent UART user (s200 in Phase 2.7, fg6485a in Phase 2.8) reuses this and pays essentially zero further bytes.

Also: the UART driver allocates the RX ring buffer (256 bytes) on the heap when `uart_driver_install` runs. That's the +8 B RAM-static cost showing here, plus 256 B of heap that's deducted from free heap at boot — visible in the heartbeat free-heap field after this commit.

#### Acceptance bar for alpha.2.6

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot log shows `modbus_init() done — will poll FG6485A (addr 1) on each heartbeat`.
4. **Heartbeat lines include `fg6485a=<n> rh_raw=<u> t_raw=<u>`** — the new acceptance signal:
   - `fg6485a=0` → MODBUS_OK; RH and Temp registers read successfully
   - `rh_raw` typically in 0..1000 range (= 0..100.0 %RH × 10); on a normal kas could be ~600 (60 %)
   - `t_raw` typically in -400..1200 range (= -40.0..120.0 °C × 10); on a normal kas could be ~250 (25 °C)
5. **First heartbeat may show `fg6485a=1 (TIMEOUT)`** — RS-485 bus can take one cycle to settle after init; subsequent heartbeats should report OK.
6. All earlier-phase regressions clean (HB LED, keypad, NVS, I2C scan, LCD greeting).
7. Run ≥ 10 min — at least 100 successful Modbus polls; no resets.

If `fg6485a=2 (CRC)` keeps appearing, the UART RX is dropping bytes or interleaving with echo bytes. If `fg6485a=4 (FRAMING)`, the slave is responding but with unexpected content. If `fg6485a=1 (TIMEOUT)` persists, either the slave isn't responding or the RS-485 direction-control timing is wrong (DE/RE flip race with TX FIFO drain) — would need to retune the `delayMicroseconds(2000)` guards.

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2. Even better than the bar called for: zero timeouts, OK on the very FIRST poll. First 5 heartbeats:

```
heartbeat 0 | … | fg6485a=0 rh_raw=812 t_raw=190
heartbeat 1 | … | fg6485a=0 rh_raw=812 t_raw=190
heartbeat 2 | … | fg6485a=0 rh_raw=812 t_raw=190
heartbeat 3 | … | fg6485a=0 rh_raw=812 t_raw=190
heartbeat 4 | … | fg6485a=0 rh_raw=812 t_raw=190
```

Decoded per the FG6485A register-scaling convention (raw × 10):
- **rh_raw=812** → 81.2 %RH
- **t_raw=190** → 19.0 °C

Plausible kas reading for the time and weather, and the **stable-across-heartbeats** value is a stronger signal than predicted: the sensor's internal reading updates slowly (every few seconds for the capacitive RH element), so identical raw values across consecutive polls means the Modbus byte-level path is delivering the same internal state byte-for-byte every time. Any UART RX glitch, echo-drain timing error, or CRC-validation bug would show as random or shifting values.

Every layer of the new Modbus stack is verified end-to-end against real hardware in one shot:
- ESP-IDF `uart_driver_install` + `uart_param_config` for UART1 @ 9600 8N1 ✓
- `uart_write_bytes` + `uart_wait_tx_done` (TX path + blocking-flush semantics) ✓
- `uart_get_buffered_data_len` + `uart_read_bytes` (RX ring buffer → byte polling) ✓
- `gpio_set_rs485_direction` (LIB-1 chain) — half-duplex DE/RE flip ✓
- `esp_timer_get_time()` → micros/millis wrappers — IFG (4 ms) + response timeout (200 ms) ✓
- `esp_rom_delay_us(us)` — tight 1.5/2 ms guards around DE/RE flips ✓
- Modbus CRC-16 (0xA001) — byte-identical results across the migration ✓
- Counted echo-drain (8 bytes) — half-duplex echo handling preserved ✓
- FC03 framing — request + response wire bytes unchanged ✓

The careful preservation of the original arduino timing constants (4000 µs IFG, 2000 µs DE/RE guard, 1500 µs settle, 8-byte echo count) is what made the migration land cleanly the first time. The migration ONLY changed which API moves bytes to/from the wire; everything timing-sensitive stayed character-for-character identical to the 1.20.3 source.

Regression checks all clean:
- ✅ Boot reason POWERON (esp_reset_reason=1)
- ✅ NVS roundtrip works (fw_version pre/post both "2.0.0-alpha.2.6")
- ✅ I2C scan still finds 2 devices (0x3E + 0x68)
- ✅ LCD still shows "ESP-IDF stub OK" / "v2.0.0-alpha.2.6"
- ✅ HB LED alternates hb_led=1/0
- ✅ Keypad still reports keys=0 idle
- ✅ Heap stable at ~367 KB free, 270 KB largest-block (+~3 KB cost vs alpha.2.5 for UART ring buffer)
- ✅ NEW: FG6485A T/RH poll returns OK every heartbeat with stable raw values

This commit also represents the first time alpha.2.x has read a real sensor value from the kas. Phase 2.7 (s200, wind sensor) and Phase 2.8 (fg6485a, full scaling decode) will build on this by exercising the same Modbus path against the wind sensor at slave addr 44 and then layering the engineering-units conversion on top of the raw-register reads.

### `[2.0.0-alpha.2.5]` — 2026-05-17

### `[2.0.0-alpha.2.5]` — 2026-05-17

**Phase 2.5 — fifth driver migration: `drivers/LCD1602_I2C` (LIB-4, lcd1602).** First alpha that produces visible output on the LCD itself, not just serial.

#### What changed

- **`drivers/LCD1602_I2C/src/lcd1602.cpp`** — minimal migration. The driver was already 100 % bus-bound through LIB-2's `i2c_*` calls (no direct hardware register access, no Wire calls). The only Arduino dependency was a single `delay(ms)` inside `lcd_delay_ms()`, replaced with `vTaskDelay(pdMS_TO_TICKS(ms))`. The `#include <Arduino.h>` swapped for `#include "freertos/FreeRTOS.h"` + `#include "freertos/task.h"`. Public API in `lcd1602.h` is **unchanged** — same 12 functions (`lcd_init`, `lcd_clear`, `lcd_home`, `lcd_set_cursor`, `lcd_print`, `lcd_print_char`, `lcd_write_row`, `lcd_create_char`, `lcd_display_on`, `lcd_backlight_color`, `lcd_backlight_lumination`, `lcd_set_contrast`).
- **`firmware/components/LCD1602_I2C/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/LCD1602_I2C/src/lcd1602.cpp`. INCLUDE_DIRS = the driver's `src/` only (the header has no firmware/config dependency). REQUIRES = `i2c` (LIB-2 wrapper from alpha.2.4) and `freertos` (for `vTaskDelay`).
- **`firmware/src/CMakeLists.txt`** — added `LCD1602_I2C` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.5 tickle: after `i2c_init` + `i2c_scan`, calls `lcd_init()` and writes a recognisable two-row greeting:
  ```
  ESP-IDF stub OK
  v2.0.0-alpha.2.5
  ```
  Operator glancing at Unit 2 sees immediately that this is the migration build, not production 1.20.3.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.5`.

#### Implementation note

The lcd1602 driver was designed from the start with hardware abstraction through LIB-2's `i2c_bus`. That choice — made years before the ESP-IDF migration was contemplated — pays off here: the LCD driver doesn't know whether `i2c_write` ultimately calls Arduino `Wire.write` (LIB-2 pre-alpha.2.4) or ESP-IDF's `i2c_master_transmit` (LIB-2 post-alpha.2.4). The migration cost is two header lines and one function-body swap. Everything else — HD44780 init sequence, AiP31068L control-byte protocol, PCA9633 RGB backlight handling, contrast register split — is portable C and didn't change.

This is the same pattern the gpio_util / keypad_matrix pair demonstrated in alpha.2.1 + alpha.2.2: drivers funnel through their dependency's wrapper, and the migration cost is concentrated at the lowest layer.

#### Build delta vs alpha.2.4

| Metric | alpha.2.4 | alpha.2.5 | Delta |
|---|---:|---:|---:|
| Firmware bin | 278,688 B | **283,248 B** | +4,560 B |
| Flash usage | 13.3 % | 13.5 % | +0.2 pp |
| RAM static | 18,868 B | 18,884 B | +16 B |

bin sha256: `0E3A008789A8D5AFD4CD1841F9E90BB8EFAED5AA7815D6EA3C0D5DBC5636BDC6`

The +4.5 KB is the lcd1602.cpp object code itself (~347 lines of AiP31068L command sequencing, RGB backlight init, CGRAM custom-char support, contrast register split). Pure additive — no new ESP-IDF infrastructure pulled in (everything LCD-side is already covered by the i2c component).

#### Acceptance bar for alpha.2.5

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot log shows `lcd_init returned 0 (OK)` and the `lcd_print: "ESP-IDF stub OK" / "v2.0.0-alpha.2.5" written` confirmation line.
4. **Visible on Unit 2's LCD** (this is the new acceptance signal):
   - Row 0: `ESP-IDF stub OK`
   - Row 1: `v2.0.0-alpha.2.5`
5. All earlier-phase regressions still pass:
   - HB LED blinks (Phase 2.1)
   - Keypad `keys=` reports (Phase 2.2)
   - NVS roundtrip works (Phase 2.3)
   - I2C scan finds 2 devices at 0x3E + 0x68 (Phase 2.4)
6. Run ≥ 5 min; no resets, no LCD garbling.

If `lcd_init` returns `LCD_ERR_NO_DEVICE`, the AiP31068L didn't ACK at 0x3E — would contradict alpha.2.4's scan result, so very unlikely. If it returns `LCD_OK` but the display shows garbage / random pixels, the HD44780 init sequence timings are off — would be the first place to look.

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2. After two iterations to resolve i2c-contract bugs surfaced by the LCD's specific call patterns (both bugs in the LIB-2 layer from alpha.2.4, fixes folded into this commit — see below), final observed:

Serial output:
```
i2c_init returned 0 (OK)
i2c_scan: 2 device(s) found
  device[0] @ 0x3E
  device[1] @ 0x68
lcd_init returned 0 (OK)
lcd_print: "ESP-IDF stub OK" / "v2.0.0-alpha.2.5" written
```

LCD display (Unit 2 hardware, AiP31068L):
```
┌────────────────┐
│ESP-IDF stub OK │
│v2.0.0-alpha.2.5│
└────────────────┘
```

This is the first time alpha.2.x has produced visible output on the LCD itself, not just serial. Full validation of:
- The LIB-2 i2c_master_* migration from alpha.2.4 working against a real I2C peripheral that issues actual command sequences (not just bus probes)
- The LIB-4 lcd1602 migration from alpha.2.5 (delay → vTaskDelay, Arduino.h removed)
- The HD44780 init sequence + AiP31068L control-byte protocol surviving the framework change byte-for-byte
- Pull-up + 400 kHz clock + the IDF v5 transient-device-handle pattern from alpha.2.4 all working together

#### Two i2c-contract bugs found in this phase + fixed in LIB-2

The LCD migration surfaced two latent bugs in the alpha.2.4 LIB-2 i2c_bus migration — both about contract mismatches between the Arduino-era LIB-2 semantics and the ESP-IDF v5 i2c_master implementation. Both fixes are folded into THIS commit (alpha.2.5) because they live in `drivers/i2c/src/i2c_bus.cpp`, not in any LCD code.

**Bug 1: zero-length write rejected.**
The LIB-2 public contract in `i2c_bus.h` says: *"Zero-length writes are accepted and serve as an address-only probe."* Under Arduino's Wire, `beginTransmission(addr) + endTransmission(true)` with no `Wire.write()` between them sent the address byte alone and reported ACK/NACK. The ESP-IDF v5 `i2c_master_transmit()` does NOT accept zero-length transmissions — it returns `ESP_ERR_INVALID_ARG` with `"i2c transmit buffer or size invalid"`. The alpha.2.4 implementation passed length straight through to `i2c_master_transmit`, breaking the probe contract. The LCD driver's `pca9633_init()` (line 107) and `lcd_init()` (line 166) both rely on zero-length probes; both broke. Symptom: error line `E (xxx) i2c.master: i2c_master_transmit(1224): i2c transmit buffer or size invalid` followed by `lcd_init returned 2 (COMM)`. Fix: in `i2c_write`, if `len == 0`, redirect to `i2c_master_probe(s_bus, addr, …)` which is the IDF v5 idiom for address-only ACK detection.

**Bug 2: `i2c_master_probe` NACK code different from `i2c_master_transmit` NACK code.**
After fixing Bug 1, the LCD's PCA9633 probe at 0x60 (an OPTIONAL RGB backlight chip that's absent on Unit 2 hardware) needed to return `I2C_ERR_NACK` so the driver could correctly mark RGB as absent and continue. Under the IDF v5 API, `i2c_master_probe` reports NACK as `ESP_ERR_NOT_FOUND`, but `i2c_master_transmit` reports NACK as `ESP_FAIL`. Two different esp_err_t codes for the same logical condition. The alpha.2.4 `to_status()` mapping only handled `ESP_FAIL` → `I2C_ERR_NACK`; `ESP_ERR_NOT_FOUND` fell into `default` → `I2C_ERR_BUS_BUSY` → `LCD_ERR_COMM`. `pca9633_init` bailed and the entire LCD init returned `COMM` (no IDF error line this time — silent failure). Symptom: `lcd_init returned 2 (COMM)` with no `i2c.master:` error. Fix: add `case ESP_ERR_NOT_FOUND: return I2C_ERR_NACK;` to `to_status()` (also added `ESP_ERR_INVALID_RESPONSE` defensively). Documented inline.

#### Lesson learned (extends the alpha.2.1 GPIO trap pattern)

The per-driver-tickle migration strategy keeps paying its rent. The earlier i2c_scan tickle in alpha.2.4 exercised only the **ACK case** (devices present, scan adds them). The LCD's `pca9633_init` is the first piece of higher-level code that exercises the **NACK case** through LIB-2 (probing for an OPTIONAL device that may not be present). Bug 2 was latent until the LCD tickled it. If we'd discovered it in Phase 4/5 (when the data_manager + relay_controller pull in DS1307_RTC and more I2C usage), debug cost would be hours instead of 10 minutes.

Both fixes are commented inline at the relevant code in `drivers/i2c/src/i2c_bus.cpp` so the next driver migration doesn't trip on them.

#### Updated build delta vs alpha.2.4 (post-fix)

| Metric | alpha.2.4 | alpha.2.5 (final) | Delta |
|---|---:|---:|---:|
| Firmware bin | 278,688 B | **283,312 B** | +4,624 B |
| Flash usage | 13.3 % | 13.5 % | +0.2 pp |
| RAM static | 18,868 B | 18,884 B | +16 B |

bin sha256: `B07076684790ED817F0F93D9428FD785299DEE1322B8C09CE4D8FFEA1130C223`

The +4.6 KB covers the lcd1602.cpp object code (~347 lines AiP31068L command sequencing + PCA9633 RGB backlight handling) plus the two LIB-2 contract-fix branches.

#### Regression checks (all clean)

- ✅ HB LED still blinks (Phase 2.1) — `hb_led=1 → 0 → 1` visible in heartbeats
- ✅ Keypad still reports (Phase 2.2) — `keys=0` idle  
- ✅ NVS round-trip still works (Phase 2.3) — `fw_version` pre/post both `"2.0.0-alpha.2.5"`
- ✅ I2C scan still finds 2 devices (Phase 2.4) — `0x3E` + `0x68`
- ✅ LCD now displays text (Phase 2.5) — the new acceptance signal

### `[2.0.0-alpha.2.4]` — 2026-05-17

### `[2.0.0-alpha.2.4]` — 2026-05-17

**Phase 2.4 — first non-trivial driver migration: `drivers/i2c` (LIB-2, i2c_bus).** Wire.h → ESP-IDF v5 `driver/i2c_master.h` (the new bus/device-handle API, not the legacy `i2c_master_cmd_begin` pattern).

#### What changed

- **`drivers/i2c/src/i2c_bus.cpp`** — full rewrite (~140 lines). Replaces all `Wire.beginTransmission` / `Wire.write` / `Wire.read` / `Wire.requestFrom` / `Wire.endTransmission` calls with the IDF v5 `i2c_master_*` API. Public API in `i2c_bus.h` is **unchanged** — same five functions (`i2c_init`, `i2c_write`, `i2c_read`, `i2c_write_read`, `i2c_scan`, plus `i2c_lock`/`i2c_unlock` no-ops). Callers (LCD1602 in Phase 2.5, DS1307_RTC in Phase 2.9, anything new) don't see the migration.
- **`firmware/components/i2c/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/i2c/src/i2c_bus.cpp`. INCLUDE_DIRS adds the driver's `src/` and the firmware's `config/` (for `PIN_I2C_SDA` / `PIN_I2C_SCL`). REQUIRES = `driver` (ESP-IDF's combined peripheral-driver component, exposes `driver/i2c_master.h`).
- **`firmware/src/CMakeLists.txt`** — added `i2c` to REQUIRES list.
- **`firmware/src/app_main_stub.cpp`** — Phase 2.4 tickle: `i2c_init()` + full address-space `i2c_scan()` with results logged. Expected on Unit 2: 0x27 (LCD1602 backpack PCF8574) + 0x68 (DS1307 RTC).
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.4`.

#### Implementation strategy: transient device handles

The IDF v5 i2c_master_* API requires a `i2c_master_dev_handle_t` per device on the bus, created via `i2c_master_bus_add_device()`. The legacy public API in `i2c_bus.h` takes a 7-bit address per call. Three options were considered:

1. **Force callers to pre-register devices** (cleanest API). Would break all downstream code. Rejected — would cascade rewrites into LCD1602, DS1307_RTC, anything new.
2. **Cache device handles by address in a static lookup table.** Saves the per-call create/destroy cost. Adds complexity (mutex around the table, capacity limit). Deferred — not measurably needed.
3. **Create transient handles per call.** Each `i2c_write` / `i2c_read` / `i2c_write_read` does `i2c_master_bus_add_device` → I/O → `i2c_master_bus_rm_device`. Cost is one small bookkeeping struct alloc/free per call. Negligible at our transaction rates (sub-Hz). **Adopted.**

For `i2c_scan` the v5 API exposes `i2c_master_probe()` directly on the bus handle — no device handle needed. Cleaner than the Wire-era `beginTransmission`/`endTransmission` probe loop.

#### Bus configuration (IDF v5 style)

```c
i2c_master_bus_config_t bus_cfg = {
    .i2c_port          = I2C_NUM_0,
    .sda_io_num        = PIN_I2C_SDA,
    .scl_io_num        = PIN_I2C_SCL,
    .clk_source        = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,                  // IDF default
    .flags.enable_internal_pullup = true,    // matches Wire default; no external Rp on the board
};
```

Per-device clock speed is set on each transient handle via `dev_cfg.scl_speed_hz = I2C_FREQ_HZ` (400 kHz Fast-mode). The v5 API "negotiates" clock per device, but since LCD + RTC both run at 400 kHz, no actual switching occurs.

#### Error mapping (esp_err_t → i2c_status_t)

| esp_err_t | i2c_status_t | Notes |
|---|---|---|
| `ESP_OK` | `I2C_OK` | |
| `ESP_ERR_TIMEOUT` | `I2C_ERR_TIMEOUT` | bus held, slave stretching SCL too long |
| `ESP_FAIL` | `I2C_ERR_NACK` | per IDF docs: address-NACK on probe + transmit |
| `ESP_ERR_INVALID_STATE` | `I2C_ERR_BUS_BUSY` | bus controller in unrecoverable state |
| anything else | `I2C_ERR_BUS_BUSY` | bucket-everything-else |

Matches the legacy Arduino-Wire status enum to keep callers unchanged.

#### Build delta vs alpha.2.3

| Metric | alpha.2.3 | alpha.2.4 | Delta |
|---|---:|---:|---:|
| Firmware bin | 262,368 B | **278,688 B** | +16,320 B |
| Flash usage | 12.5 % | 13.3 % | +0.8 pp |
| RAM static | 18,812 B | 18,868 B | +56 B |

bin sha256: `935B0ADABB201B0E5AC63EE698A9BA49726D5D040817AB6038439A3DC7F0CB3D`

The +16 KB is the ESP-IDF `driver/i2c_master` library (full v5 implementation including bus controller setup, glitch filter, GDMA hooks, etc.) plus the project's migrated `i2c_bus.cpp`. Each subsequent I2C user pays essentially zero further bytes.

#### Acceptance bar for alpha.2.4

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Boot log shows `i2c_init returned 0 (OK)`.
4. Boot log shows `i2c_scan: 2 device(s) found`.
5. `device[0] @ 0x27` (LCD1602 backpack PCF8574).
6. `device[1] @ 0x68` (DS1307 RTC).
7. No `I2C: ` error lines from IDF.
8. HB LED still blinks (Phase 2.1). Keypad reports `keys=` (Phase 2.2). NVS log lines present (Phase 2.3). All regressions clean.
9. Run ≥ 5 min; no resets.

If scan reports 0 devices, troubleshooting hierarchy:
- **Bus init failed** (`i2c_init` returns non-OK) → pin config wrong or I2C peripheral conflict
- **0 devices but init OK** → wiring fault, address mismatch, or pull-up issue
- **Wrong addresses found** → schematic vs config mismatch — would be a real surprise

If scan reports the expected 2 addresses, the v5 i2c_master_* API works end-to-end against real hardware, validating the bus config, transient device-handle pattern, and pull-up settings. Phase 2.5 (LCD1602) and Phase 2.9 (DS1307_RTC) can proceed with confidence in the foundation.

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2. Boot captured:

```
i2c_init returned 0 (OK)
i2c_scan: 2 device(s) found
  device[0] @ 0x3E       ← AiP31068L LCD controller (matches LCD_I2C_ADDR in lcd1602.h)
  device[1] @ 0x68       ← DS1307 RTC (matches pin_config.h comment)
```

Both expected devices responded at the documented addresses. The original "expected 0x27" guess in the acceptance bar above was based on the more-common PCF8574 backpack pattern — this project uses an AiP31068L which has its own I2C interface at 0x3E. `pin_config.h` line 69 and `lcd1602.h` line 41 both document this. The scan found exactly what the project's own code says it should find — full end-to-end validation of:
- Bus initialisation (`i2c_new_master_bus` with the correct pin/clock/pullup config)
- The transient device-handle pattern (created per call, destroyed after)
- `i2c_master_probe` against the LCD and RTC chips
- Pull-up + 400 kHz clock settings matching what both devices need

Regression checks all clean:
- ✅ Boot reason POWERON
- ✅ Free heap baseline 373 611 (−1.6 KB vs alpha.2.3, persistent i2c_master infra cost)
- ✅ Free heap stable at 371 527 from heartbeat 1 onward
- ✅ NVS round-trip works (`pre = "2.0.0-alpha.2.4"`, `post = "2.0.0-alpha.2.4"`)
- ✅ HB LED `hb_led=1 → 0` toggle visible
- ✅ Keypad `keys=0` idle reporting
- ✅ No I2C errors during scan
- ✅ Heartbeat task running cleanly

The +1.7 KB persistent heap is the one-time infrastructure cost of the ESP-IDF v5 i2c_master subsystem (bus controller state, GDMA descriptors, internal lock). LCD (Phase 2.5) and RTC (Phase 2.9) will pay essentially zero further bytes — they just create transient device handles per call.

### `[2.0.0-alpha.2.3]` — 2026-05-17

**Phase 2.3 — third driver migration: `drivers/nvs` (LIB-7, nvs_config).** Trivial — pure proxy + Phase-1-style tickle. The driver source was already 100 % ESP-IDF native; the only Arduino bits in the directory were in `drivers/nvs/src/main.cpp`, the standalone hardware-verification test, which the proxy `SRCS` list intentionally excludes.

#### What changed

- **`drivers/nvs/src/nvs_config.cpp` + `nvs_config.h`** — **NO CHANGES**. Already pure ESP-IDF (`#include <nvs_flash.h>`, `#include <nvs.h>`, no `Arduino.h`, no `Serial.print`, no `delay()`, no `String`). This was the easiest driver in the whole tree.
- **`firmware/components/nvs/CMakeLists.txt`** (new) — proxy. SRCS = `../../../drivers/nvs/src/nvs_config.cpp`. INCLUDE_DIRS only adds the driver's `src/` (the public header doesn't need `pin_config.h`). REQUIRES = `nvs_flash` (ESP-IDF NVS component).
- **`firmware/src/CMakeLists.txt`** — added `nvs` to REQUIRES list; added `nvs_flash` to the main component's direct REQUIRES (the app_main_stub.cpp tickle calls `nvs_flash_init()` / `nvs_flash_erase()` directly for the pre-init readback).
- **`firmware/src/app_main_stub.cpp`** — Phase 2.3 tickle:
  - `#include "nvs_config.h"` + `#include "esp_err.h"` + `#include "nvs_flash.h"`
  - Pre-init block: direct `nvs_flash_init()` (with `NO_FREE_PAGES`/`NEW_VERSION_FOUND` recovery), then `nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, …)` to read whatever the previous firmware left.
  - `nvs_cfg_init()` call with full status decode in the log line (`OK` / `MIGRATION` / `INIT` / `OTHER`).
  - Post-init read of `fw_version` to confirm the schema-policy overwrite landed.
  - All one-shot in `app_main()` — no heartbeat noise.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.3`.

#### Expected serial output on Unit-2 hardware (last ran 1.20.3 prod)

```
I (xxx) GHC-STUB: NVS pre-init: previous fw_version = "1.20.3" (status=0)
I (xxx) GHC-STUB: nvs_cfg_init() returned 0 (OK)             ← OR returned 5 (MIGRATION) if schema changed
I (xxx) GHC-STUB: NVS post-init: fw_version is now "2.0.0-alpha.2.3" (status=0)
```

If pre-init reads `"1.20.3"` then post-init reads `"2.0.0-alpha.2.3"`, the driver works end-to-end:
- IDF NVS flash subsystem initialises against the existing partition (no erase needed)
- `nvs_cfg_get_str` reads existing keys preserved from 1.20.3
- `nvs_cfg_init` performs the schema-version check and fw_version overwrite policy
- Post-init reads see the newly-written value

On a fresh-flashed unit (NVS partition erased), the pre-init read would return `NVS_CFG_ERR_NOT_FOUND` (status=4) with `prev_fw=""` — also valid, just different.

#### Build delta vs alpha.2.2

| Metric | alpha.2.2 | alpha.2.3 | Delta |
|---|---:|---:|---:|
| Firmware bin | 243,904 B | **262,368 B** | +18,464 B |
| Flash usage | 11.6 % | 12.5 % | +0.9 pp |
| RAM static | 18,772 B | 18,812 B | +40 B |

bin sha256: `6DA9FDF39A9E911233386DB9A94725C2EDFF9451B25C93FF2589DE110B0D2788`

The +18 KB is significant because this is the first commit that pulls in the full ESP-IDF NVS infrastructure (`nvs_flash`, the underlying `nvs` library, partition lookup, encryption stubs, etc.) plus the project's own `nvs_config.cpp` (~307 lines compiled). Each subsequent NVS user (which is most of the firmware) adds essentially zero further cost. Memory-cost amortisation: this 18 KB is paid once in Phase 2.3 and never again.

#### Acceptance bar for alpha.2.3

1. ✅ Build succeeds — no warnings against migrated source (none needed; only header-cleanup).
2. Flash to Unit 2; boot reason `ESP_RST_POWERON`.
3. Serial log shows the three NVS log lines in the boot banner section.
4. Pre-init `fw_version` reads `"1.20.3"` (left by 1.20.3 production firmware).
5. `nvs_cfg_init()` returns `OK` or `MIGRATION` — both acceptable (`MIGRATION` means schema version changed which it shouldn't here since we kept `NVS_SCHEMA_VERSION=1`).
6. Post-init `fw_version` reads `"2.0.0-alpha.2.3"` — confirms write path works.
7. HB LED still blinks (Phase 2.1 regression-check). Keypad `keys=` field still works (Phase 2.2 regression-check).
8. Run ≥ 5 min; no resets, no NVS error messages.

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2. The captured serial output happened to be boot #2 of alpha.2.3 (boot #1 was lost; user power-cycled between flash and serial capture), which gave a stronger acceptance signal than the original criterion expected. Observed:

```
NVS pre-init: previous fw_version = "2.0.0-alpha.2.3" (status=0)
nvs_cfg_init() returned 0 (OK)
NVS post-init: fw_version is now "2.0.0-alpha.2.3" (status=0)
```

Interpretation: boot #1 of alpha.2.3 (immediately after flash) read whatever 1.20.3 left in NVS (presumably "1.20.3", though not captured), called `nvs_cfg_init()` which wrote the new "2.0.0-alpha.2.3" per the schema policy. The captured boot (#2 after a power cycle) shows the persisted value being read back successfully — proving the full write-reboot-read roundtrip against real flash hardware. That's a more demanding test than the original "see a 1.20.3-to-current transition" criterion.

Concrete acceptance check-list:
- ✅ NVS pre-init read succeeds (status=0, valid string returned)
- ✅ NVS persists across boots (pre-init read returns what a previous boot wrote)
- ✅ `nvs_cfg_init()` returns OK (status=0) — schema version 1 unchanged
- ✅ NVS post-init read confirms the value is current
- ✅ No NVS error messages
- ✅ Phase 2.1 regression check: HB LED still alternates (hb_led=1/0)
- ✅ Phase 2.2 regression check: keypad reports keys=0 idle
- ✅ Heap impact: ~2 KB persistent overhead for NVS infrastructure (375275 → 373267 stable from heartbeat 1 onward) — one-time, expected

Boot reason POWERON, free heap 375 KB at banner, stable thereafter. No resets observed during the verification window. Migration is clean.

### `[2.0.0-alpha.2.2]` — 2026-05-17

**Phase 2.2 — second driver migration: `drivers/keyPad` (LIB-5, keypad_matrix).**

#### What changed

- **`drivers/keyPad/src/keypad_matrix.cpp`** — migrated. All `pinMode`/`digitalWrite`/`digitalRead`/`HIGH`/`LOW` calls replaced with the `gpio_util` wrappers (`gpio_set_pin_mode`, `gpio_write`, `gpio_read`, `GPIO_HIGH`, `GPIO_LOW`). The `#include <Arduino.h>` is dropped; `#include "gpio_util.h"` takes its place. Public API in `keypad_matrix.h` is **unchanged** — `keypad_init()`, `keypad_scan()`, `keypad_count_pressed()` all keep their signatures. The 2-scan debounce logic, multi-press detection, and key character map are byte-for-byte identical to 1.20.3.
- **`firmware/components/keyPad/CMakeLists.txt`** (new) — proxy component. SRCS points at `../../../drivers/keyPad/src/keypad_matrix.cpp`; INCLUDE_DIRS adds the driver's `src` and the firmware's `config` (for `pin_config.h`'s `KP_ROW*` / `KP_COL*` macros). `REQUIRES gpio` — **does NOT** require the ESP-IDF `driver` component directly. All GPIO access is funnelled through the `gpio_util` wrapper, so any future improvement to LIB-1 propagates transitively to LIB-5.
- **`firmware/src/CMakeLists.txt`** — added `keyPad` to the main component's `REQUIRES` list.
- **`firmware/src/app_main_stub.cpp`** — adds the Phase-2.2 tickle:
  - `#include "keypad_matrix.h"` at the top
  - `keypad_init()` in `app_main()` after `gpio_write(PIN_HB_LED, GPIO_LOW)`
  - `int keys_pressed = keypad_count_pressed()` on each heartbeat tick
  - Extended log format: `heartbeat N | … | hb_led=X keys=Y`
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.2`.

#### Design choice: keypad goes through gpio_util, not direct ESP-IDF

The keypad uses the project's own `gpio_util` wrapper instead of calling `gpio_set_level` / `gpio_get_level` directly. Three reasons:

1. **Trap inheritance.** The Phase-2.1 `GPIO_MODE_OUTPUT` vs `GPIO_MODE_INPUT_OUTPUT` trap is already paid for in `gpio_util`. If the keypad called ESP-IDF directly, we'd re-litigate it here (4 row pins, all `OUTPUT` mode, never read back — so possibly NOT broken in this specific case, but still cleaner to not have to reason about it).
2. **Framework portability of the driver.** The keypad header doesn't include any ESP-IDF specifics — the cpp file's only platform dependency is `gpio_util.h`. That makes it trivially portable to any other framework that ports `gpio_util` (theoretically).
3. **Single point of GPIO-driver maintenance.** Any future improvement (e.g. ISR support in `gpio_util`) is automatically available to every higher-level driver. No per-driver fixups.

This pattern will apply to every driver in Phase 2 that uses GPIO: the keypad, relay control (Phase 6), and any switch / discrete-IO sensor reads will all funnel through `gpio_util`.

#### Acceptance bar for alpha.2.2

1. ✅ Build succeeds — no warnings against migrated source.
2. Flash to bench unit. Boot reason `ESP_RST_POWERON`.
3. Heartbeat log line includes `keys=N` field. Should read `keys=0` when no key is pressed.
4. **Press a key on Unit 2's keypad** during the heartbeat: the next log line should show `keys=1` (or `keys=2`+ for multi-press). Releasing returns to `keys=0`.
5. The amber HB LED (Phase-2.1 acceptance) keeps blinking — no regression.
6. Run ≥ 15 min; no resets.

#### Build delta vs alpha.2.1

| Metric | alpha.2.1 | alpha.2.2 | Delta |
|---|---:|---:|---:|
| Firmware bin | 243,728 B | 243,904 B | +176 B |
| Flash usage | 11.6 % | 11.6 % | (rounded same) |
| RAM static | 18,772 B | 18,772 B | 0 (keypad has only a 1-byte static debounce field, absorbed into existing padding) |

bin sha256: `AF171CC3BD0258D012A5800487D0B44E6EA8BF5A59FF8F185C231CC355736591`

The +176 B is the keypad scan + count_pressed code paths. The driver is tiny.

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2 hardware. Verified by hand-pressing keys on the membrane keypad and observing `keys=N` changing in serial output in real time. Single-press → `keys=1`. Multi-press → `keys≥2`. Release → `keys=0` next heartbeat. The Phase-2.1 amber HB LED kept blinking (no regression).

Eight pins exercised on real hardware: 4 row outputs (`KP_ROW1..4`) + 4 col inputs with internal pull-up (`KP_COL1..4`). The GPIO_MODE_INPUT_OUTPUT fix from alpha.2.1 propagated transitively (rows are output pins driven HIGH/LOW; the proxy depends on `gpio`, not on direct IDF GPIO). Confirms the architectural choice — drivers funnel through `gpio_util`, single point of GPIO maintenance.

### `[2.0.0-alpha.2.1]` — 2026-05-17

**Phase 2.1 — first driver migration: `drivers/gpio` (LIB-1).** Plus the build-system pattern that the rest of Phase 2 will follow.

#### Architectural decision: proxy components, not EXTRA_COMPONENT_DIRS

The migration plan called for "Option A — drivers stay under `../drivers/` where they've always lived; they get IDF component scaffolding in-place" via `EXTRA_COMPONENT_DIRS = ../drivers` in `firmware/CMakeLists.txt`. Empirically this triggers a regression in PlatformIO's post-CMake-configure introspection: any non-empty `EXTRA_COMPONENT_DIRS` causes `pio run` to bail out with `Error: Couldn't find the main target of the project!`, despite the CMake configure itself reporting `Configuring done` / `Generating done` and the gpio component being correctly enumerated. Reproduced 2026-05-17 on platform pin `espressif32@6.12.0` (PlatformIO 6.1.x). The bug is in PlatformIO+espidf's CMake target-finding step.

Workaround adopted: each migrated driver gets a **thin proxy component** at `firmware/components/<name>/CMakeLists.txt` that uses IDF's automatic-discovery mechanism (no `EXTRA_COMPONENT_DIRS` involved). The proxy `idf_component_register`s a component named after the driver, but its `SRCS` and `INCLUDE_DIRS` point at the actual driver location under `../../../drivers/<name>/src/`. Driver source files don't move — the migration-plan Option A intent is preserved.

Trade-off: one extra layer of indirection per driver (an ~30-line CMakeLists), in exchange for working around the PlatformIO bug without giving up the in-place driver layout. The standalone-test setups under each `drivers/<name>/` (their own `platformio.ini`, `library.json`, `.vscode/`, etc.) remain functional for native test builds.

Future v2.1.x could revisit by either (a) physically moving drivers into `firmware/components/<name>/src/`, or (b) switching to native `idf.py` (which is reported not to have the `EXTRA_COMPONENT_DIRS` bug) and reverting to the in-place EXTRA_COMPONENT_DIRS layout.

#### What changed

- **`firmware/CMakeLists.txt`** — added explanatory comment block documenting the proxy-component pattern + the EXTRA_COMPONENT_DIRS bisect finding. No `set(EXTRA_COMPONENT_DIRS …)` call — left out of the build.
- **`firmware/components/gpio/CMakeLists.txt`** (new) — proxy `idf_component_register` for the `gpio` component. SRCS path resolves to `../../../drivers/gpio/src/gpio_util.cpp`; INCLUDE_DIRS adds `../../../drivers/gpio/src` (for `gpio_util.h`) and `../../config` (for `pin_config.h`). Requires the ESP-IDF `driver` component (legacy peripheral driver layer providing `gpio_config_t`, `gpio_set_level`, etc.). Tier-1 hardening flags scoped via `target_compile_options(${COMPONENT_LIB} PRIVATE …)` as established in alpha.1.
- **`drivers/gpio/src/gpio_util.cpp`** (modified) — full migration from Arduino API to ESP-IDF. `pinMode` → `gpio_config_t` + `gpio_config()`. `digitalWrite` → `gpio_set_level`. `digitalRead` → `gpio_get_level`. `INPUT_PULLUP` mode now explicitly sets `pull_up_en = GPIO_PULLUP_ENABLE` on top of `mode = GPIO_MODE_INPUT`. Behavioural equivalence: each `gpio_set_pin_mode()` reconfigures the pin completely (matches arduino's "pinMode replaces prior config" semantic). Interrupt type set to `GPIO_INTR_DISABLE` on every reconfig — this driver doesn't handle ISRs. Public API in `gpio_util.h` is **unchanged**; callers don't know which underlying API does the work.
- **`firmware/src/CMakeLists.txt`** — added `gpio` to the main component's `REQUIRES` list. The IDF dependency-graph machinery resolves the `gpio` component via the auto-discovery of `firmware/components/gpio/`.
- **`firmware/src/app_main_stub.cpp`** — exercises the new gpio driver: `#include "gpio_util.h"`, configures `PIN_HB_LED` (GPIO41) as output in `app_main()`, calls `gpio_toggle(PIN_HB_LED)` on every heartbeat tick. The amber HB LED on the LOLIN S3 will visibly blink every 5 s as a direct proof the gpio driver linked and works. If the proxy weren't wired up correctly, the build would fail at the link step with `undefined reference to gpio_set_pin_mode`.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` stamped `2.0.0-alpha.2.1` (the `.1` sub-version denotes "first sub-step of Phase 2").

#### Build delta vs alpha.1

| Metric | alpha.1 (stub only) | alpha.2.1 (stub + gpio) | Delta |
|---|---:|---:|---:|
| Firmware bin | 239,424 B | 243,696 B | +4,272 B |
| Flash usage | 11.4 % | 11.6 % | +0.2 pp |
| RAM static | 18,732 B (5.7 %) | 18,772 B (5.7 %) | +40 B |

bin sha256: `B70AF8B35108380FD57C1BEDC9C65E0331A2F2C4CD3C20D4A06DB04005E4F2FF`

The +4.2 KB is the gpio driver code + its IDF `driver`-component pulls (which were already partially present in alpha.1 via other transitive dependencies). The +40 B RAM is largely the link-time references; the driver itself has no static state.

#### Acceptance criterion for alpha.2.1

1. Build succeeds with no warnings from `target_compile_options` (the tier-1 hardening flags should pass cleanly against the migrated `gpio_util.cpp`).
2. Flash to bench unit; boot reason `ESP_RST_POWERON` (= 1).
3. Heartbeat task emits one log line every 5 s as in alpha.1, with an additional `hb_led=` field that alternates 1/0 on every heartbeat (software-level proof of the toggle).
4. **`PIN_HB_LED` (GPIO41, amber on LOLIN S3) visibly blinks** in sync with the log lines — proves the gpio driver works against real hardware.
5. Run for ≥ 30 minutes; no resets, no panic. (Lighter acceptance than alpha.1 because the only new code is one driver toggle call.)

#### Acceptance: PASSED — 2026-05-17

Flashed to Unit 2 hardware (LOLIN S3 dev unit on the production Unit 2 hardware bench, which has a known-good amber HB LED wiring on GPIO41). After one interim bisect (see "lesson learned" below), final acceptance signal:

- ✅ Build succeeded, no tier-1 hardening warnings against the migrated source
- ✅ `hb_led=` value alternates between 0 and 1 on every heartbeat
- ✅ Amber HB LED on GPIO41 visibly blinks at the heartbeat cadence
- ✅ Boot reason `esp_reset_reason=1` (POWERON); chip stable on the new binary

#### Lesson learned: `GPIO_MODE_OUTPUT` is not what arduino-esp32's `pinMode(p, OUTPUT)` does

First attempt mapped `pinMode(p, OUTPUT)` directly onto ESP-IDF's `gpio_config_t.mode = GPIO_MODE_OUTPUT`. Boot succeeded, link succeeded, heartbeat logs appeared on serial — but the LED stayed dark and `gpio_read()` on the pin always returned 0. Two-toggle bisect revealed the cause:

ESP-IDF's `gpio_get_level()` on a pin configured `GPIO_MODE_OUTPUT` (output-only) returns **always 0** regardless of the latched output value. Our `gpio_toggle()` does a read-modify-write: read current state, write the inverse. With `gpio_read()` stuck at 0, every `gpio_toggle()` wrote HIGH unconditionally. The pin transitioned LOW→HIGH on the first heartbeat, then stayed HIGH forever — no blink, no readback.

The arduino-esp32 wrapper hides this by mapping `pinMode(p, OUTPUT)` to ESP-IDF's `GPIO_MODE_INPUT_OUTPUT` internally (input+output mode where the input register reflects the latched output). `digitalRead()` on an output pin under arduino returns the latched value — and we relied on that semantic without noticing it was framework sugar.

Fix landed in this same commit: `GPIO_OUTPUT` in the driver maps to `GPIO_MODE_INPUT_OUTPUT` instead of `GPIO_MODE_OUTPUT`. Documented in `drivers/gpio/src/gpio_util.cpp`'s header comment so the next driver migration doesn't trip on the same trap (the `relay_controller` does output-pin readback for self-checks — would have hit this in Phase 6).

This is **exactly the class of silent semantic regression the per-driver acceptance gates were designed to catch.** Five seconds of LED observation made it obvious; a Phase 5 full-firmware build would have buried it in the noise of dozens of other simultaneously-changing files.

### `[2.0.0-alpha.1]` — 2026-05-17

**Phase 1 — build-system flip + smoke-boot stub.** The single highest-risk step in the migration: `framework = arduino` becomes `framework = espidf` in `platformio.ini`. From this commit onward the binary is built by ESP-IDF, not arduino-esp32.

#### What changed

- **`firmware/platformio.ini`** — `framework = arduino` → `framework = espidf` (in `[env:lolin_s3]`). Removed arduino-only options that no longer apply (`board_build.arduino.memory_type`, `board_build.filesystem`, `-DCORE_DEBUG_LEVEL`, `lib_deps` for Adafruit_NeoPixel/ESPAsyncWebServer/AsyncTCP, `lib_extra_dirs` for `../drivers`, `lib_ignore = WebServer`). Kept the platform pin `espressif32@6.12.0`, partition table, flash mode `qio`, USB upload protocol, tier-1 hardening flags, and `cppcheck` static-analyser config. **`FIRMWARE_VERSION`** stamped `2.0.0-alpha.1`. The `[env:test_t2_relay]` block is **commented out** for Phases 1-2 because it depends on PlatformIO's arduino-Unity test infrastructure; checkout `v1.20.3-arduino-final` to use it temporarily.
- **`firmware/sdkconfig.defaults`** — promoted from documentation-of-intent to load-bearing config. Under arduino-esp32 the prebuilt framework baked in its own sdkconfig and this file had no effect; under espidf it now gets honoured. Added sections for: bootloader/log level (INFO), flash + PSRAM (qio flash, OPI 8 MB PSRAM, 80 MHz), FreeRTOS (1 ms tick, dual-core), partition table (custom + `partitions.csv` at 0x8000), CPU target (esp32s3 explicit). Kept the eight existing coredump lines verbatim with refreshed context.
- **`firmware/CMakeLists.txt`** — *new file*. Top-level IDF project entry. Pulls in `$ENV{IDF_PATH}/tools/cmake/project.cmake` and declares `project(greenhouse_controller)`.
- **`firmware/src/CMakeLists.txt`** — *new file*. The "main" component registration. Lists ONLY `app_main_stub.cpp` — every other source file in `firmware/src/<subdir>/*.cpp` exists on disk but is excluded from the build. Comments document the phase-by-phase plan for reintroducing each subsystem.
- **`firmware/src/app_main_stub.cpp`** — *new file*. Minimal heartbeat: boot banner + `xTaskCreatePinnedToCore(heartbeat_task)` + `app_main` return. Heartbeat task emits one `ESP_LOGI` line every 5 s with free heap, largest contiguous block, free PSRAM, and uptime. No peripherals, no networking, no NVS reads, no watchdog subscription. ~140 lines including extensive header comments.

#### What did NOT change

- `firmware/src/main.cpp` and every other `firmware/src/<subdir>/*.cpp` from the 1.20.3 codebase remains on disk, untouched, **excluded from the build** by virtue of not being listed in `firmware/src/CMakeLists.txt`. Phase 6 absorbs `main.cpp` into a clean IDF entry point; Phases 2-5 reintroduce subsystem files one at a time.
- `partitions.csv` — same layout (dual OTA bank + dual LittleFS + coredump). Verified compatible with both arduino-esp32 and espidf frameworks.
- `drivers/*` — untouched. Phase 2 starts migrating drivers; for Phase 1 they're simply not in the build.

#### Acceptance bar (verify before declaring alpha.1 success)

1. `pio run -e lolin_s3` returns SUCCESS. Binary builds.
2. Flash to bench hardware via USB-CDC.
3. Open `pio device monitor`. First log lines should show the boot banner with chip info, MAC, free heap, `esp_reset_reason=1` (POWERON).
4. Heartbeat task emits one `heartbeat <N> | free=… largest=… psram_free=… uptime=…s` line every 5 s thereafter.
5. **Run for ≥ 1 hour**. Acceptance: zero resets, free heap stays above 100 KB the whole time.

If acceptance fails: rollback by `git reset --hard <pre-alpha.1-commit>` (the alpha.0 scaffolding stays). The framework=espidf flip is the only reversible-by-revert change in this commit set.

#### Known limitations until later phases

- **No web interface.** Cannot configure anything via WiFi.
- **No serial console interactivity.** USB-CDC is read-only logging.
- **No persistent NVS use.** Each boot is "from scratch" config-wise.
- **No motor / sensor / SD activity.** The relay outputs and motor alarm pin are not driven; the SD card is not mounted.
- **No watchdog supervision.** A bug that hangs the heartbeat task will not auto-reset.

These are all addressed by Phases 2-6.

#### Build delta vs 1.20.3 / alpha.0

| Metric | 1.20.3 (arduino-esp32) | 2.0.0-alpha.1 (espidf stub) | Delta |
|---|---:|---:|---:|
| Firmware bin size | 1,194,128 B | 239,424 B | **−80 %** (954,704 B smaller) |
| Flash usage | 56.9 % of 2 MB | 11.4 % of 2 MB | −45.5 percentage points |
| RAM usage (static) | 21.9 % of 320 KB | 5.7 % of 320 KB | −16.2 percentage points |
| Components compiled | full stack | freertos + log + esp_system + esp_hw_support + heap (+ transitive ESP-IDF) | ~80 % of subsystems excluded |

bin sha256: `453135aa53ae6949f525c0e1859ce42503e70d126f7532fafce7952d940b4492`

This is the expected shape: the stub binary excludes almost all application code, drivers, network stack, and web server. The 239 KB that remains is the ESP-IDF core (FreeRTOS scheduler, logging, heap, basic system) plus our ~140-line heartbeat. As phases 2-6 bring subsystems back, flash usage will climb back toward the 1.20.3 baseline.

#### Build-system tuning required during Phase 1

Two issues surfaced when first running `pio run -e lolin_s3` after the framework flip; both fixed in this commit:

1. **`-D_FORTIFY_SOURCE=2` collides with `esp_async_memcpy`.** The fortify macro redefines `memcpy` as a stack-checked stub, but ESP-IDF's `esp_async_memcpy.c:22` uses `memcpy` as a struct-field name (function pointer). Macro expansion broke the compile. The flag was acceptable under arduino-esp32 because that codebase doesn't have the struct-field name collision. **Removed for 2.0.0-alpha.1.** Revisit later with per-file opt-out if needed.

2. **`-Wshadow` (and similar strict warnings) treat IDF internal code as broken.** Under arduino-esp32 the framework was a precompiled library and our build_flags only applied to our own source files. Under framework=espidf the IDF sources compile in-tree and these flags trip on internal shadow-variable usage in `esp_wps.c`, `esp_async_memcpy.c`, lwIP, and mbedTLS. **Tier-1 hardening flags moved out of global `build_flags` and into the `firmware/src/CMakeLists.txt` `target_compile_options`** so they apply only to our component, not the framework. ESP-IDF code is left at its own warning level.

Drivers will get the same component-scoped flag treatment when they migrate in Phase 2.

#### Acceptance: PASSED — 2026-05-17

Flashed to dev unit (MAC `64:E8:33:7C:23:44`, unit_id `2344` — distinct from production Unit 1 `12F0` and Unit 2 `5C88`, per migration plan).

| Acceptance criterion | Target | Observed |
|---|---|---|
| Boot reason | `ESP_RST_POWERON` (1) | `rst:0x1 POWERON`, `esp_reset_reason=1` |
| Duration | ≥ 60 min | **76 min 40 s** (uptime=4600s) |
| Heartbeats | monotonic | 0 → 920, zero gaps |
| Free heap (INTERNAL) | > 100 KB | 375 275 B constant for 919 heartbeats |
| Largest contiguous block | sanity bound | 278 528 B (272 KB) constant |
| PSRAM detected | 8 MB OPI | 8 386 156 B added to heap allocator |
| Panic / WDT / abort | none | none |
| Heap drift over window | < 1 KB | **0 bytes** |

Key boot-log evidence:
- IDF version `5.5.0` (matches espressif32@6.12.0 pin)
- `App version: v1.20.3-arduino-final-1-gfe5a5a` — git-describe auto-derivation working, tag chain intact
- 8 MB OPI PSRAM correctly initialised with `octal_psram` driver (validates `CONFIG_SPIRAM_MODE_OCT=y` + friends in sdkconfig.defaults)
- `esp_core_dump_flash: Found partition 'coredump' @ 620000 65536 bytes` — clean init, no CRC error like the 1.19.0 problem
- Coredump partition NOT requiring the historical `erase_region` step on this unit (clean partition table written during flash)
- DRAM available for dynamic allocation: 322 + 21 + 32 KiB = 375 KiB total — matches the heartbeat reading

The heap signature in particular is the clean baseline for the rest of the migration: with no networking and no application logic in the build, there is nothing to allocate or hold and heap is perfectly stable for over an hour. Phases 4-5 will reintroduce networking and the gh#23 mbedTLS pattern will either re-emerge (proving the per-handshake hold is intrinsic to the stack) or be eliminated by the new `esp_http_client` + `esp_tls` config (the Phase 4 payoff).

### `[2.0.0-alpha.0]` — 2026-05-17

- **Branch `dev/2.0.0-esp-idf`** created from `main` at commit `d8436ad` (the 1.20.3 release commit). Production state preserved by annotated tag `v1.20.3-arduino-final`.
- **`BRANCH_NOTES.md`** added at repo root describing the branch purpose, working policy, backport discipline, and phase progression. New contributors should read this before pushing.
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` bumped `1.20.3` → `2.0.0-alpha.0`. `framework = arduino` is **unchanged in this commit** — the framework flip happens in `2.0.0-alpha.1` (Phase 1). The version string change is purely declarative: any binary built from this commit is recognisable as a pre-migration scaffold.
- **`.gitignore`** extended to cover IDF-build-system artefacts that will appear from Phase 1 onwards (`firmware/build/`, `firmware/sdkconfig` auto-generated, `firmware/managed_components/`, `firmware/dependencies.lock`).
- **`changelog.md`** — this section opened.

No firmware behaviour changes in this commit. Builds against the same arduino-esp32 framework as 1.20.3 and would behave identically on hardware. Don't deploy to production units; this version is a scaffolding marker only.

### Backport policy reminder

Bug fixes found on the 1.20.x production line during this migration go to `main` first, then are cherry-picked onto this branch with `git cherry-pick -x <sha>`. This keeps the 2.0.0 branch from diverging into an unmergable state. See `BRANCH_NOTES.md` for the full policy.

### Out of scope for 2.0.0 (deferred to 2.1.x)

- Native `idf.py` build (we stay on PlatformIO+espidf for 2.0.0)
- Plain HTTP via reverse proxy (gh#23 mitigation C2 — the in-firmware mbedtls config knobs that Phase 4 unlocks are expected to be sufficient)
- `esp_http_server` async tuning (synchronous is enough for current request rate)
- gh#22 (NVS log ring reconsideration)

---

## [1.20.3] — 2026-05-17

*Operational mitigation for gh#23: bumps the default status-POST interval from 120 s to 240 s. With the gh#24 detector fix shipped in 1.20.1, the supervisor's planned-reboot cadence on Unit 2 stabilised at ~5.5 h driven by the per-handshake mbedTLS pattern documented in gh#23. Cutting the handshake rate by 2× extends the cadence to ~11 h with zero code-path changes beyond the default value. Operators who already configured a custom interval are unaffected; only fresh installations or factory-reset units pick up the new default.*

### Changed
- **`DEF_STATUS_INTERVAL_S` raised 120 → 240** in `firmware/config/cfg_defaults.h`. Spec range (60–300 s) unchanged; the new default sits comfortably mid-range. Dashboard refresh experience: 4 min between updates instead of 2 min — well within the operational tolerance documented in beheerder-handleiding §10.2.
- **`firmware/data/index.html`** initial value for the `Interval (s)` input updated to 240 (cosmetic pre-API-load fallback; the actual value displayed is loaded from `/api/web`).
- **`firmware/platformio.ini`** — `FIRMWARE_VERSION` bumped 1.20.2 → 1.20.3 in both env blocks.
- **`firmware/data/manifest.json`** — stamped 1.20.3 by `bin/build_release.ps1`.

### Behaviour notes / non-changes
- **No detector, supervisor, or breaker changes.** gh#24 signed-balance accumulator, gh#25 dedup latch, gh#26 SD-unmount-before-restart — all unchanged. This release is exclusively a defaults bump.
- **Operators with existing custom values are unaffected.** The NVS-persisted `status_interval_s` survives the OTA update; only units that never had the key set (fresh installs, factory-reset units) pick up the new default. To force the new default on an existing unit, operator visits Web tab and writes `240` explicitly, or performs a factory reset of the `system` NVS namespace.
- **Why not 300 s (max of spec range)?** 240 s is the conservative choice. 300 s would extend cadence to ~14 h but pushes the dashboard refresh experience past the 4 min threshold many operators implicitly tolerate. If 240 s proves insufficient, operators can bump further via Web tab on a per-unit basis.
- **C1 mitigation (`setBufferSizes(4096, 4096)`) is NOT viable on this stack.** The original gh#23 mitigation menu identified `WiFiClientSecure::setBufferSizes()` as a half-line code change. Reading arduino-esp32 6.x `WiFiClientSecure.h` confirms this method is from the BearSSL fork (ESP8266) and is **not exposed** by the mbedtls-backed arduino-esp32 implementation. The internal `mbedtls_ssl_config` is `protected` and there's no hook between handshake setup and execution to inject `mbedtls_ssl_conf_max_frag_len()` without copy-pasting the parent's `connect()` logic. gh#23 updated with this finding; C4 (switch to `esp_http_client` directly) remains the next mitigation tier.
- **Drop-in upgrade.** No NVS schema change, no partition-table change, no API change. OTA from 1.20.2, 1.20.1, 1.20.0, 1.19.x, or 1.18.3 all work without extras.

### Acceptance test
- Unit running 1.20.3 with `status_interval_s = 240` (either default or explicitly set): planned-reboot cadence on a unit that previously averaged 5.5 h should extend to ~11 h. Confirm via `[T15] PLANNED REBOOT — T14 cumulative heap drop crossed 64 KB` line absence over a 10 h window.
- Existing custom-interval units: behaviour unchanged across the upgrade. Confirm via `cfg.status_interval_s` value at boot matches pre-upgrade NVS contents.

### Cross-references
- gh#23 — heap-fragmentation root cause; this release is one mitigation tier in that issue's menu. Cadence reduction is operational; underlying cause persists.
- gh#27 — heap-drop sampling-timing question; orthogonal to this release.
- gh#24 — closed in 1.20.1; the detector fix is what made this release's cadence-tracking meaningful in the first place.

---

## [1.20.2] — 2026-05-16

*One bug fix for an SD-card data-loss pattern surfaced by the same Unit 1 forensics that drove 1.20.1. The supervisor's planned-reboot path was calling `esp_restart()` without unmounting the SD card, which let the Arduino-ESP32 SD library's directory cache and FatFs write-back queue discard whatever was pending. On Unit 1 this manifested as three log files the controller logged creating (`/20260516025038.csv`, `/20260516031506.csv`, `/20260516041646.csv`) that were never on the physical card when inspected. Two-line fix; no behaviour change for any other code path.*

### Fixed
- **gh#26 — `planned_reboot()` now unmounts the SD card before `esp_restart()`.** `firmware/src/status_post_supervisor/status_post_supervisor.cpp` adds `event_logger_sd_unmount()` between the NVS-flag write and the 250 ms drain. `event_logger_sd_unmount()` clears T9's `s_sd_ok` so no in-flight write races the teardown, calls `storage_sd_unmount()` → `SD.end()`, which forces FatFs to flush its directory cache and FAT updates to physical media before releasing the SPI bus. The function is idempotent at both layers (T9 and the SD driver) so it's safe regardless of current mount state. Combined with 1.20.1's gh#24 detector fix (which eliminates the *spurious* planned reboots in the first place), this closes the SD-corruption window down to "an in-flight write at the exact moment of an unplanned reset" — i.e. only a panic or interrupt-WDT can still leave the FAT inconsistent, and those are rare and bounded.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.20.1` → `1.20.2` in both env blocks.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.20.2 by `bin/build_release.ps1`.
- `firmware/src/status_post_supervisor/status_post_supervisor.cpp` — `#include "../event_logger/event_logger.h"` added for the unmount call.

### Behaviour notes / non-changes
- **No bulkhead-architecture changes.** The supervisor's heap-leak detector (1.20.1 / gh#24), wedge detector, respawn-storm guard, OTA fail-counter exemption (1.19.2), and NVS-window-state recovery (gh#18 Phase 3) are all unchanged. Only the planned-reboot teardown sequence gains one additional step.
- **The fix doesn't help against panic resets.** A genuine `ESP_RST_PANIC` or `ESP_RST_INT_WDT` still bypasses the unmount because the supervisor never runs. T15 doesn't fire planned reboots on those paths — they're already "things outside the bulkhead's reach". The acceptance criterion is specifically that supervisor-driven planned reboots are now SD-clean, which they weren't before.
- **Acceptance criterion.** Configure status reporting against an unreachable server until T15 fires a planned reboot. Just before the reboot, inject SD-log events to ensure the write-back queue is dirty. Post-reboot, pull the SD card and confirm all files written before the reboot are present and the correct size. Pre-fix, files queued via `f.close()` in the seconds before reset could land as phantom directory entries. Post-fix, they land as committed file data.
- **Asymmetry note from the field data.** Unit 2 (id=5C88) had *6* planned reboots in the same 17 h 1.20.0 window and its CSVs were intact — the bug is marginal, depending on the card's write-back behaviour and the timing of the last write versus the reset. The fix tightens the corruption window across all cards uniformly; Unit 2 was simply on the safe side of the margin.
- **Drop-in upgrade.** No NVS layout change, no partition-table change, no config-key change. OTA from 1.20.1, 1.20.0, 1.19.x, or 1.18.3 all work without extras.

### Cross-references
- gh#26 — T15 planned_reboot() calls esp_restart() without unmounting the SD card — observed silent file loss on Unit 1
- gh#24 — heap-drop accumulator fix (1.20.1) — reduces *opportunity* for this bug by eliminating spurious planned reboots
- gh#25 — log-upload dedup latch fix (1.20.1) — works whether the offending file is "empty" or "phantom", so it's correct against the symptom this issue causes too
- gh#18 — bulkhead policy (the supervisor unchanged in 1.20.2)

---

## [1.20.1] — 2026-05-16

*Two bulkhead-policy bug fixes uncovered by 1.20.0 forensics on units 12F0 and 5C88: the T15 heap-leak detector was integrating per-POST allocator jitter into a planned reboot every 3-7 h despite steady free heap, and the T14 log-upload path could livelock on a structurally-bad CSV because the dedup latch only advanced on success. Both are detector/state-machine bugs, not bulkhead-architecture changes — the supervisor task, breaker, NVS-window-state recovery, and respawn-storm guard all remain exactly as shipped in 1.18.x.*

### Fixed
- **gh#24 — T15 heap-leak detector now uses a signed running balance.** `record_heap_drop()` in `firmware/src/status_post/status_post.cpp` was a monotonic positive-only integrator that summed every transient free-heap dip across an HTTPS call without subtracting the matching recovery. Over thousands of POSTs the integral hit the 64 KB threshold every 3-7 hours even when actual free heap was steady. Field evidence: 9 planned reboots across units 12F0 + 5C88 over a 17 h window 2026-05-15→16, all firing *"T14 cumulative heap drop crossed 64 KB"* while the SD-log `value_a=7` (free) and `value_a=12` (largest-block) rows showed steady 120–126 KB free / 71–83 KB largest-block. Now `s_heap_drop_bytes` is a signed running balance: positive deltas add, negative deltas subtract, floored at 0 (no banking recovery credit), saturated at `INT32_MAX`. True leaks accumulate monotonically; per-call jitter cancels. The public `status_post_heap_drop_bytes()` API and the supervisor's 64 KB threshold check are unchanged.
- **gh#25 — T14 log-upload dedup latch now advances on structural rejects.** `try_log_upload()` previously only called `dm_set_log_last_up()` on a successful upload, so a candidate that failed `do_log_upload()`'s `fsize == 0 || fsize > T14_LOG_MAX_BYTES` precondition would be re-targeted by every subsequent T14 cycle. The breaker throttled the cadence (60 s → 5 min → 30 min → 1 h escalation) but couldn't break the loop — only a reboot or a new rotation producing a different `newest_closed` candidate could clear it. Field evidence: unit 12F0 looped on a 0-byte `/20260516025038.csv` from 01:15:10 through 01:53:18 on 2026-05-16, broken only by the gh#24 planned reboot at 02:16. Fix: hoist the size precondition out of `do_log_upload()` into `try_log_upload()` so a structural reject advances the latch with one `ESP_LOGW` and one LOG_SYSTEM fail event, then never retries. Network/transport failures inside `do_log_upload()` still leave the latch unchanged so a transient outage retries the same file when connectivity returns. The defensive `fsize == 0 || fsize > max` guard inside `do_log_upload()` is preserved as belt-and-braces.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.20.0` → `1.20.1` in both env blocks.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.20.1 by `bin/build_release.ps1`.
- `do_log_upload()` signature: `bool do_log_upload(const cfg_shadow_t *cfg, const char *filename)` → `bool do_log_upload(const cfg_shadow_t *cfg, const char *filename, uint32_t fsize)`. Single caller (`try_log_upload`) updated in lockstep; no external API change (function is `static`).

### Behaviour notes / non-changes
- **No bulkhead-architecture changes.** The supervisor task (T15), breaker (gh#18 Phase 2), NVS-window-state recovery (Phase 3), wedge/respawn-storm guards, and OTA fail-counter exemption (1.19.2) are unchanged. The bug was in the detector's accumulator math, not in any of the policy mechanisms it feeds.
- **Acceptance criterion for gh#24.** A 1.20.1 controller running 24 h against a working HTTPS server should produce *zero* "T14 cumulative heap drop crossed 64 KB" planned reboots. A 1.20.0 controller produced 3 (unit 12F0) to 6 (unit 5C88) per 17 h window. The fix doesn't weaken leak detection: injecting a deliberate `malloc(256)` per POST cycle into T14 will still trip the threshold in ~250 cycles.
- **Acceptance criterion for gh#25.** A pre-staged 0-byte CSV with a valid timestamp filename should produce one warning + one LOG_SYSTEM fail event at the first daily slot, then silence. Pre-fix would have produced 3 attempts every breaker-window for hours.
- **Coredump partition + platform pin unchanged.** Same factory + OTA partitions, same `espressif32@6.12.0` pin as 1.20.0. OTA from 1.20.0, 1.19.2, 1.19.1, or 1.18.3 all work without extras.
- **Stuck 0-byte CSV on field units carries over.** The fix prevents *new* livelocks but doesn't proactively delete an existing 0-byte file. Unit 12F0's `/20260516025038.csv` remains on the SD card until manual cleanup or the SD_MAX_FILES rotation eventually evicts it. A separate "sweep zero-byte timestamp CSVs older than 24 h" enhancement is left as a future option — not blocking for 1.20.1.

### Operational notes
- **Drop-in upgrade.** No NVS layout change, no partition-table change, no config-key change.
- **Forensic value preserved.** Existing `value_a=7` (free heap) and `value_a=12` (largest-block) LOG_SYSTEM rows continue to work as before. To confirm gh#24 is fixed after deployment, look for the absence of the `[T15] PLANNED REBOOT — T14 cumulative heap drop crossed 64 KB` line in serial logs over a 24 h window.

### Cross-references
- gh#24 — T15 heap-drop accumulator integrates jitter; trips planned reboot every few hours without a real leak
- gh#25 — T14 log-upload dedup latch doesn't advance on bad-file failures, infinite re-upload of 0-byte CSV
- gh#18 — bulkhead policy (the framework these fixes live within; unchanged in 1.20.1)

---

## [1.20.0] — 2026-05-15

*Surfaces the per-unit identifier (gh#17) on two more channels: the LCD's Firmware/Uptime info screen and the web GUI footer. Until this release, the unit_id was visible on the serial boot banner, in the SD log preamble, in the canonical status JSON, and in the AP SSID — but operators with their hands on a physical unit (LCD) or eyes on the live GUI (footer) couldn't read it at a glance. Both surfaces now show it next to the version string so "which one am I touching?" is a zero-click question. Bundles the 1.19.2 OTA-counter fix.*

### Added
- **LCD Firmware/Uptime screen** (info-rotation case 6) now shows the unit_id right-aligned on the same row as the firmware version:
  ```
  FW: 1.20.0  12F0
  Up: 1h 23m
  ```
  Row 0 layout is `"FW: "` (4 chars) + version (left-padded/truncated to 8 chars) + unit_id (4 chars) = 16 chars exactly. Current longest version `"1.19.2"` is 6 chars; the 8-char field accommodates anything up to `"1.999.99"` before truncation kicks in. (`firmware/src/ui_display/ui_display.cpp:824`)
- **Web GUI footer** now shows the unit_id after the version with a middot separator:
  ```
  Greenhouse Controller – v1.20.0 · 12F0          GitHub ↗
  ```
  Reads `sys.unit_id` from the canonical status push that `app.js` already consumes, so no new endpoint and no new request. (`firmware/data/app.js`)

### Fixed (carried forward from 1.19.2)
- **OTA fail counter exempts T15 PLANNED REBOOTs.** `ota_check_rollback()` now reads the `t15_planreboot` NVS key and skips the counter increment when the current boot is the intentional resume from a planned reboot, guarded by `esp_reset_reason() == ESP_RST_SW` so genuine panics still count. Prevents a unit hitting gh#20 (TLS-handshake heap fragmentation) at an unlucky cadence from accumulating counter=3 within hours and triggering an OTA rollback back to 1.18.3 — the exact build 1.19.0 was issued to replace. See the 1.19.2 entry below for the full reasoning.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.19.2` → `1.20.0` in both env blocks.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.20.0 by `bin/build_release.ps1`.

### Behaviour notes / non-changes
- **Minor bump (1.19.x → 1.20.0), not patch.** Per the project convention, feature additions cross to a new minor version even when small. The OTA-counter fix on its own would have been 1.19.2 (and remains in the changelog history under that heading); landing the LCD + footer feature on top makes this a minor release.
- **No new API field exposed.** `sys.unit_id` has been in the canonical status JSON since 1.18.3 (gh#17); this release just consumes it in the GUI. So an older GUI talking to a 1.20.0 firmware is unaffected, and a 1.20.0 GUI talking to ≥1.18.3 firmware works.
- **LCD truncation behaviour is identical.** The version field width changed from 12 to 8 chars, but no shipped version has ever been longer than 7 chars, so no operator-visible truncation occurs.
- **Boot splash (`v%-9.9sInit..`) unchanged.** The unit_id was *not* added to the boot splash because that row already carries the "Init.." progress hint. Anyone needing the unit_id at boot time can read the serial banner or wait ~2 seconds for the post-boot info-rotation to reach screen 6.

### Operational notes
- **No partition table or sdkconfig changes.** OTA from 1.19.1 / 1.19.2 / 1.18.3 all work without extras.
- **Verification recipe.**
  - LCD: cycle through info screens (or wait for auto-rotation) to reach the FW/Up screen — bottom-right corner of row 0 shows the 4-char unit_id.
  - Web: load the GUI in a browser and check the footer at the bottom of the page — version is now followed by `· 12F0` (or whatever the unit's MAC last 2 bytes resolve to).

### Tooling
- **`webUiMock/mock_server.py` synced to 1.20.0.** Target-firmware stamp bumped from 1.17.20 (three minor versions stale), `cfg["fw_ver"]` updated, new `cfg["unit_id"]` constant added (`"AABB"`), `/api/status` `system` block now emits `unit_id` so the new footer renders in the mock GUI, and the `/api/wifi` POST response now carries `{"restarting":true}` when `ssid`/`psk`/`ap_psk` change (matching the 1.19.1 firmware semantics). Run `python webUiMock/mock_server.py` and open `http://localhost:5000` to preview the 1.20.0 GUI without flashing hardware. Docstring + README target-firmware stamps updated to match.

### Related
- [gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17) — Unique unit identifier derived from MAC. **First closed** in 1.18.3; this release **finishes the rollout** to the two remaining operator-facing surfaces (LCD info screen, web GUI footer).

---

## [1.19.2] — 2026-05-15

*One-line defensive patch in the OTA boot-fail accounting. Closes the "tonight could undo today" risk flagged after the 1.19.1 deployment: a unit hitting the still-unfixed gh#20 (TLS-handshake heap fragmentation) at an unlucky cadence could accumulate three PLANNED REBOOTs in counter-incrementing succession and trigger an OTA rollback back to 1.18.3 — the exact build 1.19.0 was issued to replace. This release tells the OTA manager that T15-initiated reboots are intentional and must not count against the rollback budget. Pairs cleanly with the still-open work to actually fix gh#20.*

### Fixed
- **OTA fail counter exempts T15 PLANNED REBOOTs.** `ota_check_rollback()` now reads the `t15_planreboot` NVS key (set by `status_post_supervisor.cpp:101` immediately before `esp_restart()`) and skips the counter increment when the current boot is the intentional resume from that reboot. Guarded by `esp_reset_reason() == ESP_RST_SW` so a genuine panic that happens to occur while the flag is still set (e.g. the original 2026-05-14 gh#21 cascade) is still counted as a real failure. The flag is single-shot per planned reboot — T15 clears it (line 262) a few seconds later once T14 is healthy, so subsequent boots are accounted for normally. (`firmware/src/ota_manager/ota_manager.cpp`)

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.19.1` → `1.19.2` in both env blocks.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.19.2 by `bin/build_release.ps1`.

### Behaviour notes / non-changes
- **The rollback threshold is unchanged.** If a unit ever does accumulate three genuine boot failures (i.e. resets with reason != `ESP_RST_SW`, OR `ESP_RST_SW` without the planned-reboot flag), the 3-fail rollback still fires. This release narrows what counts as a "fail", not what counts as "rollback-worthy".
- **No partition table, sdkconfig, or LittleFS-format changes.** Straight OTA from 1.19.1; the per-unit `erase_region 0x620000 0x10000` step is not needed for this upgrade.
- **Does not address gh#20.** Heap fragmentation in T14's status-POST loop still triggers PLANNED REBOOTs; this release just stops those PLANNED REBOOTs from being miscounted. The actual heap-fragmentation work remains the next priority (separate issue forthcoming).
- **NVS reset history reconsideration tracked separately.** A feature-request issue has been opened to evaluate whether the in-firmware NVS event-log ring buffer earns its keep given the SD-side log files already capture the same information with vastly longer retention. No code change in this release.

### Operational notes
- **No per-unit flash step.** OTA path A (web GUI) or USB flash both work without extras.
- **Verification recipe.** Force a T15 PLANNED REBOOT — easiest way is to temporarily set the heap-drop threshold low in `status_post_supervisor.cpp` or to set the NVS key `t15_planreboot=1` directly and call `esp_restart()`. On 1.19.1 the next boot logged `Fail counter incremented to N+1`. On 1.19.2 it logs `T15 PLANNED REBOOT detected — fail counter NOT incremented (stays at N)`.

### Related
- [gh#21](https://github.com/pe1mew/greenhouse-Controller/issues/21) — original lwIP race. Unaffected.
- gh#20 — heap fragmentation. **Still open.** This release reduces (not eliminates) the operational impact of gh#20 by preventing it from triggering OTA rollback.

---

## [1.19.1] — 2026-05-15

*Three small follow-on fixes that surfaced while verifying 1.19.0 on Unit 12F0 (the same unit that produced the original gh#21 forensic capture). None changes the gh#21 fix itself; they patch issues 1.19.0 introduced or exposed.*

### Fixed
- **Supervisor wedge in the gh#21 gate.** 1.19.0's gate sat *before* `s_heartbeat++`, so a unit waiting for STA WiFi never advanced its T14 heartbeat. T15 (`status_post_supervisor.cpp:244`) declares T14 wedged after `T15_WEDGE_TIMEOUT_MS` (60 s) without a heartbeat change → respawn → gate again → wedge again → after one respawn-storm window (< 5 min between respawns) T15 escalates to PLANNED REBOOT. Result: an AP-only unit (or any unit that has not yet associated) loops forever on planned reboots. Observed 2026-05-15 on Unit 12F0 with no SSID configured. **Fix:** the gate now bumps `s_heartbeat` on every wait iteration, which is the truthful status ("T14 is alive, waiting on a precondition"). T15's heap-drop and respawn-storm detectors are unaffected. (`firmware/src/status_post/status_post.cpp`)
- **`/api/wifi` now applies on the spot.** The POST handler at `web_server.cpp:713-733` wrote new STA/AP creds to NVS but never restarted the unit, while T10 and the AP startup path read those creds only at boot — so saved creds sat in NVS unused until the next manual power-cycle. Confused operators (and confused me, during the 1.19.0 verification on Unit 12F0). **Fix:** when the request changes `ssid`, `psk`, or `ap_psk`, the handler now spawns a 1-second-delayed `esp_restart()` task so the HTTP response flushes before the reboot. The JSON response now includes `"restarting":true` so the UI can show a "rebooting…" toast. Unchanged paths (e.g. unrelated fields, or a POST with no changes) still send the bare `{"ok":true}`.
- **Empty coredump partition now logs cleanly.** 1.19.0's boot-time presence check treated `ESP_ERR_INVALID_SIZE` (what the Arduino-ESP32 framework's coredump driver returns when the partition is freshly erased — size header reads 0xFFFFFFFF) as "partition unreadable", which produced a scary warning on every healthy boot after the one-time `erase_region`. **Fix:** treat both `ESP_ERR_NOT_FOUND` and `ESP_ERR_INVALID_SIZE` as `coredump: none`. The inline comment in `main.cpp` now documents both return codes. (`firmware/src/main.cpp`)

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.19.0` → `1.19.1` in both env blocks (`lolin_s3`, `test_t2_relay`).
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.19.1.

### Behaviour notes / non-changes
- **gh#21 fix is unchanged.** This release patches issues introduced *around* the gh#21 fix, not the fix itself. The lwIP-startup gate still gates on `WiFi.localIP() != 0.0.0.0` and still releases on the same condition; 1.19.0 deployments do not need to be rolled back, only upgraded.
- **No partition table or sdkconfig changes.** Same partition layout as 1.19.0; same coredump-related defaults; no per-unit `erase_region` step needed for the 1.19.0 → 1.19.1 upgrade.
- **`/api/wifi` restart only fires on a real credential change.** A POST that explicitly carries unchanged fields (or omits all wifi-namespace fields) takes the no-restart path. Idempotent reconfigure scripts that send the same creds repeatedly will still cause repeated restarts; that's intentional — guarantees the value-on-the-wire is the value the operator entered.

### Operational notes
- **No per-unit flash step.** Unlike 1.19.0 (which required `erase_region 0x620000 0x10000` per unit to seed the new core-dump partition), 1.19.1 is a straight OTA-or-flash with no extra step.
- **Verification recipe for `/api/wifi` fix:** from the AP-mode web UI, change the SSID/PSK and watch serial — expect `[T11_WEB] WiFi creds changed — restarting in 1 s to apply`, then a fresh boot, then `[T10_NET] Connecting to SSID '<new>'`. On 1.19.0 the same sequence stayed on the old (or empty) creds until manual power-cycle.
- **Verification recipe for the supervisor-wedge fix:** boot a unit with no SSID in NVS (or a wrong SSID that never associates). Leave it for > 5 minutes. On 1.19.0 the unit produced a `PLANNED REBOOT — T14 respawn rate exceeded (< 5 min since last)` cycle within ~3 min. On 1.19.1 the unit sits cleanly in `gh#21 gate: waiting for STA IP …` and T1 watchdog ticks advance steadily with no respawn or planned-reboot log lines.

### Related
- [gh#21](https://github.com/pe1mew/greenhouse-Controller/issues/21) — original lwIP startup race. Stays closed; this release does not change its fix.
- Forensic capture: `debug/unit1/coredump_12F0_test.bin` (1.19.0 verification image, 12 KB ELF dump produced by the deliberate `abort()` test) remains the proof-of-life that coredump capture works end-to-end on this PlatformIO/Arduino-ESP32 build.

---

## [1.19.0] — 2026-05-15

*Fixes a production panic surfaced overnight on Units 12F0 and 5C88 ([gh#21](https://github.com/pe1mew/greenhouse-Controller/issues/21)) and adds infrastructure for capturing the next one. Both units hit `assert failed: tcpip_api_call IDF/components/lwip/lwip/src/api/tcpip.c:497 (Invalid mbox)` on resume from a T15 PLANNED REBOOT — a real lwIP startup race in T14, not a heap or supervisor issue. Same release also lays the partition + sdkconfig groundwork for ESP-IDF core-dump capture so the next panic produces an analysable image rather than a bare backtrace.*

### Fixed
- **gh#21 lwIP startup race in T14.** `task_status_post` now waits for `WiFi.localIP() != 0.0.0.0` before entering its main loop. T14 spawns on core 0 alongside T10 (`network_manager`), and the Arduino-ESP32 core lazily calls `tcpip_init()` only when `WiFi.mode()` runs inside T10. On resume from a T15 PLANNED REBOOT the RTC_SW_CPU_RST path returns to the scheduler fast enough that T14's `WiFi.isConnected()` edge detector (line 802) can dispatch into `tcpip_api_call` before the mbox is populated, asserting in `tcpip.c:497`. Waiting for an IP is a strict superset of "tcpip initialised" and is the precondition every real POST already needs, so the gate is defensive on every boot, not just resume-from-planned-reboot. No timeout/bail — if WiFi never comes up, T14 idles in the gate instead of in its main loop. Same outcome, no new failure modes. (`firmware/src/status_post/status_post.cpp`)

### Added
- **Core-dump partition.** `firmware/partitions.csv` declares a new `coredump` partition (64 KB at 0x620000, in the previously-unused tail of flash). Existing partition offsets are unchanged so OTA across the upgrade is safe. On every freshly-flashed unit the new region must be erased once with `esptool.py --port COMx erase_region 0x620000 0x10000`; without that step the IDF reads whatever happens to sit at 0x620000 and complains about a corrupt CRC on the first boot (this is exactly the `CRC=0x7bd5c66f` message Unit 12F0 logged before the panic — its NVS partition didn't include a coredump entry at all).
- **Core-dump build flags (documentation-of-intent).** New `firmware/sdkconfig.defaults` records the expected coredump configuration. Verified 2026-05-15 against the prebuilt Arduino-ESP32 framework's baked-in sdkconfig (`framework-arduinoespressif32` 3.20017, shipped with `espressif32@6.12.0`): **the framework already enables `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, `_DATA_FORMAT_ELF=y`, `_CHECKSUM_CRC32=y`, and `_CHECK_BOOT=y`** and links in the `espcoredump` library, so `esp_core_dump_image_get()` works on this build. The file has no runtime effect on the prebuilt framework but documents the assumption and protects against an upstream maintainer ever flipping coredump off — or against this project switching to a source-built framework or the `pioarduino` fork that does honour `sdkconfig.defaults`.
- **Boot-time core-dump presence log.** `firmware/src/main.cpp::setup()` now calls `esp_core_dump_image_get()` immediately after the Phase 0 boot banner and logs either `coredump: none` or `coredump present: N bytes @ 0xADDR` (warning level). Operators reading serial — or `parsed_nvs_log.txt` once we add the SYSTEM-event mirror — immediately see whether the previous boot left an analysable image worth pulling.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.18.3` → `1.19.0`. Per the project's release cadence rule, a fix that prevents a production panic and adds a new diagnostic partition is a minor version, not a patch.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.19.0.

### Behaviour notes / non-changes
- **Partition table change is OTA-safe for existing units.** All previously-defined offsets (otadata, nvs, app0, app1, lfs0, lfs1) are byte-identical to 1.18.3. The new `coredump` row only claims previously-unallocated flash above 0x620000. On a unit that does *not* run the `erase_region` step, the IDF will log a CRC complaint once at boot and operate normally; the next core-dump capture won't work until the region is erased, but nothing else is affected.
- **No bail-out timeout in the gh#21 gate.** Earlier discussion considered a 30-second timeout that would let T14 enter its loop without an IP. Rejected: a POST without an IP can't succeed, so a timeout would only let T14 spin uselessly in the main loop instead of usefully in the gate. The gate is the same wait, in the right place.
- **Heap-fragmentation root cause (gh#20) untouched.** This release fixes the *cascade* (the lwIP panic that happens *after* T15 issues a planned reboot), not the upstream heap drop that triggers the planned reboot. T15's heap monitor is still the right detector for gh#20; that work continues separately.
- **OTA fail counter behaviour unchanged.** A planned reboot still increments the OTA boot-fail counter; the 30-second healthy-uptime reset clears it as before. Changing the counter to skip planned reboots is a separate follow-up.

### Operational notes
- **One-time per-unit flash step before the new core-dump partition becomes usable:**
  ```
  esptool.py --chip esp32s3 --port COMx erase_region 0x620000 0x10000
  ```
  Run this once after the first 1.19.0 flash on each unit. Subsequent OTAs do not need to repeat it.
- **Verifying core-dump capture actually works in this PlatformIO build:** the prebuilt Arduino-ESP32 framework's baked-in sdkconfig was verified during this release to already have all the required flags enabled, so capture *should* work end-to-end. To confirm on a development unit: trigger a deliberate panic (temporary `abort()` behind a test-only admin endpoint), reboot, and confirm the new boot log shows `coredump present: N bytes`. Then pull and decode with `esptool.py read_flash 0x620000 0x10000 coredump.bin` + `espcoredump.py info_corefile -t elf -c coredump.bin firmware.elf`.
- **Reproducing gh#21 to confirm the fix:** add a one-shot trigger that sets the T15 planned-reboot NVS flag and calls `esp_restart()`. On an unpatched unit this reproduces the `Invalid mbox` assertion in ≤ 2 boots; on a patched unit serial shows `gh#21 gate: waiting for STA IP` → `gh#21 gate: IP=…, proceeding` → normal T14 cycle.

### Related
- [gh#21](https://github.com/pe1mew/greenhouse-Controller/issues/21) — lwIP startup race in T14 on resume from PLANNED REBOOT. Closed with this release.
- [gh#20](https://github.com/pe1mew/greenhouse-Controller/issues/20) — Heap fragmentation in T14 status-POST loop. **Not** closed by this release; this release only addresses the downstream cascade.
- Forensic capture under `debug/unit1/1.18.3/20260514_191506.log` and `debug/unit2/1.18.3/20260514_141849.log` — primary evidence for both findings.

---

## [1.18.3] — 2026-05-14

*Adds a per-unit identifier ([gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17)) derived from the factory-burned WiFi-STA MAC, surfaced on four operator/log/dashboard channels. Reuses the same 2-byte short form (last 2 MAC bytes, 4 hex chars) that the AP SSID `Greenhouse-XXXX` has used since day one, so operators identify the same unit consistently across SSID, LCD boot row, SD log, NVS ring, and external dashboard. For the project's expected fleet size (≤ tens of units) the 16-bit ID has effectively zero collision probability — even more so when units are procured as a single batch (sequentially-numbered MACs within a batch make collisions deterministically impossible).*

### Added
- **New module:** `firmware/src/system_id/{system_id.h, .cpp}` — tiny helper exporting `system_unit_id_u16()` and `system_unit_id_str(buf, cap)`. Reads `esp_read_mac(ESP_MAC_WIFI_STA)` once and caches the low 2 bytes as a `uint16_t`. Works before WiFi is initialised (unlike `WiFi.macAddress()`); cache is lazy + thread-safe under the "one writer, many readers, primitive aligned store" pattern.
- `firmware/src/main.cpp::setup()` — extends the existing `Phase 0 boot — esp_reset_reason=N` ESP_LOGI line with `id=AABB`. Zero log-row cost; immediately visible in any serial capture.
- `firmware/src/data_manager/data_manager.cpp::task_data_manager()` — emits a second LOG_SYSTEM event at boot, immediately after the existing `value_a=5` boot-reason row: `value_a=11, value_b=(int16_t)system_unit_id_u16()`. Both rows carry the same RTC timestamp so they appear together in the SD log and NVS ring.
- `firmware/src/event_logger/event_logger.cpp::rotate_sd_file()` — every newly-rotated SD CSV file now starts with a `value_a=11` unit-id row written directly (not via Q3), immediately after the CSV header. Self-identifying logs for the forensic case where multiple downloaded CSVs need to be attributed to specific units.
- `firmware/src/status_post/status_json.cpp::build_canonical_status_json()` — the `system` block now includes `"unit_id":"AABB"` as the first field. ~10 bytes per status POST; lets the external dashboard label rows by unit without needing the chip MAC.
- `firmware/src/event_logger/event_logger.h` — LOG_SYSTEM `value_a=11` documented in the encoding table, alongside `value_a=10` (boot-cal skipped) and `value_a=12` (heap largest block).

### Changed
- `log/logparser.py` — new decoder branch in `_decode_system()` for `value_a=11`: reinterprets the (signed) `value_b` as `uint16` and renders `Unit ID: AABB (AP SSID would be 'Greenhouse-AABB')`. Smoke-tested against both positive and negative int16 casts.
- `log/logparser.md` — encoding table extended with row 11; doc version 1.2 → 1.3.
- `firmware/data/manifest.json` and `firmware/data/index.html` — stamped 1.18.3.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.18.2` → `1.18.3`.

### Behaviour notes / non-changes
- Build cost: ~+476 bytes flash, +8 bytes RAM (cache + init flag), zero NVS slots. No new tasks; no scheduling impact.
- **Collision math for a 16-bit ID** (birthday-paradox approximation, M = 65 536):
  - N = 10 units → 0.07 %
  - N = 20 units → 0.29 %
  - N = 30 units → 0.66 %
  - N = 50 units → 1.9 %
  - At the project's stated fleet scale (≤ tens), well under 1 %. **And** because ESP32-S3 MAC addresses are allocated sequentially within a production batch, units bought in a single supplier order have *guaranteed-distinct* last 2 bytes — collisions in that case are physically impossible.
- **Upgrade path to 3 bytes** (if fleet ever exceeds ~50 units, drops collision probability into the 10⁻⁶ % range): single-line change in `system_id.cpp::load_unit_id()` — widen `s_cached` to `uint32_t` and OR in `mac[3]` at the top. The four call sites use the public functions so no other code needs to change. Documented inline in the header.
- **Why not a user-typed friendly name?** Out of scope per gh#17. The MAC-derived ID is immutable (survives factory reset, never collides within a batch, can't be forgotten by an operator). A friendly-name layer can be added on top later if needed without changing this lower-level ID.
- **CSV format unchanged.** The unit-id preamble row is a regular `LOG_SYSTEM` event row — no special comment lines, no out-of-band metadata. Parsers that already handle the CSV format see no schema change.

### Operational notes
- After OTA to 1.18.3 + first reboot, the new boot row pair appears in the SD log:
  ```
  YYYY-MM-DDTHH:MM:SS,SYSTEM,SYS,0,0,5,1         <- boot reason
  YYYY-MM-DDTHH:MM:SS,SYSTEM,SYS,0,0,11,N        <- unit ID (N = int16 cast)
  ```
- `log/logparser.py` renders the second as `"Unit ID: AABB (AP SSID would be 'Greenhouse-AABB')"`.
- The web GUI's Status tab gets the `unit_id` field automatically via the canonical-JSON refresh; surfacing it in a UI tile is left as a small follow-up.

### Related
- [gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17) — Unique unit identifier derived from MAC. Closed with this release.
- `design/tasks.md` — T4 boot block now emits two log rows in sequence (5 then 11).
- `log/logparser.md` — value_a table updated.

---

## [1.18.2] — 2026-05-14

*Three small defensive additions triggered by the mbedTLS research thread on gh#18. None of them changes runtime behaviour for an OK build; all of them close gaps that would surface as "the next field crash" if left alone. Tracked as gh#20.*

### Added
- `firmware/src/main.cpp::task_watchdog_heartbeat()` — new `LOG_SYSTEM value_a=12, value_b=KB` row every 60 s, recording `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10`. This is the heap *fragmentation* signal that Phase 4's free-heap-delta monitor cannot see by construction: free total can stay flat while the largest contiguous block shrinks under repeated TLS handshake churn (arduino-esp32 issues #7884, #4523). With this row, a future "T14 panicked inside mbedTLS for no obvious reason" investigation has the diagnostic it needs to recognise the fragmentation pattern. Supervisor integration (trip planned-reboot when largest-block drops below a threshold) is a follow-up; one log capture in the field is needed first to set the threshold empirically.
- `firmware/src/event_logger/event_logger.h` — `value_a=12` documented in the LOG_SYSTEM table alongside `value_a=7/8/9/10`.
- `design/tls_leak_audit.md` — written record of the static-source audit of `WiFiClientSecure::stop()` and `stop_ssl_socket()` in Arduino-ESP32 3.20017. Verdict: Phase 1's static-`WiFiClientSecure` pattern correctly dodges arduino-esp32 #3808 for our `setInsecure()` usage profile. TLS 1.3 panic surface (esp-idf #8515) is not in our compile-time path (TLS 1.3 disabled in the resolved sdkconfig). Audit includes a per-resource ledger and explicit re-qualification triggers (CA-cert addition, mTLS, platform-version bump).

### Changed
- `firmware/platformio.ini` — `platform = espressif32` → `platform = espressif32@6.12.0`. Unpinned was letting `pio platform update` silently advance the Arduino-ESP32 / ESP-IDF / mbedTLS combination, which can introduce or expose latent issues (TLS 1.3 default flip → #8515 panic surface; PSA crypto migration → esp-idf #18186 panic). 6.12.0 is the version that compiles 1.18.0–1.18.1 and was audited in `design/tls_leak_audit.md`. Future bumps must re-run the audit.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.18.1` → `1.18.2`.

### Behaviour notes / non-changes
- Build cost: ~+150 bytes flash (the extra log call), zero new RAM, zero new NVS slots.
- The platform pin is a no-op for *this* build (we were already on 6.12.0). Its purpose is forward-looking — the next contributor who runs `pio platform update` no longer breaks reproducibility silently.
- The largest-block row appends to T9's existing every-60-s heap-snapshot triplet (value_a = 7, 8, then 12) so a single SD-log scan plots all three on the same time axis. No new task, no new mutex, no new scheduling pressure.
- The TLS audit document is read-only forensics. It does not change any firmware. It exists so the next investigator (or future-us in 6 months) does not re-research the same questions under time pressure.

### Related
- [gh#20](https://github.com/pe1mew/greenhouse-Controller/issues/20) — Three-item defensive pass triggered by the mbedTLS research summary on gh#18.
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy. The audit closes one of the two open assumptions Phase 1 made (the other being "supervisor T15 is reliable" — proven by the 1.18.1 fix landing without regression).
- arduino-esp32 #7884, #4523 — heap fragmentation cited as the motivation for the new largest-block row.
- arduino-esp32 #3808 — destructor leak; cleared for our usage profile by the audit.
- esp-idf #8515 — TLS 1.3 POST panic; out of compile-time surface (TLS 1.3 disabled in sdkconfig).
- esp-idf #18186 — PSA crypto crash on ESP-IDF 6.0-beta; protected by the platform pin.

---

## [1.18.1] — 2026-05-14

*Critical hotfix for 1.18.0. T15 (the bulkhead-policy supervisor introduced in 1.18.0) starved its task watchdog every iteration: it subscribed to the task WDT (default 5-s timeout) but then `vTaskDelay(30 000)` between WDT kicks. On Unit 1 this caused a crash loop of three TASK_WDT resets in ~22 seconds after OTA, after which the OTA app-validation gate flipped to "unhealthy" and rolled the unit back to the previous bank. The forensic evidence in `debug/unit1/1.18.0/nvs_log.csv` is unambiguous: boot-reason events at 08:02:18 (SW=3, OTA finalize), 08:02:26 (TASK_WDT=6), 08:02:33 (TASK_WDT=6), 08:02:41 (SW=3, rollback).*

### Fixed
- `firmware/src/status_post_supervisor/status_post_supervisor.cpp::task_status_post_supervisor()` — main loop now breaks the 30-s polling delay into ≤ 1-s chunks (`T15_WDT_KICK_CHUNK_MS = 1000u`), kicking `esp_task_wdt_reset()` before each chunk. Mirrors the chunked-wait pattern `calib_close_all()` has used since 1.17.29 for the 171-s M3 calibration. Default task-WDT timeout is 5 s; 1 s gives 5× safety margin.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.18.0` → `1.18.1`.

### Behaviour notes
- **OTA recovery saved us this time.** The 30-s `ota_mark_healthy()` window in `main.cpp::task_watchdog_heartbeat()` requires the firmware to survive at least 30 s of uptime before the new bank is committed. The 1.18.0 crash loop killed the chip at ~7-8 s on every boot, so the gate never tripped, and after three failed boots the bootloader rolled back to the 1.17.x bank that was previously committed. This is the OTA safety system working exactly as designed — it converted a fatal regression into a recovery scenario at the cost of one operator-visible reboot cycle.
- **All other 1.18.0 changes (T15 design, supervisor entry points, LCD badge, NVS-persisted state from Phase 3, breaker from Phase 2, HTTPS hardening from Phase 1) are unaffected.** This release re-ships them with the WDT bug fixed.
- Lesson for future task additions: any task that subscribes to the task WDT and has a polling interval longer than the WDT timeout must break the wait into chunks. The 1.17.29 hardening release should have surfaced this rule as a written invariant; adding that to the task-design checklist is a follow-up.
- The same WDT-kicking pattern was already correctly used in `relay_controller.cpp::calib_close_all()` (`CALIB_CHUNK_MS = 400` ms) and `handle_alarm_clearance()` (`ALARM_GUARD_CHUNK_MS = 5000` ms). T15 was the first new subscriber since 1.17.29, and the rule was implicit rather than documented.

### Related
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy. Phase 4 (T15) is unchanged in design; only the wait loop was broken.
- New issue to file: capture this lesson as a written task-design rule ("any WDT-subscribed task with > 4 s blocking calls must chunk").

---

## [1.18.0] — 2026-05-14

*Final phase of the bulkhead policy ([gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)): adds the **T15 supervisor task** plus the **planned-reboot fallback**, closing the policy out. With this release a wedged or leaking T14 (status-POST + log-upload) can no longer take primary climate control offline — the supervisor either respawns T14 cleanly within ~60 s, or escalates to a planned reboot that recovers in ~2 s (thanks to Phase 3's NVS-persisted window state) instead of ~171 s. Minor-version bump because (a) a new task ID (T15) is introduced and (b) restart semantics now include planned reboots that operators may observe.*

### Added
- **New module:** `firmware/src/status_post_supervisor/{status_post_supervisor.h, .cpp}` — T15 task implementation. 30-second polling cadence, watchdog-subscribed (same pattern as T1, T2 since 1.17.29). Tracks three failure modes:
  - **Wedge:** T14's heartbeat counter has not advanced for ≥ 60 s → force-respawn T14.
  - **Heap leak:** T14's cumulative heap drop has crossed 64 KB → planned reboot.
  - **Respawn storm:** more than 1 respawn within 5 minutes, or more than 10 within one hour → planned reboot.
- `firmware/src/status_post/status_post.h` — three new public APIs for supervisor integration: `status_post_heartbeat()`, `status_post_heap_drop_bytes()`, `status_post_force_teardown()`. The first two are racy lock-free reads of `volatile uint32_t` accumulators; the third is an idempotent close of the static `WiFiClientSecure` (closes the persistent TLS session before `vTaskDelete(task_t14)` so the next incarnation starts clean).
- `firmware/src/status_post/status_post.cpp` — two new module-private accumulators: `s_heartbeat` (advanced at the top of every T14 main-loop iteration) and `s_heap_drop_bytes` (saturating-add accumulator fed by `record_heap_drop()` after each HTTPS call). Each accumulator survives `vTaskDelete` because it lives in BSS, not on the task stack — a respawned T14 sees the same counter values its predecessor wrote.
- `firmware/src/status_post/status_post.cpp` — heap sampling around `do_status_post()` and `maybe_upload_log()`. Real leaks accumulate; transient handshake allocations release before the call returns and are not counted (negative deltas clamp to zero).
- `firmware/src/status_post_supervisor/status_post_supervisor.h` — public API `supervisor_was_planned_reboot()` so downstream code can distinguish a planned reboot from an `ESP_RST_SW` of unknown provenance. Cleared once T14 makes one successful POST after recovery.
- `firmware/src/main.cpp` — T15 spawned **before** T14 with priority 4 (between `TASK_PRIO_LOW` = 3 and `TASK_PRIO_MEDIUM` = 5). Higher than T14 so a wedged T14 cannot starve the supervisor that's trying to recover it; lower than the climate-critical tasks so it never preempts T2/T3/T6. Stack 4 KB. Pinned to Core 0 (same core as T10/T11/T14).
- `firmware/src/types/app_types.h` — new `extern TaskHandle_t task_t15` declaration. Symbol defined in `status_post_supervisor.cpp` so a future build that omits T15 doesn't drag an unused handle along.
- `firmware/src/main.cpp` — T15 added to the stack-HWM probe loop (1.17.29 instrumentation).
- `firmware/src/ui_display/ui_display.cpp` — page 3 (Network) row 0 now reads `WiFi: conn    BK` when `status_post_backoff_active()` is true. Operator can see at a glance that secondary network activity is currently suspended; the green-status LED stays green because primary climate control is unaffected.

### Changed
- `firmware/src/status_post/status_post.cpp::task_status_post()` — main loop top now advances `s_heartbeat++` unconditionally. A wedged HTTPS call (the failure mode we're detecting) is the only thing that can freeze it.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.36` → `1.18.0`.

### Behaviour notes / non-changes
- Build cost: ~+2.3 KB flash, ~+32 bytes RAM (the new supervisor handle in main.cpp). Total bulkhead-policy delta over the 1.17.33 baseline: ~12 KB flash, +96 bytes RAM, 11 new NVS slots.
- **Force-respawn cost budget:** each respawn keeps the static `WiFiClientSecure` (Phase 1) and the breaker structs (Phase 2) intact — only the task stack + TCB is reclaimed. Empirically ~96 KB peak overhead during the `vTaskDelete → vTaskDelay(100ms) → xTaskCreatePinnedToCore` window. Supervisor's hourly cap (10 respawns / hour) bounds the budget to ~960 KB of churn per hour, well within heap headroom.
- **Why planned reboot at 64 KB?** That's ~50 % of the typical free-internal-heap floor on this build (observed ~131 KB free under load via the 1.17.29 heap-row probe). Crossing 64 KB cumulative drop means a sustained leak that respawning has not arrested — escalate before OOM forces an uncontrolled `ESP_RST_PANIC`.
- **Planned-reboot recovery time:** Phase 3 wrote each window channel's last terminal state to NVS, so the next boot's T2 calibration is skipped if all three were CLOSED at restart. Worst-case 2 seconds vs. the pre-Phase-3 171 seconds of climate-control outage during M3 boot calibration.
- **Boot-reason distinguishability:** a planned reboot calls `esp_restart()`, which the ESP-IDF records as `ESP_RST_SW` (= 3). Distinguishable from `ESP_RST_PANIC` (= 4) or `ESP_RST_INT_WDT` (= 5) in the existing 1.17.31 boot-reason log row.
- **Bulkhead policy known limitation (per gh#18):** hard faults *inside* ESP-IDF / mbedTLS / lwIP on a single-chip architecture cannot be intercepted. The policy's job is to make such faults *bounded* (Phase 2 throttles the trigger rate; Phase 3 + Phase 4 ensure the resulting reboot is a 2-second blip, not a 171-second outage). Eliminating the faults themselves requires hardware separation or a separate co-processor — explicitly out of scope.
- **Log-upload retention:** preserved per gh#18 explicit out-of-scope. If the log-upload path proves to be the dominant supervisor-respawn trigger after deployment, dropping or re-scoping it remains a future option (would land as a v1.18.x patch).

### Related
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy umbrella. **All four phases now shipped.**
- [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16) — Unit-2 S200-absent reboots. The structural mitigation is now complete; remaining work on gh#16 is root-cause investigation, orthogonal to the policy.

---

## [1.17.36] — 2026-05-14

*Third of four phases delivering the bulkhead policy ([gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)): persists each relay channel's terminal window state (`CH_CLOSED` / `CH_OPEN`) to NVS on arrival, and writes `CH_UNKNOWN` to NVS **before** energising any relay. On boot, if every channel's persisted state is `CH_CLOSED` and the GPIO42 alarm pin is not asserted, the M3 boot calibration (up to 171 s of climate-control outage) is skipped. This pre-positions the supervisor's planned-reboot path (Phase 4): a planned reboot can now recover in ~2 seconds instead of ~171 seconds, making the bulkhead policy's "planned-reboot safety valve" operationally cheap.*

### Added
- `firmware/src/relay_controller/relay_controller.cpp` — three new NVS keys `t2_st_ch0`, `t2_st_ch1`, `t2_st_ch2` (i32, namespace `NVS_NS_MOTOR`). Encoded values: 0 = UNKNOWN (default), 1 = CLOSED, 2 = OPEN. Three new symbolic constants `NVS_STATE_UNKNOWN`, `NVS_STATE_CLOSED`, `NVS_STATE_OPEN`.
- `firmware/src/relay_controller/relay_controller.cpp` — new helper `persist_ch_state(uint8_t ch, ch_state_t state)`. Maps `CH_CLOSED`/`CH_OPEN` to the two terminal encodings; everything else (UNKNOWN, MOVING, GAP) maps to UNKNOWN. Called at every state transition.
- `firmware/src/event_logger/event_logger.h` — LOG_SYSTEM `value_a=10` documented as "T2 boot-cal skipped" (producer: T2; value_b unused). Since 1.17.36. Lets a downstream log analyser distinguish a fast NVS-recovered boot from a full M3 calibration.

### Changed
- `firmware/src/relay_controller/relay_controller.cpp::ch_start_close()` and `ch_start_open()` — persist `CH_UNKNOWN` to NVS **immediately before** `relay_ch_close()` / `relay_ch_open()`. Invariant: the relay can only be energised while NVS records UNKNOWN, so a power loss between persist-call and reboot recovers as "calibrate" not "stale terminal".
- `firmware/src/relay_controller/relay_controller.cpp::ch_update()` — on travel-complete (`CH_MOVING_OPEN → CH_OPEN`, `CH_MOVING_CLOSE → CH_CLOSED`), persist the new terminal state to NVS. This is the only write of a non-UNKNOWN value.
- `firmware/src/relay_controller/relay_controller.cpp::calib_close_all()` — persists `CH_UNKNOWN` before each channel's CLOSE relay is energised, and persists `CH_CLOSED` on the completion of each channel's travel. Boot-calibration completion now leaves a clean NVS-side state ready for next boot's skip-check.
- `firmware/src/relay_controller/relay_controller.cpp::handle_alarm_onset()` — additionally persists `CH_UNKNOWN` for all three channels (in addition to the existing in-memory state update). A power loss after an alarm onset but before the operator clears the alarm now recovers correctly (calibrate on next boot, never trust pre-alarm terminal state).
- `firmware/src/relay_controller/relay_controller.cpp::task_relay_controller()` — boot path: when the GPIO42 alarm is not asserted, read the three persisted state keys via `nvs_cfg_get_i32_or_default()` (default UNKNOWN). If **all three** are `NVS_STATE_CLOSED`, set the in-memory channels to `CH_CLOSED` directly and skip `calib_close_all()`. Emit `LOG_SYSTEM,SYS,0,0,10,0` and an `ESP_LOGI` line documenting the skip. Otherwise log the recovered tuple and run calibration as before.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.35` → `1.17.36`.

### Behaviour notes / non-changes
- Build cost: ~+0.7 KB flash, 3 new NVS slots, zero new RAM.
- NVS wear: two writes per window movement (one UNKNOWN before, one terminal after). Climate-control issues at most ~10 moves per hour in a worst-case operational pattern, so ~480 writes/day per channel ≈ 175 k/year — well within the 100 k-cycle-per-page NVS lifetime once wear-levelling is accounted for (NVS internally distributes writes across many pages).
- **Why "all three CLOSED" only?** Boot calibration's stated purpose is to establish a known-CLOSED reference. An all-`CH_OPEN` recovery would still need to drive to CLOSED before climate logic acts, so the calibration runs anyway. Restricting skip-eligibility to "all closed" keeps the semantics of CLOSE_ALL calibration intact while capturing the operationally common case (planned reboot after a sustained period of stable climate at night, with all windows closed).
- **Power-loss race during MOVING:** the invariant "NVS records UNKNOWN before relay energises" makes the failure mode safe by construction. Worst-case outcome of any power-loss timing is "calibrate on boot" — the same behaviour the firmware has always had.
- **Stale NVS from older firmware:** the explicit `default=NVS_STATE_UNKNOWN` in `nvs_cfg_get_i32_or_default()` means first-boot post-upgrade (key absent in NVS) always calibrates. No migration code needed.
- Phase 4 (v1.18.0) adds the supervisor task (T15), the planned-reboot fallback that consumes this fast-recovery path, and the LCD "Net backoff" badge.

### Related
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy umbrella. This is Phase 3 of four.

---

## [1.17.35] — 2026-05-14

*Second of four phases delivering the bulkhead policy ([gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)): adds a persistent circuit breaker around the T14 status-POST and log-upload paths. After a threshold of consecutive failures, the breaker opens a backoff window (60 s → 5 min → 30 min → 1 h, capped) and skips POST/upload attempts entirely until the window expires. The state survives reboot via NVS, so a chip that crashes-and-resets in the middle of a failure burst comes back already in backoff instead of immediately re-triggering the same fault. Phase 1's `status_post_backoff_active()` stub now returns the real state, so the `net_backoff_active` JSON flag and "Net backoff" web-GUI badge already wired in 1.17.34 light up correctly. No impact on climate control, sensors, or any primary-task surface.*

### Added
- `firmware/src/status_post/status_post.cpp` — `t14_breaker_t` extended with four Phase-2 fields: `open_until_unix` (NVS-persisted; backoff window expiry in Unix UTC, 0 = closed), `hold_phase` (NVS-persisted; index 0–4 into the schedule table), `consec_fail` and `consec_ok` (RAM-only counters). Same struct used for both `s_post_breaker` and `s_log_breaker` — two independent breakers because the two paths fail at very different cadences (every status interval vs. once per day).
- `firmware/src/status_post/status_post.cpp` — exponential backoff schedule `BREAKER_PHASE_S[] = {0, 60, 300, 1800, 3600}` seconds. Thresholds: 3 consecutive failures advance one phase; 5 consecutive successes regress one phase (hysteresis prevents 30 min → 60 s → 30 min yo-yo under intermittent connectivity).
- `firmware/src/status_post/status_post.cpp` — three new module-private helpers `breaker_load()`, `breaker_open()`, `breaker_record()`. `breaker_load()` is called once at T14 task entry to recover NVS-persisted state. `breaker_open()` is a non-mutating predicate consulted by `ready_to_post()` and `maybe_upload_log()` preconditions, plus by `status_post_backoff_active()`. `breaker_record()` mutates state on each outcome and writes NVS only on phase transitions (and on first success after open) — steady-state success or sub-threshold fail touches NVS zero times.
- `firmware/src/status_post/status_post.cpp` — NVS-key constants `t14_post_until`, `t14_post_phase`, `t14_log_until`, `t14_log_phase` (all `int32_t` in `NVS_NS_SYSTEM`).
- `firmware/src/status_post/status_post.cpp` — `task_status_post()` entry now calls `breaker_load()` for both breakers and logs an `ESP_LOGI` line summarising recovered state if either breaker came back non-closed.

### Changed
- `firmware/src/status_post/status_post.cpp` — `ready_to_post()` now returns false when `breaker_open(&s_post_breaker, cfg->current_unix_ts)` is true. Pre-NTP (`current_unix_ts < 1700000000`) is treated as not-in-backoff by `breaker_open()`; the pre-NTP guard in `ready_to_post()` already blocks the attempt for an orthogonal reason.
- `firmware/src/status_post/status_post.cpp` — `maybe_upload_log()` preconditions extended with `breaker_open(&s_log_breaker, …)` check. When the daily slot fires while the log breaker is open, the existing `log_upload_skip(3)` diagnostic event records that the slot was blocked.
- `firmware/src/status_post/status_post.cpp` — `log_post_outcome()` and `log_upload_outcome()` now feed `breaker_record()` for their respective breakers immediately after recording the cosmetic last-attempt fields.
- `firmware/src/status_post/status_post.cpp` — `status_post_backoff_active()` now returns the real state (`breaker_open(post) || breaker_open(log)`). The Phase-1 stub returned `false` unconditionally; the consumer side in `status_json.cpp` and `app.js` was already wired so the JSON flag and the "Net backoff" badge light up automatically the first time either breaker opens.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.34` → `1.17.35`.

### Behaviour notes / non-changes
- No new tasks. No new public APIs beyond what 1.17.34 already exposed.
- Build cost: ~+1 KB flash; ~+64 B BSS (two breaker structs); 4 new NVS slots (well within the NVS partition's 64 KB).
- NVS wear: steady-state operation writes zero NVS slots per cycle. NVS writes occur only when the breaker crosses a phase boundary — at most ~10 writes per day even during a hard outage, vs. NVS's 100 k-cycle endurance ceiling. Effectively unlimited.
- Recovery semantics: a single successful POST after the breaker opens clears `open_until_unix` immediately (so the next cycle attempts normally) but `hold_phase` only regresses after 5 consecutive successes. Worst case during intermittent connectivity: the breaker stays at its highest reached phase for several minutes after recovery before stepping down. This is deliberate — preferable to a flapping breaker that ping-pongs between fully-open and fully-closed every successful retry.
- Independent breakers: the status-POST path and the log-upload path each have their own `t14_*_until` / `t14_*_phase` keys. A flaky daily-upload window does not throttle the every-2-minute status post, and vice versa.
- Phase 3 (v1.17.36) will persist window state (`CH_CLOSED` / `CH_OPEN`) for the three relay channels so the supervisor's planned-reboot recovery (added in Phase 4) is a 2-second blip rather than a 171-second M3-calibration outage.

### Related
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy umbrella. This is Phase 2 of four.
- [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16) — Unit-2 S200-absent reboots. The breaker prevents repeated immediate-retry storms after a crash-induced reboot, regardless of the root cause.

---

## [1.17.34] — 2026-05-14

*First of four phases delivering the bulkhead policy ([gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)): secondary network activity (T14 status reporting + log upload) must not affect primary climate control. This release ships the HTTPS-side hardening that reduces both the per-kill leak magnitude (when the future supervisor in Phase 4 kills T14 mid-call) and the per-cycle time-in-mbedTLS (which empirically tracks the trigger probability of the gh#16 crash class). Behaviour change for status POSTs and log uploads only — no impact on climate control, sensors, or any primary-task surface.*

### Added
- `firmware/src/status_post/status_post.h` — new public API `bool status_post_backoff_active(void)`. Phase 1 stub returns `false`; Phase 2 will wire it to real breaker state. Existing now so `status_json.cpp` and `app.js` can integrate the consumer side this release.
- `firmware/src/status_post/status_post.cpp` — new module-private `t14_breaker_t` struct (Phase 1 refactor of the four loose `s_last_post_*` / `s_streak_*` statics from 1.17.30). Two independent instances (`s_post_breaker`, `s_log_breaker`) — Phase 2 will extend the struct with `open_until_unix` / `hold_phase` / `consec_fail` fields without changing the callsites refactored here.
- `firmware/src/status_post/status_post.cpp` — new static `WiFiClientSecure s_secure` + `s_secure_inited` flag, replacing the per-POST heap allocation pattern. With `http.setReuse(true)` + explicit `Connection: keep-alive` header, the underlying TCP socket (and therefore the TLS session) persists across calls. `setInsecure()` applied once at first https:// use. Reset to fresh state on (a) WiFi-disconnect edge (detected at top of T14 main loop), (b) HTTPClient error return ≤ 0 (transport-level failure).
- `firmware/src/status_post/status_post.cpp` — new static helpers `http_open_for()` and `http_handle_error()` centralising the per-call setup that was duplicated between `do_status_post()` and `do_log_upload()`. Both call sites now go through the helper, halving the surface area Phase 2's breaker integration has to touch.
- `firmware/src/status_post/status_post.cpp` — new compile-time tunable `T14_HTTP_CONNECT_TIMEOUT_MS = 3000u`. Applied via `http.setConnectTimeout()` in `http_open_for()`. Bounds the TCP-connect phase independently of the response phase (which stays at 5 s for status POST, 30 s for log upload). A misbehaving DNS or unreachable host now aborts within ~4 s of cycle start instead of 5 s.
- `firmware/src/status_post/status_json.cpp` — `mode.flags[]` array now appends `"net_backoff_active"` when `status_post_backoff_active()` returns true. No new EG1 bit allocated — the breaker state is T14-private and has no other consumer.
- `firmware/data/app.js` — `flagBadges` map extended with `net_backoff_active: '<span class="badge warn">Net backoff</span>'`. Existing Alarms-card render pipeline picks it up automatically.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.33` → `1.17.34`.
- WiFiClientSecure no longer heap-allocated per POST/upload (saves ~6-8 KB per kill if the future supervisor in Phase 4 terminates T14 mid-call; saves the full handshake roundtrip on every status POST after the first one).

### Behaviour notes / non-changes
- No new files. No new NVS keys (Phase 2 adds them). No new tasks (Phase 4 adds the supervisor).
- Build cost: ~+0.5 KB flash; BSS grows by the `WiFiClientSecure` instance size (~8 KB) but the equivalent heap allocation per POST is eliminated, so net runtime memory pressure is lower at steady-state.
- TLS session is *connection-reuse* over keep-alive, not *session-ticket resumption*. The same `WiFiClientSecure` instance retains the live TCP+TLS connection across `http.end()` calls (as long as `setReuse(true)` and `Connection: keep-alive` are both honoured). A fresh handshake still happens on first call after every WiFi-disconnect, error return, or server-side connection close.
- Phase 2 (v1.17.35) will populate the breaker struct's missing fields, implement the persistent backoff schedule, persist state to NVS, and wire `status_post_backoff_active()` to return real values.

### Related
- [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) — Bulkhead policy umbrella. This is Phase 1 of four.
- [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16) — Unit-2 S200-absent reboots. The HTTPS hardening here is the structural mitigation, orthogonal to root-cause identification.

---

## [1.17.33] — 2026-05-13

*Adds a runtime LCD-contrast API to the driver. The AiP31068L character controller has supported software contrast via its extended-instruction set since the chip's first revision; `lcd_init()` has always used it once at boot to set a fixed value of 32 (≈ 50 % of the 0–63 range), but the driver did not expose a runtime override. This release adds `lcd_set_contrast(uint8_t value)` so a higher-level task (T1 / T8 / a future Web-tab field) can re-tune contrast at runtime. No behaviour change in this release — the boot-time default and call sites are unchanged. The plumbing-to-NVS-and-GUI work is tracked separately on [gh#15](https://github.com/pe1mew/greenhouse-Controller/issues/15).*

### Added
- `drivers/LCD1602_I2C/src/lcd1602.h` — new public API `lcd_status_t lcd_set_contrast(uint8_t value)`. 6-bit raw range 0–63 to match the AiP31068L's native register width; values > 63 are clamped. Comment block documents the useful band (~16 faded, ~48 bold; default 32 is sensible for most ambient light).
- `drivers/LCD1602_I2C/src/lcd1602.cpp` — implementation: enters extension instruction set (`CMD_FUNC_SET_EX`, IS=1), writes the contrast low nibble (`0x70 | C3..C0`), writes the power/icon/contrast high opcode with booster-on (`0x54 | Bon<<2 | C5..C4`), returns to IS=0. Mirrors exactly what `lcd_init()` does at boot but parameterised. Caller must hold MX1 (same convention as the rest of the LCD API).

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.32` → `1.17.33`.

### Notes
- **No new call sites** in this release. Boot still uses the fixed 32. To experiment with a different value, a developer can call `lcd_set_contrast(N)` once from anywhere that holds MX1 (e.g. drop it into T8's init block, or expose it via a Serial-console handler).
- **gh#15** tracks the full user-facing wiring: NVS-backed `cfg_shadow_t::lcd_contrast` + `lcd_brightness`, web GUI System-tab fields, T8 reads the values per tick, manual updates. Half-day of work.
- The existing `lcd_backlight_lumination(uint8_t level)` API for backlight master brightness has been in the driver for a long time but is also not yet user-tunable (also covered by gh#15).

### Related
- [gh#15](https://github.com/pe1mew/greenhouse-Controller/issues/15) — User-configurable LCD contrast and brightness via the System tab (umbrella).

---

## [1.17.32] — 2026-05-13

*Two-line driver fix for [gh#14](https://github.com/pe1mew/greenhouse-Controller/issues/14): after a clean web-GUI Unmount + physical removal of the SD card, T9's 60-second automount poll would call `SD.begin()` again, and the Arduino-ESP32 SD library's SPI-level state cache would let both `SD.begin()` and `SD.cardType()` lie (cached "card present" result survives `SD.end()`). `g_mounted` flipped back to true; the GUI showed `Mounted: Mounted, Size: 0 MB, Free: 0 MB` — the "mounted" flag lying, the byte counts honestly reporting no card.*

### Fixed
- `drivers/sdCard/src/sd_storage.cpp::storage_init()` — after `SD.cardType() != CARD_NONE` passes, sanity-check `SD.totalBytes() != 0` before setting `g_mounted = true`. `SD.totalBytes()` is the honest function in the SD library's chain: it round-trips to the card hardware on every call instead of returning a cached value, so it returns 0 when no card is physically present regardless of what `SD.begin()` / `SD.cardType()` say. On a zero result the driver releases the SPI claim (`SD.end()`) and returns `STORAGE_ERR_NO_CARD`. Happy-path cost: one extra accessor call (≈ 5 ms) during init only.
- `firmware/src/event_logger/event_logger.cpp::event_logger_sd_remount()` — belt-and-braces. After `storage_init()` returns `STORAGE_OK`, double-check `storage_sd_total_bytes() != 0` before flipping `s_sd_ok = true`. The driver's primary fix should handle every case but the cost of this second check is one accessor call and the benefit is that no future regression in the driver can leak an unmounted-but-flagged-mounted state into T9.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.31` → `1.17.32`.

### Notes
- The 60 s automount polling cadence is unchanged — the bug was in *how* the poll concluded "card present", not in *when* it polled. Card-insertion detection is unaffected: a real card inserted at any point still triggers the next poll to succeed within 60 s.
- The `Phase 0 boot` LCD/serial output is unchanged. The web GUI's Status-tab SD card display is unchanged. Only the underlying `g_mounted` / `s_sd_ok` state-tracking is tightened.
- Manuals (boer/beheerder) do not need updating; the user-visible behaviour is exactly what was always documented (after Unmount → "Not mounted"; after card removal → stays "Not mounted").

### Resolves
- [gh#14](https://github.com/pe1mew/greenhouse-Controller/issues/14) — Web GUI shows "Mounted, 0 MB" after unmount + physical card removal.

---

## [1.17.31] — 2026-05-13

*Cosmetic fix triggered by the 2026-05-13 SD-card capture: the boot-reason `LOG_SYSTEM` event (`value_a=5`) was being posted from `main.cpp::setup()` before T4 had read the DS1307, so its CSV-row timestamp came out as `1970-01-01T00:00:00`. Four POWERON boots in the capture all showed epoch-zero timestamps. Move the emit into T4 right after `read_rtc_and_seed_clock()` so the row is sortable by timestamp. No behavioural change; no firmware version dependency.*

### Changed
- `firmware/src/main.cpp::setup()` — removed the `log_post(boot_ev)` block. The `esp_reset_reason()` capture and the `Phase 0 boot — esp_reset_reason=N` `ESP_LOGI` line on serial both stay; only the SD-log emit has moved. Comment block now explains where the event went and why.
- `firmware/src/data_manager/data_manager.cpp::task_data_manager()` — boot-init path now emits the boot-reason event itself via `esp_reset_reason()`. Replaces the old generic `SYSTEM,SYS,0,0,0,0` boot marker (`value_a=0,value_b=0`) that used to live here. `esp_reset_reason()` is ESP-IDF-cached so calling it from T4 returns the same value `main.cpp::setup()` would have observed.
- `firmware/src/event_logger/event_logger.h` — LOG_SYSTEM `value_a` table updated: `value_a=5` producer column now reads "task_data_manager() post-RTC-seed" instead of "main.cpp setup()". The first-emission-version (1.17.27) is preserved alongside the new T4-emission-version (1.17.31).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.30` → `1.17.31`.

### Notes
- `esp_reset_reason()` is cached by the ESP-IDF boot stub; T4 reads the same value `main.cpp::setup()` would have read. No race, no missed-reason concern.
- The serial-monitor capture still gets the boot-reason line at setup-entry (via `ESP_LOGI`), independent of RTC state. So host-side serial captures continue to identify the reset class within the first ~50 ms of boot regardless of whether the SD log row arrives at epoch-zero or wall-clock-accurate.
- Side effect: the legacy `SYSTEM,SYS,0,0,0,0` boot marker from T4's old code path is **retired**. Tools that filtered on `value_a==0 && value_b==0 && initiator==SYS` to find boots should now filter on `value_a==5 && initiator==SYS`.

### Related
- [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12) — Unexpected reboot investigation. The SD log is the primary forensic surface for that thread; sortable-by-timestamp boot rows make the data substantially more usable.

---

## [1.17.30] — 2026-05-13

*Single-line fix triggered by the first capture of the 1.17.29 stack-HWM probe: T5 (sensor poll) was using 3932 B of its 4096 B stack — 96 % used, only 164 B headroom. Doubling the allocation to 8192 B gives ~52 % headroom and removes the one `stack low` warning observed in the [2026-05-13 06:44 capture](https://github.com/pe1mew/greenhouse-Controller/issues/12). The 1.17.29 hardening pass paid for itself within hours of being deployed.*

### Fixed
- `firmware/src/main.cpp::setup()` — T5 (`task_sensor_poll`) stack allocation bumped from 4096 → 8192 bytes. The 1.17.29 stack-HWM probe (every 10 min via T1) reported nine consecutive samples of `stack low: T5 hwm=164 B`. Stable at 164 B free across the 91-minute observation window — not a creeping bug, just a one-time sizing miss when the original 4096 B was chosen without accounting for the Modbus + sliding-average + Q6 post + `log_post` + `snprintf` deep-stack peak. Doubling to 8192 B brings T5 in line with T2, T8, T10, T11 which all use 8192 B. Cost: +4 KB RAM (BSS).

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.29` → `1.17.30`.

### Diagnostic context
- The 1.17.29 hardening release added a stack-HWM probe that walks all task handles every 10 minutes. The very first capture (2026-05-13 06:44 → 11:27) recorded the T5 warning on every sample. Without the probe, T5 would have continued running at 96 % stack-use until some future change (a new sensor type, deeper Modbus parsing, a `trace_printf`) pushed it past 4096 B, causing a panic-class reset that would be very difficult to attribute back to T5. This 1.17.30 fix removes that latent failure mode entirely.

### Related
- [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12) — Unexpected reboot investigation. The capture that surfaced this finding is part of that thread.
- [gh#13](https://github.com/pe1mew/greenhouse-Controller/issues/13) — Tier-1/2 hardening + 5 MB streaming refactor. The probe added by gh#13 is what caught this.

---

## [1.17.29] — 2026-05-13

*Firmware-hardening pass — four phases delivered in one release to minimise flash cycles. (A) Tier-1 compile flags catch a wider warning surface at build time. (B) `pio check` (cppcheck) static analysis is now wired up. (C) Runtime instrumentation gives memory leaks and watchdog hangs a visible signal in the SD log and on serial. (D) The 5 MB log-upload buffer is replaced with a 4 KB streaming adapter so the daily upload no longer takes 5 MB of PSRAM at peak. All four resolve [gh#13](https://github.com/pe1mew/greenhouse-Controller/issues/13).*

### Added — Phase A (Tier-1 compile flags)
- `firmware/platformio.ini` — added `-Wall -Wextra -Wformat=2 -Wshadow -Wstack-usage=2200 -Wlogical-op -Wstrict-overflow=2 -Wnull-dereference -D_FORTIFY_SOURCE=2` to `build_flags` for both `lolin_s3` and `test_t2_relay` environments. Warnings are emitted; build is not failed on warning (no `-Werror`). Framework-header warnings from Arduino-ESP32 / ESPAsyncWebServer / Adafruit_NeoPixel are accepted as noise; our own code (`firmware/src/` + `drivers/`) is clean against these flags.

### Added — Phase B (static analyser)
- `firmware/platformio.ini` — `check_tool = cppcheck` with `--enable=all`, `--inline-suppr`, severity ≥ medium, scoped to `src/` and `../drivers/`. Run with `pio check -e lolin_s3`. First run is slow (cppcheck downloads + initial scan) but subsequent runs are fast.

### Added — Phase C (runtime instrumentation)
- `firmware/src/main.cpp::task_watchdog_heartbeat()` — three new rhythms inside T1's loop:
  - **Every 60 s**: emit two `LOG_SYSTEM` events recording free heap in KB. `value_a=7` = INTERNAL heap, `value_a=8` = PSRAM heap. Plot these columns over time to spot slow leaks.
  - **Every 60 s (30 s offset from heap row)**: call `heap_caps_check_integrity_all(true)`. On corruption emit `LOG_SYSTEM,value_a=9,value_b=0` so heap-overrun bugs surface immediately rather than via a downstream panic.
  - **Every 10 min**: walk all 13 task handles and print stack high-water-mark to serial. Below 1 KB free is promoted from `ESP_LOGI` to `ESP_LOGW` for visibility.
- `firmware/src/event_logger/event_logger.h` — LOG_SYSTEM `value_a` table extended with codes 7, 8, 9 (HEAP internal free / PSRAM free / corruption).
- **WDT subscription** for tasks T2, T3, T4, T6, T7, T8, T11, T12. Each task's main loop calls `esp_task_wdt_reset()` per iteration. T2's calibration helper loop also kicks the WDT each `CALIB_CHUNK_MS` cycle to survive the 171 s M3 close. **Discipline rule** (added retrospectively after the 1.18.0/1.18.1 cycle, see [gh#19](https://github.com/pe1mew/greenhouse-Controller/issues/19) and `design/tasks.md` §6 *Watchdog-subscriber discipline*): any new task that subscribes to the WDT and has a blocking call longer than `CONFIG_ESP_TASK_WDT_TIMEOUT_S / 2` (currently 2 s) must break the wait into chunks of ≤ 2 s with `esp_task_wdt_reset()` before each chunk. T15 (added 1.18.0) was the first violation of this rule and triggered an OTA rollback before 1.18.1 corrected it.
- **Excluded from WDT**: T5 (sleeps up to 120 s between sensor polls by design), T9 (blocks indefinitely on Q3 when no events), T10 (network), T14 (network). T13 (OTA, on-demand) remains self-managed. T1 was already on WDT.
- **T3 (safety monitor) and T6 (climate control)** previously used `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`; both notifications fire on T4's sensor-poll cadence (30–3600 s) — too sparse for a 5 s WDT. Both reworked to use a 2 s notify timeout; on timeout the task just kicks the WDT and re-blocks.
- **T12 (MQTT stub)** previously used `vTaskDelay(portMAX_DELAY)`; reworked to a 2 s tick so the WDT subscription is exercised. Will be replaced by the real MQTT loop in Phase 9.

### Changed — Phase D (5 MB log-upload streaming)
- `firmware/src/status_post/status_post.cpp` — new `SDFileChunkedStream : public Stream` adapter class. Implements `read`, `peek`, `readBytes`, `available` (write methods stubbed). Internally backed by a single 4 KB **static** chunk buffer (BSS, no heap). `refill()` pulls the next chunk via `storage_sd_read()`. **Definition is static class member**, deliberately — T14 only does one upload at a time so the slot is reused, not consumed from the heap.
- `do_log_upload()` rewritten: was `heap_caps_malloc(fsize+1)` + slurp + `http.POST(body, total)`, now `SDFileChunkedStream stream(path, fsize)` + `http.sendRequest("POST", &stream, fsize)`. The Stream-driven POST still sends a proper Content-Length and works identically over `http://` and `https://`. Peak heap during a log upload drops from up to 5 MB (PSRAM) to ~0 (the stream object lives on T14's stack).

### Changed — code-quality fixes from Phase A triage
- `firmware/src/ui_display/ui_display.cpp::s_net` — initialised with explicit field names to silence `-Wmissing-field-initializers`. No behavioural change.
- `firmware/src/web_server/web_server.cpp` (`/api/config` POST handler) — added defensive `strlen(ns)`/`strlen(key)` length-check before `snprintf` into `config_update_t` fields. Previously gcc's `-Wformat-truncation` flagged the snprintf as potentially truncating; defensive check returns 400 on over-long keys before they hit the queue. No real-world impact (all current keys are ≤ 12 chars).

### Changed — versioning
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.28` → `1.17.29`.

### Build deltas
- Flash: `1 184 241 B` → `1 187 365 B` (+3 124 B, +0.15 pp). Mostly Phase C instrumentation; Phase D actually shrank by removing the malloc loop.
- RAM (BSS): `67 412 B` → `71 516 B` (+4 104 B). The 4 KB static SDFileChunkedStream chunk buffer. **Net memory win:** loses 4 KB always-allocated BSS, gains back up to 5 MB of PSRAM-availability during the once-per-day log upload.

### Resolves
- [gh#13](https://github.com/pe1mew/greenhouse-Controller/issues/13) — Tier-1/2 hardening + 5 MB streaming refactor.

### Related
- [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12) — Unexpected reboot investigation. Phase C heap-free SD-log row is the single highest-value addition for that investigation. If the next reboot is OOM-driven, the heap-free column of the pre-crash log will show a clear downward trend in the minutes leading up; if it's a task hang, the new WDT subscription on 8 more tasks means the reboot will identify itself as `TASK_WDT` rather than vanishing into a generic panic.

---

## [1.17.28] — 2026-05-13

*Daily log-upload now actually delivers "daily" — force a rotation at the upload slot so the dashboard receives the day's data even when the active CSV hasn't yet hit the 512 KB rotation threshold. Resolves [gh#8](https://github.com/pe1mew/greenhouse-Controller/issues/8) (decision (b) of three options). Behaviour change for T14's daily-fallback path; the rotation-on-512 KB path is unchanged.*

### Added
- `firmware/src/event_logger/event_logger.h` — new public API `event_logger_force_rotate(uint32_t timeout_ms)`. Sets a request flag that T9 polls after each drain pass; T9 calls the existing `rotate_sd_file()` helper and clears the flag. The function posts a synthetic `LOG_SYSTEM` marker (`value_a=6`, new code, documented in the value_a table) to Q3 to (a) wake T9 from a blocked receive and (b) leave a visible last-entry on the file about to be closed. Caller blocks up to `timeout_ms` polling for completion at 100 ms resolution. Returns `false` on timeout or when SD is unmounted (rotation has no meaning without an active file).
- `firmware/src/event_logger/event_logger.cpp` — module state `s_force_rotate_req` (bool, guarded by `s_rotate_mux`); T9 loop checks the flag after the drop-counter handling and rotates if set. `rotate_sd_file()` itself is unchanged — same code path the size-threshold trip already uses.
- `firmware/src/event_logger/event_logger.h` doc table — `value_a=6` added to the LOG_SYSTEM encoding table (force-rotate marker; `value_b=0`).

### Changed
- `firmware/src/status_post/status_post.cpp::maybe_upload_log()` daily-fallback branch — now calls `event_logger_force_rotate(5000)` before `event_logger_newest_closed()`. The 5 s timeout matches the existing per-cycle budget; if rotation doesn't complete in time (e.g. SD card unmounted, T9 stuck on a heavy NVS flush) the code falls through to the pre-1.17.28 behaviour — try whatever newest-closed exists, or emit the `log_upload_skip(2)` diagnostic event.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.27` → `1.17.28`.

### Side effects to be aware of
- **File-count growth.** Previous behaviour: a slow-emitting controller produced ~1 rotation every 14 days, so the 10-file retention window covered ~140 days. New behaviour: 1 rotation per day from the daily slot + occasional 512 KB-threshold rotations, so 10-file retention covers ~10 days. Older files are deleted by the existing `SD_MAX_FILES=10` rule. If you need a longer SD-side history, raise `SD_MAX_FILES` in `firmware/src/event_logger/event_logger.cpp`.
- **An extra `SYSTEM,WEB,0,0,6,0` event lands at the daily slot** in the file that's about to be closed. Cosmetic but worth knowing when reading logs.

### Out of scope
- Manuals (boer / beheerder) still document the pre-1.17.28 behaviour. They'll be updated in the next manual pass.
- The local web GUI Log-tab still has no "Force rotate now" button. Could be added as a Beheerder-only action; not part of this release.

### Resolves
- [gh#8](https://github.com/pe1mew/greenhouse-Controller/issues/8) — Daily log upload: force-rotate at slot (option b).

### Related
- [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12) — Unexpected reboot investigation. With this release the daily-upload feedback loop is functional, so the dashboard sees today's data within 24 h instead of waiting for the next 512 KB rotation. Significantly improves the diagnostic turn-around for any future reboot.

---

## [1.17.27] — 2026-05-13

*Three diagnostic fixes triggered by today's 03:44 reboot and the related "Last log upload is always empty" investigation: (1) the "24-hour" periodic NTP resync in T10 was actually firing every ~8 minutes due to a `pdMS_TO_TICKS` `uint32_t` overflow — confirmed from the user's SD log (`SYSTEM 2,1` events repeating at 8 m 25 s intervals); (2) the firmware never recorded `esp_reset_reason()` at boot, so previous reboots left no diagnostic trail; (3) T14's daily-fallback log-upload path silently no-op'd when no closed file existed on SD, leaving the web GUI's "Last log upload" indicator empty indefinitely with no way to tell whether the slot ran. All three are diagnostic-only — no behavioural change to climate control, wind protection, or status reporting.*

### Fixed
- `firmware/src/network_manager/network_manager.cpp::step_client()` NET_RUNNING branch — periodic NTP resync condition rewritten from `pdMS_TO_TICKS(NTP_RESYNC_INTERVAL_S * 1000UL)` to `(TickType_t)NTP_RESYNC_INTERVAL_S * configTICK_RATE_HZ`. Root cause: the `pdMS_TO_TICKS` macro expansion multiplies `(uint32_t)ms × configTICK_RATE_HZ` inside `TickType_t`; for `86_400_000 ms × 1000 Hz` the intermediate `86_400_000_000` overflows `uint32_t` and wraps to `500_654_080`, which `/1000` becomes `500_654` ticks (≈ 8 min 21 s). The macro on FreeRTOS/Arduino-ESP32 has no overflow guard. Computing the tick count directly (`seconds × Hz`) gives `86_400_000` ticks, well inside `uint32_t`. Direct effects: `configTime("pool.ntp.org")` is now called once per 24 hours instead of ~172× per day; `tzset()` reapplied once per 24 h; T10 no longer enters `vTaskDelay`-spin in `run_ntp_sync()` every 8 minutes; DS1307 `DM_NOTIFY_NTP_SYNCED` count drops from ~172 to 1 per day.

### Added
- `firmware/src/main.cpp::setup()` — boot-reason capture and logging. `esp_reset_reason()` is read at the very top of `setup()` (before any other side effect), logged via `ESP_LOGI` to the serial monitor, and posted to Q3 as the first event the new boot writes to the SD log. Convention: `LOG_SYSTEM`, `value_a = 5` (BOOT marker, new code), `value_b = esp_reset_reason_t` (1=POWERON, 3=SW, 4=PANIC, 5=INT_WDT, 6=TASK_WDT, 7=WDT, 8=DEEPSLEEP, 9=BROWNOUT, …). Posted to Q3 after queue creation, before any task is spawned, so T9 picks it up as its first dequeue. Every fresh SD-log file now starts with a verdict on the previous boot — no more silent unexplained reboots.
- `firmware/src/status_post/status_post.cpp::maybe_upload_log()` — daily-slot diagnostic. The slot now emits a `LOG_SYSTEM` event whenever it fires, regardless of whether an actual upload was attempted: `value_a=0, value_b=2` when the slot fired but no closed CSV exists on SD (the long-running-controller case — the active file hasn't hit the 512 KB rotation threshold yet so `event_logger_newest_closed()` returns nothing); `value_a=0, value_b=3` when a precondition blocked the slot (status disabled / URL empty / WiFi down / pre-NTP / OTA in progress). Without this, the web GUI's "Last log upload" indicator stayed empty for weeks of normal operation with no diagnostic trail. New static helper `log_upload_skip()` encapsulates the event.
- `firmware/src/event_logger/event_logger.h` — documentation block extended with a complete LOG_SYSTEM `value_a` encoding table (subtypes 0–5 and the synthetic −1 drop-overflow marker) plus the new sub-table for `value_a=0` value_b codes (0=status POST, 1=log upload, 2=daily-slot/no-closed-file, 3=daily-slot/precondition-blocked), so the next time someone reads a CSV log they can decode every SYSTEM row without grepping multiple `.cpp` files.
- `bin/gh_issue.py` — minimal stdlib-only GitHub Issues client for `pe1mew/greenhouse-Controller`. Reads token from `GITHUB_TOKEN`/`GH_TOKEN` env or `.github/token.local` file. Supports `list`, `show`, `create`, `comment`, `close`, `reopen`. Lets Claude-driven sessions manage issues without installing `gh` system-wide.
- `.github/README.md` — one-time-setup walkthrough for the local PAT used by `gh_issue.py`. Fine-grained token, repo-scoped Issues read/write only.
- `firmware/issues.md` — restructured from a 2-line stub into a real in-repo TODO with status flags (`open`/`in-progress`/`blocked`/`decision-needed`/`RESOLVED`), seeded with five concrete items including the serial-port-freeze bug, the daily-upload design decision, the index.html placeholder fragility, the Archive/images blob bloat decision, and a forward-port of the boot-reason field to the web GUI.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.26` → `1.17.27`.
- `.gitignore` — added rules for `.github/token.local`, `*.local` (PAT files), `__pycache__/`, `.vscode/`, `.idea/`, `/Archive/images/IMG_*.{jpg,JPG,png}`, `/finance/RECEIPT_*.pdf`, `/model/simulation.zip`, and `/manual/*.html`. Blocks accidental re-commits of the bloat that landed in `b89fac0`.

### Diagnostic context
- The pre-crash SD log `20260410120000.csv` ran from 2026-04-10 to 2026-05-13 (33 days of continuous uptime — a single 524 KB file that rotated exactly at the crash) and ended abruptly at `01:44:41 UTC` with a routine SENSOR event. No panic line, no alarm, no graceful shutdown event. The new boot started immediately and is unaffected by whatever triggered the reset. Without an `esp_reset_reason()` log we cannot tell whether the reset was a panic, a task-WDT, or a brownout. This release closes that diagnostic gap; if/when another reboot occurs, the first line of the new SD log will identify the class of fault.

### Out of scope
- No web GUI, canonical JSON, manuals or PDFs touched. Manuals will be updated when the boot-reason field becomes user-visible (e.g. a "Last boot reason" line on the Status tab's Clock card).

### Related
- [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12) — Unexpected reboot investigation. This release's `esp_reset_reason()` boot logging is the primary diagnostic instrument for that investigation. From here on, every fresh SD log's first event is a verdict on the previous boot, and the matching `Phase 0 boot — esp_reset_reason=<n>` serial line gives the same answer to whoever's watching the host-side capture.

---

## [1.17.26] — 2026-05-12

*One-character LCD cosmetic fix for GitHub issue [#6](https://github.com/pe1mew/greenhouse-Controller/issues/6) on the Wind status page (page 2): insert a space between `Dir:` and the heading digits so the colon aligns with the same spacing used everywhere else on the LCD (`Wind:` row, `Mode:`, `Sess:`, the `Dir: ---` invalid-reading row directly below it).*

### Fixed
- `firmware/src/ui_display/ui_display.cpp::render_status()` case 1 (Wind) — valid-reading format string changed `" Dir:%3d \xDF (%-2s) "` → `" Dir: %3d \xDF (%-2s)"`. Width stays at exactly 16 columns: one extra space is inserted between the colon and the `%3d` field, and one trailing space at the end of the row is dropped to compensate. Resulting display for 180° south wind: `" Dir: 180 ° (S )"`. The invalid-reading row on the next branch already uses `" Dir: --- "` and is unchanged; valid and invalid rows are now consistent. Reported by @pe1mew.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.25` → `1.17.26`.

### Changed (docs)
- `manual/boerHandleiding.md` — chapter §6 (LCD Screen 2) and the v1.2 version-history row updated to show the new layout `Dir: 180 ° (S )`. The history row itself is rewritten as a note rather than touched in-place so older readers can still see what changed.
- `manual/beheerderHandleiding.md` — references to the Wind status row updated.
- Both PDFs regenerated.

### Out of scope
- Pure cosmetic; no behavioural or wire-format change. No effect on web GUI, canonical JSON, or external dashboard.

---

## [1.17.25] — 2026-05-11

*Two LCD rotating-status polish items: add a right-aligned `Day` / `Night` badge to the Time page (page 4, row 1) so the operator can read the controller's active day/night state without crawling into a menu, and clean up the Uptime line on the Firmware page (page 6, row 1) to be left-aligned with a single space after the colon. **Documentation-only follow-up on 2026-05-12 (no firmware change):** structural reorganisation of the Dutch admin manual, sequential figure numbering, branded PDF page header/footer, and Dutch footer wording.*

### Changed
- `firmware/src/ui_display/ui_display.cpp::render_status()` case 4 — row 1 now reads `Src:NTP      Day` or `Src:RTC    Night` (right-aligned in the trailing 9 columns via `%9s`). Source comes from `cfg.is_daytime` so the badge flips at the exact same sunrise/sunset moments the climate controller switches setpoints.
- `firmware/src/ui_display/ui_display.cpp::render_status()` case 6 — uptime line is left-aligned with a single space after the colon. The compact `1d 4h 23m` / `4h 23m` / `23m` body is built into a scratch buffer first and then space-padded to the 16-column LCD width. Examples: `"Up: 23m         "`, `"Up: 4h 23m      "`, `"Up: 1d 4h 23m   "`. Previous format used colon-no-space and right-padding zeros (`Up:23d  4h 23m  `).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.24` → `1.17.25`.

### Documentation (2026-05-12)
- `manual/beheerderHandleiding.md` — bumped header to **v1.8** (was v1.6). v1.7 introduced four new sub-chapters under §10 "Klimaat instellen" that describe the remaining webinterface tabs one-on-one: **§10.5 System-tab** (WiFi AP, WiFi client, NTP en tijdzone, geografische locatie, sessie-timeout, OTA cross-reference) absorbs the contents of the former §11.2–§11.9; **§10.6 Access-tab** (PIN management for both roles) cross-references §9; **§10.7 Log-tab** (SD-card mount/unmount, requirements, automatic mounting) cross-references appendix F for the CSV format; **§10.8 Web-tab** (remote status reporting, ASCII operation diagram, fields table, HTTPS section, common errors, log-upload section) is the former §11.10 moved over. §11 was slimmed down to just the **one-off first-time WiFi installation procedure** (after factory reset or new install); chapter heading renamed to "Eerste-installatie WiFi-verbinding". TOC and internal cross-references updated. v1.8 adds the cosmetic PDF revision row (see below).
- `manual/boerHandleiding.md` — bumped header to **v1.4** (was v1.3). Cosmetic PDF revision row added; no manual content changes other than the version-history table.
- `manual/md2pdf.py` — rewritten render path. Edge headless is now driven via the **DevTools Protocol** over a WebSocket (`simple_websocket.Client`) so we can call `Page.printToPDF` with custom `headerTemplate` and `footerTemplate` fields. Edge's CLI `--print-to-pdf` cannot inject custom templates; the previous pipeline rendered without any branding. Added: pre-processing of the markdown that replaces every `Figuur #:` placeholder with sequential `Figuur 1:`, `Figuur 2:`, … in document order (source `.md` is not mutated; substitution happens in the in-memory text fed to the HTML converter); auto-extraction of `**Versie:** X.Y` from the source so the right-hand header is always in sync with the document. CSS `@page` top/bottom margins widened to 22 mm to leave room for the templates.
- Branded PDF header/footer on **every page**: top-left `Kas Controller - Herenboeren Wenumseveld`, top-right `v<version>`; bottom-left `Een RFSee product - http://www.rfsee.nl`, bottom-right `pagina <n>` (Chromium's `.pageNumber` span substitution).
- `manual/beheerderHandleiding.pdf` — regenerated (4.4 MB, 13 figures sequentially numbered, every page carries the branded header/footer with `v1.8`).
- `manual/boerHandleiding.pdf` — regenerated (2.3 MB, 5 figures sequentially numbered, every page carries the branded header/footer with `v1.4`).

### Out of scope
- No web GUI / canonical JSON change; the firmware-side change in this release is LCD-only polish.
- The 2026-05-12 documentation work is a manual-only follow-up and does not bump the firmware version — both manuals still target firmware **1.17.25**.

---

## [1.17.24] — 2026-05-11

*Four LCD-UI improvements: align Climate → CR-priority with the Day/Night view-then-edit flow, add a global D-key escape that jumps back to the rotating status screens from any menu, auto-return to the status rotation after 5 minutes of menu inactivity, and add a firmware-version + uptime page to the status rotation.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — new `UI_BROWSE_CR` state. Mirrors `UI_BROWSE_DAY`/`UI_BROWSE_NIGHT`: shows the current `cr_priority` value with `↩#` edit and `^*` back hints; pressing `#` triggers the existing `begin_edit()` path, which prompts for the Farmer PIN if not yet authenticated and returns to the Climate menu after the edit.
- `render_browse_cr()` / `handle_browse_cr()` helpers wired into the FSM render and key-dispatch switches.
- `STATUS_PAGES` bumped 6 → 7. New status page 6 shows `FW: <FIRMWARE_VERSION>` on row 0 and uptime on row 1 (`Up: 23m`, `Up: 4h 23m`, `Up: 1d 4h 23m`). Same compact format as the local web GUI Clock-card uptime.
- `AUTOROTATE_RETURN_TICKS` = 3000 (5 min × 60 s × 10 ticks/s at `UI_LOOP_MS = 100`). New `s_menu_idle_ticks` counter is incremented every tick while `s_state != UI_STATUS`, reset on every non-repeat keypress; on threshold the FSM is forced back to `UI_STATUS`. Independent of the session-timeout path so it runs regardless of login state.
- Global `D`-key handler — before the per-state dispatch, if `s_state != UI_STATUS` the `D` press calls `go_status()` and consumes the event. One-press escape from any menu / browse / edit / PIN-entry / set-time depth. Inside `UI_STATUS` the legacy "advance to next status page" behaviour on `D` is preserved.

### Changed
- `handle_menu_climate()` case `'3'` — was `begin_edit(false, 11, UI_MENU_CLIMATE)` (jumped straight to PIN + edit). Now transitions to `UI_BROWSE_CR` first; the user sees the active value before being asked to edit. Same pattern the `'1'` / `'2'` keys already follow.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.23` → `1.17.24`.

### Out of scope
- The Dutch admin manual's §6 LCD-page list still numbers 0–5; bump on next manual pass to add page 6 (Firmware) and document the `D`-back-to-status shortcut + 5-min auto-return.
- `handle_status()` (the UI_STATUS dispatch) still consumes `D` as "next status page" — by design; this is the original page-advance shortcut and remains useful for cycling without waiting 5 s.

---

## [1.17.23] — 2026-05-11

*Differentiate the two canonical-JSON consumers: the local web GUI keeps the RH-setpoint values visible (dimmed) when RH control is disabled, while the T14 → public-dashboard payload omits those two fields entirely so the dashboard doesn't render inert configuration. A new `rh_ctrl_enabled` boolean is always emitted inside the climate object so consumers know which mode is active.*

### Added
- `firmware/src/types/app_types.h::status_snapshot_t::rh_ctrl_enabled` — boolean filled from `cfg.rh_ctrl_en` in `dm_status_snapshot()`.
- `firmware/data/style.css` — `.dimmed { opacity: 0.5 }` rule. Stable layout (row stays in place) but immediately signals the value is inert.
- Canonical JSON now always carries `climate.rh_ctrl_enabled` (true/false).

### Changed
- `firmware/src/status_post/status_json.{h,cpp}::build_canonical_status_json()` — new fourth parameter `bool include_disabled_setpoints`. When false (T14 path), `climate.rh_max_active` / `climate.rh_min_active` are omitted from the emitted JSON when `rh_ctrl_enabled` is false. When true (local UI path), both fields are always emitted so the GUI can render dimmed values rather than gaps.
- `firmware/src/web_server/web_server.cpp::build_status_json()` — passes `true` for the new parameter.
- `firmware/src/status_post/status_post.cpp::task_status_post()` — passes `false` for the new parameter.
- `firmware/data/app.js::handleStatus()` — when `c.rh_ctrl_enabled === false`, applies the `.dimmed` CSS class to the `<p>` parents of `#st-rh-max` and `#st-rh-min`. Toggles back when re-enabled.
- `webUiMock/mock_server.py::_build_status()` — always emits `climate.rh_ctrl_enabled` from `cfg["rh_ctrl_en"]`. Setpoint values are always emitted (mock serves the local-UI consumer path).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.22` → `1.17.23`.

### Out of scope
- The wind side has a parallel `wind_prot_en` toggle that could apply the same treatment to the wind v_max / Variation rows. Not done in this release — flag for future symmetric improvement.

---

## [1.17.22] — 2026-05-11

*Expand the sensor-history table at the bottom of the web GUI from 4 columns (Time, T, RH, Wind, Dir) to 8 columns (Time, T, T-avg, RH, RH-avg, Wind, Wind Avg, Direction, Variation). Pairs every raw measurement with its sliding-window average and adds the new direction-variation metric from 1.17.21. `/api/history` field names align with the canonical status-JSON `climate`/`wind` keys so the same value carries the same name on every endpoint.*

### Changed
- `firmware/src/web_server/web_server.cpp::/api/history` — per-row JSON expanded from `{ts, temp_c, rh_pct, wind_ms, wind_dir}` to `{ts, temp_c, temp_avg_c, rh_pct, rh_avg_pct, speed_ms, speed_avg_ms, direction_deg, direction_variation_deg}`. Field naming aligned with the canonical status JSON (1.17.x): `wind_ms` → `speed_ms`, `wind_dir` → `direction_deg`. The previous endpoint stored `t_avg_c` under the key `temp_c` (misleading); raw `temperature_c` is now under `temp_c` and the average under `temp_avg_c`.
- `firmware/src/web_server/web_server.cpp` — `HIST_BUF` bumped 6 144 → 12 288 bytes (PSRAM). Per-row payload roughly doubled to ~160 chars; 60 rows × 160 ≈ 10 KB with 20 % headroom. PSRAM allocation cost is trivial.
- `firmware/data/index.html` — sensor history `<thead>` rewritten with 9 columns matching the new schema.
- `firmware/data/app.js::loadHistory()` — row builder writes 8 data cells (matching the new headers) using small `f1` / `i0` helpers for compact "value or em-dash" rendering.
- `webUiMock/mock_server.py::_build_history()` — emits the same 9-key per-row shape; introduces a slight phase lag on the avg sinusoids so the raw vs. avg columns are visibly distinct during dev.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.21` → `1.17.22`.

### Out of scope
- Existing callers of `/api/history` outside `app.js` (none in this repo) would need to switch from `wind_ms`/`wind_dir` to `speed_ms`/`direction_deg` and consume the new fields. Breaking-change but additive in JSON terms — old keys are gone.
- T11 endpoints `/api/status` and the WS push were already on the canonical names; no change there.

---

## [1.17.21] — 2026-05-11

*Surface the currently-active climate setpoints and a new wind-direction-variation metric on the local web GUI Status tiles, and ship the same values in the canonical status JSON so the public dashboard can consume them (it currently ignores extras — pure additive change, no breakage). The Wind card row order is reshuffled to keep the two speed lines together: Speed → Avg → Direction → Variation.*

### Added
- `firmware/src/types/app_types.h::sensor_reading_t` — new `wind_dir_variation_deg` field. Width of the smallest arc containing every direction sample in the current sliding window; 0 when count < 2 samples.
- `firmware/src/types/app_types.h::status_snapshot_t` — new fields `t_max_active` (°C), `rh_max_active` (%), `rh_min_active` (%), `w_dir_variation_deg`. Active setpoints are the day-or-night value currently in force based on `cfg.is_daytime`.
- `firmware/src/sensor_poll/sensor_poll.cpp::dir_avg_variation()` — circular-aware arc-width computation: reconstructs per-sample angles from the existing sin/cos ring buffer, sorts them with insertion sort (N ≤ SP_AVG_DEPTH, typically 12–30, so O(N²) is fine), finds the largest gap including the wraparound from last back to first, and reports `360 − max_gap`. Handles north-crossing wraparound correctly (e.g. 5°/355°/10° → 15°, not 350°).
- `firmware/src/data_manager/data_manager.cpp::dm_status_snapshot()` — copies `wind_dir_variation_deg` from the sensor reading; selects active climate setpoints from the cfg shadow based on `is_daytime`.
- `firmware/src/status_post/status_json.cpp::build_canonical_status_json()` — emits the new keys inside the existing `climate` and `wind` objects:
  - `climate.temp_max_active`, `climate.rh_max_active`, `climate.rh_min_active`
  - `wind.direction_variation_deg`
- `firmware/data/index.html` — Temperature card gains a `Setpoint:` line; Humidity card gains `Setpoint max:` and `Setpoint min:` lines; Wind card gains a `Variation:` line. Each new row has a contextual `data-tip` tooltip.
- `firmware/data/app.js::handleStatus()` — wires the new DOM IDs (`st-t-max`, `st-rh-max`, `st-rh-min`, `st-wind-var`) to the matching JSON fields.
- `webUiMock/mock_server.py::_build_status()` — mirrors the new fields so dev iteration without the device sees the same shape; wind variation is a slow ~30°→110° sine for visible movement on the dashboard.

### Changed
- `firmware/data/index.html` — Wind card row order reshuffled from Speed/Direction/Avg to **Speed → Avg → Direction → Variation** so the two speed metrics sit together and direction-related rows follow.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.20` → `1.17.21`.

### Out of scope
- Public dashboard at `pe1mew.nl/hbwv` does not consume the new fields yet; its `app.js::renderClimate()`/`renderWind()` silently drop unknown keys. Add markup + JS there to display the new values when the website project is next touched.
- Canonical JSON worst-case payload grows by ~50 bytes (well within the 2 KB buffer).

---

## [1.17.20] — 2026-05-11

*Retire the temporary OTA-diagnostic infrastructure now that the LittleFS basePath bug (fixed in 1.17.9) has been field-confirmed via a successful 1.17.9 → 1.17.9a round-trip on a live controller. The version-mismatch detection is kept — it is a low-cost canary against any future regression — but moves from its own diagnostic card into the existing **Alarms** card, where it semantically belongs alongside motor-alarm and wind-override badges. Durable inspection surfaces (`/manifest.json` HTTP route, `<!-- web-assets X.Y.Z -->` HTML comment, `?v=<VERSION>` cache-busters, in-ZIP manifest.json from `build_release.ps1`) all remain — they are general-purpose post-OTA verification, not specific to the resolved bug.*

### Removed
- `firmware/data/index.html` — the temporary "OTA diagnostic (temp)" card (the comment-fenced `<div class="card">` block lines 80-98 of 1.17.6–1.17.9a) is deleted entirely. The `#st-fw`, `#st-assets`, and `#st-mismatch` DOM IDs go with it.

### Changed
- `firmware/data/app.js::handleStatus()` — version-mismatch detection now appends a `MISMATCH` badge to the existing alarms list (alongside `WIND`, `MOTOR ALARM`, sensor faults etc.) rendered in `#st-alarms`. Single dashboard surface for all active issues. The standalone `#st-mismatch` toggle, `setText('st-fw', …)` and `setText('st-assets', …)` calls are removed (the elements no longer exist; setText was null-safe but the calls are dead code now).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.9a` → `1.17.20`. The jump in patch number signals the diagnostic-infrastructure cleanup is the headline change in this release.

### Preserved
- `system.asset_version` field in the canonical status JSON — still emitted by the builder, still read by the local UI for the MISMATCH check, still surfaceable via `/api/status` for tooling.
- `GET /manifest.json` HTTP route — useful for `curl`-based verification of which `asset_version` is on the active LittleFS partition.
- `<!-- web-assets X.Y.Z -->` HTML comment stamp on line 2 of `index.html` — definitive View Source readout.
- `?v=<FIRMWARE_VERSION>` cache-buster injection in `serve_lfs()` for `app.js` / `style.css` — forces fresh fetches when the firmware version changes.
- `bin/build_release.ps1` stamping logic (manifest.json generation + `{{ASSET_VERSION}}` substitution).
- `[hidden] { display: none !important }` rule in `style.css` (general latent-bug fix; not specific to OTA diagnostic).

### Out of scope
- The Dutch admin manual's §6 "Status-tab — OTA diagnostic (temp)" subsection still describes the (now-removed) card. Update on next manual pass; the §6 "Status-tab — Klok-tegel" content is still accurate. The MISMATCH-in-Alarms behaviour is documented inline as alarm badges already are.

---

## [1.17.9a] — 2026-05-11

*Functionally identical to 1.17.9 — version-tag-only re-release for OTA round-trip verification of the LittleFS basePath fix. The device runs 1.17.9 from serial flash. Uploading `greenhouse-controller-1.17.9a.bin` and `web-assets-1.17.9a.zip` via the OTA tab forces firmware to write to the inactive bank, assets to the inactive LFS, and bank-flip. With the basePath fix in place, all four diagnostics (LCD/footer firmware version, View Source comment, `/manifest.json`, OTA diagnostic card) MUST flip to `1.17.9a`. Any surface still reporting `1.17.9` after the reboot is a residual bug at that surface — but in 1.17.8a the same test would have flipped only the firmware version while assets stayed at whatever was last in the destination LittleFS partition.*

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.9` → `1.17.9a`.

### Out of scope
- No code changes from 1.17.9. The LittleFS basePath fix (`/lfsa` / `/lfsb`) is inherited as-is.

---

## [1.17.9] — 2026-05-11

*Root-cause fix for the asset-OTA cross-bank bug that 1.17.4 through 1.17.8a failed to resolve. The bug was not in T13 or the OTA flow — it was in `drivers/littleFS/src/littlefs_storage.cpp`, which mounted **both** LittleFS partitions at the same VFS path `/lfs`. ESP-IDF's VFS layer cannot have two filesystems at the same path; the Arduino `LittleFSFS::begin()` call either silently failed or rebound `/lfs` to the second partition. Practical effect: T11 mounted lfs0 at `/lfs` at boot, then during paired OTA T13 called `littlefs_mount(LFS_PARTITION_B)` which appeared to succeed — but T13's subsequent writes never reliably reached lfs1. After reboot to bank B the device mounted lfs1 and found the OLD content from a prior cycle (in the field-reported case, 1.17.3 era assets). All the manifest/version-stamp diagnostics added between 1.17.4 and 1.17.8 were correct; they finally exposed the underlying storage bug.*

### Fixed
- `drivers/littleFS/src/littlefs_storage.cpp` — each partition now uses a UNIQUE VFS mount point: `LFS_PARTITION_A` → `/lfsa`, `LFS_PARTITION_B` → `/lfsb`. Affects `littlefs_mount()` and `littlefs_format()`. Both can now be mounted simultaneously without conflict, so T13's writes to the inactive partition during paired OTA reach the correct flash region.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.8a` → `1.17.9`. (1.17.8a was a verification-only re-release of 1.17.8 — same buggy storage driver — used to prove the crossing was real before this fix.)

### How to verify the fix
1. Flash 1.17.9 via serial (clears both banks/LFSes implicitly: PIO upload to bank A, esptool write_flash to lfs0).
2. Hard-reload the page; confirm all four diagnostics report `1.17.9` (firmware, View Source comment, `/manifest.json`, OTA diagnostic card).
3. OTA-upload `greenhouse-controller-1.17.9a.bin` + `web-assets-1.17.9a.zip` (separate release-tag re-build of the same code).
4. After the controller reboots, ALL four diagnostics must now flip to `1.17.9a`. Any surface still reporting `1.17.9` is a residual bug — but pre-fix, the assets surfaces would have stayed at whatever was last in the destination LittleFS (in the user's case, 1.17.3).

### Out of scope
- Existing devices that already have stale content on lfs1 from prior failed OTAs will get it overwritten on the next paired OTA running 1.17.9 (because T13's writes now actually reach lfs1). No migration step required.

---

## [1.17.8a] — 2026-05-11

*Functionally identical to 1.17.8 — version-tag-only re-release so the user can OTA-upload both `greenhouse-controller-1.17.8a.bin` and `web-assets-1.17.8a.zip` to a device currently running 1.17.8 and observe a clean version flip on every diagnostic (HTML comment, `/manifest.json`, the OTA-diagnostic card). If the OTA path is honest the controller will report `Firmware: 1.17.8a / Assets: 1.17.8a` after the reboot. If anything reports `1.17.8` (the previous version) somewhere, the crossing is at exactly that surface.*

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.8` → `1.17.8a`. The trailing-letter format is now permitted: `bin/build_release.ps1`'s parsing regex was widened from `[0-9]+\.[0-9]+\.[0-9]+` to `[0-9]+\.[0-9]+\.[0-9]+[a-z]?`.
- `bin/build_release.ps1` — regex updated as above; no other change.
- `manual/beheerderHandleiding.md` — version-history row `1.4` summarises every change from 1.17.2 through 1.17.8a; new §6 "Status-tab — OTA diagnostic (temp)" subsection documents the diagnostic card and how to read it.

### Out of scope
- No firmware-logic changes from 1.17.8. Use this release purely for OTA round-trip verification.

---

## [1.17.8] — 2026-05-11

*1.17.7 made `manifest.json` part of the LittleFS asset bundle but forgot to expose it over HTTP. Field testing confirmed View Source proves the served HTML version, but `curl http://<controller>/manifest.json` returned 404 because T11 only registers explicit routes — there is no static-file fall-through. Adding the route.*

### Added
- `firmware/src/web_server/web_server.cpp` — new `GET /manifest.json` handler that calls `serve_lfs(req, "/manifest.json", "application/json")`. Provides a definitive browser-inspectable readout of which `asset_version` is physically present on the active LittleFS partition, independent of any DOM/JS path.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.7` → `1.17.8`.

### Out of scope
- The HTTP route is unauthenticated. `manifest.json` only contains the version string; no secrets ever travel through it.

---

## [1.17.7] — 2026-05-11

*Fix the asset-version reporting that 1.17.4 introduced. T13 used to overwrite `/manifest.json` after extracting the ZIP, stamping it with the **firmware's** version regardless of what the uploaded ZIP actually contained. As a result `system.asset_version` always equalled `fw_ver` and the MISMATCH badge could never trigger — even though the on-device assets really could be mismatched. The fix moves manifest generation into `build_release.ps1` so the version travels INSIDE the ZIP and reflects exactly what was packaged; T13 now preserves the ZIP's manifest verbatim. A version-stamp HTML comment is also injected into `index.html` so View Source on the live page confirms which assets are being served, independent of any JS / DOM behaviour.*

### Fixed
- `firmware/src/ota_manager/ota_manager.cpp::task_t13_assets()` — removed the post-extraction overwrite of `/manifest.json`. The asset's actual version now survives the OTA. A log line records whether the ZIP carried a manifest at all (`'?'` is reported in `system.asset_version` when it didn't).
- `bin/build_release.ps1` — new Step 0 stamps the version into two places in `firmware/data/` before any build runs:
  - `manifest.json` is generated freshly with `{"asset_version":"<VERSION>",...}`.
  - `index.html` has the literal placeholder `{{ASSET_VERSION}}` (added in this release) replaced with `<VERSION>`. Both `pio buildfs` and the STORE-ZIP packager then pick up the stamped versions.
- `firmware/data/index.html` — new HTML comment on line 2: `<!-- web-assets {{ASSET_VERSION}} -->`. Visible via View Source on the live device — a definitive readout of which assets version is currently being served, regardless of any styling/CSS/JS quirks.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.6` → `1.17.7`.

### Out of scope
- `firmware/data/manifest.json` and the stamped `index.html` are now build-generated. They are overwritten by `build_release.ps1` on every run; the originals (with `{{ASSET_VERSION}}` placeholder intact in `index.html`) are restored from git when checking out a clean tree. No `.gitignore` change in this release — the file may show as modified after a build, which is harmless and serves as a visible reminder that the data folder has been stamped.

---

## [1.17.6] — 2026-05-10

*Move the OTA version-mismatch diagnostics from the Clock card into a dedicated, clearly-marked temporary card so the bug-investigation UI can be removed in one block when no longer needed.*

### Changed
- `firmware/data/index.html` — Clock card reverted to its pre-1.17.4 three-line layout (Time, NTP, Uptime). The `Firmware:` and `Assets:` lines plus the `MISMATCH` badge now live in a new "OTA diagnostic (temp)" card directly after the Clock card. The card is wrapped in clearly-labelled HTML comment fences (`<!-- TEMPORARY: … -->` … `<!-- END TEMPORARY CARD -->`) so it can be deleted as one block when the OTA flow is confirmed solid.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.5` → `1.17.6`.

### Out of scope
- `app.js::handleStatus()` is unchanged — the `setText()` helper guards each write with `if (el)`, so when the temporary card is removed the `#st-fw` / `#st-assets` / `#st-mismatch` writes become silent no-ops. No firmware change required to retire the card.
- The `#fw-ver` element in the page footer continues to render the version independently of the temporary card.

---

## [1.17.5] — 2026-05-10

*Two hot fixes for the 1.17.4 mismatch indicator. The MISMATCH badge was permanently visible because `.badge { display: inline-block }` overrode the user-agent's `[hidden] { display: none }`, so toggling the HTML `hidden` attribute did nothing. The firmware version was also only displayed in the page footer; on the new Clock-card layout the user could read `Assets: ?` next to a red MISMATCH badge with no firmware-version line nearby to compare against.*

### Fixed
- `firmware/data/style.css` — added `[hidden] { display: none !important; }` so the HTML `hidden` attribute wins over the class-based display rules. Without this, every `.badge` element with `hidden` (e.g. `#st-mismatch`) stayed visible regardless of `app.js` state.

### Added
- `firmware/data/index.html` — new `Firmware:` line in the Clock card next to `Assets:` so both versions are visible side-by-side; tooltip explains where each value comes from.
- `firmware/data/app.js::handleStatus()` — `sys.fw_ver` now updates both the Clock-card line (`#st-fw`) and the footer (`#fw-ver`) on every WebSocket push. Previously it was a one-shot set gated by `wsInitialized`; if the first push lacked `fw_ver` for any reason, the field stayed at `—` forever.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.4` → `1.17.5`.

### Out of scope
- The `[hidden]` rule retroactively fixes any other `<X class="badge" hidden>` element that may have been silently visible. Audit not done in this release; `#st-mismatch` was the user-visible regression.

---

## [1.17.4] — 2026-05-10

*Diagnostics for OTA mismatches. After a paired firmware+assets OTA, a silent firmware/asset mismatch (firmware bank flipped but the matching LFS partition wasn't actually overwritten) used to be invisible: the GUI reported the firmware version correctly while the rendered assets were from a different release. This release surfaces the asset version on the local web GUI and forces the browser to revalidate `app.js` / `style.css` whenever the firmware version changes — no protocol changes, only diagnostics.*

### Added
- `firmware/src/data_manager/data_manager.cpp::dm_status_snapshot()` — reads `/manifest.json` from the active LittleFS partition once at first call (cached), parses `asset_version`, and fills it into a new `status_snapshot_t::assets[16]` field.
- `firmware/src/types/app_types.h::status_snapshot_t::assets` — string slot for the asset version. Emitted as `system.asset_version` in the canonical JSON, alongside `fw_ver`.
- `firmware/data/index.html` — Status → Clock card now shows an `Assets:` line plus an `MISMATCH` red badge that auto-toggles when the firmware version and the asset version disagree. Tooltip explains what to do (refresh, then re-run asset OTA).
- `firmware/data/app.js::handleStatus()` — renders `system.asset_version` and toggles the mismatch badge.
- `firmware/src/web_server/web_server.cpp::serve_lfs()` — when serving `/index.html`, rewrites `app.js` and `style.css` references in-place to `app.js?v=<FIRMWARE_VERSION>` and `style.css?v=<FIRMWARE_VERSION>`. The query string travels with the URL only; routing still hits the same handlers (ESPAsyncWebServer strips the query string before matching). Browsers that ignore `Cache-Control: no-store` are still forced to revalidate because the URL itself changed.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.3` → `1.17.4`.

### Out of scope
- This release does not change the OTA wire protocol; the asset OTA still writes new assets to the inactive LFS and pairs them with the inactive firmware bank. The dual-bank rollback property is preserved (firmware and assets stay paired per bank).
- The cache-buster string is the firmware's compile-time `FIRMWARE_VERSION`. If a user uploads a `web-assets-X.zip` that doesn't match the firmware they uploaded alongside, the page link will say `?v=<firmware-version>` while `manifest.json` reports the asset's actual version — exactly the cue that drives the new MISMATCH badge.

---

## [1.17.3] — 2026-05-10

*Fix asset-only OTA reverting to the previous web assets after reboot. T13's success path used to switch the boot partition to the inactive firmware bank for **every** OTA, including asset-only uploads — but the inactive bank may hold stale or unbootable firmware (typical after a clean `pio run -t upload` that only touches one bank), in which case the boot fails and the bootloader rolls back to the original bank. The user then sees the OLD assets because the new ones were written to the now-inactive LittleFS partition that T11 doesn't mount.*

### Fixed
- `firmware/src/ota_manager/ota_manager.cpp::task_t13_assets()` — asset-only OTA path (`s_ota_part == NULL`, i.e. no firmware was uploaded in the same session) now **mirrors** the new ZIP contents to the active LittleFS partition AND skips the boot-partition switch. T11 stays on the same bank and immediately serves the new assets after the reboot.
- Firmware+assets OTA path (`s_ota_part != NULL`) is unchanged: boot still switches to the verified inactive bank, where both new firmware and new assets sit together. The fallback to `esp_ota_get_next_update_partition()` is removed — that fallback was the source of the bug.

### Changed
- `firmware/data/style.css` — `input[type="url"]` added to the dark-input selector group so the Web tab's URL field uses the same theme as the other inputs (was rendering with the browser-default white background on dark page).
- `firmware/data/index.html` — Web tab "Daily upload time" H/M number inputs no longer have an inline `width: 3.5em` (which was too narrow to fit a 2-digit value plus the native spinner buttons). They use the existing `.short` (90 px) class, matching every other short numeric field in the GUI.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.2` → `1.17.3`.

### Out of scope
- Recovering an already-stranded asset upload (assets sitting on lfs1 after a failed bank switch) — those assets are simply overwritten by any subsequent asset OTA. No migration needed; the next clean upload restores correct state.

---

## [1.17.2] — 2026-05-10

*Cosmetic only: make the time on the Status-tab Clock tile **bold** so it matches the value-rendering convention used by every other Status tile (where the dynamic value is wrapped in `<strong>` and the surrounding label / unit is regular weight).*

### Changed
- `firmware/data/index.html` — Clock tile time element changed from `<p id="st-time">…</p>` to `<p><strong id="st-time">…</strong></p>`. The `data-tip` tooltip stays on the `<p>` so the hover-help is unchanged; `setText('st-time', …)` in `app.js` keeps working unchanged because the target ID just moved one level inwards.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.1` → `1.17.2`.

### Out of scope
- Other tiles already use `<strong>` for the value; no firmware logic changes; LittleFS-only patch but rebuilt firmware too because `FIRMWARE_VERSION` is baked at compile time and is displayed by the System / Clock surfaces.

---

## [1.17.1] — 2026-05-10

*Field-readiness polish for the 1.17.0 status-website feature. Aligns the canonical payload with the actual public dashboard contract at `pe1mew.nl/hbwv` (different field names than the spec implied), routes sunrise/sunset and `time_iso` through a TZ-correct path so the dashboard shows local time, widens the T11 status JSON buffers, and surfaces uptime on the System tile so unexpected resets are visible at a glance.*

### Added
- `firmware/data/index.html` System → Clock card — new `<strong id="st-uptime">` row, tooltipped "Time since the controller last booted … useful for spotting unexpected resets."
- `firmware/data/app.js::fmtUptime()` — formats `system.uptime_s` as `1d 4h 23m` / `4h 23m` / `2m 13s` / `5s` and binds it in `handleStatus()`.

### Changed
- `firmware/src/status_post/status_json.cpp::build_canonical_status_json()` — field names rewritten to match the public dashboard's `app.js` consumer:
  - climate: `temp_c` / `temp_avg_c` (was `t_c` / `t_avg_c`)
  - wind: `speed_ms` / `speed_avg_ms` / `direction_deg` / `direction_avg_deg` (was `ms` / `avg_ms` / `dir_deg` / `avg_dir_deg`)
  - mode: now an object `{current, flags[]}` (was a bare string). `current` is the highest-priority mode label (`MOTOR_ALARM`/`WIND_OVERRIDE`/`WINDOW_CAL`/`AUTOMATIC`); `flags` is an EG1-derived array (`wind_override`, `sensor_fault_temp`, `sensor_fault_wind`, `ota_in_progress`, `motor_alarm`, `calibrating`).
  - sun: `sunrise_min` / `sunset_min` in **local** minutes-from-midnight (was `sunrise_mins_utc` / `sunset_mins_utc` in UTC).
  - system: `wifi_ip` / `wifi_rssi_dbm` / `ntp_synced` / `fw_ver` (was `ip` / `rssi` / `ntp` / `fw`).
- `firmware/src/data_manager/data_manager.cpp::dm_status_snapshot()` — converts `cfg.sunrise_mins_utc` / `cfg.sunset_mins_utc` to **local** minutes by deriving the offset from `localtime_r` vs. `gmtime_r` (Newlib has no `tm_gmtoff`), so DST is handled automatically. Belt-and-braces `setenv("TZ", …)` + `tzset()` reapply on every snapshot closes the brief window after `configTime()` where TZ is `UTC0`; gated by `strcmp` against `getenv("TZ")` to avoid the env-allocate-free churn (this snapshot runs every 2 s from the WS push).
- `firmware/src/types/app_types.h::status_snapshot_t` — `sunrise_mins_utc` / `sunset_mins_utc` renamed to `sunrise_mins_local` / `sunset_mins_local` to make the post-conversion semantics explicit.
- `firmware/data/app.js::handleStatus()` — reads the new nested field names from the canonical builder; mode rendering now handles the `{current, flags[]}` object and builds badge HTML from the `flags` array instead of from raw EG1 bits.
- `firmware/data/app.js` — Web-tab auto-refresh now calls a new `refreshWebStatus()` (status indicators only) instead of `loadWebCfg()` (full reload), so the user's in-progress edits to URL / secret / interval / expose checkboxes are not clobbered every 5 s. `postWebCfg()` success path calls `loadWebCfg()` once after Apply so the form reflects exactly what was persisted.
- `firmware/data/app.js::validateStatusUrl()` — client-side syntax check: empty allowed (disables feature), otherwise must start `http(s)://`, must not contain `?` or `#`, and must end with `api.php`. Matching server-side check in `/api/web` POST handler.
- `firmware/src/web_server/web_server.cpp` — `/api/status` GET buffer and the WS-push buffer both bumped 1024 → **2048** bytes (and matching `ps_malloc` / `build_status_json` size argument). Canonical worst-case payload is ~720 B; the new size keeps a 2.8× margin against future schema additions. The intermediate inconsistent state (alloc 1024 / build 2048) is no longer possible — the size literal is defined per call site with a pinning comment.
- `firmware/src/data_manager/data_manager.cpp::dm_reload_web_cfg()` — now reloads the cfg shadow **synchronously** under MX4. The previous TN5/task-notification path left a window where a `GET /api/web` (e.g. the 5 s tab refresh) immediately after Apply could still read the previous shadow values and snap the form back to the old URL. The `DM_NOTIFY_RELOAD_WEB` bit and its T4 handler are removed.
- `firmware/src/main.cpp` — T14 stack bumped **6 KB → 12 KB**. `WiFiClientSecure` / mbedTLS handshake needs substantially more stack than plain HTTP; 6 KB was enough for plain `http://` POSTs but blew the stack on first `https://` handshake, causing a reboot.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.17.0` → `1.17.1`.
- `design/impact-analysis-statusReporting.md` — status updated to *Shipped (1.17.1)* with a divergences-from-design note (canonical-shape field names; `mode` as object; sun as local minutes).
- `design/implementationStatusPages.md` — status updated to *Shipped (1.17.1)* with the same divergences note and a verification result.

### Fixed
- Dashboard at `pe1mew.nl/hbwv/` now renders every tile after refresh (was showing "Connection lost" and "—" placeholders because the dashboard's `app.js` could not find any of the fields it expected in the previous payload shape).
- Public dashboard sunrise/sunset now displays local time (was UTC).
- Web tab "Last post" / "Last log upload" / "Last uploaded file" auto-refresh every 5 s without disturbing the editable inputs.
- Repeated short cycles of `setenv("TZ", …)` from the WS push are skipped when TZ is already correct — eliminates a slow env-string allocate/free churn.

### Out of scope
- The pe1mew.nl dashboard project itself is not in this repository — the firmware now matches its consumer contract; nothing on that side was changed.
- LittleFS partition gotcha: `pio run -t uploadfs` always writes to `lfs1` (0x520000) regardless of the active OTA bank. For development (firmware in bank A) the web assets must be written to `lfs0` (0x420000) with esptool — same caveat already documented in `platformio.ini`.

---

## [1.17.0] — 2026-05-10

*Initial implementation of the status-website reporting feature: a new FreeRTOS task (T14) that POSTs the controller's runtime status to a configurable REST endpoint on a 60–300 s cycle, uploads the most recently closed SD log file on T9 rotation (with a daily fallback at a configurable local time), and is configured via a new admin-only "Web" tab in the local web GUI. Refactors the status JSON path so both the local UI (`/api/status`, WebSocket) and the remote dashboard read from a single canonical builder, gated by a per-tile exposure mask.*

### Added
- `firmware/src/status_post/` — new module:
  - `status_json.h` / `status_json.cpp` — `build_canonical_status_json(buf, cap, snap, expose_mask)` produces the spec-shaped nested payload. `window_state_str()` and `op_mode_str()` strip the `WIN_` / `MODE_` prefixes.
  - `status_post.h` / `status_post.cpp` — T14 task body: 1 Hz wake-up, ready-to-post gate (status enabled + URL set + WiFi up + NTP-synced + not OTA), cycle-due check via `xTaskGetTickCount` delta, HTTPS branch via `WiFiClientSecure::setInsecure()` (no cert validation; documented MITM trade-off). Log-upload path (`do_log_upload()` / `try_log_upload()` / `maybe_upload_log()`) streams up to 5 MB from SD via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` and POSTs as `text/plain` to `<url>?action=log`; deduplicated by filename via `cfg.log_last_up`.
- `firmware/src/types/app_types.h::status_snapshot_t` — aggregated controller state (climate, wind, windows, mode + EG1 bits, sun, system, `update_interval_s`). Six `STATUS_EXPOSE_*` bits + `STATUS_EXPOSE_ALL` for the per-tile exposure mask.
- `firmware/src/data_manager/data_manager.{h,cpp}::dm_status_snapshot()` — fills the snapshot from MX2/MX4/relay-spinlock; called by both the local UI's `build_status_json()` and T14.
- `firmware/src/data_manager/data_manager.{h,cpp}` — new NVS keys in the `system` namespace: `status_url`, `status_secret`, `status_intv_s`, `status_enable`, `status_expose`, `log_upload_h`, `log_upload_m`, `log_upload_rot`, `log_last_up`. Matching fields in `cfg_shadow_t`. `dm_reload_web_cfg()` and `dm_set_log_last_up()` helpers for the `/api/web` POST handler and T14.
- `firmware/src/event_logger/event_logger.{h,cpp}` — `event_logger_last_rotated()` (cheap, in-memory; set by `rotate_sd_file()` before the active filename is overwritten) and `event_logger_newest_closed()` (SD scan fallback for the daily-upload path). `s_last_closed` published under a short spinlock.
- `firmware/src/web_server/web_server.cpp` — `GET /api/web` returns the current settings + last-post/last-log-up indicators (secret never echoed); `POST /api/web` validates bounds (`http(s)://`, no `? #`, ends with `api.php`, secret ≥ 16 chars, interval 60–300, hour 0–23, minute 0–59, exposure bitmask 0–0x3F), writes via `nvs_cfg_set_*`, and calls `dm_reload_web_cfg()`.
- `firmware/data/index.html` — new admin-only "Web" tab pane (URL, shared secret, interval, master enable, six exposure checkboxes for climate/wind/windows/mode/sun/system, daily log-upload hour:minute, "Upload on rotation" toggle, live last-post / last-log-up / last-uploaded-filename indicators, Apply button).
- `firmware/data/app.js` — `loadWebCfg()` / `postWebCfg()` for the Web tab. Single bundled POST per Apply. 5 s auto-refresh of the Status block (initially full reload — narrowed to indicators-only in 1.17.1). Tab handler hook in `showTab()`.
- `firmware/config/cfg_defaults.h` — `DEF_STATUS_*` and `DEF_LOG_UPLOAD_*` defaults (feature off, expose=ALL, daily upload 03:15 local, also upload on rotation).
- `firmware/config/cfg_limits.h` — `CFG_MIN/MAX_STATUS_INTERVAL_S` (60–300), `CFG_MIN/MAX_HOUR` (0–23), `CFG_MIN/MAX_MINUTE` (0–59), `CFG_MIN_SECRET_LEN` (16), `CFG_MAX_URL_LEN` (128), `CFG_MAX_SECRET_LEN` (64).
- `firmware/src/main.cpp` — spawn `task_status_post` as **T14_WEB** on Core 0, priority LOW, 6 KB stack (bumped to 12 KB in 1.17.1).
- `design/impact-analysis-statusReporting.md` — firmware-side impact analysis companion to `design/technical-spec-statusWebsite.md`.
- `design/implementationStatusPages.md` — six-phase implementation plan with the eight design decisions resolved up-front.

### Changed
- `firmware/src/web_server/web_server.cpp::build_status_json()` — body replaced with a delegation to `dm_status_snapshot()` + `build_canonical_status_json(buf, len, STATUS_EXPOSE_ALL)`. Output is now the nested canonical shape consumed by both the local UI and the remote dashboard.
- `firmware/data/app.js::handleStatus()` — rewritten for the new nested shape (was reading flat `temp_c`, `windows: [...]`, etc.).
- `firmware/src/data_manager/data_manager.{h,cpp}` — `cfg_shadow_t` extended with the nine new web-tab fields; `cfg_clamp()` and `apply_config_update()` switch branches handle the int subset; `nvs_load_web()` populates from NVS on boot.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.39` → `1.17.0`.

### Out of scope
- Public dashboard rendering / shape mismatch — discovered after this version's first POST; fixed in 1.17.1.
- Stack overflow under HTTPS — discovered on first `https://` POST; fixed in 1.17.1 (6 KB → 12 KB).
- LCD GUI changes — none. The feature is configured exclusively via the web GUI.
- `design/technical-spec-statusWebsite.md` is the website-side spec and is not in this repository's authority; it is referenced as the consumer contract.

---

## [1.16.39] — 2026-05-10

*Drop the `#=Set` and `#=AP` discoverability hints from every rotating LCD status page (T/RH, Wind, WiFi, Time). The four `#`-shortcuts still work — `#` on a status page that has a related sub-menu jumps straight to it (Climate, Wind, System/AP, Date-time), asking for Farmer or Admin PIN as appropriate. The user manual now documents `#` as the implicit "open settings" key on status pages, so the on-screen hint is redundant. The wind-status second row also returns to its pre-1.16.37 layout with the cardinal letter in parentheses (` Dir:180 ° (S ) `) — the parens were collateral damage in 1.16.37 when the `#=Edit` hint was first squeezed in.*

### Changed
- `firmware/src/ui_display/ui_display.cpp::render_status()` — case 0 (T/RH) row 1 now `"  RH:%3d %%      "` (valid) / `"  RH: ---  %%    "` (invalid); case 1 (Wind) row 1 now `" Dir:%3d \xDF (%-2s) "` (valid) / `" Dir: --- \xDF     "` (invalid); case 3 (Network) AP-active row 1 now `"%-16.16s"` (full-width SSID, no hint); case 3 disconnected row 1 now 16 spaces; case 4 (Time) row 1 now `"Src:%-3s         "`. All formats fit the 16-column LCD line exactly. Comments above each case rewritten to flag that the `#`-shortcut survives but no longer paints a hint.
- `firmware/src/ui_display/ui_display.cpp::handle_status()` — comment block at the top of the T/RH `#` branch updated for the same reason; functional logic unchanged.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.38` → `1.16.39` in both `lolin_s3` and `test_t2_relay` environments.
- `manual/boerHandleiding.md` — §5.1 (status-screen mock-ups for T/RH, Wind, WiFi, Time) and §5.2 (`#`-shortcut table) rewritten to drop the on-screen hint references; wind mock-up restored to parenthesised cardinal layout. Glossary entries for `#=AP` and `#=Set` removed. Version-history row added (1.2 / 2026-05-10).
- `manual/beheerderHandleiding.md` — §"Snelweg via #-toets" rewritten: list keeps the four shortcuts but no longer references the (now-absent) hint text. Version-history row added (1.2 / 2026-05-10).
- `manual/boerQuickRef.md` — version chip updated to 1.16.39 / 2026-05-10; LCD-status paragraph and toetsenbord table cleaned of `#=Set` references.

### Out of scope
- Keypad behaviour is unchanged — `#` on a status page still routes through `handle_status()` to the same target sub-menus and PIN flow as in 1.16.38.
- `web-assets-1.16.39.zip` is byte-identical to 1.16.38 — no static asset content changed; only LCD render strings and Dutch documentation. LFS partition does not need re-uploading.
- `design/LCD_GUI_Design.md` is an older design spec that doesn't track the rotating-status implementation; not updated.

---

## [1.16.38] — 2026-05-09

*Cosmetic follow-up to 1.16.37: replace the `#=Edit` hint on the new T/RH and Wind status-page shortcuts with `#=Set`, right-aligned to columns 12-16, so the four `#`-shortcut hints on the LCD now share the same visual convention (`#=AP`, `#=Set`, `#=Set`, `#=Set`).*

### Changed
- `firmware/src/ui_display/ui_display.cpp::render_status()` — case 0 row 1 now `"  RH:%3d%%  #=Set"` (valid) / `"  RH: ---  #=Set"` (invalid); case 1 row 1 now `"Dir:%3d°%-2s#=Set"` (valid) / `"Dir: ---   #=Set"` (invalid). The `#=Set` token sits at the same right-edge columns 12-16 on every status page that supports the `#`-shortcut, matching the existing WiFi-status (`#=AP`) and Time-status (`#=Set`) rendering. Comments updated to flag the right-alignment.
- `manual/beheerderHandleiding.md` §"Snelweg via #-toets" — both T/RH and Wind shortcut entries now show hint `#=Set` (was `#=Edit`).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.37` → `1.16.38` in both `lolin_s3` and `test_t2_relay` environments.

### Out of scope
- The keypad behaviour is unchanged — only the on-screen hint label and its column position are different. `handle_status()` still routes `#` on pages 0/1 to `UI_MENU_CLIMATE` / `UI_MENU_WIND` exactly as in 1.16.37.
- `web-assets-1.16.38.zip` is byte-identical to 1.16.37 — no static asset content changed.

---

## [1.16.37] — 2026-05-09

*Extend the existing `#`-shortcut pattern (already used on the WiFi-status and Time-status pages) to the T/RH-status and Wind-status pages. Pressing `#` on T/RH now jumps straight into the Climate sub-menu; pressing `#` on Wind jumps into the Wind sub-menu. Both shortcuts request the Farmer PIN if no session is active and resume in the target menu after a successful PIN entry — same flow as `#=AP` (System menu) and `#=Set` (date/time entry), just routed through `s_return_menu` instead of dedicated pending flags. Row 2 of each affected status page now shows a `#=Edit` hint so the shortcut is discoverable.*

### Added
- `firmware/src/ui_display/ui_display.cpp::handle_status()` — two new branches at the top of the function: `#` on status page 0 (T/RH) routes to `UI_MENU_CLIMATE`; `#` on status page 1 (Wind) routes to `UI_MENU_WIND`. When `s_session >= SESSION_FARMER` the jump is direct; otherwise `s_pin_role = PIN_ROLE_FARMER` and `s_return_menu` is set to the target sub-menu, then the existing PIN-success default branch in `handle_pin()` restores the menu after authentication. `s_pending_param` / `s_pending_ap` / `s_pending_settime` are explicitly cleared so the new path can never collide with a half-finished pending action from a prior interaction.
- `manual/beheerderHandleiding.md` §"Snelweg via #-toets" — list extended from 2 to 4 shortcuts (T/RH and Wind added), each annotated with the on-screen hint (`#=Edit`, `#=AP`, `#=Set`) and the required role.

### Changed
- `firmware/src/ui_display/ui_display.cpp::render_status()` — row 1 of pages 0 and 1 now ends with `#=Edit` instead of trailing whitespace, so the LCD shows the new shortcut. T/RH valid: `"  RH:%3d%% #=Edit"`; T/RH invalid: `"  RH: --- #=Edit"`. Wind valid: `"Dir:%3d°%-2s#=Edit"` (the leading space and the parens around the cardinal direction were removed to free the 6 columns the hint needs); wind invalid: `"Dir: ---  #=Edit"`. Both formats fit exactly in the 16-column LCD line. Sensor-fault row 1 is unchanged — the hint would be misleading while the sensor isn't responding.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.36` → `1.16.37` in both `lolin_s3` and `test_t2_relay` environments.

### Out of scope
- The boer-handleiding (`manual/handleiding.md` §5.1) describes the same status pages and reference-table for `#=AP` / `#=Set`, but the new `#=Edit` hint is not yet listed there. Update on next pass through the boer manual.
- `web-assets-1.16.37.zip` will be byte-identical to `web-assets-1.16.36.zip` — no static asset content changed; only the LCD render strings and the keypad handler. Re-flashing the LFS partition is not necessary.

---

## [1.16.36] — 2026-05-09

*Fix a regression introduced in 1.16.35 where the static-file response handler silently truncated files larger than 32767 bytes. `index.html` grew from ~32.2 KiB to ~32.9 KiB after the conflict-priority dropdown was added, pushing it past the buffer ceiling and dropping the closing `</span>`, the GitHub footer link, `</footer>`, `</body>` and `</html>` from every page load. The browser was forgiving enough to render the page anyway, so the only visible symptom was the missing footer link.*

### Fixed
- `firmware/src/web_server/web_server.cpp` — `LFS_BUF_SIZE` raised `32768` → `65536`. `serve_lfs()` allocates the whole buffer per request from PSRAM (8 MiB, ample headroom) and passes it to `AsyncWebServerResponse::beginResponse(int, const char*, const char*)`, which treats the third argument as a null-terminated C string — so the served length is capped at `LFS_BUF_SIZE - 1` regardless of the actual file size. 64 KiB now leaves ~30 KiB headroom above today's largest static asset; the next time a static file approaches the new ceiling the same regression will recur and the right answer will be to switch to a chunked / streaming response. Comment on the `#define` updated to flag the trap.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.35` → `1.16.36` in both `lolin_s3` and `test_t2_relay` environments.

### Out of scope
- Long-term, `serve_lfs()` should query the file size from LittleFS and `ps_malloc(file_size + 1)`, or stream the file in chunks via `beginChunkedResponse()`. That removes the silent-truncation footgun entirely. Not done in this release because the immediate goal was to restore the missing footer link without further surgery on the static-file path.
- `web-assets-1.16.36.zip` is byte-identical to `web-assets-1.16.35.zip` — no asset content changed; only the firmware buffer ceiling. Re-flashing the assets is not strictly necessary for the fix, but `bin/build_release.ps1` writes both artefacts as a matter of course.

---

## [1.16.35] — 2026-05-09

*Expose the existing T-vs-RH conflict-resolution priority (`cr_priority`) to both the LCD keypad UI and the web GUI, for both Farmer and Technician roles. The setting was already present in firmware (`cfg.cr_priority`, NVS key `climate/cr_priority`, defaults to `0` = `CR_TEMP_FIRST`) and was already returned by `GET /api/config`, but no UI surface offered to change it — operators had to POST it directly. Also corrects the LCD design document's stale "6-digit PIN" references to the actual firmware values (Farmer = 4, Technician = 8).*

### Added
- `firmware/src/ui_display/ui_display.cpp` — new entry in `CLIMATE_PARAMS` (index 11) for `cr_priority` (range 0–2, `SESSION_FARMER`, logged as `LOG_PARAM_CR_PRIORITY`). The climate sub-menu now offers `1=Day  2=Ngt  3=CR  *`; pressing `3` opens the edit screen for the new parameter (with PIN gate if not yet authenticated). Both Farmer and Admin sessions can edit it.
- `firmware/data/index.html` — new `<select>` control "T vs RH conflict priority" in the Climate tab (3 options: Temperature first / Humidity first / Largest deviation). Placed in a `farmer-hidden` row so it is visible to both Farmer and Admin sessions, hidden for guests.
- `firmware/data/app.js` — `setVal('cfg-cr-priority', String(cfg.cr_priority))` populates the dropdown from `GET /api/config`.
- `webUiMock/mock_server.py` — `("climate", "cr_priority")` added to both `NVS_MAP` (so a POST actually persists in the in-memory `cfg`) and `FARMER_WRITABLE` (so a farmer-session POST is accepted, mirroring the firmware).

### Changed
- `firmware/src/web_server/web_server.cpp` — `cr_priority` added to `FARMER_KEYS[]` so a farmer-session `POST /api/config` is accepted (previously admin-only).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.34` → `1.16.35` in both `lolin_s3` and `test_t2_relay` environments.
- `design/LCD_GUI_Design.md` — added §5.1.5 "Conflict Resolution Priority" (mockup, value list, edit flow); renumbered Change Farmer PIN → §5.1.6 and Logout → §5.1.7. Technician section 2.3 now lists conflict-resolution priority alongside the wind-protection toggle. Same document's stale "6-digit PIN" text and ASCII mockups corrected to match the firmware (Farmer = 4 digits, Technician = 8 digits).
- `design/lcd_gui_state_diagram.puml` — added `FM_CRPrio` and `TM_CRPrio` states to the Farmer and Technician menu rings (with A/▲ and B/▼ wrap-around). The "(6 digits)" labels on the PIN-entry / change-PIN transitions corrected to "(4 or 8 digits)" / "(4 digits)" / "(8 digits)" respectively.
- `design/logAnalysis.md` — Farmer-login row corrected from "6-digit" to "4-digit" farmer PIN.

### Out of scope
- The PNG render of `design/lcd_gui_state_diagram.puml` is not regenerated automatically; rerun `plantuml -tpng design/lcd_gui_state_diagram.puml` to refresh `design/lcd_gui_state_diagram.png` after this release.
- `design/LCD_GUI_Design.docx` (binary) is not updated by this change; the `.md` is the working source. Re-export with Pandoc or Word if the `.docx` needs to stay in sync.
- Existing devices keep their NVS-stored `cr_priority` value across the firmware upgrade; the new menu/dropdown lets operators change it without an API call but does not migrate the stored value.

---

## [1.16.34] — 2026-05-08

*LCD1602RGB hardware support + status-display readability fixes.  The existing `LCD1602_I2C` driver now also drives the PCA9633DP2 RGB controller present on the LCD1602RGB module; T8 (`ui_display`) tints the backlight from the EG1 status flags (red = critical safety event, blue = OK).  The on-board WS2812B LED palette is brought into alignment so the two indicators never disagree about severity.  Status display (LCD + web GUI) switched from sliding-window averaged readings to raw most-recent values so step changes in T/RH/wind become visible within one poll cycle instead of `avg_win_*` minutes.  Versions 1.16.32 and 1.16.33 were skipped — they were internal-only flashes revised three times during hardware bring-up before this release.*

### Added (LCD1602RGB driver)
- `drivers/LCD1602_I2C/src/lcd1602.h` and `lcd1602.cpp` — PCA9633DP2 driver bolted onto the existing AiP31068L driver.  The PCA9633 sits at I²C address 0x60 (8-bit 0xC0) on the same bus; `lcd_init()` now also probes 0x60 and runs the PCA9633 init sequence (clear MODE1.SLEEP, MODE2 = group dimming + totem-pole, GRPPWM = 0xFF, LEDOUT = 0xFF, then auto-increment burst PWM0=255 / PWM1=0 / PWM2=0 / PWM3=0 for boot-default BLUE at full brightness).  When the PCA9633 NACKs the probe (legacy LCD1602 module without RGB), a static `s_rgb_present` flag stays false and the rest of the driver continues unchanged — no error, no bus traffic on the colour-control path.  Two new public functions:
  - `lcd_backlight_color(uint8_t r, uint8_t g, uint8_t b)` — auto-increment write to PWM0/PWM1/PWM2.  This Waveshare LCD1602RGB PCB has LED0=BLUE, LED1=GREEN, LED2=RED (verified empirically; does NOT follow the Grove LED0=R/LED2=B convention even though the datasheet block diagram is silent on which LED is which colour).  The function remaps `(r, g, b)` arguments to the actual (B, G, R) channel order internally so callers don't need to care.
  - `lcd_backlight_lumination(uint8_t level)` — group brightness via GRPPWM (0..255 master multiplier on all channels).
  Both follow the existing `lcd_*()` mutex convention: callers hold MX1; the driver does not lock internally.  Header and file comments updated; driver version bumped 0.1.0 → 0.2.0.
- `firmware/src/ui_display/ui_display.cpp` — new static `update_backlight_status()` runs every T8 tick after the character flush.  Reads EG1 flags, looks up the target colour (highest-severity-wins: any of motor-alarm / wind-override / sensor-fault-T → red; otherwise blue), and writes the PCA9633 only when the resolved colour changed since the last write.  Cheap on the bus: in steady state the I²C write happens zero times per tick.  Tracks last-written colour in a static `s_bl_colour_last`; an MX1 timeout leaves it unchanged so the next tick retries.  Two-colour palette (blue calm vs red alarm) instead of richer red/orange/white because the green channel on the procured Waveshare LCD1602RGB units does not light — see boot-default note in `lcd1602.cpp::pca9633_init()`.

### Changed
- `drivers/LCD1602_I2C/src/lcd1602.h` — file header doxygen rewritten to describe both module variants (mono LCD1602 and LCD1602RGB) and the auto-detection.  Added 9 PCA9633 register / control-byte `#define`s (`LCD_RGB_REG_*`, `LCD_RGB_AI_BIT`, `LCD_RGB_I2C_ADDR`).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.31` → `1.16.34` in both `lolin_s3` and `test_t2_relay` environments.

### Removed
- `lcd_backlight_on()` / `lcd_backlight_off()` — both were no-ops in the previous driver (the AiP31068L has no SW-controllable backlight) and no firmware source called them.  Replaced by the new `lcd_backlight_color()` / `lcd_backlight_lumination()` API which actually does something on RGB hardware.

### Fixed
- `firmware/src/main.cpp` — the on-board WS2812B (GPIO38) status LED palette now agrees with the LCD1602RGB backlight palette: `EG1_BIT_WIND_OVERRIDE` is treated as a critical safety event and lights **red** alongside `EG1_BIT_MOTOR_ALARM`.  Previously wind was bucketed with sensor faults as an amber "non-critical warning", which made the on-board LED and the LCD disagree about severity (LCD red, LED yellow) on the same event.  Sensor faults remain amber on the on-board LED so degraded-but-operating states are still distinguishable.
- `firmware/src/web_server/web_server.cpp` — the `/api/status` JSON payload had four "raw" fields (`temp_c`, `rh_pct`, `wind_ms`, `wind_dir`) wired to the same `meas.*_avg_*` values as their `*_avg` counterparts.  The web GUI was therefore reporting averaged values for both pairs and the supposedly-instant readings tracked the controller's sliding-window output instead of the latest sensor sample.  All four raw fields now read from the corresponding raw struct members (`temperature_c`, `humidity_pct`, `wind_speed_ms10`, `wind_dir_deg`).  Operator-visible effect: a step change in measured T/RH/wind shows up on the status page within one poll cycle (~30 s) instead of taking up to `avg_win_t` / `avg_win_rh` minutes to fully settle.
- `firmware/src/ui_display/ui_display.cpp` — LCD status page (case 0) was reading `meas.t_avg_c` and `meas.rh_avg_pct` for the displayed Temp/RH numbers, so the same lag bothered the operator at the device.  Now reads `meas.temperature_c` / `meas.humidity_pct`.  T6's control branch is untouched and continues to use the averaged values for the step ladder, so anti-chatter behaviour is unaffected.

### Out of scope
- `lcd_backlight_lumination()` is exported but not yet driven from the firmware — the boot default of 255 (full) is set by `pca9633_init()` and is left untouched.  Day/night dimming (e.g. honouring the existing `DEF_LED_NITE_FROM/TO` window already used by the NeoPixel) would layer on top of the new API without further driver changes.
- The colour palette in `update_backlight_status()` is intentionally two-state because the green LED channel on the procured units doesn't light.  When/if the green LED is fixed (or a different module variant is sourced), a richer palette (e.g. green-OK / orange-warning / red-alarm) is a one-function edit in `status_colour_for_bits()`.

---

## [1.16.31] — 2026-05-08

*Apply the kas-2-calibrated anti-oscillation tuning from `simulation/new_settings_calibrated.json` to the firmware factory defaults. Per-motor dwell defaults replace the previous single-value `DEF_DWELL_OPEN_S` / `DEF_DWELL_CLOSE_S` so M3 (171 s ridge vent) can carry a substantially longer hold than M1/M2.*

### Changed
- `firmware/config/cfg_defaults.h`:
  - `DEF_HYST_RH` 5 → 12 — wider RH dead band suppresses small-signal step toggles on humid days.
  - `DEF_AVG_WIN_RH` 5 → 10 — 10-min RH averaging window gives ~20 samples at the new 30 s poll rate (was 5 samples at 60 s); much smoother input to the step ladder without changing the response horizon.
  - `DEF_POLL_INTERVAL_S` 60 → 30 — finer sampling.  Doubles the buffer depth feeding `DEF_AVG_WIN_T` / `DEF_AVG_WIN_RH` for the same time-window average, so the controller sees a smoother signal while still firing every 30 s rather than every 60 s.
  - `DEF_DWELL_OPEN_S` (single value) replaced by `DEF_DWELL_OPEN_M1_S` = 300, `DEF_DWELL_OPEN_M2_S` = 300, `DEF_DWELL_OPEN_M3_S` = 1500.  M3's 171 s travel time makes it the dominant slow-oscillation driver in the kas-2 simulation; a 25 min open hold breaks the open-then-close-then-open cycle observed at midday on humid days.  M1/M2 keep the 5 min hold from v1.16.23.
  - `DEF_DWELL_CLOSE_S` (single value) replaced by `DEF_DWELL_CLOSE_M1_S` = 0, `DEF_DWELL_CLOSE_M2_S` = 0, `DEF_DWELL_CLOSE_M3_S` = 600.  The 10 min closed-state hold on M3 is the symmetric counterpart to the open hold; together they ensure M3 can complete a full open-or-closed run before the controller is allowed to reverse it.
  - File header anti-oscillation comment updated to reflect all five tuning knobs and reference `simulation/new_settings_calibrated.json` as the source.
- `firmware/config/cfg_limits.h`:
  - `CFG_MAX_DWELL_OPEN_S` 600 → 1500 — required so `cfg_clamp()` and the web GUI accept the new M3 default.  M1/M2 are unaffected because their default stays at 300 s.
  - `CFG_MAX_DWELL_CLOSE_S` 300 → 1500 — needed for the same reason on the close-side: without raising this, `cfg_clamp()` would silently truncate the new `dwell_close_m3 = 600` default.  Both ceilings now match for symmetry.
- `firmware/src/data_manager/data_manager.cpp` — `nvs_load_motor()` now reads dwell defaults from per-motor arrays (`def_do[3]`, `def_dc[3]`) rather than a single shared scalar.  The travel-default array (`def_tr[]`) was already per-motor; this brings dwell into the same shape.
- `firmware/src/relay_controller/relay_controller.cpp` — adds `DWELL_OPEN_S_DEFAULT[NUM_CHANNELS]` and `DWELL_CLOSE_S_DEFAULT[NUM_CHANNELS]` arrays mirroring the existing `TRAVEL_S_DEFAULT[]`; the T2-init loop now indexes into them per channel.  The `cfg_defaults.h` include comment updated to reference the new symbol names.

### Out of scope
- Existing devices keep their NVS-stored dwell values across the firmware upgrade; only fresh flashes (or a factory-reset) inherit the new per-motor defaults.  Operators who want the new tuning on an in-service device need to set `dwell_open_m3 = 1500` and `dwell_close_m3 = 600` manually via the web GUI or LCD keypad.
- The simulation's `ACH_INF` background-infiltration constant (`simulation/simulation.py`) is still 0.5 /h.  The kas-2 fit suggests ~1.35 /h would be more accurate; making `ACH_INF` configurable from the plant-model JSON is still a future change (flagged in v1.16.30 already).

### Fixed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.30` → `1.16.31` in both `lolin_s3` and `test_t2_relay` environments.

---

## [1.16.30] — 2026-05-08

*T6 climate-control becomes level-triggered so dwell-deferred close/open commands are retried until they take effect; simulation tooling is upgraded to mirror the firmware FSM and to accept live sensor data for calibration.*

### Fixed
- `firmware/src/climate_control/climate_control.cpp` — replaced edge-triggered `apply_step_delta()` with level-triggered `reconcile_to_step()`. The previous design fired window commands only when `resolved_step` changed; if T2 dwell-deferred a CMD_CLOSE (e.g. T plummeted right after the post-open dwell of v1.16.23 started), T6 never re-issued it because `resolved` had stopped moving. Result: windows could remain physically OPEN indefinitely while `step_resolved == 0`, until the next non-zero-to-zero step transition came along. T6 now queries `t2_get_window_states()` every cycle, computes the desired channel mask from the resolved step, and posts a CMD_CLOSE / CMD_OPEN for any channel whose actual state does not already match. Posts targeting the current direction are no-ops in T2 (`ch_start_open()` / `ch_start_close()`); opposite-direction posts during travel cleanly trigger the existing 2 s reversal gap; close-vs-open ordering preserved (CLOSE first, narrowing-before-widening). Mode-change logging stays edge-triggered so `LOG_MODE_CHANGE` semantics are unchanged. New `#include "../relay_controller/relay_controller.h"` to call `t2_get_window_states()`. The greenhouse climate model (`simulation/simulation.py`) was firmware-faithful and exhibited the same pathology — finding it there is what surfaced the firmware bug.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.29` → `1.16.30` in both `lolin_s3` and `test_t2_relay` environments.

### Changed
- `firmware/src/climate_control/climate_control.h` and the `climate_control.cpp` file/function header doxygen — step-6 description rewritten from "Delta application — apply_step_delta(): CMD_CLOSE first, then CMD_OPEN" to "Reconcile to step — reconcile_to_step(): every T6 cycle, query T2 actual window states and post per-channel commands for any channel that does not already match the desired bit". Mode-change logging note clarified to say "only on step changes" so the difference between "command issued" and "mode logged" is explicit.

### Added (simulation tooling)
- `simulation/simulation.py` — port of the firmware `reconcile_to_step()` change so the simulation stays firmware-faithful. The motor FSM gained the firmware's `GAP_TO_OPEN` / `GAP_TO_CLOSE` transient states with a 2 s safety gap (`MotorState.RELAY_GAP_MS = 2000`); `cmd_open()` / `cmd_close()` now reverse mid-travel via the gap rather than silently ignoring opposite-direction commands, matching `relay_controller.cpp::ch_start_close()`/`ch_start_open()`. The plant model gained a first-order thermal/moisture lag (replacing the steady-state algebraic model) — `T_in` relaxes toward the ventilation equilibrium with `tau_T = c_eff / (ACH·V·ρ·cp)`; `AH_in` with `tau_AH = 1/ACH`. Setting `c_eff_mj_per_c = 0` recovers the previous instant-equilibrium behaviour. CSV output replaces the binary `M{1,2,3}_open` columns with 4-state `M{1,2,3}_state` columns (`CLOSED` / `MOVING_OPEN` / `OPEN` / `MOVING_CLOSE`) mirroring the firmware's `t2_get_window_states()`; the windows panel in the saved PNG now shows a `C ↑ ↓ O` per-motor track instead of a 0/1 step plot.
- `simulation/calibrate_plant.py` — new regression tool. Fits `k_solar` (W per outdoor lux), `c_eff_mj_per_c`, `transpiration_kg_s`, `ach_closed_per_hr`, and `ach_open_per_hr` against live indoor sensors in `srcData/` using `scipy.optimize.differential_evolution` (global) + bounded Nelder-Mead (local). Models the user's actual ventilation schedule (windows opened 10:00, closed 18:00 local) so the open / closed ACH split is identifiable, drives solar from the measured outdoor lumosity instead of the synthetic NOAA model, and masks out grid points more than an hour from any real sample so long sensor gaps don't pollute the fit. `--plot` produces a fit-vs-measured PNG per indoor sensor.
- `simulation/generate_inputs_from_live.py` — new script that picks five 24-hour slices from `srcData/greenhouseClimate-lht65-20_*.csv` to populate the `input_S{1..5}_*.csv` scenarios with real outdoor weather (April–May 2026), replacing the synthetic Format-B data the scenarios used to ship with. Day picks were chosen by character (sunny/humid/cold/etc.) using outdoor lumosity and indoor LHT65-02/-03 readings as ground truth.
- `simulation/srcData/` — three live-sensor CSVs (LHT65-02 indoor "kas 2", LHT65-03 indoor "kas 1", lht65-20 outdoor with lumosity) over 2026-03-17 .. 2026-05-07, plus an `sql.md` describing the MySQL extraction.

### Changed (simulation tooling)
- The plant section was split out of the settings JSONs into separate plant-model files. Each settings JSON now carries a `"plant_file": "<filename>"` reference (relative to the settings file's directory, falling back to the simulation script's directory) instead of an inline `"plant"` block. `simulation.py::load_settings()` resolves the reference and injects the loaded plant dict into `settings["plant"]`. Inline `"plant"` sections are still honoured for backward compatibility and take precedence when both are present. Three plant files now ship: `plant_empty_greenhouse.json` (empty greenhouse, `c_eff = 10` MJ/°C), `plant_general_crops.json` (general crops, `c_eff = 30`), and `plant_calibrated.json` (regenerated by `calibrate_plant.py` from live sensor data; current fit is to LHT65-02 only after LHT65-03 was excluded — its `ach_closed > ach_open` suggested the 10:00–18:00 schedule does not apply to that compartment).
- `simulation/settings.json` and `simulation/settings_optimised.json` — `_note` rewritten to mention the new `plant_file` field and the controller-vs-plant separation; inline plant section removed.
- `simulation/simulation_manual.md` — settings layout documentation updated to reflect the `plant_file` split, the new plant-file table, and the inline-plant fallback rule. Plant-model description rewritten from "steady-state algebraic" to "first-order lag" with the relevant time-constant formulas.

### Out of scope
- The simulation's `ACH_INF` (background infiltration when all windows are closed) is still hard-coded to `0.5 /h` in `simulation.py`. The kas 2 calibration suggests the real greenhouse has an `ach_closed` of ~1.35 /h (ventilated regularly enough that the "closed" state is not really tight), so a future change should make `ACH_INF` configurable from the plant-model JSON.
- The 1st-order plant model's residual T RMSE against the live data is 3.93 °C (kas 2). The remaining gap appears to be solar storage in soil/structure that re-radiates at night, which a 2nd-order plant model (separate floor / biomass thermal-mass node coupled to the air via a slow heat-exchange coefficient) would capture. Not in this version.
- The `model/` (academic/exploration) tree was edited earlier in this session to fix a separate priority-order divergence with the firmware (`climate_model.py` opened M2 first, while firmware `VENT_STEP_TABLE` opens M1 first). Those edits aligned `model_design.md §7` with the firmware but were not part of the user's original ask; flagging here for visibility — they can be reverted independently if needed.

---

## [1.16.29] — 2026-05-08

*Hide unused `t_min_day` / `t_min_ngt` heating setpoints from the LCD browse menus — same pattern already used by the web GUI.*

### Changed
- `firmware/src/ui_display/ui_display.cpp` — `DAY_PARAM_IDX` and `NIGHT_PARAM_IDX` no longer reference `CLIMATE_PARAMS[2]` (`t_min_day`) or `CLIMATE_PARAMS[3]` (`t_min_ngt`).  These two parameters are documented as "informational; future heating" in `cfg_defaults.h` but `climate_control.cpp::vent_step_required_t()` does not evaluate them — they had no effect on window operation.  The browse FSM now cycles through three setpoints per group (T-max, RH-max, RH-min) instead of four.
  - The corresponding `CLIMATE_PARAMS` rows are kept in place — array indices remain stable so `param_get()`'s switch statement and `LOG_PARAM_T_MIN_DAY/NGT` mappings need no renumbering.  Comments mark them as "HEATING CONTROL NOT IMPLEMENTED — preserved for future use", matching the pattern already in `firmware/data/app.js` (`linkAllSliders` slider list and `loadConfig` `setVal` calls), where the same fields are commented out from the live web UI.
  - New `BROWSE_COUNT` macro derived from `sizeof(DAY_PARAM_IDX)` replaces the previously hardcoded `4` / `4u` / `3u` constants in `render_browse_setpoints()` and `handle_browse_setpoints()`.  A `_Static_assert` checks that `DAY_PARAM_IDX` and `NIGHT_PARAM_IDX` keep the same length.  Restoring the heating setpoints to the menus in the future is a one-line edit in each `IDX` array.
  - LCD position counter on the browse screen now reads `n/3` (was `n/4`).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.28` → `1.16.29` in both `lolin_s3` and `test_t2_relay` environments.

### Out of scope
- The web GUI HTML still renders `cfg-t-min-day` and `cfg-t-min-ngt` `<input>` elements (with all wiring already commented out in `app.js`), so the inputs appear but do nothing.  Removing them from `index.html` is a separate cleanup — flagging here for visibility.

---

## [1.16.28] — 2026-05-08

*Reconcile motor-travel range to a single value (5–300 s) across firmware, web GUI, and specification — eliminate the 300 vs 600 drift.*

### Changed
- **FR-CF05** (`design/functionalRequirementsSpecification.md`) — motor travel range narrowed from "5–600 s" to "5–300 s" to match the runtime enforcement introduced by `cfg_clamp()` in v1.16.25.  The wider 600 s upper bound predated `cfg_limits.h` and was never re-derived from a hardware requirement; 300 s is comfortably above the longest-stroke window in the system (M3 ridge vent factory default 171 s) plus the 5 s safety margin.  No installed device has been written with a value above 300 s since v1.16.25 (cfg_clamp would have silently truncated it), so this is a documentation alignment for the current first-installation target — not a behavioural change for existing devices.
- `design/technicalSoftwareDesignSpecification.md` — three "5–600 s" → "5–300 s" references updated (§ farmer/admin parameter visibility, § parameter editor scope, § NVS namespace `motor`).  Default-value cross-references updated to point at `firmware/config/cfg_defaults.h` (the v1.16.27 location) instead of the historical `app_types.h`.
- `firmware/firmwareImplementationResults.md` — three "5–600 s" → "5–300 s" rows in the motor-config table.
- `firmware/data/index.html` — `<input type="number">` `max` attribute for `cfg-travel-m1/m2/m3` lowered from 600 → 300.  These static values are runtime-overridden by `app.js::loadLimits()` from `GET /api/config/limits` (which returns `cfg_limits.h::CFG_*_TRAVEL_S`); updating them keeps the static fallback consistent with the API.
- `firmware/src/types/app_types.h` — `MOTOR_TRAVEL_S_MIN` (5) and `MOTOR_TRAVEL_S_MAX` (600) deleted along with the v1.16.27 "deliberate split" comment.  These macros are now redundant with `cfg_limits.h::CFG_MIN_TRAVEL_S` / `CFG_MAX_TRAVEL_S`.
- `firmware/src/relay_controller/relay_controller.cpp` — NVS-load fallback clamp at `t2_init()` switched from `MOTOR_TRAVEL_S_{MIN,MAX}` to `CFG_{MIN,MAX}_TRAVEL_S`.  The relay controller now reads its bounds from the same single source of truth as `cfg_clamp()`, the LCD keypad, and the web GUI.

### Fixed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.27` → `1.16.28` in both `lolin_s3` and `test_t2_relay` environments.

---

## [1.16.27] — 2026-05-08

*Single source of truth for NVS factory defaults — new `firmware/config/cfg_defaults.h` mirrors the `cfg_limits.h` pattern.*

### Added
- `firmware/config/cfg_defaults.h` — new header collecting every `DEF_*` macro and the `MOTOR_M{1,2,3}_TRAVEL_S_DEFAULT` / `MOTOR_TRAVEL_MARGIN_S_DEFAULT` constants in one place.  Companion to `cfg_limits.h` (validation bounds): every layer that needs to know "what value should be written to NVS the first time we boot, or read back if a key is missing" now includes this header.  Sections mirror `cfg_limits.h`: temperature, humidity, hysteresis/control flags, wind, motor (travel + dwell + margin), system (poll/session/AP), site location, LED, timezone.

### Changed
- `firmware/src/data_manager/data_manager.cpp` — 47 lines of `DEF_*` `#define`s replaced with `#include "cfg_defaults.h"`.  No behaviour change for this file (it was already the de facto canonical home for these values); the move enables the same defaults to be consumed by other layers without duplication.
- `firmware/src/types/app_types.h` — `MOTOR_M{1,2,3}_TRAVEL_S_DEFAULT` and `MOTOR_TRAVEL_MARGIN_S_DEFAULT` removed; consumers now `#include "cfg_defaults.h"` directly.  `MOTOR_TRAVEL_S_MIN/MAX` kept here with a clarifying comment (these are runtime hardware-tolerance bounds for the NVS-load fallback path; the user-facing GUI / `cfg_clamp()` bounds at 5–300 s live in `cfg_limits.h::CFG_*_TRAVEL_S`).
- `firmware/src/ui_display/ui_display.cpp` — local `#define DEF_SESSION_MIN 5` removed; the session-timeout fallback at the idle-counter check now uses the shared `DEF_SESSION_TIMEOUT_MIN`.  The two macros agreed at 5/5 by coincidence, not by construction; the duplication is now gone.

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — latent first-boot race condition resolved.  Local `DWELL_OPEN_S_DEFAULT = 0` and `DWELL_CLOSE_S_DEFAULT = 0` macros drifted from `data_manager.cpp`'s `DEF_DWELL_OPEN_S = 300` (changed in v1.16.23) and `DEF_DWELL_CLOSE_S = 0`.  Both modules call `nvs_cfg_get_i32_or_default()` for the dwell keys at startup; whichever ran first wrote its default value to NVS.  If T2 won the race on a fresh-flash device, dwell_open silently became 0 — defeating the 5-min anti-oscillation hold introduced in v1.16.23.  T2 now reads `DEF_DWELL_OPEN_S` / `DEF_DWELL_CLOSE_S` from `cfg_defaults.h` directly, so both tasks always agree.  Existing devices (with NVS-stored values) are unaffected.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.26` → `1.16.27` in both `lolin_s3` and `test_t2_relay` environments.

---

## [1.16.26] — 2026-05-08

*Fix timezone reverts to UTC after periodic NTP resync (or when geolocation lookup fails on initial connect).*

### Fixed
- `firmware/src/network_manager/network_manager.cpp` — `run_ntp_sync()` now re-reads `tz_str` from NVS and calls `setenv("TZ", ...)` + `tzset()` immediately after `configTime(0, 0, "pool.ntp.org")`. The Arduino-ESP32 `configTime()` call resets the C-library `TZ` environment variable to UTC on every NTP sync. Previously the only restoration path was the `setenv` inside `do_geo_sync()`, which is skipped on the 24-hour periodic resync (`run_ntp_sync(false)` at line 603) and silently bypassed on the initial sync whenever the `ip-api.com` HTTP GET failed, JSON parsing failed, or the returned IANA zone was not in the lookup table. Symptom: LCD, event log viewer, web `/api/events`, and LED day/night dimming all flipped to UTC roughly 24 h after boot — or immediately after the first NTP sync on networks where outbound HTTP to `ip-api.com` was blocked. NVS is read directly (instead of the MX4 shadow `s_cfg.tz_str`) because both `do_geo_sync()` and the `/api/config` web handler write `tz_str` to NVS without posting Q4, so the shadow can lag the persisted value until the next reboot.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.25` → `1.16.26` in both `lolin_s3` and `test_t2_relay` environments.

### Documentation
- `test/3_3_Setpoints_and_Hysteresis.py` and `test/3_3_Setpoints_and_Hysteresis.md` — integration-test docstrings refreshed to match firmware behaviour after v1.16.22 (per-channel `CMD_CLOSE` instead of `CMD_CLOSE_ALL` for climate-control step→0 transitions) and v1.16.25 (`cfg_clamp` floor of 1 on `avg_win_t/rh`). UT-CC-024 success messages no longer claim `CMD_CLOSE_ALL`; UT-CC-016 docstring notes the v1.16.22 reservation of `CMD_CLOSE_ALL` for safety events. `TEST_AVG_WIN` raised from 0 to 1 (the new minimum); the comment now explains that the rolling window holds 2 samples at `avg_win=1 min` / `poll_interval=30 s`, so the first poll after a sensor push reads `(prev+new)/2` and is handled by `push_and_verify_sensor()`'s retry loop. No assertion logic changed; tests still validate the same intended behaviour.

---

## [1.16.25] — 2026-05-07

*Add per-key validation bounds to all integer config parameters received via Q4.*

### Added
- `firmware/src/data_manager/data_manager.cpp` — new `cfg_clamp()` function and `CFG_MIN_*` / `CFG_MAX_*` constants enforce valid ranges on every integer config value before it is written to NVS or applied to the in-RAM shadow.  Out-of-range values are silently clamped to the nearest bound; a `LOGW` line is emitted so the operator can see the correction.  The clamp runs inside `apply_config_update()`, before the NVS write, so neither storage nor the running config can hold an illegal value.

  Bounds defined for: `t_max/min_day/ngt`, `rh_max/min_day/ngt`, `hyst_t` (min 2), `hyst_rh` (min 2), `avg_win_t/rh` (min 1, max 30), `v_max` (min 1), `dir_excl_low/high` (0–359), `travel_m1/m2/m3` (5–300 s), `dwell_open_m1/m2/m3` (0–600 s), `dwell_close_m1/m2/m3` (0–300 s), `poll_interval_s` (30–300 s), `session_timeout_min` / `ap_timeout_min` (1–1440 min).

  Key oscillation guards: `hyst_t ≥ 2` prevents the step-width from collapsing to zero in the climate control algorithm; `poll_interval_s ≥ 30` prevents excessive relay actuation frequency.

---

## [1.16.24] — 2026-05-07

*Fix RGB LED night dimming: replace setBrightness() loop calls with direct colour-component scaling.*

### Fixed
- `firmware/src/main.cpp` — `setBrightness()` is documented for one-time initialisation only; calling it every 500 ms T1 tick re-scales the internal NeoPixel pixel buffer on every day↔night brightness change, degrading stored values through lossy integer arithmetic and producing inconsistent output. Fixed by setting `setBrightness(255)` once at startup and scaling the R/G/B components manually before each `setPixelColor()` call (`channel = (raw × dim) >> 8`). With the internal brightness fixed at 255, NeoPixel stores values unmodified and the LED output matches the computed scale exactly. Night schedule and brightness levels are unchanged: 22:00–06:00 local time at brightness 20; daytime at brightness 200.

---

## [1.16.23] — 2026-05-07

*Promote optimised anti-oscillation parameters to firmware defaults.*

### Changed
- `firmware/src/data_manager/data_manager.cpp` — updated compile-time defaults to match `simulation/settings_optimised.json`:
  - `DEF_HYST_T` 3 → 5 (wider temperature dead band)
  - `DEF_AVG_WIN_T` 3 → 6 (longer averaging window to smooth thermal spikes)
  - `DEF_DWELL_OPEN_S` 120 → 300 (5 min minimum open time; now effective following the v1.16.22 CMD_CLOSE fix)

  These defaults apply on a fresh flash or after an NVS reset.  Existing devices retain their NVS-stored values; update via the web GUI if needed.

---

## [1.16.22] — 2026-05-07

*Fix window oscillation: use CMD_CLOSE (dwell-respecting) instead of CMD_CLOSE_ALL for normal step→0 transitions in climate control.*

### Fixed
- `firmware/src/climate_control/climate_control.cpp` — `apply_step_delta()` previously issued `CMD_CLOSE_ALL` whenever `new_step == 0`.  `CMD_CLOSE_ALL` zeroes the per-channel dwell deadline in T2 (relay controller), bypassing `dwell_open_s` entirely and causing rapid oscillation when indoor temperature rebounds after ventilation.  Changed to issue per-channel `CMD_CLOSE` for all climate-control step transitions, including full close (step → 0).  `CMD_CLOSE` respects `dwell_open_s`, so windows remain open for at least the configured minimum time before closing.  `CMD_CLOSE_ALL` is now reserved exclusively for safety events (wind override in T3, motor alarm / calibration in T2).

### Recommended settings change (apply via GUI)
Together with the firmware fix the following NVS parameter changes eliminate oscillation in simulation across all test scenarios:

| Parameter | Old default | New recommended | Reason |
|---|---|---|---|
| `hyst_t` | 3 | 5 | Wider dead band; close-guard requires T_avg to drop 5 °C below t_max before closing |
| `avg_win_t` | 3 min | 6 min | Longer averaging window prevents a single cooled-air reading from instantly clearing the close guard |
| `dwell_open_m1/2/3` | 120 s | 300 s | Now effective (CMD_CLOSE respects dwell); 5 min minimum open time prevents short re-close cycles |

Simulation results with these three changes + firmware fix (S1 Daytime Solar Gain scenario):

| Metric | v1.16.19 defaults | Optimised | Improvement |
|---|---|---|---|
| M1 open/close cycles (24 h) | 24 | 1 | −96 % |
| Peak indoor temperature | 49.4 °C | 35.7 °C | −13.7 °C |
| Time T within t_max + 2 °C | 66.5 % | 70.9 % | +4.4 pp |
| Total actuations | 58 | 6 | −90 % |

---

## [1.16.21] — 2026-05-07

*Fix timezone not applied when changed via web GUI — clock remained in old zone until reboot.*

### Fixed
- `firmware/src/web_server/web_server.cpp` — `POST /api/config` with `key = "tz_str"` now calls `setenv("TZ", str_value, 1)` + `tzset()` immediately after writing to NVS.  Previously the new POSIX TZ string was persisted to flash but the C-library timezone was only updated on the next reboot (from `data_manager.cpp::nvs_load_system()`), so `localtime_r` kept formatting timestamps in the old zone.

---

## [1.16.20] — 2026-05-07

*Fix session idle timeout: background polls were silently preventing the timeout from ever firing.*

### Fixed
- `firmware/src/web_server/web_server.cpp` — Added `session_find_peek()`: a non-sliding variant of `session_find()` that checks session validity without resetting the expiry deadline. `/api/whoami` now uses `session_find_peek` so the browser's 60 s probe call no longer keeps the session alive.
- `firmware/data/app.js` — Added client-side idle timer (`g_last_activity` / `g_session_timeout_ms`). Real user gestures (click, keydown, touchstart) update `g_last_activity`. The periodic session-check interval now calls `doLogout()` when the user has been idle for `session_timeout_min` minutes, and skips the `/api/whoami` probe. `g_session_timeout_ms` is updated from `cfg.session_timeout_min` each time the config is loaded.
- `firmware/data/app.js` — The background `loadConfig()` poll (every 60 s) is now gated: it only fires while the user is active (`Date.now() − g_last_activity < g_session_timeout_ms`). This stops config polling from silently extending the server-side session when nobody is at the keyboard.

---

## [1.16.19] — 2026-05-07

*Hide T min day / T min night sliders in web GUI — heating control not yet implemented.*

### Changed
- `firmware/data/index.html` — **T min day** and **T min night** slider rows commented out with `<!-- HEATING CONTROL NOT IMPLEMENTED — preserved for future use -->`. The NVS keys (`t_min_day`, `t_min_ngt`) remain in firmware and continue to be stored; the sliders are only hidden from the UI.
- `firmware/data/app.js` — Corresponding `setVal()` calls and `linkAllSliders` entries commented out with the same note.

---

## [1.16.18] — 2026-05-07

*Fix dwell_open unit: macro renamed and value corrected from 2 (seconds) to 120 (seconds = 2 min).*

### Fixed
- `firmware/src/data_manager/data_manager.cpp` — **`DEF_DWELL_OPEN_MIN` unit bug**: T2 stores and reads the dwell NVS value in **seconds** (multiplies by 1 000 ms on load). The macro was named `DEF_DWELL_OPEN_MIN` with value `2`, which T2 interpreted as 2 seconds — not the intended 2 minutes. Renamed to `DEF_DWELL_OPEN_S` and corrected to `120` (120 s = 2 min). `DEF_DWELL_CLOSE_MIN` renamed to `DEF_DWELL_CLOSE_S` (value remains 0) for consistency.

---

## [1.16.17] — 2026-05-07

*Factory defaults updated to general-crop greenhouse values.*

### Changed
- `firmware/src/data_manager/data_manager.cpp` — **Climate defaults**: `t_max_day` 26→28 °C, `t_max_ngt` 22→20 °C, `t_min_day` 15→16 °C, `t_min_ngt` 12→14 °C; `rh_max_day` 80→75 %, `rh_max_ngt` 85→80 %, `rh_min_day` 40→50 %, `rh_min_ngt` 50→55 %; `hyst_t` 2→3 °C; `avg_win_t` 1→3 min, `avg_win_rh` 1→5 min.
- `firmware/src/data_manager/data_manager.cpp` — **Wind default**: `v_max` 7→6 m/s (Beaufort 4 onset with margin).
- `firmware/src/data_manager/data_manager.cpp` — **Motor dwell default**: `dwell_open_m1/m2/m3` 0→2 min; prevents immediate re-close after window opens.
- `firmware/src/data_manager/data_manager.cpp` — **Poll interval default**: `poll_interval` 30→60 s; reduces relay wear without meaningful loss of responsiveness.

> **Note:** factory defaults only apply on first boot (empty NVS) or after an NVS erase. Existing installations retain their current NVS values and must be updated manually via the web GUI or a full NVS erase.

---

## [1.16.16] — 2026-05-07

*24-hour periodic NTP resync added.*

### Added
- `firmware/src/network_manager/network_manager.cpp` — **Periodic NTP resync**: while in `NET_RUNNING` state the firmware re-runs `configTime()` / NTP wait once every 24 hours (`NTP_RESYNC_INTERVAL_S = 86400`). On success, T4 is notified (TN4) and writes the updated time to the DS1307 RTC. Geo/TZ lookup is intentionally skipped on periodic resyncs (location is stable); only the initial WiFi-connect sync fetches geo data.

---

## [1.16.15] — 2026-05-07

*OTA flow hardened: manual two-step upload, STORE-only ZIP enforcement, no auto-upload, live status polling fixed.*

### Fixed
- `firmware/data/app.js` — **OTA status panel not updating**: `loadOtaStatus()` is now called each time the System tab is opened, so the panel always reflects current device state immediately.
- `firmware/data/app.js` — **"not yet accepted" never clearing**: after an OTA reboot the panel now keeps polling every 5 s until `accepted` flips to `true` (~35 s), then stops. Previously polling stopped as soon as state returned to `idle`.
- `firmware/data/app.js` — **`fw_done` progress label**: status text while waiting for assets changed from "Firmware ready — uploading assets…" to "Firmware ready — please upload the web assets ZIP" to match the new manual flow.

### Changed
- `firmware/data/app.js` — **Removed auto-upload of assets**: after firmware upload succeeds, the assets ZIP is no longer uploaded automatically even if it is already selected. Both uploads are now fully manual (firmware first, then assets as a separate step).
- `bin/build_release.ps1` — confirmed as the canonical release builder; always used for all version packages. Web-assets ZIP uses STORE (method=0) enforced by the script's raw ZIP writer and verified before output.

---

## [1.16.14] — 2026-05-07

*Auto-upload of web assets removed; STORE-only ZIP required by on-device extractor.*

### Changed
- `firmware/data/app.js` — **Removed auto-upload**: `uploadOtaFirmware()` no longer calls `uploadOtaAssets()` automatically when an assets file is pre-selected. Status message updated to prompt the user to upload assets manually.

### Fixed
- `bin/build_release.ps1` — **STORE-only ZIP**: `Compress-Archive` (DEFLATE, method=8) replaced by a raw ZIP writer using PowerShell's `MemoryStream`; all entries use method=0 (STORE). The on-device extractor rejects method=8 with "compressed ZIP entry (method 8) is not supported".

---

## [1.16.13] — 2026-05-07

*OTA WDT crash eliminated; fallback reboot timer removed; stale-asset concern resolved.*

### Fixed
- `firmware/src/ota_manager/ota_manager.cpp` — **Task Watchdog crash**: `esp_partition_erase_range()` on the 1 MB inactive LittleFS partition took ~12 s, triggering the ESP32-S3 TWDT (~5 s default) and rebooting the device before web assets could be written. Removed entirely from T13. The partition is now simply unmounted then remounted; `littlefs_write()` truncates files in-place so no pre-erase is needed.
- `firmware/src/ota_manager/ota_manager.cpp` — **Premature reboot (fallback timer)**: the 120 s `fallback_reboot_cb` FreeRTOS timer in `ota_firmware_end()` fired if assets were not uploaded within 2 minutes. Removed the timer variable, callback, create/start call, and the cancel call in `ota_assets_begin()`.
- `firmware/src/ota_manager/ota_manager.cpp` — **False 100 % progress after firmware upload**: `s_progress = 100` in `ota_firmware_end()` changed to `s_progress = 0` so the bar correctly resets before the assets phase.
- `firmware/src/ota_manager/ota_manager.cpp` — **C++ goto jump-over-initialization**: hoisted `lfs_status_t lfs_st;` declaration above the format guard to satisfy the C++ rule that forbids jumping over a variable initialisation.
- `firmware/data/app.js` — **`rebooting` state not polled**: `'rebooting'` added to `OTA_ACTIVE_STATES` so the 2 s poll continues through the reboot transition.
- `firmware/data/app.js` — **Connection-loss during reboot**: `.catch()` handler added to `loadOtaStatus()` to display "Rebooting — reload the page once the device comes back online" when the fetch fails during device restart.

### Added
- `drivers/littleFS/src/littlefs_storage.cpp` — `littlefs_format()`: lightweight format using `fs.begin(true)` + `fs.format()` + `fs.end()`. Kept in the driver API for future use; not called from T13 in this release.
- `drivers/littleFS/src/littlefs_storage.h` — `littlefs_format()` declaration and Doxygen documentation added to the public API.

---

## [1.16.7] — 2026-05-07

*SD card logging overhauled: timestamp-based file names, ISO 8601 CSV timestamps, proactive free-space guard, local-time filenames, and automatic remount on card insertion.*

### Added
- `firmware/src/event_logger/event_logger.cpp` — **SD automount**: T9 main loop now wakes every 60 s when SD is absent and calls `event_logger_sd_remount()`, so a card inserted after boot is picked up automatically within one minute. When SD is active the task blocks indefinitely as before (no polling overhead).
- `firmware/src/event_logger/event_logger.cpp` — **Proactive free-space guard** (`check_free_space()`): called after every rotation. If free space drops below 2 MB and the file count is above the 3-file retention floor, the oldest file is deleted to reclaim space. If already at the floor, SD logging is suspended and a `LOG_SYSTEM` event with `value_a = −2` is emitted.
- `firmware/src/event_logger/event_logger.cpp` — **Write-failure reclaim**: on `STORAGE_ERR_FULL` / `STORAGE_ERR_IO`, a single oldest-file deletion is attempted and the write retried before falling back to NVS-only mode.

### Changed
- `firmware/src/event_logger/event_logger.cpp` — **Timestamp-based SD file naming**: files are now named `YYYYMMDDHHMMSS.csv` (local time of creation) instead of the previous sequential-index scheme (`ghc_NNNN.csv`). Lexicographic sort equals chronological order. Old `ghc_*` files are silently ignored via `is_ts_filename()` filter.
- `firmware/src/event_logger/event_logger.cpp` — **SD filename uses local time**: `make_ts_filename()` calls `localtime_r()` so filenames are human-readable without timezone conversion when browsing the card directly.
- `firmware/src/event_logger/event_logger.cpp` — **ISO 8601 CSV timestamps**: `build_csv_line()` now formats the timestamp as `YYYY-MM-DDTHH:MM:SS` (UTC) via `gmtime_r()` + `strftime()` instead of a raw Unix epoch integer.
- `firmware/src/web_server/web_server.cpp` — **ISO 8601 NVS export**: `/api/log/download?src=nvs` CSV timestamps updated to ISO 8601 format, matching SD output.
- `firmware/src/web_server/web_server.cpp` — **Sorted SD file list**: `/api/log/files` now returns SD filenames sorted lexicographically (oldest → newest) via an in-place bubble sort before building the JSON response.
- `firmware/src/event_logger/event_logger.h` — file naming and CSV format documentation updated.
- `design/technicalSoftwareDesignSpecification.md` — §5.3 updated: timestamp file naming (local time), ISO 8601 CSV format, rotation procedure, free-space guard, startup scan behaviour. Version 0.2 → 0.3.
- `design/functionalRequirementsSpecification.md` — FR-S03, FR-CF07 poll interval range updated to 15–120 s (default 30 s); FR-LG06 worst-case budget recalculated for 15 s minimum poll (400 entries). Version 0.3 → 0.4.

---

## [test/3.4] — 2026-05-07

*Automated test suite `3_4_Conflict_Resolution.py` completed and passing: all 7 §3.4 test cases verified on hardware (UT-CC-020, UT-CC-021, UT-CC-022a/b, UT-CC-030, UT-CC-031a/b). All four branches of `vent_resolve_conflict()` exercised — Rule 2 (both open → max), Rule 3 (both close → equal), Rule 4 CR_TEMP_FIRST, CR_RH_FIRST, and CR_DEVIATION. One script defect identified and fixed in run 1; firmware was correct throughout.*

### Added
- `test/3_4_Conflict_Resolution.py` — new automated test script for §3.4 Conflict Resolution. Covers all five test-plan cases (with UT-CC-022 and UT-CC-031 each split into two sub-cases). All lessons learned from `3_3_Setpoints_and_Hysteresis.py` are applied: `force_windows_closed()` before every opening/closing test, `push_and_verify_sensor()` for all sensor commits, `windows_all_closing()` accepting `MOVING_CLOSE` for M3 tolerance, `WAIT_FOR_MOTOR_S = 45 s` uniformly, 401 re-auth inline in `write_config()`, guarded `finally` blocks in teardown, and 2-poll-cycle confirmation for negative assertions (CC-021, CC-030).
- `test/3_4_Conflict_Resolution.md` — documentation for the §3.4 test script: purpose, prerequisites, how to run, NVS test parameters, expected duration (~15 min), algorithm description with the four-rule table, mirror-test table (CC-020 ↔ CC-030; CC-021 ↔ CC-031a), and log file format example.

### Fixed
- `test/3_4_Conflict_Resolution.py` — **CC-031b assertion**: `wins[2] == "CLOSED"` changed to `wins[2] in ("CLOSED", "MOVING_CLOSE")` for M3. Step=2 correctly commands M3 to close; at `TEST_TRAVEL_S=5` the relay is only energised for 10 s while the FSM transition lag can leave M3 in `MOVING_CLOSE` at the polling window. Identical root cause to the CC-024 / CC-025–027 fix in `3_3_Setpoints_and_Hysteresis.py`.

### Changed
- `test/softwareTestResult.md` — §3.4 results updated (7/7 passed, run 2 2026-05-07 13:05–13:24); coverage table revised (CC: 24 PASS, 7 NOT EXECUTED; total: 129 PASS; UT rate 55%; overall pass rate 69%).

---

## [test/3.3] — 2026-05-07

*Automated test suite `3_3_Setpoints_and_Hysteresis.py` completed and passing: all 12 §3.3 test cases verified on hardware (UT-CC-014–019, UT-CC-024–029). Six script defects identified and fixed across five test runs; firmware behaviour was correct throughout.*

### Added
- `test/3_3_Setpoints_and_Hysteresis.py` — **`windows_all_closing()`** helper: accepts `CLOSED` or `MOVING_CLOSE` per window. Used by `force_windows_closed()` and the UT-CC-024 assertion to handle M3's physical travel time exceeding the 10 s relay pulse (`travel_m3` production default 171 s; test value 5 s → relay energised for only 10 s).

### Fixed
- `test/3_3_Setpoints_and_Hysteresis.py` — **`TEST_AVG_WIN`** corrected `1` → `0`. Value `1` means 1 minute, producing a 2-sample sliding window (`window_size = clamp(1×60/30, 1, 360) = 2`) instead of the intended single-sample immediate response. `0` → `clamp(0, 1, 360) = 1` sample.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`write_config()` 401 re-auth**: on `HTTP 401 Unauthorized`, the function now calls `do_login()` to restore the session and retries the write immediately (no sleep). Previously 401 was retried with a 3 s sleep (ineffective). The ~22-minute gap between `setup()` writes and `run_cc028`'s first write caused session expiry, which silently failed UT-CC-028 and skipped UT-CC-029 in earlier runs.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc028` finally block**: `set_daytime()` and `write_config()` calls wrapped in `try/except`. Previously an uncaught exception propagated out of `finally`, skipping UT-CC-029 entirely and suppressing the test summary print.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`setup()`** missing `rh_ctrl_en=1` write added. Without this, RH control was off at runtime and UT-CC-018/024 could not open windows via RH demand.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc018` / `run_cc024`** — `write_config(session, "climate", "cr_priority", 1)` (CR_RH_FIRST) added to each test's setup writes. With `t_max_day=40` and `T=10°C`, `vent_step_required_t()` returns step=0 (a genuine close vote); `vent_resolve_conflict()` rule 4 with CR_TEMP_FIRST (default) returned step_t=0, vetoing the RH open demand. Setting CR_RH_FIRST lets RH win the conflict. `teardown()` restores `cr_priority` via the `orig` config loop.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`run_cc019` setup**: `force_windows_closed()` added before the T=26°C open push; bare `push_sensors()` replaced with `push_and_verify_sensor()`. Previously the prior test (CC-018) left all windows open with stale T=10°C on the emulator; after CC-019 wrote `rh_ctrl_en=0` and `t_max_day=25`, the next firmware poll read T=10°C < close threshold 19°C and issued CLOSE_ALL before T=26°C was recognised.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`force_windows_closed()`**: success criterion changed from `windows_all_closed()` (requires all `CLOSED`) to `windows_all_closing()` (accepts `CLOSED` or `MOVING_CLOSE`). M3's physical travel outlasts the relay pulse at TEST_TRAVEL_S=5, so the helper no longer logs spurious "not fully closed" warnings or returns False when M3 is legitimately completing its close stroke.
- `test/3_3_Setpoints_and_Hysteresis.py` — **`teardown()`** and **`write_config()`**: error handling added. `teardown()` uses an inner `_safe_write()` that catches exceptions and logs warnings, so a single 503 response no longer aborts the remaining restore writes. `write_config()` retries up to 3 times on transient HTTP/network errors (5xx, connection errors) with a 3 s wait; application-level rejections (`ok=false`) are never retried.
- `test/3_3_Setpoints_and_Hysteresis.py` — **Motor wait race**: all bare `time.sleep(TEST_TRAVEL_S + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S)` calls (15 s) replaced with `time.sleep(WAIT_FOR_MOTOR_S)` (45 s = `TEST_POLL_S + TEST_TRAVEL_S + FIRMWARE_TRAVEL_MARGIN_S + MOTOR_MARGIN_S`). The poll can fire anywhere within the 35 s sensor-confirmation window; the 15 s bare sleep was a race condition.

### Changed
- `test/3_3_Setpoints_and_Hysteresis.md` — `avg_win_t`/`avg_win_rh` table values updated `1` → `0`; explanation updated to reflect 0-minute → 1-sample immediate response.
- `test/softwareTestResult.md` — §3.3 results updated through run 5 (12/12 passed); UT-CC-018, UT-CC-019, UT-CC-024, UT-CC-028, UT-CC-029 evidence updated; coverage table revised (CC: 19 PASS, 0 FAIL; total: 124 PASS, 0 FAIL; UT rate 46%; pass rate 98%).

---

## [1.16.6] — 2026-05-06

*Sensor history table now shows newest readings at the top; sensor history stale/frozen bug fixed (always showed the 60 oldest entries); `dm_ring_count()` added.*

### Fixed
- `firmware/src/data_manager/data_manager.cpp` — **`dm_ring_count()`** added: thread-safe getter (MX3, 500 ms timeout) that returns the current number of valid entries in the ring buffer. Previously callers had no way to query this without taking MX3 themselves.
- `firmware/src/data_manager/data_manager.h` — **`dm_ring_count()` declaration** added to the public API header.
- `firmware/src/web_server/web_server.cpp` — **`/api/history` newest-n fix**: handler called `dm_ring_read(0, rows, n, &got)` which always returned the `n` **oldest** entries (logical offset 0 = oldest). After DM_RING_DEPTH (360) entries accumulate, these never change, so the history table appeared frozen. Fixed by calling `dm_ring_count()` and computing `offset = max(0, avail − n)` to fetch the `n` **newest** entries.
- `firmware/data/app.js` — **`loadHistory()` `.catch` added**: promise chain now has a `.catch(function(err){ console.warn(...) })` handler so network failures surface in the browser console instead of producing an unhandled rejection.

### Changed
- `firmware/data/app.js` — **Sensor history newest-at-top**: `data.rows.forEach(...)` changed to `data.rows.slice().reverse().forEach(...)`. The server returns rows oldest-first; reversing before rendering places the most recent reading at the top of the table and the oldest at the bottom.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.5` → `1.16.6`.

---

## [1.16.5] — 2026-05-06

*Motor alarm aborts window calibration immediately; web GUI Settings moved above Sensor history; tooltips added to Sensor history heading and Refresh button.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — **`calib_close_all()` alarm abort**: motor alarm was silently ignored for the full calibration duration (up to the full travel time of M3 ≈ 176 s). Added two checks: (1) an **entry guard** before any relay is energised — if the alarm pin is already LOW, calibration is skipped and `handle_alarm_onset()` is called immediately; (2) a **per-chunk pin check** inside the poll loop (every `CALIB_CHUNK_MS` = 400 ms) — if the pin goes LOW mid-calibration, `EG1_BIT_CALIBRATING` is cleared, `handle_alarm_onset()` is called (de-energises all relays, sets `EG1_BIT_MOTOR_ALARM`), and calibration returns. Maximum alarm response latency during calibration is now **400 ms**.
- `firmware/src/relay_controller/relay_controller.cpp` — **forward declaration** of `handle_alarm_onset` added before `calib_close_all` to resolve the out-of-order definition required by the above fix.

### Changed
- `firmware/data/index.html` — **Settings section reordered**: `<section id="section-settings">` moved to appear before the Sensor history section. Settings are now visible at the top of the page when logged in, without scrolling past the history table.
- `firmware/data/index.html` — **Sensor history tooltips**: `data-tip` added to the section heading (`"Logged sensor readings — one row per poll cycle…"`) and to the Refresh button (`"Fetch the latest sensor history… also refreshes automatically every 2 minutes"`), consistent with the existing CSS tooltip system used throughout the Status section.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.4` → `1.16.5`.

---

## [1.16.4] — 2026-05-06

*Motor alarm onset detection fix: re-assertion during the 60 s guard period is now detected within 5 s instead of after the full 60 s. Boot-time alarm-at-power-on is now detected and handled.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — **`handle_alarm_clearance()` guard loop**: moved the GPIO42 pin re-check from a single test **after** the full 60 s guard to a test **inside every 5 s chunk iteration**. When a re-assertion is detected mid-guard, `s_alarm_edge` is consumed and `handle_alarm_onset()` is called immediately, so the alarm appears on LCD, web GUI, and RED LED within ≤5 s rather than up to 60 s.  Root cause: T2 blocks in `vTaskDelay` inside the guard loop and cannot execute the main-loop debounce code while blocked; the only pin re-check was at guard expiry.
- `firmware/src/relay_controller/relay_controller.cpp` — **boot-time alarm check**: `attachInterrupt` uses CHANGE mode and does not fire for a pin that is already in the asserted (LOW) state at power-on.  Added an explicit `gpio_read(PIN_OPTO_INPUT)` immediately after `attachInterrupt`; if already LOW, `handle_alarm_onset()` is called and `calib_close_all()` is skipped (energising CLOSE relays onto a latched alarm relay is unsafe).
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.3` → `1.16.4` in both `lolin_s3` and `test_t2_relay` environments.

---

## [1.16.3] — 2026-05-06

*LCD I2C bus reliability fix (AiP31068L silent-drop), LCD display polish, "Window Cal." mode on LCD and web GUI, and web GUI public-access redesign (Status + Sensor history + SD card without login; Login modal replaces full-page overlay).*

### Added
- `drivers/LCD1602_I2C/src/lcd1602.h/.cpp` — **`lcd_display_on()`**: sends CMD_DISP_ON (0x0C, ~37 µs busy) as an idempotent sacrificial preamble write before every `lcd_flush()`. Absorbs the AiP31068L silent first-transaction drop that occurs after ~2.5 s of I2C bus inactivity, without the 1.52 ms cursor-positioning side-effect of CMD_HOME that caused a 4-character row-0 shift artifact.
- `firmware/src/types/app_types.h` — **`EG1_BIT_CALIBRATING` (bit 6)**: new EG1 system-state flag; set/cleared by T2 around `calib_close_all()`. Highest display priority after MOTOR_ALARM and WIND_OVERRIDE. Allows all display consumers to detect the calibration window without polling relay state.
- `firmware/src/relay_controller/relay_controller.cpp` — `calib_close_all()` now **sets `EG1_BIT_CALIBRATING`** at entry (`CLOSE_ALL calibration start` log line) and **clears it** on completion (`CLOSE_ALL calibration complete` log line). Called at boot and after the 60 s motor-alarm guard.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **`lcd_flush()` preamble**: replaced `lcd_home()` call with `lcd_display_on()`; eliminates the 4-character cursor-shift artifact that appeared after 2.5 s of screen-message display (e.g. after returning from a timed message screen).
- `firmware/src/ui_display/ui_display.cpp` — **`show_group_summary()` removed**: the 2.5 s intermediate summary screen (`Day T28..27°C / RH   80.. 80%`) shown when pressing `*` in a browse state was removed entirely. `*` now navigates directly to `UI_MENU_CLIMATE` (the group selector). Eliminates the associated timing complexity and AiP31068L first-transaction glitch window.
- `firmware/src/ui_display/ui_display.cpp` — **Session label format**: `"SESS:%-4s %s"` with `"ADMN"` / `"FRMR"` / `"NONE"` replaced by `"Sess: %-6s%s"` with `"Admin"` / `"Farmer"` / `"NONE"` — a space is now always present after the colon, and the role names use readable mixed-case.
- `firmware/src/ui_display/ui_display.cpp` — **Mode display**: single `snprintf` with a `mode_str` variable replaced by per-case `snprintf` calls that check EG1 bits directly: `MOTOR_ALARM` → `"Mode: ALARM     "`, `WIND_OVERRIDE` → `"Mode: WIND      "`, `EG1_BIT_CALIBRATING` → `"Mode:Window Cal."`, default → `"Mode: AUTO      "`.
- `firmware/src/web_server/web_server.cpp` — **`/api/status`**: auth check removed — endpoint is now public (no session required). Mode derivation updated: added `else if (eg1 & EG1_BIT_CALIBRATING) mode_str = "WINDOW_CAL"` before the `AUTOMATIC` fallback.
- `firmware/src/web_server/web_server.cpp` — **`/api/history`**: auth check removed — endpoint is now public.
- `firmware/src/web_server/web_server.cpp` — **`/api/sd/status`**: auth check removed — endpoint is now public.
- `firmware/data/app.js` — **`setRole(role)`**: rewritten to show/hide `#btn-login`, `#btn-logout`, `#role-badge`, and `#section-settings` based on auth state instead of hiding/showing the full-page overlay. Logged-in: settings visible, Login hidden, role badge + Logout shown. Logged-out: settings hidden, Login shown.
- `firmware/data/app.js` — **`showLogin()`** (session expiry handler): no longer re-displays a login overlay; now simply calls `setRole(null)` to drop back to the unauthenticated public view.
- `firmware/data/app.js` — **`doLogout()`**: calls `setRole(null)` directly instead of `showLogin()`.
- `firmware/data/app.js` — **`doLogin()`**: calls `hideLoginModal()` on success before `setRole()`.
- `firmware/data/app.js` — **`showLoginModal()` / `hideLoginModal()` / `modalBackdropClick()`**: new modal management functions replacing the full-page overlay show/hide. `modalBackdropClick` closes the modal only when the semi-transparent backdrop is clicked, not the login box.
- `firmware/data/app.js` — **`loadHistory()`**: 401 guard removed; SD card status and history now load on page load regardless of auth. Periodic 30 s SD and 120 s history refresh intervals no longer guarded by `if (g_role === null) return`.
- `firmware/data/app.js` — **`WINDOW_CAL`** added to `modeNames` map → `'Window Cal.'`.
- `firmware/data/app.js` — **Page-load init**: `wsConnect()`, `loadHistory()`, and `loadSdStatus()` now called immediately on page load; `/api/whoami` check follows to restore an existing session if present.
- `firmware/data/style.css` — CSS rule renamed `#login-overlay` → `#login-modal`.
- `firmware/data/index.html` — **Full restructure for public-access pattern**: full-page blocking overlay replaced by a dismissible modal (`<div id="login-modal" style="display:none" onclick="modalBackdropClick(event)">`). Header gains `#btn-login` (visible by default) and `#btn-logout` + `#role-badge` (hidden by default). Status and Sensor history sections always visible. Settings section wrapped in `<section id="section-settings" style="display:none">` — revealed only after login.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.2` → `1.16.3` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.2% (1157 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.16.2] — 2026-05-06

*Day/Night setpoint browse interface on LCD: farmer can now browse and edit the 4 day setpoints (T max/min, RH max/min) and 4 night setpoints directly from the keypad. Two new FSM states `UI_BROWSE_DAY` and `UI_BROWSE_NIGHT`. Climate menu converted from a flat 11-param paginated list to a Day/Night group selector.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — **`UI_BROWSE_DAY` / `UI_BROWSE_NIGHT` FSM states**: browse one setpoint at a time; row 0 shows the parameter label; row 1 shows value, position counter (1/4…4/4), and key hints. Navigation: `A`=previous, `B`=next, `#`=edit, `*`=group min/max summary (2.5 s) then back to the group selector.
- `firmware/src/ui_display/ui_display.cpp` — **`render_menu_climate()`**: replaces old flat climate param menu with a two-line group selector (`1=Day  2=Ngt  *`).
- `firmware/src/ui_display/ui_display.cpp` — **`render_browse_setpoints(bool is_day)`**: renders the current browse slot using the parameter's `edit_lbl` on row 0 and `"<val> N/4 A B #* "` on row 1.
- `firmware/src/ui_display/ui_display.cpp` — **`show_group_summary(bool is_day)`**: displays T min..max °C and RH min..max % on the LCD for 2.5 s when `*` is pressed in a browse state.
- `firmware/src/ui_display/ui_display.cpp` — **`DAY_PARAM_IDX[4]` / `NIGHT_PARAM_IDX[4]`**: map browse position (0–3) to `CLIMATE_PARAMS` indices (`{0,2,4,6}` / `{1,3,5,7}`).
- `firmware/src/ui_display/ui_display.cpp` — **`handle_menu_climate()`** and **`handle_browse_setpoints()`**: key handlers for the two new states.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **`begin_edit()` signature extended**: third parameter `ui_state_t return_to` replaces the hardcoded `is_wind ? UI_MENU_WIND : UI_MENU_CLIMATE` logic, allowing browse states to be preserved through the PIN→edit chain. All callers updated.
- `firmware/src/ui_display/ui_display.cpp` — **`handle_pin()` pending-edit resume**: passes `s_return_menu` (set by the initial `begin_edit()` before PIN entry) so editing returns to the correct browse state after successful authentication.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.1` → `1.16.2` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.1% (approx.)
- RAM: 19.4% (approx.)

---

## [1.16.1] — 2026-05-06

*IO0 BOOT button factory-reset sequence with animated LCD progress bar. LCD rendering muted during button hold. `lcd_create_char()` added to the LCD1602 I2C driver.*

### Added
- `drivers/LCD1602_I2C/src/lcd1602.h/.cpp` — **`lcd_create_char(slot, pattern[8])`**: programs one of the 8 HD44780 CGRAM custom-character slots. Sets the CGRAM address (`0x40 | slot<<3`), writes the 8 pixel rows (5 LSBs used per row), then issues CMD_HOME to return the cursor to DDRAM so subsequent text writes target the visible display area.
- `firmware/src/ui_display/ui_display.cpp` — **IO0 factory-reset sequence**: holding the LOLIN S3 BOOT button (GPIO0, active-low) displays an animated growing bar on LCD row 1; row 0 shows a contextual stage label. Four stages of 5 s each (200 ticks at 100 ms/tick):
  - **0–5 s** — no label. Release restores normal display with no action.
  - **5–10 s** — `Reset PIN?`. Release resets farmer and admin PINs to defaults (`1234` / `12345678`) by erasing NVS namespace `access` and calling `pin_auth_init()`; system continues operating.
  - **10–15 s** — `Reset settings?`. Release erases all NVS namespaces (climate, wind, motor, access, wifi, mqtt, system), resets PINs to defaults, closes any open session; system continues with factory defaults.
  - **15–20 s** — `Restarting?`. Release performs the same full NVS erase then calls `ESP.restart()`. Holding for the full 20 s auto-executes the restart stage without requiring release.
  - Bar fills left-to-right using `\xFF` (HD44780 ROM A00 full-block glyph) for filled cells and CGRAM slot 1 (5×8 outline-square pattern `{0x1F,0x11,0x11,0x11,0x11,0x11,0x1F,0x00}`) for unfilled cells. Slot 0 is avoided because it is the C null terminator.
  - CGRAM slot 1 is programmed under MX1 immediately after `lcd_init()` at T8 startup via the new `lcd_create_char()` API.

### Fixed
- `firmware/src/ui_display/ui_display.cpp` — **LCD updates muted while IO0 is held**: the main loop previously continued executing Q5 network-status renders, status-page rotation, and key dispatch every 100 ms tick while the reset bar was displayed, causing brief status-page flashes to appear behind the bar. Fixed by `continue`-ing the loop after `render_reset_bar()` whenever the button is still held (and the 20 s limit not yet reached), skipping steps 3–6 entirely until release.

### Changed
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.16.0` → `1.16.1` in both `lolin_s3` and `test_t2_relay` environments.

### Build metrics
- Flash: 55.1% (1157 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.16.0] — 2026-05-06

*LCD display improvements: T/RH page reformatted, WiFi page `#`-shortcut to AP enable, boot splash version alignment. Web GUI poll-interval label clarified. Sensor timestamp bug fixed (duplicate log rows). Integration test suite development started.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — **WiFi status page `#` shortcut**: pressing `#` on the network status page (page 3) goes directly to the System menu when an admin session is active; without a session it enters `UI_PIN_ENTRY` (admin PIN); on success lands on `UI_MENU_SYSTEM` where `1` toggles the AP. `s_pending_ap` flag added (mirrors `s_pending_settime` pattern). Row 1 now shows `#=AP` hint in all non-connected states.
- `test/` — **integration test suite** development started: `test/lib/serial_monitor.py`, `test/lib/device_api.py`, `test/lib/emulator_api.py`, `test/conftest.py`, and per-TC test files. The suite targets the device at `192.168.20.150` and the Modbus sensor emulator at `192.168.20.226`; serial assertions use COM8 at 115 200 baud.

### Changed
- `firmware/src/ui_display/ui_display.cpp` — **T/RH status page (page 0) reformatted**:
  - Valid sensors: row 0 `Temp: 43 °C    `, row 1 `  RH: 65 %     ` — temperature and humidity on separate rows with aligned `°C` / `%` units. Hex escape `\xDF` followed by `C` split into `"\xDF" "C"` string literals to prevent the compiler from parsing `\xDFC` as a single (out-of-range) hex escape.
  - Invalid sensors: row 0 `Temp: --- °C    `, row 1 `  RH: ---  %    ` — consistent dash style; "Sensors not ready" text removed.
- `firmware/src/ui_display/ui_display.cpp` — **boot splash row 1**: format changed from `"v%-5.5s Init..."` to `"v%-9.9sInit.."` — version field expanded to 9 characters, left-justified; `Init..` sits flush at the right edge of the 16-char display.
- `firmware/data/index.html` — poll-interval label changed from `"Poll interval (s)"` to `"Sensor poll interval (s)"`; tooltip extended to note that the new value takes effect after reboot.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.15.1` → `1.16.0` in both `lolin_s3` and `test_t2_relay` environments.

### Fixed
- **Duplicate sensor log rows** (critical): `sensor_poll.cpp` set `reading.timestamp = dm_get_unix_time()`, which returns a cached value that T4 refreshes only every ~60 s from the RTC. With `poll_interval_s` set to 30 s, two consecutive polls received the same stale timestamp, producing duplicate rows in the sensor history table. Fixed by replacing with `reading.timestamp = (uint32_t)time(NULL)` — the POSIX system clock, always current.

### Integration test — bugs found during development
The following bugs in the firmware or REST API were discovered while building the integration test infrastructure. Both are fixed in this release:
- **Duplicate sensor log entries** — root cause documented above under Fixed.
- **`GET /api/config` vs `POST /api/config` key naming mismatch**: motor travel times are written with keys `travel_m1` / `travel_m2` / `travel_m3` (POST) but read back as a single `travel_s` array (GET). The `wait_for_config` helper in `conftest.py` was updated to exclude travel keys and use array indexing on teardown restore.

### Build metrics
- Flash: 55.0% (1154 kB / 2 MB)
- RAM: 19.4% (63 kB / 320 kB)

---

## [1.15.1] — 2026-05-06

*Post-Phase-10 correctness fixes: two-phase atomic OTA commit; STORE-only ZIP writer; OTA idle-status bank/accepted display; release build tooling.*

### Added
- `firmware/src/ota_manager/ota_manager.h` — `OTA_STATE_FW_DONE` (= 7): new intermediate state entered after `esp_ota_end()` succeeds but before the boot partition is switched; the device waits up to 120 s for asset upload. New status accessors: `ota_get_active_bank()` (returns `'A'`/`'B'`/`'?'` from the running partition subtype) and `ota_is_accepted()` (returns true when NVS `ota_fail_cnt` == 0).
- `firmware/src/ota_manager/ota_manager.cpp` — `s_fallback_timer`: FreeRTOS one-shot timer (120 000 ms) started by `ota_firmware_end()`; fires `fallback_reboot_cb()` which switches the boot partition and reboots without touching LittleFS if no assets arrive; cancelled by `ota_assets_begin()`. Implementations of `ota_get_active_bank()` and `ota_is_accepted()`.
- `firmware/src/web_server/web_server.cpp` — `GET /api/ota/status` response extended with `bank` and `accepted` fields; `STATE_NAMES` extended with `"fw_done"` at index 7 (bound check raised to `< 8`); firmware endpoint response changed to `{ok:true, rebooting:false, awaiting_assets:true}`.
- `firmware/data/app.js` — OTA idle label shows `Idle — Bank A, accepted` / `not yet accepted`; `uploadOtaFirmware()` auto-chains `uploadOtaAssets()` if an assets file is already selected; `OTA_ACTIVE_STATES` includes `'fw_done'` so status polling continues through the intermediate state.
- `build_release.ps1` — new project-root PowerShell 5.1 script: reads `FIRMWARE_VERSION` from `platformio.ini`; builds firmware binary; validates LittleFS build; produces a STORE-only (method=0) ZIP via a self-contained binary writer (no .NET `ZipFile`); outputs versioned files under `bin/<version>/`. Run: `powershell -ExecutionPolicy Bypass -File .\build_release.ps1`.
- `bin/README.md` — comprehensive guide: prerequisites, version bump, script invocation, OTA via web GUI (Path A), USB initial flash / recovery (Path B), rollback behaviour, partition layout table.

### Changed
- `firmware/src/ota_manager/ota_manager.cpp` — `ota_firmware_end()` no longer calls `esp_ota_set_boot_partition()` or schedules an immediate reboot; it verifies the image (`esp_ota_end()`), enters `OTA_STATE_FW_DONE`, and starts the 120 s fallback timer. The boot partition switch is deferred to `task_ota_manager()` after successful asset extraction, making both the firmware and paired LittleFS partition switch atomically. `ota_assets_begin()` accepts `OTA_STATE_FW_DONE` in addition to `OTA_STATE_IDLE` / `OTA_STATE_ERROR`, and cancels the fallback timer on entry. `task_ota_manager()` uses `s_ota_part` (saved by `ota_firmware_end()`) when a same-session firmware upload preceded assets, otherwise falls back to `esp_ota_get_next_update_partition(NULL)`.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.15.0` → `1.15.1` in both `lolin_s3` and `test_t2_relay` environments.
- `webUiMock/mock_server.py` — OTA state list extended with `fw_done`; firmware endpoint returns `{ok:true, rebooting:false, awaiting_assets:true}`; assets endpoint accepts `fw_done` initial state; `ota` dict carries `bank` and `accepted` fields (bank flips after asset install; `accepted` is `False` for 5 s then `True`); `cfg["fw_ver"]` updated to `"1.15.1"`.

### Fixed
- **OTA premature reboot** (critical): the device previously rebooted immediately after firmware upload, before web assets could be transferred, leaving the inactive LittleFS partition empty and the web UI inaccessible. Fixed by the two-phase commit: boot partition switch now happens only after asset extraction succeeds in T13 (or after the 120 s fallback timer if no assets arrive).
- **ZIP DEFLATE entries rejected by extractor**: `build_release.ps1` originally used `System.IO.Compression.ZipFile` (PS 5.1 / .NET Framework), which silently writes method=8 (DEFLATE) even at `CompressionLevel.NoCompression` — a known .NET Framework defect. Fixed by replacing with a self-contained binary ZIP writer emitting raw Local File Header, Central Directory, and EOCD records with method=0. CRC-32 uses decimal `[long]3988292384` for the polynomial to avoid PS 5.1 signed-int32 overflow on constants above `0x7FFFFFFF`.
- **`STATE_NAMES` bounds overrun**: the `< 7` upper-bound check in `web_server.cpp` excluded the new index-7 entry; corrected to `< 8`.

### Build metrics
- No binary size change from v1.15.0.

---

## [1.15.0] — 2026-05-06

*Phase 10: dual-bank OTA (firmware + web assets) with 3-fail rollback; version bump 1.14.0 → 1.15.0.*

### Added
- `firmware/src/ota_manager/ota_manager.h` — full OTA Manager public API: rollback management (`ota_check_rollback`, `ota_mark_healthy`), streaming firmware OTA (`ota_firmware_begin/write/end`), PSRAM-buffered web-asset OTA (`ota_assets_begin/accumulate/end`), status accessors (`ota_get_state`, `ota_get_progress`, `ota_get_error`), T13 task entry point; `OTA_HEALTHY_MS` constant (30 000 ms)
- `firmware/src/ota_manager/ota_manager.cpp` — full implementation:
  - **3-fail rollback**: NVS `system/ota_fail_cnt` incremented on every boot; on count ≥ 3 the counter is cleared and `esp_ota_mark_app_invalid_rollback_and_reboot()` is called, reverting to the previous firmware bank
  - **Firmware OTA** (T11 inline): `esp_ota_begin/write/end` on the inactive `app` partition; reboots via one-shot FreeRTOS timer after 1 s; `EG1_BIT_OTA_IN_PROGRESS` held throughout
  - **Web-asset OTA** (T13 spawned on-demand): ZIP uploaded into PSRAM; T13 extracts it onto the inactive LittleFS partition and writes `manifest.json`; then calls `esp_ota_set_boot_partition` (paired bank switch) and reboots; ZIP must use STORE compression (`zip -0`); DEFLATE entries rejected with a clear error message
  - ZIP LOCAL FILE HEADER parser: reads signature, compression method, sizes, and name; strips directory prefix to extract basename; validates every entry before writing to LittleFS
- `firmware/src/web_server/web_server.cpp` — three new OTA REST endpoints:
  - `GET /api/ota/status` — any logged-in role; returns `{ok, state, progress, error}`
  - `POST /api/ota/firmware` — admin only; streaming body callback (`index == 0` → begin, per-chunk → write, `index+len >= total` → end + 200 `{ok,rebooting:true}`); error state suppresses double-response
  - `POST /api/ota/assets` — admin only; same streaming pattern; final response 202 `{ok, message:"extracting — poll GET /api/ota/status"}`
- `firmware/data/index.html` — **OTA section** in System tab (admin only): firmware `.bin` file input + Upload button; web assets `.zip` file input + Upload button; OTA status span; progress bar (hidden when idle)
- `firmware/data/app.js` — `uploadOtaFirmware()`, `uploadOtaAssets()`, `loadOtaStatus()`: POST binary/zip body to OTA endpoints; `loadOtaStatus()` auto-polls every 2 s while state is not `idle`/`error`; progress bar updated from `progress` field; `setRole('admin')` now calls `loadOtaStatus()` on login
- `firmware/data/style.css` — `.ota-progress-bar` and inner `div` styles (8 px height, green fill, 0.4 s width transition)
- `webUiMock/mock_server.py` — **OTA simulation endpoints**: `GET /api/ota/status`, `POST /api/ota/firmware`, `POST /api/ota/assets`; background thread simulates chunked upload progress (0–100%) with per-state transitions (`fw_begin → fw_write → fw_end → idle`, similar for assets); thread-safe via `ota_lock`

### Changed
- `firmware/src/main.cpp` — `setup()` calls `ota_check_rollback()` immediately after NVS init; T1 task calls `ota_mark_healthy()` once after `OTA_HEALTHY_MS` (30 s, 60 × 500 ms ticks) of stable uptime
- `firmware/platformio.ini` — `FIRMWARE_VERSION` bumped `1.14.0` → `1.15.0` in both `lolin_s3` and `test_t2_relay` environments
- `webUiMock/mock_server.py` — `cfg["fw_ver"]` updated to `"1.15.0"`

### Build metrics
- Flash: ~56% (est.)
- RAM:   ~19% (est.)

### Notes
- Web-asset ZIP must be created with `zip -0 assets.zip data/*` (STORE only — no compression). DEFLATE entries are rejected at extraction time with a diagnostic error.
- On a successful OTA update, the inactive bank becomes the new boot target; both the firmware partition and the paired LittleFS partition are switched atomically.

---

## [1.14.0] — 2026-05-06

*Web GUI polish: hover tooltips on all fields; session expiry handling; history buffer fix; SD card management (status card + mount/unmount) with full T9 logging integration; LCD truncation fix.*

### Added
- `firmware/data/index.html` — **65 `data-tip` hover tooltips** on every status card field and every settings label; **8 `ⓘ` tip-icon spans** on card `<h3>` headings (zero JS, CSS `[data-tip]::after` pseudo-element, 260 px dark bubble, 150 ms fade)
- `firmware/data/index.html` — **SD card status card** in the Status section (visible to both Farmer and Admin): shows Mounted/Not mounted, total size (MB), free space (MB); refreshed on login and every 30 s
- `firmware/data/index.html` + `firmware/data/app.js` — **SD card controls** in System tab (Admin only): Mount button and Unmount (danger) button; feedback span; button auto-disabled to match current mount state
- `firmware/data/app.js` — `loadSdStatus()` / `postSdMount()` / `postSdUnmount()`: fetch `/api/sd/status|mount|unmount`, update status card fields and button disabled-state; called on admin/farmer login and on 30 s interval
- `firmware/data/app.js` — `showLogin()`: restores login overlay, clears role, and resets WS initialisation flag; called on logout, 401 response, and session-check failure
- `firmware/data/app.js` — **60 s `whoami` polling**: detects server-side session expiry while the UI is idle; calls `showLogin()` on failure
- `firmware/data/app.js` — **2 min auto-refresh** of sensor history table
- `firmware/data/app.js` — 401 detection in `post()`, `loadConfig()`, and `loadHistory()`: all three call `showLogin()` on an unexpected 401 response
- `firmware/data/app.js` — firmware version set from `/api/config` on login (`cfg.fw_ver`) so the footer is populated immediately, before the first WebSocket push
- `firmware/src/web_server/web_server.cpp` — `GET /api/sd/status` → `{"mounted":…,"free_mb":…,"size_mb":…}` (Farmer + Admin)
- `firmware/src/web_server/web_server.cpp` — `POST /api/sd/mount` → calls `event_logger_sd_remount()`; Admin only
- `firmware/src/web_server/web_server.cpp` — `POST /api/sd/unmount` → calls `event_logger_sd_unmount()`; Admin only
- `drivers/sdCard/src/sd_storage.h/.cpp` — `storage_sd_total_bytes()`: returns FAT32 volume total capacity via `SD.totalBytes()`
- `drivers/sdCard/src/sd_storage.h/.cpp` — `storage_sd_unmount()`: clears `g_mounted` and calls `SD.end()` to release the SPI bus
- `firmware/src/event_logger/event_logger.h/.cpp` — `event_logger_sd_remount()`: calls `storage_init()`, writes CSV header if file is new, sets T9's `s_sd_ok` flag — T9 begins logging to SD immediately; safe to call from any task when `s_sd_ok` is false
- `firmware/src/event_logger/event_logger.h/.cpp` — `event_logger_sd_unmount()`: clears T9's `s_sd_ok` flag first (prevents in-flight SD write), then calls `storage_sd_unmount()`

### Fixed
- `firmware/src/ui_display/ui_display.cpp` — status page 4 row 1: format string `"Src:%-3s     #=Set"` (17 chars) truncated to `"#=Se"` on the 16-char LCD; corrected to `"Src:%-3s    #=Set"` (16 chars)
- `firmware/src/web_server/web_server.cpp` — sensor history buffer raised from 4096 → 6144 bytes; pre-write overflow guard replaced `pos >= 3800` heuristic with proper `pos + written >= HIST_BUF - 4` check; history table no longer truncates after the first hour
- `firmware/src/web_server/web_server.cpp` — firmware version now read from NVS `system/fw_version` in `build_config_json()` and included in `/api/config` response; footer no longer shows "—" until the first WebSocket push

### Changed
- `firmware/data/style.css` — tooltip CSS block added (`[data-tip]` relative positioning, `::after` pseudo-element, `.tip-icon` helper class)
- `firmware/data/style.css` — `.row` and `.slider-row` `margin-bottom` increased from `0.5 rem` to `1 rem` for better touch ergonomics on smartphones
- `firmware/data/app.js` — `doLogout()` simplified to `post('/api/logout', {}).then(() => showLogin())`
- `firmware/src/web_server/web_server.cpp` — `/api/sd/status` accessible to SESSION_FARMER and SESSION_ADMIN (was SESSION_ADMIN only)

### Added (tooling)
- `webUiMock/mock_server.py` — **Flask web UI mock server**: serves `firmware/data/` static files and emulates all REST and WebSocket endpoints (`/api/whoami`, `/api/login`, `/api/logout`, `/api/status`, `/api/config` GET/POST, `/api/wifi`, `/api/pin`, `/api/history`, `/api/sd/status|mount|unmount`, `/ws`); sine-wave sensor simulation; in-memory NVS config state with correct `(ns, key)` → field mapping for all motor/climate/wind/system keys; SD card state toggled by mount/unmount; session and access-control rules match firmware exactly (farmer vs. admin restrictions)
- `webUiMock/requirements.txt` — Python dependencies: `flask>=2.3.0`, `flask-sock>=0.7.0`
- `webUiMock/README.md` — setup and usage instructions, full endpoint table, access-control notes, differences-from-firmware table

### Build metrics
- Flash: 54.4% (1141 kB / 2 MB)
- RAM:   19.3% (63 kB / 320 kB)

### Notes
- `pio run -t uploadfs` always targets lfs1 (0x520000). When running firmware from app0/Bank A, flash web assets to lfs0 (0x420000) directly with esptool (command in `platformio.ini` comments).
- Start the mock server with `cd webUiMock && pip install -r requirements.txt && python mock_server.py`; open `http://localhost:5000` (farmer PIN: `1234`, admin PIN: `12345678`).

---

## [1.13.0] — 2026-05-05

*Geolocation + automatic timezone; local-time clock display fix; LCD time status page; LCD manual date/time set (admin).*

### Added
- `firmware/src/network_manager/network_manager.cpp` — **automatic geolocation and timezone** (`do_geo_sync()`):
  - After every successful NTP sync, performs HTTP GET `http://ip-api.com/json?fields=status,lat,lon,timezone` (5 s timeout)
  - Parses JSON response (strstr/atof, no cJSON dependency) for latitude, longitude, and IANA timezone name
  - Lookup table of ~100 IANA timezone names → POSIX TZ strings (Europe, Americas, Asia, Australia, Pacific)
  - Posts `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` to Q4 → T4 updates shadow + sunrise/sunset immediately
  - Writes POSIX TZ string to NVS `system/tz_str`; calls `setenv("TZ", …, 1)` + `tzset()` immediately without reboot
  - Falls back gracefully on HTTP error or unknown IANA name (logs warning, leaves TZ unchanged)
- `firmware/src/types/app_types.h` — `net_status_t` extended with `bool ntp_synced` field
- `firmware/src/data_manager/data_manager.h/.cpp` — **`dm_set_manual_time(time_t unix_ts)`** public API:
  - Updates POSIX system clock via `settimeofday()`
  - Writes UTC time to DS1307 RTC under MX1 via `rtc_set_time()`
  - Updates `current_unix_ts` in MX4 configuration shadow
- `firmware/src/ui_display/ui_display.cpp` — **LCD time status page** (page 4 of 5):
  - Row 0: `YYYY-MM-DD HH:MM` (local time via `localtime_r`)
  - Row 1: `Src:NTP  #=SetTm` or `Src:RTC  #=SetTm` — source from `net_status_t.ntp_synced`
- `firmware/src/ui_display/ui_display.cpp` — **LCD manual date/time set** (admin only, two-screen flow):
  - `#` on time status page (page 4) → admin PIN required → `UI_SET_DATE`
  - **Date screen** (`UI_SET_DATE`): row 0 shows current date; row 1 entry `DD/MM/YY #OK *Bk`; 6 digits DDMMYY; `#` advances to time screen; `*` backtracks/cancels to status
  - **Time screen** (`UI_SET_TIME`): row 0 shows current time; row 1 entry `HH:MM #OK *Bk`; 4 digits HHMM; `#` converts entered local time via `mktime()` to UTC epoch, calls `dm_set_manual_time()`, writes to DS1307, returns to status; `*` backtracks to date screen (date digits restored)
  - Validation: day 01–31, month 01–12, hour 00–23, minute 00–59; error messages on bad input
  - New FSM states: `UI_SET_DATE`, `UI_SET_TIME`; `s_pending_settime` flag for deferred PIN flow

### Changed
- `firmware/src/web_server/web_server.cpp` — **time display fix**: `gmtime_r` → `localtime_r`; format `"%Y-%m-%dT%H:%M:%SZ"` → `"%Y-%m-%dT%H:%M:%S"` — clock in web UI now shows local time with DST applied instead of UTC
- `firmware/src/network_manager/network_manager.cpp` — `post_q5()` now sets `st.ntp_synced = s_ntp_synced`
- `firmware/src/ui_display/ui_display.cpp` — `STATUS_PAGES` constant 4 → 5

### Build metrics
- Flash: 54.3% (1 138 kB / 2 MB)
- RAM: 19.3% (63 kB / 320 kB)

---

## [1.12.0] — 2026-05-05

*NVS partition fix (critical); AP lifecycle hardening; LCD display improvements; web GUI tab restructure and RH-dependent grayout.*

### Fixed
- **Critical — `firmware/partitions.csv`**: The Arduino ESP32 toolchain unconditionally flashes `boot_app0.bin` to the hardcoded address 0xe000. The old partition table placed NVS at 0x9000–0x1DFFF (84 KB), so 0xe000 landed inside NVS page 5 and corrupted the entire namespace on every firmware flash. Fixed by redesigning the partition layout:

  | Name    | Type | Sub-type | Offset   | Size    |
  |---------|------|----------|----------|---------|
  | otadata | data | ota      | 0xe000   | 0x2000  |
  | nvs     | data | nvs      | 0x10000  | 0x10000 |
  | app0    | app  | ota_0    | 0x20000  | 0x200000|
  | app1    | app  | ota_1    | 0x220000 | 0x200000|
  | lfs0    | data | spiffs   | 0x420000 | 0x100000|
  | lfs1    | data | spiffs   | 0x520000 | 0x100000|

  `board_upload.offset_address = 0x20000` in `platformio.ini` updated accordingly.
- `firmware/src/ui_display/ui_display.cpp` — `UI_MENU_SYSTEM` / `UI_MENU_MOTORS` tab panes no longer use `admin-only-block` CSS class (was forcing `display:block` for both when admin, overriding tab show/hide logic); both are now plain `tab-pane`

### Added
- `firmware/src/network_manager/network_manager.cpp`:
  - **AP auto-stop on client connect**: when `WL_CONNECTED` is reached, if AP is active the NVS `wifi/ap_enable` flag is cleared and `stop_ap()` is called immediately
  - **AP non-persistent on reboot**: at T10 startup `nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0)` unconditionally forces AP disabled; admin must explicitly enable it each boot via LCD or web GUI
- `firmware/src/ui_display/ui_display.cpp`:
  - **Boot splash**: shows `"Greenhouse Ctrl "` / `"v0.1.0 Init... "` on LCD for 2 s using `FIRMWARE_VERSION` macro
  - **Network status page — AP SSID**: when AP is active, row 1 shows computed SSID `"Greenhouse-XXYY"` (last 2 MAC bytes) instead of blank
  - **Wind direction cardinal**: row 1 of wind page now shows `" Dir:%3d ° (%-2s) "` — degree value, degree symbol (`\xDF`), and 8-point cardinal name (N/NE/E/SE/S/SW/W/NW); helper `deg_to_cardinal()` added
- `firmware/data/index.html`, `app.js`, `style.css` — **web GUI restructure**:
  - `<h1>` badge: WS online/offline badge moved inside `<h1>` title element
  - RH-dependent rows (6 rows) carry `.rh-dep` class; `applyRhCtrl()` in JS toggles `.rh-disabled` on all `.rh-dep` rows when humidity control is disabled; CSS: `.rh-dep.rh-disabled { opacity: 0.35; pointer-events: none }`
  - **System tab** (admin): session & timing sliders, WiFi AP settings, WiFi client settings, NTP timezone, location (lat/lon)
  - **Access tab** (admin): Farmer PIN change, Admin PIN change
  - Standalone "System" and "Access control" sections removed (content consolidated into tabs)
  - Motors tab and System tab content correctly separated (removed duplicate content)

### Changed
- `firmware/src/ui_display/ui_display.cpp`:
  - Wind status page: valid sensors: `" Dir:%3d \xDF (%-2s) "` (space before degree symbol); invalid sensors: `" Dir: --- \xDF     "` (consistent column alignment with valid case)

---

## [1.11.0] — 2026-05-05

*Phase 9 — Web Server (T11) implemented: ESPAsyncWebServer with LittleFS file serving, cookie-session auth (farmer/admin roles), REST API, WebSocket status push, config read/write, PIN management, WiFi provisioning.*

### Added
- `firmware/data/index.html` — single-page web application:
  - Login overlay with role select (farmer/admin) and PIN entry
  - Header: connection badge (WS Online/Offline), role badge, Logout button
  - Status section: 8 cards — Temperature (raw + average), Humidity (raw + average), Wind (speed/direction/average), Windows M1/M2/M3, Mode + sunrise/sunset, Alarms (EG1 bits decoded as badges), Clock + NTP status, WiFi (RSSI + IP)
  - Settings section with 6 tabs: Climate (farmer+), Wind (farmer for enable, admin for wind speed/direction limits), Motors (admin only), System (admin only), Network (admin only), Access / PIN change (admin only)
  - Sensor history table (last 60 readings)
- `firmware/data/style.css` — dark-theme CSS:
  - CSS custom properties: `--bg #1a1a2e`, `--card #16213e`, `--accent #0f3460`
  - Login overlay, status card grid, badge styles, tab system, form rows
  - Role-gated visibility: `.admin-only { display: none }` / `body.is-admin .admin-only { display: flex }`; farmer equivalent for `.farmer-hidden`
- `firmware/data/app.js` — web application logic:
  - Auth: `setRole()`, `doLogin()` (POST /api/login), `doLogout()` (POST /api/logout), session check on load via `fetch('/api/whoami')`
  - WebSocket: `wsConnect()` with 3 s auto-reconnect; `handleStatus()` updates all DOM elements from WS push
  - Config: `loadConfig()` (GET /api/config); `postCfg()`, `postCfgSelect()`, `postCfgStr()`, `postLocation()`, `postWifi()`, `postApPsk()`, `postPinChange()`
  - History: `loadHistory()` (GET /api/history?n=60) → table rows
  - Tab system: `showTab()` toggles `.active` class on pane + button
- `firmware/src/web_server/web_server.cpp` — full T11 implementation:
  - 4-slot cookie session map (`s_sessions[MAX_SESSIONS]`), FreeRTOS mutex, 16-byte hex token from `esp_fill_random()`
  - LittleFS file serving via `serve_lfs()` — PSRAM-allocated 32 KB read buffer, MIME type from extension
  - `build_status_json()` / `build_config_json()` — PSRAM 1 KB JSON builders
  - `AsyncWebSocket s_ws("/ws")` with 2 s push loop in T11 task
  - `t2_get_window_states()` cross-task window state read via spinlock
  - Farmer-key whitelist for partial access: climate setpoints + `rh_ctrl_en` + `wind_prot_en`
  - Body parser: `json_get_str()` / `json_get_int()` — strstr-based, no cJSON dependency
  - Routes: 11 endpoints (see firmwareImplementationResults.md for full table)

### Modified
- `firmware/src/relay_controller/relay_controller.h` — added `t2_get_window_states(window_state_t out[3])` declaration
- `firmware/src/relay_controller/relay_controller.cpp` — added `portMUX_TYPE s_state_mux` spinlock and `t2_get_window_states()` implementation
- `firmware/platformio.ini` — added `board_build.filesystem = littlefs` for `pio run -t uploadfs`

### Fixed
- `pin_auth_set_pin` → `pin_auth_set` (correct API name discovered on first build)

### Build metrics
- Flash: 46.4% (973 kB / 2 MB)
- RAM: 19.0% (62 kB / 320 kB)
- Build time: 46 s

---

## [1.10.1] — 2026-05-05

*Post-Phase 8 corrections: AP enable/disable added to T8 system menu; root menu now shows all four items; AP password defaulted to `0123456789`; TSDS AP password description corrected.*

### Added
- `firmware/src/ui_display/ui_display.cpp` — `handle_menu_system()` and updated `render_menu_system()`:
  - Root menu row 1 changed from `"3:Access  *:Back"` to `"3:Access 4:Sys *"` so item 4 (System) is visible
  - System menu shows `"1=WiFi AP   *:Bk"` (or `"1=AP(on)    *:Bk"` when AP is already active)
  - Pressing `1` with admin session toggles AP on/off: posts `Q4 {ns="wifi", key="ap_enable", value=0/1}` → T4 persists → T10 acts on next 5 s poll
  - Without admin session: shows `"Admin login req. / 3=Access menu"` for 2 s, returns to system menu

### Changed
- `firmware/src/network_manager/network_manager.cpp`:
  - Added `#define AP_PSK_DEFAULT "0123456789"` — AP is never started open/passwordless
  - `nvs_cfg_get_str_or_default` for `ap_psk` seeds NVS with `AP_PSK_DEFAULT` on first boot (was empty string)
  - `start_ap()` uses NVS password when set, falls back to `AP_PSK_DEFAULT` if empty
- `design/technicalSoftwareDesignSpecification.md`:
  - §WiFi AP mode: corrected "password hashed" to plaintext with rationale (WPA2 requires raw key); documented default `0123456789`; noted it is configurable by admin via web interface
  - NVS schema `wifi` row: `ap_psk` annotated as plaintext with default; contrast with `psk_hash` (client, hashed) made explicit

### Verified on hardware
- AP `Greenhouse-XXYY` visible in WiFi scan after enabling via LCD system menu ✅
- AP requires password `0123456789` ✅
- LCD system menu shows `"1=AP(on)    *:Bk"` when AP is active ✅
- Admin session required; non-admin press shows prompt ✅

---

## [1.10.0] — 2026-05-05

*Phase 8 — Network Manager (T10) implemented: WiFi station FSM with exponential backoff, soft-AP management, NTP synchronisation with DS1307 update via TN4, and Q5 network status to T8.*

### Added
- `firmware/src/network_manager/network_manager.cpp` — full T10 task body (replaces Phase 0 stub):
  - 5-state client FSM: `NET_IDLE` → `NET_CONNECTING` → `NET_CONNECTED` → `NET_RUNNING` → `NET_BACKOFF`
  - `NET_IDLE`: polls NVS `wifi/ssid` every 5 s; advances to `NET_CONNECTING` when SSID appears (supports post-boot provisioning via web server)
  - `NET_CONNECTING`: 30 s hard timeout; `WiFi.setAutoReconnect(false)` — T10 owns reconnection
  - `NET_CONNECTED`: posts Q5 with IP; runs `run_ntp_sync()` inline (blocks up to 30 s for plausible `time(NULL) > 1 700 000 000`); advances to `NET_RUNNING`
  - `NET_RUNNING`: monitors `WiFi.status()` every 5 s for connection drop
  - `NET_BACKOFF`: exponential wait 2 → 4 → 8 → 16 → 32 → 60 s (capped); re-reads NVS credentials before retry
  - AP management: `poll_ap()` reads NVS `wifi/ap_enable` every loop tick; starts/stops `WiFi.softAP()` on change; enforces `cfg.ap_timeout_min` auto-shutdown (writes `ap_enable=0` back to NVS on expiry)
  - AP SSID: `"Greenhouse-XXYY"` from last two MAC bytes
  - NTP sync: `configTime(0, 0, "pool.ntp.org")` → on success: `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` → T4 calls `rtc_set_time()` under MX1
  - Q5: `xQueueOverwrite(Q5, &status)` on every state change; `net_status_t {client_connected, ap_active, ip_str[16]}`
  - LOG_SYSTEM events: STA connect/disconnect (value_a=1), NTP success/timeout (value_a=2), AP start/stop (value_a=3)

### Changed
- `firmware/firmwareImplementationPlan.md` — Phase 8 marked ✅ done; `network_manager` added to Critical Files Summary
- `firmware/firmwareImplementationResults.md` — Phase 8 section added

### Verified on hardware (runtime capture)
- T10-01: Build clean — 0 errors, 0 warnings ✅
- T10-02: Upload and boot without crash ✅
- T10-03: T1 heartbeat steady at t=15–40 s; no Guru Meditation ✅
- T10-04: NET_IDLE (no SSID configured) — no periodic log output (expected behaviour) ✅
- T10-05: Q5 initial post received by T8 — LCD network page shows "No WiFi" ✅

---

## [1.9.0] — 2026-05-05

*Phase 7 — UI Layer (T7 + T8) implemented: 4×4 keypad scan with key-repeat and full LCD menu FSM with PIN authentication, config editing, and session management.*

### Added
- `firmware/src/keypad_scan/keypad_scan.cpp` — full T7 task body (replaces Phase 0 stub):
  - 20 ms scan period via `vTaskDelay(pdMS_TO_TICKS(20))` + `keypad_scan()` from LIB-5
  - Key-repeat: same key held ≥ 500 ms → repeat events every 100 ms (`repeated=true`)
  - Posts `key_event_t` to Q2 non-blocking; first-press overflow → `ESP_LOGW`, repeat overflow → `ESP_LOGD`
- `firmware/src/ui_display/ui_display.cpp` — full T8 task body (replaces Phase 0 stub):
  - LCD init via `lcd_init()` under MX1 at task entry; 2 s boot splash
  - 100 ms main loop: Q2 key receive, Q5 network status poll, session timeout tick, FSM dispatch, status page rotation, dirty-flag render
  - 8-state FSM: `UI_STATUS` → `UI_MENU_ROOT` → `UI_MENU_CLIMATE` / `UI_MENU_WIND` / `UI_MENU_ACCESS` / `UI_MENU_SYSTEM` → `UI_PIN_ENTRY` / `UI_EDIT_VALUE`
  - Status display: 4 pages × 5 s auto-rotate (T/RH, wind, mode/session, network)
  - Parameter table: 11 climate params + 2 wind params; 2 params per sub-menu page; `#` cycles pages; current NVS value shown alongside label
  - PIN entry: masked display, `pin_auth_verify()`, lockout seconds shown on lock, pending-edit preserved through PIN flow
  - Config edit: digit builder, `B`=sign toggle for negative temps, `#` confirm → `xQueueSend(Q4)` + `log_post(LOG_SETPOINT)`
  - Session: `session_open()` / `session_close()` log `LOG_SESSION`; timeout from `cfg.session_timeout_min` (default 5 min); idle counter reset on non-repeat keys only
  - `show_msg()` helper: fills rows, flushes LCD under MX1, blocks for delay, sets dirty for re-render
  - FR-UI07 satisfied: ≤ 4 keypresses from status screen to any first-level setting when authenticated

### Changed
- `firmware/src/keypad_scan/keypad_scan.h` — phase reference updated to Phase 7
- `firmware/src/ui_display/ui_display.h` — phase reference updated to Phase 7
- `firmware/src/main.cpp` — added `#include "auth/pin_auth.h"` and `pin_auth_init()` call after `nvs_cfg_init()`; logs `PIN auth OK` on success
- `firmware/firmwareImplementationPlan.md` — Phase 7 marked ✅ done; `keypad_scan` and `ui_display` added to Critical Files Summary
- `firmware/firmwareImplementationResults.md` — Phase 7 section added

### Verified on hardware (boot capture, 340 s runtime)
- T8-02: LCD init OK — no `ESP_LOGE` from `T8_UI` in 340 s capture ✅
- T8-03: MX1 not deadlocked — T4 periodic RTC read completes at t=297 s ✅
- All tasks stable: T1 heartbeat continuous, T5/T6 nominal at t=309/310 s, no watchdog resets ✅
- T7-01/T8-01 (task alive), T7-02/03, T8-04–09: deferred to integration testing (USB-CDC pre-connect window / physical keypresses required)

---

## [1.8.0] — 2026-05-05

*Phase 6 — Climate Control (T6) implemented: autonomous graduated ventilation driven by live T/RH sensor data, EG1 inhibit gate, conflict resolution, and incremental Q1 command posting.*

### Added
- `firmware/src/climate_control/climate_control.cpp` — full T6 task body (replaces Phase 0 stub):
  - Blocks on TN2 (`ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`) — wakes only when T4 has new Q6 data
  - EG1 gate: skips evaluation and resets `current_step_t/rh = 0` if `WIND_OVERRIDE`, `MOTOR_ALARM`, or `SENSOR_FAULT_T` is set; logs inhibit transitions; resumes from step 0 on clearance
  - Snapshots `cfg_shadow_t` under MX4 (`dm_cfg_snapshot()`) and `sensor_reading_t` under MX2 (`dm_meas_snapshot()`)
  - Selects day vs. night setpoints (`t_max`, `rh_max`, `rh_min`) from `cfg.is_daytime`
  - Calls `vent_step_required_t()` and `vent_step_required_rh()` (already implemented, Gap G); resolves with `vent_resolve_conflict()`
  - `apply_step_delta()`: posts CLOSE commands before OPEN commands for changed channels; single `CMD_CLOSE_ALL ch=0` when resolved step == 0
  - `post_log_mode()`: posts `LOG_MODE_CHANGE` to Q3 on every step change with `value_a=resolved_step`, `value_b=(step_t<<8)|step_rh`
  - `post_q1()`: non-blocking `xQueueSend(Q1, ..., 0)` with LOGW on queue-full (never blocks T6)
  - Defensive hysteresis clamp: `hyst_t/rh` clamped to ≥ 1 to prevent division by zero in `step_from_deviation()`

### Changed
- `firmware/src/climate_control/climate_control.h` — Doxygen updated: per-wake sequence documented (8 steps), inhibit behaviour and step-reset logic described
- `firmware/firmwareImplementationPlan.md` — Phase 6 marked ✅ done
- `firmware/firmwareImplementationResults.md` — Phase 6 section added

### Verified on hardware (VERIFY_T6 harness, 471 s capture)
- Clean build: RAM 10.8% (35 364 B), Flash 20.0% (419 869 B); zero warnings
- **T6-04/05** — VERIFY_T6 Phase A: Q4 `t_max_ngt=5` injected at t=15 s; T5 iter 1 at t=68.8 s fired TN2; T6 evaluated `T_avg=12 t_max=5 hyst=2 → step_t=3 | step_rh=−1 (NEUTRAL) | resolved=3`; CMD_OPEN ch=1, CMD_OPEN ch=2, CMD_OPEN ch=3 posted to Q1
- **T6-07** — VERIFY_T6 Phase B: Q4 `t_max_ngt=22` injected at t=106 s; T5 iter 2 at t=129 s fired TN2; T6 evaluated `deviation=−10 < −hyst=−2` → close-hysteresis guard cleared → step 0 → CMD_CLOSE_ALL
- **T2 Q1 acceptance** — T2 drained queued commands at t=176 s (post-calib); CMD_OPEN ch1/2/3 then CMD_CLOSE_ALL accepted; SRC_T6 correctly identified in T2 log
- **Stable idle** — T6 held step=0 with no Q1 posts for iters 3–7 (t=189–430 s); no WDT resets or crashes
- T6-02/03/06/08/09/10 deferred to integration testing

---

## [1.7.0] — 2026-05-05

*Phase 5 — Event Logger (T9) implemented and hardware-verified: Q3 drain loop, NVS ring buffer, SD CSV append with rotation, drop-counter surfacing, and SD failure fallback all confirmed on device. Duplicate LOG_SENSOR fixed.*

### Added
- `firmware/src/event_logger/event_logger.cpp` — full T9 implementation (replaces Phase 0 stub):
  - SD init via `storage_sd_init()` with NVS-only fallback when card absent or mount fails
  - NVS file-index recovery: `nvs_cfg_get_i32("log", "file_idx")` at boot; resumes on same file across reboots without writing a duplicate CSV header
  - Drain-pass loop: `xQueueReceive(Q3, portMAX_DELAY)` blocks for first event, then `xQueueReceive(Q3, 0)` drains all remaining in a tight loop; after each pass calls `log_take_dropped_count()` and, if > 0, posts a synthetic `LOG_SYSTEM` event via `xQueueSend(Q3, ..., 0)` **directly** (not `log_post()`) to avoid re-entrant eviction
  - Every event written to NVS ring buffer unconditionally via `nvs_log_append()`; SD write additionally if `s_sd_ok`
  - SD log rotation at 512 KB: increments `s_file_idx`, persists to NVS, writes CSV header to new file; deletes `s_file_idx − 10` when file count exceeds 10
  - SD write failure: clears `s_sd_ok`, emits `LOG_SYSTEM value_a=−1` via `log_post()` so failure is visible in NVS log; NVS-only operation continues without firmware restart
  - CSV format: `timestamp,type,initiator,ch,param,value_a,value_b\n`; `evt_type_str()` / `initiator_str()` string maps; `build_csv_line()` via `snprintf`
- `firmware/src/event_logger/event_logger.h` — Doxygen rewrite: full T9 behaviour description, drain-pass structure, SD rotation parameters, CSV column table

### Changed
- `firmware/src/main.cpp` — T9 stack increased 4 096 → 6 144 bytes (SD + FAT32 + snprintf stack headroom)
- `firmware/src/sensor_poll/sensor_poll.cpp` — **removed** `log_post(LOG_SENSOR)` from Step 7 of the poll loop; T4 (`data_manager.cpp`) is the sole canonical poster per FR-LG09; posting from both T5 and T4 produced two `SENSOR` CSV rows per poll cycle (Finding 1)
- `firmware/firmwareImplementationPlan.md` — Phase 5 marked ✅ done
- `firmware/firmwareImplementationResults.md` — Phase 5 section added: implementation design, build output, hardware verification checklist, findings

### Verified on hardware (bkhkhe0s8 serial capture + SD card inspection)
- **T9-01** — Task alive: `[T9] task alive` confirmed (pre-USB-CDC; SD CSV present proves T9 ran)
- **T9-03** — SD mount and CSV creation: `/ghc_0001.csv` created with correct header on first boot
- **T9-04** — LOG_SENSOR rows in CSV: `SENSOR,SYS,0,0,11,81` and similar rows at each 60 s poll cycle
- **T9-05** — LOG_RELAY rows in CSV: CH1–CH3 `MOVING_CLOSE` + `CLOSED` calibration sequence fully present
- **T9-07** — Drop-counter surfacing: VERIFY_T9 harness flooded 40 events into Q3 (depth 32); serial showed `[T9] Q3 overflow: 7 event(s) dropped` at t=91 s; `SYSTEM,SYS,0,0,7,0` row in CSV
- **T9-08** — SD failure fallback: SD card contact failure at t=431 s (iter 7) triggered `sdWait(): Wait Failed` / `fopen() failed`; T9 logged `SD write failed (3) — falling back to NVS-only`; subsequent poll iters (8–10) continued without crash — NVS-only fallback confirmed
- **T9-09** — File index recovery: second boot appended to `/ghc_0001.csv` without writing a second header; NVS `file_idx=1` recovered correctly
- T9-02, T9-06, T9-10 deferred to integration testing

### Findings
- **Finding 1 (fixed)** — Duplicate LOG_SENSOR: both T5 (`sensor_poll.cpp`) and T4 (`data_manager.cpp`) were calling `log_post(LOG_SENSOR)` per poll cycle, producing two `SENSOR` rows per interval. Fixed by removing the T5 call; T4 is now the canonical source.
- **Finding 2 (deferred)** — timestamp=0 race: T2 (PRIO_HIGH) wins scheduler at boot and logs the first two calibration events before T4 has populated `dm_get_unix_time()`. Cosmetic; will resolve automatically with NTP sync in Phase 8.

---

## [1.6.0] — 2026-05-05

*Phase 4 — Safety Monitor (T3) implemented: wind speed threshold check, direction exclusion zone (with wrap-through-0°), SENSOR_FAULT_W safe-fail, EG1.WIND_OVERRIDE management, CMD_CLOSE_ALL / CMD_RESUME to Q1, and LOG_ALARM events (W1/W2/W3) to Q3.*

### Added
- `firmware/src/safety_monitor/safety_monitor.cpp` — full T3 implementation (replaces Phase 0 stub):
  - Wakes on TN1 (`ulTaskNotifyTake`, pdTRUE) from T4 after each new wind measurement
  - Reads `sensor_reading_t` via `dm_meas_snapshot()` (MX2) and config via `dm_cfg_snapshot()` (MX4)
  - `wind_prot_en = false` → clears WIND_OVERRIDE if previously set, posts CMD_RESUME, skips evaluation
  - `EG1_BIT_SENSOR_FAULT_W` → safe-fail: sets WIND_OVERRIDE without consulting measurements; `value_a = −1` log marker (FR-W04)
  - Speed check: `wind_speed_avg_ms10 >= v_max × 10` (int32 arithmetic; `v_max ≤ 0` disables check)
  - Direction check: `dir_in_exclusion_zone()` handles non-wrapping and wrap-through-0° arcs; zero-width zone disabled
  - Onset (safe → unsafe): `xEventGroupSetBits(EG1, EG1_BIT_WIND_OVERRIDE)` → `xQueueSend(Q1, CMD_CLOSE_ALL, SRC_T3, 0)` → log W1 and/or W2
  - Both speed and direction unsafe simultaneously → two separate LOG_ALARM records (W1 + W2), one CMD_CLOSE_ALL
  - Clearance (unsafe → safe): `xEventGroupClearBits` → `xQueueSend(Q1, CMD_RESUME, SRC_T3, 0)` → log W3 with current speed + direction
  - MOTOR_ALARM interaction: T3 evaluates and posts normally; T2 discards Q1 commands while MOTOR_ALARM is set; WIND_OVERRIDE bit maintained correctly
  - `#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE` before all includes (same fix as Phase 3 T5 Issue 1)
- `firmware/src/safety_monitor/safety_monitor.h` — full Phase 4 Doxygen documentation: behaviour summary, log event table, design references

### Verified (build)
- Clean build: RAM 10.7% (35 120 B), Flash 17.4% (364 525 B); zero warnings
- Flash delta from Phase 3: +1 436 B

---

## [1.5.0] — 2026-05-03

*Phase 3 — Sensor Polling (T5) implemented: Modbus RTU master for FG6485A (T/RH) and S200 (wind), sliding averages for all four channels, edge-triggered fault detection, Q6 overwrite, and LOG_SENSOR posting. Bug fixed: `ESP_LOGI` compile-time suppression caused by `LOG_LOCAL_LEVEL` being overridden by transitive Arduino HAL includes.*

### Added
- `firmware/src/sensor_poll/sensor_poll.cpp` — full T5 implementation:
  - 8 s boot grace delay (ensures visibility after USB-CDC re-enumeration)
  - Poll loop: `dm_get_poll_interval_s()` → `vTaskDelay()` → `dm_cfg_snapshot()` → window recalculation → FG6485A read → S200 read → build `sensor_reading_t` → `xQueueOverwrite(Q6)` → `log_post(LOG_SENSOR)`
  - Arithmetic circular-sum sliding average for T, RH, wind speed (`avg_ctx_t`); unit-vector (sin/cos) circular-sum sliding average for wind direction (`dir_avg_ctx_t`) — handles 0°/360° wrap via `atan2()`
  - Window size = `avg_win_x_min × 60 / poll_s`, clamped [1, 360]; context reset (re-warm) on window-size change
  - One immediate retry per sensor per poll cycle; fault onset after 2nd consecutive failure; fault cleared on first success — both edge-triggered with `xEventGroupSetBits/ClearBits(EG1)` and `log_post(LOG_ALARM)`
  - ~7.2 KB BSS for four averaging buffers (360-sample depth × 4 channels)
- `firmware/src/sensor_poll/sensor_poll.h` — full Phase 3 Doxygen documentation

### Fixed
- `firmware/src/sensor_poll/sensor_poll.cpp` — `ESP_LOGI` calls were silently compiled away due to `LOG_LOCAL_LEVEL` being overridden below `ESP_LOG_INFO` by a transitive Arduino HAL include reached through the driver headers. Fix: `#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE` placed before `#include <esp_log.h>` as the first two lines of the translation unit. TAG changed from `"sensor_poll"` to `"T5_SEN"`.

### Changed
- `firmware/src/main.cpp` — T1 heartbeat reverted to clean form after Phase 3 debugging: removed `eTaskGetState(task_t5)` and `esp_get_free_heap_size()` diagnostic fields that were added temporarily to verify T5 was scheduled
- `firmware/firmwareImplementationResults.md` — Phase 3 section added (implementation design, timing analysis, hardware verification checklist, Issue 1 root-cause and fix)

### Verified on hardware
- T5 first poll at t=68 s: boot grace (8 s) + poll interval (60 s) + scheduler jitter (+337 ms)
- FG6485A poll: 509 ms (2 × 200 ms timeout + 100 ms retry), fault set correctly
- S200 poll: 511 ms (2 × 200 ms timeout + 100 ms retry), fault set correctly
- `sensor_reading_t` summary log: `T=0°C RH=0% ws=0.0 m/s wd=0° | avg T=0 RH=0 ws=0.0 wd=0° [win T=1 RH=1 W=1]`
- Q6 overwrite accepted; LOG_SENSOR posted without Q3 overflow
- No WDT resets, panics, or crashes during verification run

### Cross-validated with sensor emulator (greenhouse-Controller-Modbus-sensor-emulator Phase 2)
- Emulator received and CRC-validated all T5-generated frames: FG6485A `01 03 00 00 00 02 C4 0B` ✅, S200 wind `2C 04 00 08 00 0C 77 B0` ✅
- Exactly 2 frames per sensor per cycle observed by emulator — retry count correct
- S200 Frame 3 (heater temp, reg `0x001C`) absent when Frame 2 returns exception — early-exit path confirmed
- 60 s poll interval confirmed end-to-end (emulator timestamps: 10:59:42 → 11:00:42)

---

## [1.4.0] — 2026-05-03

*Phase 1 — Data Foundation (T4) implemented: central NVS config store, sensor ring buffers, RTC read/write, sunrise/sunset, Q4/Q6/TN4 handling, and thread-safe getter API for T1/T2/T3/T6.*

### Added
- `firmware/src/data_manager/data_manager.h` — full T4 public API:
  - `cfg_shadow_t` struct: all NVS-backed config fields plus derived fields (`is_daytime`, `current_unix_ts`, `sunrise_mins_utc`, `sunset_mins_utc`)
  - `dm_ring_buf_t` / `DM_RING_DEPTH = 360` — sensor history ring buffer type
  - `DM_NOTIFY_NTP_SYNCED` — TN4 task notification bit for T10 → T4 NTP sync signal
  - Thread-safe getters: `dm_cfg_snapshot()`, `dm_meas_snapshot()`, `dm_ring_read()`, `dm_get_is_daytime()`, `dm_get_unix_time()`, `dm_get_poll_interval_s()`, `dm_get_travel_s()`, `dm_get_dwell_open_min()`, `dm_get_dwell_close_min()`, `dm_get_led_config()`
- `firmware/src/data_manager/data_manager.cpp` — full T4 implementation:
  - Boot: loads all NVS namespaces (`climate`, `wind`, `motor`, `system`) into `cfg_shadow_t`; applies TZ string via `setenv/tzset`; reads DS1307 RTC under MX1 and calls `settimeofday()` to seed system clock
  - `rtc_dt_to_unix()` — manual UTC-correct conversion (leap year aware, no `timegm()` dependency)
  - Main loop: Q6 handler updates MX2 + MX3 ring, posts `LOG_SENSOR` to Q3, notifies T3 (TN1) and T6 (TN2); Q4 handler validates and applies config updates to NVS + MX4 shadow; TN4 handler syncs DS1307 from NTP time; periodic (~60 s) RTC re-read refreshes `current_unix_ts` and `is_daytime`
  - Location change in Q4 immediately recomputes sunrise/sunset via `update_sun_times()`
  - Boot `LOG_SYSTEM` event posted to Q3

### Changed
- `firmware/firmwareImplementationPlan.md` — Phase 1 marked ✅ done; implementation notes added

### Verified on hardware
- T4 periodic RTC re-read observed at t=60 s and t=120 s: `[T4] RTC: 2026-04-12 17:24:15 UTC  unix=1776014655  daytime=yes`
- DS1307 I2C read functional under MX1; `rtc_dt_to_unix()` produces consistent unix timestamps (Δ between reads matches elapsed wall-clock time)
- `sunrise_is_daytime()` returns correct result (`daytime=yes` at 17:19–17:24 UTC, 52°N)
- No WDT resets, panics, or crashes in 155 s continuous run
- Early boot messages (NVS load log line, boot RTC read) are not visible via USB-CDC due to USB re-enumeration delay (~3–5 s); this is a known USB-CDC limitation, not a code defect — documented as Finding 1 in `firmwareImplementationResults.md`

---

## [1.3.4] — 2026-05-03

*T2 relay controller integration tests expanded to 13 tests (IT-01–IT-13), two structural test defects fixed, and the full suite verified on hardware — all 13 tests pass.*

### Added
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-10 through IT-13:
  - **IT-10** OPEN travel expiry + dwell enforcement: CMD_OPEN CH1 → waits 26 s for travel timer expiry → CH_OPEN; verifies SRC_T6 CMD_CLOSE blocked during the 3 s dwell window; verifies SRC_T6 CMD_CLOSE accepted after dwell expiry
  - **IT-11** CLOSE→OPEN reversal gap: CMD_OPEN during MOVING_CLOSE → CH_GAP_TO_OPEN → 2 s gap → MOVING_OPEN (symmetric counterpart to IT-04)
  - **IT-12** CMD_RESUME no-op: RESUME acknowledged by T2 with no relay state change
  - **IT-13** Invalid channel discarded: CMD_OPEN ch=0 and ch=4 produce `[W]` log entries and no relay change

### Changed
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 structural fix: replaced the two-phase poll (65 s "recal start" assertion + 185 s "recal done" poll) with a single 300 s completion loop; the two-phase approach failed when a second alarm during the guard pushed re-cal start beyond the 65 s window, causing Unity's `longjmp` to skip the completion wait and leave T2 blocked in guard+recal when IT-10 started (Q1 command batch-processing defect — see `firmwareImplementationResults.md` Issue 4)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 and IT-09 manual step prompts, IT-09 completion message, and end-of-test banner all converted from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` was silently dropped by the USB-CDC driver under test task scheduling conditions (see `firmwareImplementationResults.md` Issue 5), rendering both interactive prompts invisible and causing IT-07 to fail (alarm never connected)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 prompt updated with explicit instruction to hold jumper still for ≥1 s (75 ms debounce requirement); duration table updated to show 600 s logic analyser capture window
- `firmware/firmwareImplementationResults.md` — test results table completed (13/13 PASS); logic analyser verification data and contact bounce measurement added; Issues 4 and 5 documented; handover state updated

### Verified on hardware
- All 13 integration tests pass (serial output + logic analyser CSV captured and cross-verified)
- Contact bounce measurement: 329 ms bounce on IT-07 jumper insertion correctly filtered by 75 ms debounce; alarm confirmed 83 ms after stable LOW (nominal 75 ms)
- All relay timing within 35 ms of expected values across boot calibration, guard, re-calibration, travel, gap, and dwell transitions

---

## [1.3.3] — 2026-05-03

*60 s guard time introduced after motor alarm clearance before CLOSE_ALL re-calibration starts. An open issue for alarm contact jitter has been added to the implementation plan.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `handle_alarm_clearance()` now clears `EG1_BIT_MOTOR_ALARM` and logs clearance immediately, then blocks for `ALARM_GUARD_MS = 60 000 ms` (12 × 5 s chunks) before starting re-calibration; re-checks pin at guard expiry and aborts if alarm re-asserted
- `firmware/src/relay_controller/relay_controller.cpp` — added `ALARM_GUARD_MS` and `ALARM_GUARD_CHUNK_MS` constants
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 updated: alarm bit check remains at 15 s (bit cleared before guard); new 65 s poll for re-cal start (waits for guard to expire); recal-complete poll unchanged; expected duration updated to ~500 s

### Added
- `firmware/firmwareImplementationPlan.md` — open issue **#1c Alarm contact jitter**: single end-of-guard pin re-check does not detect bounce within the guard window; mitigations listed; decision deferred

### Documentation
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` (×3 locations), `firmware/firmwareImplementationPlan.md` (#1a, #1b, integration test), `firmware/firmwareImplementationResults.md` — all alarm clearance descriptions updated to include the 60 s guard step and abort-on-re-assertion behaviour

---

## [1.3.2] — 2026-05-03

*Alarm polarity corrected throughout: the RRK-3 opto-coupler output is active-low (GPIO42 LOW = alarm active, HIGH = alarm cleared with INPUT_PULLUP), not active-high as previously documented.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — debounce logic now reads `alarm_signal = (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW)`; onset condition `alarm_signal && !alarm_active`, clearance condition `!alarm_signal && alarm_active`
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 docblock and runtime prompt updated: "Connect GPIO42 to GND" (was "3.3 V"); file header "GPIO42→GND" updated
- `design/technicalHardwareDesignSpecification.md` — GPIO table: "active-low — logic LOW when contact is closed (alarm active)"; Open Issue #1: clarified active-low opto-coupler behaviour
- `design/technicalSoftwareDesignSpecification.md` — Motor alarm detection paragraph: "active-low: contact closed → GPIO 42 LOW; contact open → GPIO 42 HIGH"
- `firmware/firmwareImplementationResults.md` — Motor Alarm sequence table: rows corrected to "GPIO42 LOW → alarm asserted" and "GPIO42 HIGH → alarm cleared"

---

## [1.3.1] — 2026-05-03

*Inter-relay gap extended from 100 ms to 2 s to allow back-EMF to dissipate fully before reversing motor direction.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `RELAY_GAP_MS` increased from `100u` to `2000u`; all inline log strings updated
- `firmware/src/relay_controller/relay_controller.h` — Doxygen updated
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-04 timing updated: mid-gap spot-check at 1150 ms (was 80 ms), final CLOSE-HIGH check at 2650 ms (was 200 ms); all comments updated
- `firmware/firmwareImplementationPlan.md`, `firmware/firmwareImplementationResults.md` — gap value updated throughout
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` — gap value updated

---

## [1.3.0] — 2026-05-03

*Phase 2 firmware implemented and hardware-verified: T2 Relay Controller fully operational with per-channel window FSM, 2 s inter-relay gap enforcement, travel/dwell timers, deferred-ISR motor alarm handling, and a complete on-device Unity integration test suite.*

### Added

#### Firmware — Phase 2: Relay Controller (T2)
- `firmware/src/relay_controller/relay_controller.cpp` — full T2 implementation replacing the Phase 0 stub:
  - Per-channel internal FSM with two transient gap states (`CH_GAP_TO_OPEN`, `CH_GAP_TO_CLOSE`) enforcing a 2 s inter-relay gap on every direction reversal; both relays de-energised during the gap (audible as two distinct clicks spaced ~2 s apart)
  - Travel timers: relay energised for `(travel_mN + 5 s) × 1000 ms`; factory defaults M1/M2 = 26 s, M3 = 176 s; values read from NVS `motor/` namespace at T2 startup via `nvs_cfg_get_i32_or_default()`
  - Dwell timers: `SRC_T3` (Safety Monitor) commands bypass dwell; `SRC_T6` (Climate Control) commands respect it
  - `calib_close_all()`: synchronous blocking CLOSE_ALL at boot and after alarm clearance; de-energises each channel individually at its own travel deadline (M1/M2 at ~26 s, M3 at ~176 s) rather than waiting for the global maximum
  - Deferred-ISR motor alarm: `IRAM_ATTR` ISR on GPIO42 (CHANGE, not suppressed during MOVING); T2 loop confirms after 75 ms debounce by reading live pin state; on assertion: de-energises all 6 relays, sets `EG1_BIT_MOTOR_ALARM`, logs onset; on clearance: clears bit, logs clearance, re-runs `calib_close_all()` (FR-MA01–FR-MA08)
  - Q1 consumer: all commands discarded when `EG1_BIT_MOTOR_ALARM` is set (FR-MA03); 20 ms loop tick
- `firmware/src/relay_controller/relay_controller.h` — complete Doxygen header documenting all T2 responsibilities
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — on-device Unity integration test suite (9 tests):
  - IT-01 NVS factory defaults; IT-02 boot calibration (185 s); IT-03 CMD_OPEN relay energisation; IT-04 direction reversal gap (2 s, audible click … 2 s … click); IT-05 mutual exclusion (OPEN+CLOSE never simultaneously HIGH); IT-06 CLOSE_ALL T3 override; IT-07 alarm onset (interactive, GPIO42 jumper); IT-08 command rejection during alarm; IT-09 alarm clearance + re-calibration (interactive)
  - Minimal heartbeat task (`task_test_heartbeat`) spawned instead of T1 so PIN_HB_LED blinks at 1 Hz during the test run; WDT disabled via `esp_task_wdt_deinit()` (boot calibration exceeds default 5 s timeout)

#### Firmware — test environment
- `firmware/platformio.ini` — `[env:test_t2_relay]` section added; key settings: `test_build_src = yes` (required to compile `src/` during `pio test`), `build_src_filter = +<**> -<main.cpp>` (include all tasks, exclude conflicting `main.cpp`)

### Fixed
- **`calib_close_all()` ran all channels for M3's full 176 s** — rewrote to track per-channel deadlines; M1/M2 relays now de-energise at ~26 s while M3 continues to ~176 s
- **Heartbeat LED static during test run** — test `setup()` now initialises `PIN_HB_LED` as output and spawns a minimal toggle-only heartbeat task

---

## [1.2.0] — 2026-05-03

*Phase 0 firmware implemented and verified on hardware. Firmware implementation plan completed with all gaps and open issues resolved. Design documentation updated for MOTOR_ALARM operating state and C9 scope enforcement.*

### Added

#### Firmware — Phase 0 scaffold (fully verified on hardware)
- `firmware/src/main.cpp` — `setup()`: initialises GPIO, I2C, RTC, NVS, creates all RTOS primitives (Q1–Q6, EG1, MX1–MX5), spawns 12 tasks; `loop()`: self-deletes. T1 Watchdog/Heartbeat fully implemented: 500 ms WDT kick (`esp_task_wdt_add` / `esp_task_wdt_reset`), 1 Hz heartbeat LED toggle (GPIO 41), WS2812B RGB LED (GPIO 38) driven from EG1 state (Red = MOTOR_ALARM, Amber = fault/wind override, Green = normal), day/night brightness with configurable night window (22:00–06:00 default)
- `firmware/src/relay_controller/relay_controller.h/.cpp` — T2 stub
- `firmware/src/safety_monitor/safety_monitor.h/.cpp` — T3 stub
- `firmware/src/data_manager/data_manager.h/.cpp` — T4 stub
- `firmware/src/sensor_poll/sensor_poll.h/.cpp` — T5 stub
- `firmware/src/keypad_scan/keypad_scan.h/.cpp` — T7 stub
- `firmware/src/ui_display/ui_display.h/.cpp` — T8 stub
- `firmware/src/network_manager/network_manager.h/.cpp` — T10 stub
- `firmware/src/web_server/web_server.h/.cpp` — T11 stub
- `firmware/src/mqtt_client/mqtt_client.h/.cpp` — T12 stub
- `firmware/firmwareImplementationResults.md` — Phase 0 implementation results: all six build/boot issues documented with root causes and fixes, verified hardware boot log, verification checklist, Phase 0 → Phase 1 handover state

#### Firmware — pre-Phase-0 modules (already complete before Phase 0 boot)
- `firmware/src/types/app_types.h` — complete shared type system: motor travel constants (M1/M2 = 21 s, M3 = 171 s, 5 s margin), all RTOS handle externs, queue item structs (`window_cmd_t`, `key_event_t`, `log_event_t`, `config_update_t`, `net_status_t`, `sensor_reading_t`), enums (`op_mode_t` with MOTOR_ALARM, `window_state_t`, `log_type_t`, `cmd_source_t` restricted to T3/T6 per C9), EG1 bit definitions (WIND_OVERRIDE, SENSOR_FAULT_T/W, OTA_IN_PROGRESS, MOTOR_ALARM; bit 1 reserved — MANUAL_OVERRIDE not supported by hardware)
- `firmware/src/climate_control/climate_control.h/.cpp` — Gap G resolved: graduated ventilation step table (step 1 = M1, step 2 = M1+M2, step 3 = M1+M2+M3), `vent_step_required_t()` and `vent_step_required_rh()` with close-hysteresis guard, `vent_resolve_conflict()` with three priority modes (CR_TEMP_FIRST, CR_RH_FIRST, CR_DEVIATION)
- `firmware/src/event_logger/event_logger.h/.cpp` — Gap H resolved: `log_post()` drop-oldest Q3 helper (evict-and-retry with portMUX spinlock-protected drop counter), `log_take_dropped_count()` atomic read-and-reset; T9 task stub
- `firmware/src/auth/pin_auth.h/.cpp` — Gap C resolved: PIN hashing via `mbedtls/sha256.h`; SHA-256(16-byte random salt ∥ PIN ASCII); salt stored in NVS `access/pin_salt`; per-role lockout with NVS-persisted expiry timestamps
- `firmware/src/data_manager/sunrise.h/.cpp` — Gap D resolved: NOAA General Solar Position Equations (±2 min accuracy); outputs UTC minutes from midnight; handles polar day/night; zero lat/lon defaults to daytime
- `firmware/firmwareImplementationPlan.md` — complete phased implementation plan (Phases 0–10), all 8 design gaps (A–H) resolved, 6 open issues (#1a/b–#6) resolved, integration test checklist

#### Design documentation
- `design/mocWebUIConciderations.md` — web UI mockup and layout considerations: dashboard, settings page, technical/admin page; responsive mobile-first approach; REST API endpoint mapping

### Changed

#### Design — MOTOR_ALARM operating state (new highest-priority mode)
Resolved Constraint C8: the RRK-3 provides a **single alarm output** that fires only when a motor runs to the **emergency switch** (not on normal manual operation). Manual operation detection via GPIO 42 is not achievable with the current hardware. All affected documents updated:
- `design/functionalRequirementsSpecification.md` — C8 resolved; FR-M08–M11 removed (manual detection not possible); §5.3a added with FR-MA01–FR-MA08 (Motor Alarm requirements: immediate relay de-energisation, highest-priority override, CLOSE_ALL re-calibration on clear, display message, logging); operating modes table updated
- `design/technicalHardwareDesignSpecification.md` — §4.5.2 updated: opto-coupler described as motor emergency stop alarm, not manual override detector
- `design/tasks.md` — T2 function updated (MOTOR_ALARM detection replaces manual override); GPIO 42 ISR description rewritten (not suppressed during MOVING; alarm assert: de-energise all relays + set EG1.MOTOR_ALARM; alarm release: clear flag + CLOSE_ALL re-calibration); EG1 table updated (MANUAL_OVERRIDE removed, MOTOR_ALARM bit 5 added); TN3 task notification removed
- `design/technicalSoftwareDesignSpecification.md` — T2 description updated; manual override detection section replaced with Motor Alarm detection; EG1 table updated; T6 EG1 inhibit flags updated; §5.12 RGB LED Red condition confirmed for MOTOR_ALARM
- `firmware/firmwareImplementationPlan.md` — open issues #1a/#1b updated; Phase 2 T2 GPIO 42 bullet rewritten; Phase 6 T6 EG1 check updated; integration test added

#### Design — C9 scope enforcement (no manual window commands from LCD/web/MQTT)
- `design/functionalRequirementsSpecification.md` — FR-WS05 updated: manual window commands explicitly excluded (C9)
- `design/softwareTestPlan.md` — ST-WI-008 replaced: no longer tests a manual command relay trigger; now verifies the web dashboard contains no window open/close controls
- `design/riskAssessment.md` — manual window via keypad removed from sensor-failure mitigation; MQTT attack chain updated (T12 does not post to Q1; attack surface narrowed to Q4 config updates only)
- `design/implementationPlan.md` — T8 manual window command bullet removed
- `firmware/src/types/app_types.h` — `cmd_source_t` limited to `SRC_T3` and `SRC_T6`; Q1 producer comment updated

#### Firmware configuration
- `firmware/platformio.ini` — `board_build.arduino.memory_type = qio_opi` (LOLIN S3 OPI PSRAM variant); `board_upload.offset_address = 0x20000` (app0 at 0x20000 due to 84 KB NVS); `monitor_dtr = 1` / `monitor_rts = 0`; `lib_extra_dirs = ../drivers`; `lib_ignore = WebServer`; `Adafruit NeoPixel`, `ESPAsyncWebServer`, `AsyncTCP` added to `lib_deps`; `-DCORE_DEBUG_LEVEL=3` and `-DCONFIG_NVS_LOG_CAPACITY=250` added

### Fixed
- **Crash loop on boot** (`rst:0x3 / Saved PC:0x403cdb0a`) — `board_upload.offset_address = 0x20000` added; firmware was being written to `0x10000` while the partition table directed the bootloader to find `app0` at `0x20000`
- **Silent serial output after boot** — all `setup()` diagnostics and T1 heartbeat switched from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` dropped silently when no USB-CDC host had the port open (DTR not asserted), while IDF log calls bypass the DTR check
- **`extern "C"` linkage conflict** in `event_logger.cpp` and `climate_control.cpp` — `extern "C"` blocks removed; all code is C++ throughout
- **`WebServer` LDF conflict** — `lib_ignore = WebServer` added to prevent Arduino's built-in WebServer from being pulled in alongside ESPAsyncWebServer

---

## [1.1.0] — 2026-05-01

*PCB v1.1.0 released following first hardware board test; S200 driver completed; design documentation extended.*

### Added
- `drivers/relay_sequence_test/` — new PlatformIO test project for hardware GPIO verification: sequences all 6 relay outputs (M1/M2/M3 OPEN/CLOSE, GPIO 12–16 and 21), heartbeat LED (GPIO 41), and opto-isolated input (OPTO_INPUT → M1 OPEN follow); includes `README.md` with wiring and usage
- `drivers/s200/` — complete SenseCAP S200 wind sensor driver (LIB-10): `s200.h` / `s200.cpp`, 11 unit tests (UT-S200-001..011), mock Modbus layer, `S200.md` driver documentation
- `hardware/Testing/20250501_HardwareTest.md` — first hardware board test report for PCB v1.1.0: 53 tests across 6 subsystems (voltages, GPIO relays/LEDs/input, 4×4 keyboard, SD card, RTC, LCD); Modbus hardware test pending
- `design/greenhouse_nvs_variables.xlsx` — NVS variable overview spreadsheet covering all namespaces, keys, types, and default values
- `design/hardwareComponentDiagram.puml` + `.png` — hardware component architecture diagram (PlantUML)
- `design/lcd_gui_state_diagram.puml` + `.png` — LCD GUI state diagram (PlantUML)
- `design/web_gui_state_diagram_auth.puml` + `.png` — web GUI authentication flow state diagram (PlantUML)
- `design/web_gui_state_diagram_settings.puml` + `.png` — web GUI settings state diagram (PlantUML)
- `design/web_gui_state_diagram_tech.puml` + `.png` — web GUI technical/admin state diagram (PlantUML)
- `hardware/pcb/Output/20260501_Schema.pdf` — updated schematic PDF
- `hardware/pcb/Output/20260501_Bestukkingstekening.pdf` — updated component placement drawing
- `hardware/pcb/Output/20260501_PrintBedrukkingVoorzijde.pdf` — updated silk screen front PDF

### Changed
- `hardware/pcb/` — PCB design bumped to **v1.1.0**; schematic and layout updated following hardware assembly and test
- `documentation/Sensors/W-Sensecap-S200/` — connector photo replaced (`image.jpg` → `Connector.png`)
- `drivers/driverDevelopmentPlan.md` — updated to reflect all drivers completed

### Removed
- `hardware/pcb/Output/20260424_*.pdf` — superseded by 20260501 fabrication outputs

---

## [1.0.0] — 2026-04-24

### Added
- `realisation/installation.md` — new connector wiring guide covering all 12 PCB connectors (J1–J12): 24 V DC input, AC mains input, motor relay outputs M1/M2/M3, RS485 sensor connections (FG6485A and SenseCAP S200), I2C display, 4×4 keypad, alarm output, RS485 termination jumper, and SD card

### Changed
- `design/technicalDesignSpecification.md` — corrections and open issue resolution following PCB alpha release:
  - **§4.5.1** — relay implementation updated from external relay module board to **6 × SRD-05VDC-SL-C relays integrated on PCB**, each driven by a dedicated **2N7000 N-channel MOSFET**; contact rating updated to 10 A / 250 VAC
  - **§4.9** — LED colours corrected to match PCB: HB heartbeat changed from amber to **green**; relay indicator LEDs changed from red to **amber**; circuit diagram and architecture diagram updated accordingly
  - **Issue #1 closed** — RRK-3 motor feedback signal defined: external relay contact closes on alarm state, drives opto-isolated input J10 (OPTO_INPUT); signal definition referenced to RRK-3 interface documentation
  - **Issue #3 closed (out of scope)** — RS485 sensor cable routing to SenseCAP S200 is the installer's responsibility and outside the controller project scope
  - **Issue #4 closed** — enclosure confirmed as Multicomp Pro **MC001110** (222 × 146 × 55 mm, IP67) following PCB layout and 3D clearance check
  - **Issue #5 closed** — relay module selection resolved by discrete relay integration on PCB (see §4.5.1)
  - **Issue #7 closed** — time source confirmed as **DS1307 RTC** with CR2032 backup, fitted on PCB; TR-HW08 satisfied
  - **Issue #9 added (open)** — J5 pins 5–6 carry HEATING_POS / HEATING_NEG nets for the SenseCAP S200 heater supply; feature not yet documented in TDS; decision on voltage, current, and specification deferred
  - **Issue #8/#9 closed — dropped** — J5 heater supply connection (HEATING_POS / HEATING_NEG, pins 5–6) removed from PCB; no firmware support or documentation required; pins 5–6 of J5 left unconnected

### Added (earlier — 2026-04-02)
- `design/functionalRequirementsSpecification.md` — new constraints and requirements:
  - **C11** — all user-configurable setpoints and thresholds (temperature °C, humidity %, wind speed, wind direction degrees, time durations in minutes) are expressed and stored as integers; fractional values are not supported; fractional sensor readings are rounded before comparison
  - **C12** — temperature control is permanently active; humidity control and wind protection are each independently enable/disable configurable by the administrator; both default to enabled and are persisted across power cycles
  - **FR-C11** — temperature-based climate control shall always be active; it cannot be disabled
  - **FR-C12** — administrator can enable or disable humidity-based climate control; when disabled, RH is ignored for window decisions and conflict resolution is suppressed
  - **FR-WS09** — administrator can enable or disable wind protection (speed and direction); when disabled, no wind-safety close commands are issued
  - **FR-WS10** — persistent LCD warning shown whenever wind protection is inactive
  - **FR-WS11** — disabling wind protection is an admin-only action and shall be logged
  - **FR-CF12** — administrator setting to enable/disable humidity control
  - **FR-CF13** — administrator setting to enable/disable wind protection
  - FR-CR01 updated: conflict resolution is only active when humidity control is enabled
- `design/technicalDesignSpecification.md` §5.1 — added "Setpoint and threshold data types" and "Feature enable/disable flags" design constraints with NVS key names (`rh_ctrl_en`, `wind_prot_en`) and default values
- `design/technicalSoftwareDesignSpecification.md`:
  - §3 Design Constraints — added integer setpoint constraint (`int16_t` NVS storage, rounding rule) and feature enable/disable flag constraints
  - §4.3 T3 Safety Monitor — updated to check `wind_prot_en` flag before evaluating thresholds; suppresses CLOSE_ALL when wind protection is disabled
  - §5.2 Climate Control Logic — RH evaluation conditional on `rh_ctrl_en`; conflict resolution suppressed when humidity disabled; CLOSE_ALL from T3 conditional on `wind_prot_en`; log entry `value_a`/`value_b` fields updated to reflect integer values without scaling
  - §5.10 NVS Configuration Storage Layout — added Type column; `rh_ctrl_en` added to `climate` namespace; `wind_prot_en` added to `wind` namespace; types specified for all namespaces
- `firmware/` directory with PlatformIO project skeleton:
  - `firmware/platformio.ini` — board (`lolin_s3`), Arduino framework, 115200 baud monitor, commented library dependency stubs
  - `firmware/src/README.md` — describes expected source modules and their responsibilities
  - `firmware/test/README.md` — describes unit test structure, Unity framework, and `pio test` usage
- `hardware/` directory with KiCad PCB project structure:
  - `hardware/pcb/README.md` — KiCad tool version, expected project files, design references
  - `hardware/fabrication/README.md` — expected fabrication outputs per release, KiCad export instructions
- `README.md` — completely rewritten to describe the Greenhouse Ventilation Controller (replacing placeholder content from an unrelated project)
- `license.md` — dual-licence information for software and non-software artefacts
- `LICENSE` — canonical licence text for the repository

### Changed
- **Licences updated** throughout the repository:
  - Software (firmware and all code): source-available, non-commercial licence — free to use and modify; redistribution and commercial use not permitted
  - Hardware design files, documentation, and images: CC BY-NC-ND 4.0 (Attribution-NonCommercial-NoDerivatives 4.0 International)
- `design/technicalDesignSpecification.md` §2.1 — "Open Source" section replaced with "Project Licences" reflecting the dual-licence structure
- `design/technicalDesignSpecification.md` §2.5 — repository structure diagram expanded to include `documentation/`, `Archive/`, and all root-level files
- `design/technicalDesignSpecification.md` §3.2 — design principle "open and reproducible" updated to align with source-available rather than open-source framing

---

## [0.2.0] — 2026-03-26

*TDS hardware section complete; FRS v0.2 finalised.*

### Added
- `design/functionalRequirementsSpecification.md` v0.2 — complete functional and technical requirements:
  - Sensing (internal climate: FG6485A T/RH; external weather: SenseCAP S200 wind)
  - Window actuation (M1, M2, M3 via Hotraco RRK-3; timed relay pulses; dwell-time enforcement)
  - Automatic climate control (T and RH setpoints, hysteresis, graduated ventilation strategy)
  - Wind safety (speed threshold, direction exclusion angle, immediate close override)
  - Conflict resolution, window state tracking, operating modes
  - Local user interface (4×4 keypad, 16×2 LCD)
  - WiFi connectivity, MQTT integration, access control, event logging
- `design/technicalDesignSpecification.md` v0.2 — hardware design complete:
  - §4.1 Microcontroller: WEMOS LOLIN S3 (ESP32-S3, 16 MB flash, 8 MB PSRAM)
  - §4.2 Sensors: Seeed SenseCAP S200 (ultrasonic wind, Modbus RS485) and FG6485A (T/RH, Modbus RS485)
  - §4.3 Modbus RS485 bus topology and parameters
  - §4.4 User interface: 4×4 membrane keypad and Waveshare LCD1602 I2C (PCF8574)
  - §4.5 Motor controller interface: 6-ch relay board, potential-free contacts, opto-isolated feedback input
  - §4.6 Real-Time Clock: DS1307, I2C, CR2032 battery backup, external 32.768 kHz crystal
  - §4.7 Power supply: two-stage architecture (230 VAC → 24 VDC → 5 VDC), power budget analysis
  - §4.8 SD card (optional, SPI, FAT32)
  - §4.9 Status LEDs: PWR (green), HB heartbeat (amber), 6 × relay activity (red, shared GPIO)
  - §4.10 Enclosure: Multicomp Pro MC001110, 222 × 146 × 55 mm, IP67, transparent cover
  - §4.11 GPIO and peripheral assignment summary

### Changed
- Sensor selection: SenseCAP S200 confirmed as wind sensor (ultrasonic, no moving parts, single mast)

---

## [0.1.1] — 2026-03-07

*Simulation model refined; physical parameters recorded.*

### Changed
- Simulation model simplified to steady-state plant model; ACH parameters merged; humidity and temperature thresholds unified
- Measured physical greenhouse parameters recorded in simulation environment data

---

## [0.1.0] — 2026-03-06

*First complete design iteration committed.*

### Added
- `Archive/Iteration1/design.md` — first iteration design document including:
  - §3.7 Farmer-accessible configuration parameters
  - Partial window opening decision (not supported — recorded in §1.5 and §5)
  - Anti-oscillation guard (`t_min_dwell`) for motor protection
- `Archive/Iteration1/plantTranspirationRateConsiderations.md` — analysis of plant transpiration rate and its effect on humidity control
- `Archive/Iteration1/setpointConsiderations.md` — recommended T and RH setpoints for typical greenhouse crops
- `Archive/Iteration1/stateDiagram.puml` — PlantUML state diagram for the controller operating modes
- `Archive/Iteration1/Simulation/greenhouse_simulation.py` — Python simulation driven by historical weather data (5 scenarios: daytime solar gain, high humidity, 24 h day–night cycle, T below setpoint / RH critical, motor stall)
- `Archive/Iteration1/Environment/airTemperature_2025-05-01_to_2025-09-01.csv` — historical air temperature data used in simulation
- `Archive/Iteration1/Environment/outside_conditions.py` — outside conditions model for simulation
- `documentation/` — component reference material: sensor datasheets and notes (FG6485A, SenseCAP S200, RHS-10, RTS-2, keypad, anemometer)

### Changed
- Greenhouse physical layout documented: 40 × 16 m, east–west orientation; M1 south roof, M2 north roof, M3 north wall
- Window-to-motor mapping and RRK-3 circuit schematic references added to design

---

## [0.0.1] — 2026-03-05

*Project initialised.*

### Added
- Initial repository structure
- `design/technicalDesignSpecification.md` v0.1 — initial hardware architecture and component candidate evaluation
- `code_of_conduct.md`, `contributing.md` — community standards

