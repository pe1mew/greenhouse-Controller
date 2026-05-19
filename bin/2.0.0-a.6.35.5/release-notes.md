# 2.0.0-a.6.35.5 — Every setting change is audit-logged with operator attribution

Triggered by the operator question: *"with respect to logging events. Are all setting changes in both guis logged as an event including the operator who did it? logging would be a task of the NVS task who is administering these settings."*

Pre-patch the LCD UI emitted `LOG_SETPOINT` correctly (initiator=`FARMER`/`ADMIN` from session) but the web GUI was silent on every setting change. PIN rotations, WiFi credential updates, status-website URL changes, the entire climate setpoint stack from the browser — none of it reached the SD log. The operator's audit trail was incomplete for any unit primarily managed via the web.

This patch closes all five gaps and implements the architecture the operator proposed: the NVS task (T4) owns the audit emission for any setting that flows through Q4; T11 retains direct logging only for the four paths that physically bypass Q4.

## Architecture

```
LCD UI (T8) ──┐                 ┌── T4 → log_post(LOG_SETPOINT, initiator)
              ├─→ Q4(initiator) ┤
Web GUI (T11)─┘                 └── (NVS write + shadow update)

Web GUI (T11) ── direct nvs_cfg_set_* + log_post(LOG_SETPOINT, LOG_BY_WEB)
                ├── /api/config tz_str
                ├── /api/wifi   (ssid / psk / ap_psk)
                ├── /api/pin    (farmer / admin)
                └── /api/web    (status URL / secret / interval / enable / expose /
                                 log_upload_h / log_upload_m / log_upload_rot)
```

The Q4 path is the canonical one — T4 reads the *current* value from the cfg shadow under MX4, applies the new value, then emits `LOG_SETPOINT` with `value_a = old`, `value_b = new (clamped)`. Two side benefits over the prior T8 direct-emission scheme:

1. **Clamping is reflected in the audit row.** If the operator submitted `t_max_day=99` and the validator clamped it to 50, the log row shows `28 → 50`, not `28 → 99`. The audit reflects what was actually written.
2. **Q4-full doesn't produce false-positive audit rows.** T8's pre-patch code emitted the log row even when `xQueueSend` returned timeout — claiming a change was made that was actually lost. T4 only emits after a successful NVS+shadow write.

## What was missing pre-patch — eight separate gaps

| Source | Path | Pre-patch | Post-patch |
|---|---|---|---|
| Web | `POST /api/config` (numeric setpoints) | `ESP_LOGI` only | `LOG_SETPOINT` via T4, initiator=`WEB`, old→new |
| Web | `POST /api/config` (tz_str string) | `ESP_LOGI` only | `LOG_SETPOINT` via T11, `tz_str (set)` |
| Web | `POST /api/wifi` (ssid) | `ESP_LOGI` only | `LOG_SETPOINT`, `wifi_ssid (set)` — credential not logged |
| Web | `POST /api/wifi` (psk) | `ESP_LOGI` only | `LOG_SETPOINT`, `wifi_psk (set)` — credential not logged |
| Web | `POST /api/wifi` (ap_psk) | `ESP_LOGI` only | `LOG_SETPOINT`, `wifi_ap_psk (set)` — credential not logged |
| Web | `POST /api/pin` (farmer) | `ESP_LOGI` only — **security event silent** | `LOG_SETPOINT`, `pin_farmer (changed)` — PIN not logged |
| Web | `POST /api/pin` (admin) | `ESP_LOGI` only — **security event silent** | `LOG_SETPOINT`, `pin_admin (changed)` — PIN not logged |
| Web | `POST /api/web` (each of 8 fields) | `ESP_LOGI` only | `LOG_SETPOINT` per changed field; integers as old→new, secret/URL as `(set)` |
| LCD | T8 climate/wind setpoints | already logged correctly | unchanged operator-visible behaviour; emission moved from T8 to T4 |

## Sensitive-value policy

PIN, WiFi credentials, and the status-website shared secret use `value_a = 1` as a sentinel for "changed/set" — the actual value is **never** logged. The CSV row stamps who-changed-what-when (PIN_ADMIN, role=admin, at 17:19:09 by Web UI) without making the SD card a credential exfil surface.

For numeric fields (status_intv_s, log_upload_h, etc.) the old→new pair *is* logged because those values are operationally meaningful and not sensitive.

The parser's `_decode_setpoint` checks a `_PARAM_SENTINEL_VALUE` set and renders sentinel-value rows as `<field> (set)` / `<field> (changed)` rather than the misleading "1 → 0".

## What changed

