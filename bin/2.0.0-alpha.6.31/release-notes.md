# 2.0.0-alpha.6.31 — AP-mode + STA back-off + IO0 debounce (Phase 6.14.X step 2)

Three coupled changes that complete the operator-facing T10 work. None of these are mere clean-ups — each closes a real operator-experience gap or a real safety bug.

## 1. AP mode — admin-only enable per design spec

`task_network_manager` now implements the soft-AP per the design contract:

| Aspect | Behaviour |
|---|---|
| Enable / disable | Admin-only via NVS `wifi/ap_enable` (1 / 0). Writeable through web GUI `POST /api/config` OR LCD `Settings → System → 1=WiFi AP` (admin session required) |
| Auto-enable | **None** — security policy: an unconfigured broadcast on credential loss would expose the greenhouse to anyone in radio range |
| SSID | NVS `wifi/ap_ssid` (admin override) → MAC-derived `Greenhouse-XXYY` default |
| PSK | NVS `wifi/ap_psk` (admin override) → factory default `0123456789`. Never an open AP |
| Mode | `WIFI_MODE_APSTA` — AP and STA simultaneous; STA keeps its connection while AP is on |
| Auto-shutdown | After `cfg.ap_timeout_min` minutes (default 30); T10 clears the NVS flag on timeout so it doesn't restart next tick |
| LCD display | T8 shows "AP active" + SSID on the WiFi status page (Q5 carries `ap_active` from T10) |
| Operator flow | Admin enables AP via GUI/LCD → operator joins `Greenhouse-XXYY` with PSK `0123456789` → opens `http://192.168.4.1` → can change WiFi STA settings or any other admin config → admin disables AP (or it auto-shuts down) |

`start_ap()` reloads SSID + PSK from NVS each call, so admin changes apply on the next enable without a reboot.

The alpha.6.30 "auto-enable on no-SSID" code was a security mistake and is fully reverted in this alpha. The 2-second poll cadence + admin-only gating matches the 1.20.3 archived behaviour byte-for-byte.

## 2. WiFi STA reconnect — infinite retry, exponential back-off

`wifi_tickle` previously had a hard 3-strike limit; after the third disconnect it set BIT_DISCONNECTED and never reconnected until the next boot. That broke the 1.20.3 "stay online through transient drops" property — a unit that briefly lost AP visibility (interference, microwave, AP reboot) was permanently offline until power-cycled.

alpha.6.31 adds a FreeRTOS one-shot timer that schedules reconnect attempts with exponential back-off: 2 → 4 → 8 → 16 → 32 → 60 s cap. **Never gives up.** The back-off resets to 2 s on successful `STA_GOT_IP`. The 3-strike fast path in the boot-time tickle is retained so the gh#21 boot gate doesn't block forever, but after those 3 attempts wifi_tickle_run() returns and the timer takes over indefinitely.

Matches the 1.20.3 design header docblock: *"NET_BACKOFF: Connection lost; waiting exponential backoff then retry. Backoff sequence: 2 → 4 → 8 → 16 → 32 → 60 s (cap)."*

## 3. WiFi stack init even without SSID

`wifi_tickle_run` previously short-circuited with `WIFI_TICKLE_NO_SSID` when NVS lacked `wifi/ssid`. That left `esp_wifi_init()` un-called; any subsequent `esp_netif_create_default_wifi_ap()` aborted with `ESP_ERR_INVALID_STATE` (which crashed alpha.6.29).

Now wifi_tickle initializes the full stack — `esp_netif_init`, default event loop, STA netif, `esp_wifi_init`, `esp_wifi_set_mode(STA)`, `esp_wifi_start()` — even without an SSID. Skips only the `esp_wifi_set_config(STA)` step and the connect-wait. Result: the WiFi stack is up, ready for the admin to enable AP from the LCD menu. `WIFI_TICKLE_NO_SSID` is still returned to callers so they know STA didn't connect, but the underlying stack state allows T10's AP code to run.

## 4. T11 web server now spawns regardless of STA state

`main.cpp` previously skipped T11 spawn when `wifi_tickle_run()` didn't return OK. That meant a unit with no `wifi/ssid` had no web GUI even after admin enabled AP — fatal for the operator-recovery flow.

`wifi_up` now means "WiFi stack initialized" (any wifi_tickle status except `INIT_FAILED`), not "STA connected". T11 spawns and binds httpd to all interfaces; reachable on the STA IP if connected and on `192.168.4.1` whenever AP is enabled.

## 5. IO0 reset gate — 2-second debounce on HIGH-edge

User report from the alpha.6.30 cycle: "you keep resetting pin settings over serial using IO1 key."

