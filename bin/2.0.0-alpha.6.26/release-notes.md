# 2.0.0-alpha.6.26 — `/ws` is now PUBLIC (matches design intent)

## Bug fix

The WebSocket route `/ws` was gated to farmer-or-higher in alpha.6.21 (Phase 6.16-η). That was wrong: operator-visible status data — temperature, humidity, wind, window states, current mode, alarms, clock, WiFi state, sensor history, firmware version, unit ID — is **public by design**. The 1.20.3 firmware never gated `/ws`. `webUiMock/mock_server.py` does not gate `@sock.route("/ws")` either. `firmware/data/app.js:935` literally has the comment "Connect WebSocket and load sensor history immediately — both are public."

### Operator-visible symptom

User report from alpha.6.25 acceptance: "when logged out in webgui, only SD-card seems updated. the webgui states offline. Temp, Hum, Wind, Windows, Mode Alarms, Clock, WiFi, sensor history, version number and id are not restricted and open data. sensor logdat is not populated either."

Root cause: after logout the browser cleared the session cookie. The WS reconnect attempt hit the alpha.6.21 gate, returned 401 at the upgrade, and `ws.onclose` fired → `setBadge('ws-badge', 'Offline', 'offline')`. The dashboard's "live tile" rendering hangs off the WS message stream, so once WS dropped every live value froze even though the underlying REST endpoints (`/api/status`, `/api/history`) were still public and answering. `/api/sd/status` happens to be polled on a separate timer that doesn't depend on the WS health flag, so SD status alone kept refreshing — exactly the symptom reported.

### Fix

Removed the `require_auth(req, WEB_ROLE_FARMER)` call from the `HTTP_GET` upgrade branch of `ws_handler`. The frame-receive branch (called per inbound frame after upgrade) was already auth-free since the dashboard never sends WS payloads — only drains for protocol compliance. Updated the docstring to spell out the public-data design contract and explain why the gate was a mistake.

```c
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* GET = upgrade handshake. No auth gate. */
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "[T11] /ws upgrade fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    /* … frame-drain branch unchanged … */
}
```

Sensitive surfaces stay gated as before: `POST /api/config` (setpoint writes), `POST /api/wifi` (credentials), `POST /api/pin` (PIN change), all OTA endpoints (`/api/ota/firmware`, `/api/ota/assets`, `/api/ota/status`), and the log-download endpoints (`/api/log/files`, `/api/log/download`).

## Acceptance — hardware verified on 192.168.20.160

Curl with no cookie, no Authorization header, no session of any kind:

```
GET /ws (Upgrade: websocket, Sec-WebSocket-Version: 13, valid Key)
  → HTTP/1.1 101 Switching Protocols
    Sec-WebSocket-Accept: OBSRQ41Cm6rKLUwPGIPxa3yEszc=

  10-second window: 5 push frames received, exactly 2.000 s apart
  (uptime_s = 166, 168, 170, 172, 174)
```

Per push, every data point the user listed as "open data" is in the payload:

| Field          | Source                                |
|----------------|---------------------------------------|
| Temp           | `climate.temp_c`, `temp_avg_c`        |
| Hum            | `climate.rh_pct`, `rh_avg_pct`        |
| Wind           | `wind.speed_ms`, `direction_deg`      |
| Windows        | `windows.M1/M2/M3`                    |
| Mode           | `mode.current`                        |
| Alarms         | `mode.flags[]`, `system.eg1`          |
| Clock          | `system.time_iso`, `ts_unix`          |
| WiFi           | `system.wifi_ip`, `wifi_rssi_dbm`     |
| Version        | `system.fw_ver`                       |
| ID             | `system.unit_id`                      |
| Sensor history | `GET /api/history` (already public)   |

## What changed

- **`firmware/src/web_server/web_server.cpp`** — dropped the `require_auth` call from the WS upgrade branch (~8 lines); docstring rewritten to document the public-data design contract and capture the failure-mode story.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.26`.

## Carried forward — asset_version mismatch flag

After this flash the device shows `fw_ver=2.0.0-alpha.6.26` but `asset_version=2.0.0-alpha.6.25` (the version stamped into the active LittleFS manifest by the alpha.6.25 asset upload). The dashboard's MISMATCH badge will fire. This is the **intended behaviour** of the mismatch detector: a firmware OTA without a paired asset OTA leaves the manifest at the old version.

Either:
- Upload a fresh alpha.6.26 web-assets ZIP (rebuild via `bin/build_release.ps1`, then `POST /api/ota/assets`) — clears the badge
- Or accept the cosmetic badge until the next alpha that needs asset changes

The alpha.6.25 assets are forward-compatible with alpha.6.26 firmware (no API surface changes); only the manifest version differs.

## Build delta vs alpha.6.25

| Metric | alpha.6.25 | alpha.6.26 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 307 168 B | 1 307 152 B | -16 B |
| RAM static | ~60 256 B | ~60 256 B | unchanged |

bin sha256: `E2B265B18289D5F4…`

-16 B because we removed code (one `require_auth` call + the surrounding `if`).