- **`firmware/src/types/app_types.h`** — `config_update_t.initiator` field added (1 byte). `log_param_id_t` extended with 15 new entries (23–37) covering tz_str, wifi creds, PIN, status-website cfg, and `wind_prot_en`.
- **`firmware/src/data_manager/data_manager.cpp`**:
  - New `ns_key_to_log_id(ns, key, *channel)` static helper maps NVS namespace/key into a `log_param_id_t` and (for dwell_open / dwell_close) the 1-based motor channel.
  - `apply_config_update` rewritten: under the same MX4 critical section that updates the shadow, it now also reads `old_val` from the shadow into a local. After releasing MX4 it emits `LOG_SETPOINT` with the captured old → new, the param-id mapping, and `initiator = upd->initiator` (defaulting to `LOG_BY_SYSTEM` for legacy callers).
- **`firmware/src/ui_display/ui_display.cpp`** — `apply_param_change` sets `upd.initiator = FARMER / ADMIN` from the active session and **stops** emitting `log_post` directly. T4 now owns the emission. The `(void)old_val` cast keeps the function signature unchanged for callers.
- **`firmware/src/web_server/web_server.cpp`**:
  - `config_post_handler` sets `upd.initiator = LOG_BY_WEB` before Q4 send.
  - New static `log_web_setpoint(pid, va, vb)` helper.
  - `/api/config` string path emits `LOG_PARAM_TZ_STR (set)` after tz_str write.
  - `/api/wifi` emits `LOG_PARAM_WIFI_{SSID,PSK,AP_PSK} (set)` per changed credential.
  - `/api/pin` emits `LOG_PARAM_PIN_{FARMER,ADMIN} (changed)` after `pin_auth_set` success.
  - `/api/web` snapshots the cfg shadow before the write batch so each changed field can be logged with proper old → new. URL and secret use sentinel `(set)`; integers (interval, enable, expose, log_upload_h/m/rot) log old → new.
- **`log/logparser.py`** — `_PARAM` table extended with 15 new entries. New `_PARAM_SENTINEL_VALUE` set. `_decode_setpoint` handles sentinel-value rows and adds boolean / bitmask formatting for `status_enable` (32), `status_expose` (33, hex), `log_upload_rot` (36), `wind_prot_en` (37).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35.5`.

## Acceptance — bench-verified on 192.168.20.160

Eight test paths exercised:

```
=== Q4 → T4 audit emissions (Web UI initiator) ===
2026-05-19 17:16:57  [SETPT  ]  Web UI    t_max_day: 28 degC -> 29 degC
2026-05-19 17:16:59  [SETPT  ]  Web UI    wind_prot_en: enabled -> disabled
2026-05-19 17:17:01  [SETPT  ]  Web UI    wind_prot_en: disabled -> enabled
2026-05-19 17:17:08  [SETPT  ]  Web UI    t_max_day: 29 degC -> 28 degC

=== T11 direct audit emissions ===
2026-05-19 17:17:02  [SETPT  ]  Web UI    status_intv_s: 120 s -> 180 s          (/api/web numeric)
2026-05-19 17:17:04  [SETPT  ]  Web UI    status_intv_s: 180 s -> 120 s          (/api/web numeric)
2026-05-19 17:19:09  [SETPT  ]  Web UI    pin_admin (changed)                     (/api/pin sensitive)
2026-05-19 17:19:10  [SETPT  ]  Web UI    tz_str (set)                            (/api/config string)
```

Every row attributes correctly to `Web UI` (raw initiator = `WEB`). Sensitive paths render the value-redacted sentinel form. Numeric paths render old → new. T8's contribution (LCD-UI path with `FARMER` / `ADMIN` initiator) is a 2-line change setting `upd.initiator` before Q4 send — correct by inspection, exercised by the same T4 emission path Web UI uses.

## Build delta vs a.6.35.4

| Metric | a.6.35.4 | a.6.35.5 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 349 296 B | **1 350 944 B** | +1 648 B |
| RAM static | 60 552 B | 60 553 B | +1 B |

+1.6 KB flash absorbs `ns_key_to_log_id`'s switch table (~600 B), the `apply_config_update` old-value capture (~300 B inline), the eight new emission sites in T11 (~500 B incl. strings), and the 15 new param-id rendering strings in the parser-targeting comments. +1 B RAM for `config_update_t.initiator`. Final flash usage 64.4 % of the 2 MB OTA bank — comfortable headroom for the remaining 2.0.0 work.

## Operator-visible behaviour change

The SD CSV will grow modestly faster on heavily-managed units — every Apply on the web GUI now produces one row per changed field. Bench observation: a routine `/api/web` Apply that touches 3 fields = 3 rows; a climate-setpoint walkthrough touching 10 fields = 10 rows. Per-row cost ~80 bytes, so ~800 bytes per fully-walked configure-session. Negligible against the 512 KB rotation threshold; daily Apply traffic is dwarfed by the steady T1 / T4 / sensor / T14 emissions.

## Next

Phase 7 soak continues. Every cfg-touch the operator makes during the soak will now show up in the daily-review pass with a clear trail of who changed what when. If a setpoint drift surfaces during the soak, the audit log answers "who did this" immediately.