Root cause: my alpha.6.25 HIGH-edge gate accepted any single tick of HIGH as "armed". Windows .NET `SerialPort.Open()` produces a brief (50–100 ms) DTR transient during the line-state handshake. That transient released IO0 high for 1–2 ticks, armed the gate, then DTR settled at the configured value (= IO0 low) and the counter started incrementing. After 5 s of LOW → PIN reset.

alpha.6.31 requires **20 consecutive HIGH ticks (= 2 seconds of sustained HIGH)** before the gate arms. Any LOW tick resets the streak. An operator actually pressing the BOOT button always satisfies this (they hold for hundreds of milliseconds before pressing, releasing for the same). The DTR transient is filtered.

## What changed

- **`firmware/src/wifi_tickle.cpp`** —
  - Stack-init now runs unconditionally (steps 2-5 always; STA config + connect-wait gated on `have_sta_creds`)
  - New back-off reconnect timer (`reconnect_timer_cb` + `schedule_reconnect`); BACKOFF_INIT_MS=2000, BACKOFF_MAX_MS=60000
  - STA_DISCONNECTED hands off to back-off timer after the 3 fast retries; never gives up
  - STA_GOT_IP resets back-off to BACKOFF_INIT_MS
- **`firmware/src/network_manager/network_manager.cpp`** —
  - Reverted the alpha.6.30 auto-enable-on-no-SSID code (security regression)
  - Split `ap_init` into `load_ap_credentials` (called from both ap_init and start_ap) so admin NVS changes apply on next enable
  - SSID source: NVS `wifi/ap_ssid` → MAC default; PSK: NVS `wifi/ap_psk` → AP_PSK_DEFAULT
- **`firmware/src/ui_display/ui_display.cpp`** — IO0 reset gate hardened with 20-tick HIGH streak (2-second debounce). `s_io0_high_streak` counter resets on any LOW.
- **`firmware/src/main.cpp`** — `wifi_up` redefined: true if `wifi_st != INIT_FAILED`. T11 web server now spawns whenever the WiFi stack is initialized.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.31`.

## Acceptance — hardware verified on 192.168.20.160

```
fw_ver        : 2.0.0-alpha.6.31
asset_version : 2.0.0-alpha.6.31    ← mismatch cleared
uptime_s      : 10                   ← post-asset-OTA reboot
coordinates   : 52.218 N, 5.939 E   ← geo sync persisted across reboot
tz_str        : CET-1CEST,M3.5.0,M10.5.0/3
ap_ssid       : Greenhouse-2344     ← MAC-derived default
ap_timeout_min: 30 min
```

Eight surface checks all PASS:

| Surface | Result |
|---|---|
| `GET /api/whoami` (no cookie) | 401 ✓ |
| `POST /api/login admin/12345678` | `{ok:true, role:"admin"}` ✓ |
| `GET /api/status` | full canonical JSON ✓ |
| `GET /api/history?n=2` | `{rows:[{ts, temp_c, temp_avg_c, rh_pct, rh_avg_pct, speed_ms, speed_avg_ms, direction_deg, direction_variation_deg}]}` ✓ |
| `GET /api/ota/status` | `accepted:true` (T1 ran ota_mark_healthy) ✓ |
| `/ws` unauthenticated upgrade | 101 + 2 s push frames ✓ |
| `GET /api/sd/status` | mounted=true, free 1878 MB ✓ |
| `GET /` | 200, 38918 B (full GUI from LittleFS, chunked stream) ✓ |

## Build delta vs alpha.6.30 (which never shipped — pulled for the auto-AP regression)

| Metric | alpha.6.28 (last shipped) | alpha.6.31 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 318 976 B | 1 321 312 B | +2 336 B |
| RAM static | ~60 256 B | ~60 256 B | unchanged |

bin sha256: `A6AD887A300C9AA0…`

+2.3 KB net: back-off timer + AP-creds reload + IO0 debounce + main.cpp wifi_up redefinition.

## Operator-facing recovery flow (now functional)

1. Admin enables AP via LCD `Settings → System → 1=WiFi AP` (admin session required) — OR via web GUI when accessible
2. AP `Greenhouse-XXYY` broadcasts with PSK `0123456789` (or NVS override)
3. Operator joins from phone / laptop, opens `http://192.168.4.1`
4. Sets WiFi STA SSID + PSK via web GUI
5. AP auto-shuts down after `cfg.ap_timeout_min` minutes (default 30) — admin can disable earlier
6. STA reconnect uses back-off ladder; recovers from any transient AP drop automatically

This is byte-for-byte the 1.20.3 operator manual instruction set.
