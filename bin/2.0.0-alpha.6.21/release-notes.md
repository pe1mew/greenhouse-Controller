# 2.0.0-alpha.6.21 — Phase 6.16-η (T11 /ws WebSocket — final T11 route)

## What landed

The **last** T11 route. **T11 surface complete at 25 / 25 (= 100 % of the v1.20.3 plan).**

| Route | Method | Role gate | Purpose |
|---|---|---|---|
| `/ws` | GET (Upgrade) | farmer+ | Subscribes to the live status stream — same canonical JSON as `GET /api/status`, pushed every 2 s |

## Architecture

- **Auth gate at upgrade time only.** `ws_handler` checks `require_auth(req, WEB_ROLE_FARMER)` on the initial GET; once the upgrade completes, the subscription persists until the client disconnects. Matches 1.20.3 (which also did not re-verify per push).
- **Push runs in its own task** (`task_ws_push`, 4 KB stack, pinned to core 1). Off the httpd worker pool so a slow `dm_status_snapshot()` / `build_canonical_status_json()` can't block concurrent HTTP requests.
- **Client tracking via esp_http_server's own list** (`httpd_get_client_list` + `httpd_ws_get_fd_info`). No parallel fd table — stale fds prune themselves: `httpd_ws_send_frame_async` returns an error code for a closed socket and the push loop simply skips it.
- **Skip-when-idle.** If no client is connected, the push task does NOT call `dm_status_snapshot()` or `build_canonical_status_json()`. A typical deployed greenhouse has the dashboard tab closed most of the time; this keeps the snapshot+build cost off the hot path.
- **WS_PUSH_MS = 2000** — identical cadence to 1.20.3.
- **JSON shape and `expose` policy identical to GET /api/status**: `STATUS_EXPOSE_ALL`, `include_disabled_setpoints=true` (local-UI mode).

## sdkconfig change required

`CONFIG_HTTPD_WS_SUPPORT=y` is disabled by default in ESP-IDF 5.5. Added explicitly to `firmware/sdkconfig.defaults`; the auto-generated `sdkconfig.lolin_s3` is gitignored and gets regenerated on each clean build. The toggle pulls in ~6.9 KB of WS framing + handshake code (`flash 1 296 481 → 1 303 385 B`, +6 904 B vs alpha.6.20) and unlocks the `httpd_uri_t.is_websocket` field plus the `httpd_ws_*` API.

The first build attempt after the WS handler edit failed with `'httpd_ws_frame_t' was not declared in this scope` because PlatformIO had cached the prior sdkconfig (where WS support was off). Deleting `sdkconfig.lolin_s3` and letting PlatformIO regenerate from `sdkconfig.defaults` fixed it. Documented in the release notes so future migrators know the pattern.

## Acceptance — hardware verified on 192.168.20.160

### Upgrade handshake — admin cookie

```
GET /ws HTTP/1.1
Host: 192.168.20.160
Cookie: session=4be5fdace5fc3f71
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: dGVzdC13ZWJzb2NrZXQta2V5MTI=

→ HTTP/1.1 101 Switching Protocols
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Accept: kTjCX124s2JzwSqlutGQ3yTMyaE=
```

### Push cadence verified

Two pushes captured during a 5 s curl window:

```
uptime_s=60  → first push received
uptime_s=62  → second push received   (exactly +2 s — WS_PUSH_MS honoured)
```

### Push payload — same JSON as /api/status

```json
{
  "type": "status",
  "climate": {"temp_c":36.0,"temp_avg_c":36.0,"rh_pct":66,…},
  "wind":    {"speed_ms":3.6,"direction_deg":258,…},
  "windows": {"M1":"CLOSED","M2":"CLOSED","M3":"MOVING_CLOSE"},
  "mode":    {"current":"WINDOW_CAL","flags":["calibrating"]},
  "sun":     {"is_daytime":true,"sunrise_min":341,"sunset_min":1292},
  "system":  {"unit_id":"2344","wifi_ip":"192.168.20.160",
              "wifi_rssi_dbm":-54,"ntp_synced":true,
              "fw_ver":"2.0.0-alpha.6.21","uptime_s":60,…},
  "update_interval_s": 240
}
```

### Role gate verified — farmer cookie also succeeds

```
GET /ws (farmer session)  → HTTP/1.1 101 Switching Protocols ✓
                            Sec-WebSocket-Accept: ZxbsKQFyR24BdBi40U2UYDmcR98=
                            push received within 2 s of upgrade
```

### Liveness after disconnect

After the curl `--max-time` 5 s expires and the WS connection drops:

```
GET /api/whoami → 401 in 241 ms  (httpd worker still healthy)
GET /api/status → uptime_s=183   (system stable)
```

No stale fd accumulation, no heap leak symptom. The push task's `httpd_ws_send_frame_async` returned an error code for the dropped fd and the loop continued to the next client.

## Artifacts

| File | Bytes | Notes |
|---|---|---|
| firmware-2.0.0-alpha.6.21.bin | 1 304 064 | Flash @ 0x20000 |
| firmware-2.0.0-alpha.6.21.elf | 12 655 920 | For `addr2line` if needed |
| partitions.bin | 3 072 | Same partition table as alpha.0 |
| bootloader.bin | 22 528 | Regenerated (was identical to alpha.6.20 by sha256) |

bin sha256: `83CE2D0213AAEF47…`

Flash usage: **1 303 385 B (62.2 %)** of the 2 MB OTA bank.
RAM usage:  **60 232 B (18.4 %)** of 320 KB DRAM.

## Phase 6.16 retrospective

T11 (web server) migration complete:

| Sub-phase | Tag | Routes added | Cumulative |
|---|---|---|---|
| α | alpha.6.16 (Set-Cookie fix in .16.1) | 4 static + 3 auth | 7 |
| β | (collapsed into α) | — | 7 |
| γ | alpha.6.17 (status_snapshot fw[24] fix in .17.1) | 2 status | 9 |
| δ | alpha.6.18 | 5 config + admin | 14 |
| ε | alpha.6.19 | 5 SD + log | 19 |
| ζ | alpha.6.20 (ota_get_state NULL-mutex fix) | 5 OTA + web-tab | 24 |
| η | **alpha.6.21** | 1 WebSocket | **25 / 25** |

ESPAsyncWebServer / AsyncWebSocket entirely retired. `web_server_1.20.3_original.cpp.archived` can stay in tree for a few more alphas as a porting reference, then drop in Phase 6.N consolidation.

## Deferred to Phase 6.N

- T1 watchdog task (currently no task subscribes to TWDT)
- `ota_check_rollback()` boot wiring (currently never called; the fail counter is dormant)
- `main.cpp` replaces `app_main_stub.cpp` (the stub still has a few Phase-X TODOs)
- Delete `web_server_1.20.3_original.cpp.archived`, `status_post_1.20.3_original.cpp.archived`, `network_manager_1.20.3_original.cpp.archived`
- 14-day soak on bench unit → 2.0.0-rc.1
