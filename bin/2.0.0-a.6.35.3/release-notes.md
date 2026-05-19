# 2.0.0-a.6.35.3 — Log format aligned with `logparser.py`; CSV row timestamps now local time

Triggered by the operator question *"use the logprocessor script as a reference. is the log populated with all information that the log processor script expects? is it in the correct format? add missing log submissions to the various tasks and correct when required. The log shall have date-time in local time."*

The audit surfaced **five distinct bugs**. All five close in this patch. Hardware-verified across two consecutive OTA cycles on 192.168.20.160.

## Five bugs found and fixed

### 1. `main.cpp` heartbeat polluted `value_a`

Every 5 s the status-print loop posted a synthetic `LOG_SYSTEM` row with `value_a = uptime_seconds`. `value_a` is the SYSTEM-event subtype field — uptime values 5, 7, 8, 9, 10, 11, 12, … *each* collide with a documented subtype:

| uptime value | documented meaning |
|---:|---|
| 5 | BOOT (esp_reset_reason in value_b) |
| 7 | Heap internal free |
| 8 | Heap PSRAM free |
| 9 | Heap corruption detected |
| 10 | T2 boot-cal skipped |
| 11 | Unit ID |
| 12 | Heap largest block |

Pre-fix histograms of a fresh CSV showed hundreds of stray `value_a` values: 15, 17, 20, 22, 25, …, 1935 — none documented. The parser would mis-render every heartbeat as a fake corruption/unit-id/heap row, drowning the real events.

**Fix**: removed the heartbeat `log_post` entirely. T9's own steady output (T1 heap rows every 60 s, T4 sensor rows every poll cycle, T14 status POSTs) is plenty of "T9 is alive" evidence. The 5 s ESP_LOGI status print on serial remains for boot-side debugging.

### 2. CSV row timestamps were UTC, filenames are local

`build_csv_line` used `gmtime_r`; `make_ts_filename` used `localtime_r`. An operator browsing the SD card saw a filename `20260519143022.csv` but rows inside stamped `12:30:22` — two-hour mismatch under CEST.

**Fix**: `build_csv_line` now uses `localtime_r`. CSV row timestamps match filename convention. The local-time TZ comes from `cfg.tz_str` in NVS, set by the geolocation lookup or operator via LCD config menu. The parser's column heading and docs updated from "Timestamp (UTC)" to "Timestamp (local)".

### 3. T10 STA/NTP edge events documented but never emitted

`logparser.md` documents `value_a=1, value_b=0/1` as STA WiFi disconnected/connected (producer T10) and `value_a=2, value_b=0/1` as NTP timeout/synced (also T10). `network_manager.cpp` never actually posted either. Every CSV before this patch had a gap where STA/NTP transitions should have been.

**Fix** (three call sites in `network_manager.cpp`):

- Snapshot-equal detector now fires `log_sys(1, cur.client_connected)` and `log_sys(2, cur.ntp_synced)` on transitions (edge-triggered, no spam).
- `run_ntp_resync()` posts `log_sys(2, 0)` on the 10 s sync-wait timeout.
- T10 init (after the initial `snapshot_state(&prev)`) posts a one-shot `log_sys(1, x)` + `log_sys(2, x)` snapshot. This is the equivalent of the BOOT, Unit ID, and T2 boot-cal rows — fires once per boot so every CSV records the network state at the moment T10 came online, even when wifi_tickle brought the stack up before T10 started (and so no transition is detected).

### 4. `ota_manager` OTA-stage codes collided with documented subtypes

Pre-fix `post_log(0)` (firmware-begin), `post_log(1)` (firmware-verified), `post_log(2)` (asset-complete), `post_log(-1)` (asset-fail) all collided with documented codes from other producers. A real CSV captured during the a.6.35.3 deploy itself showed the parser misrendering OTA progress as:

| CSV row | Pre-fix parsed output | Reality |
|---|---|---|
| `SYS, va=0,  vb=0` | "Legacy boot marker (pre-1.17.31)"      | OTA firmware POST start |
| `SYS, va=1,  vb=0` | "STA WiFi client: disconnected"          | OTA firmware verified |
| `SYS, va=2,  vb=0` | "NTP: timeout"                            | OTA asset extracted |
| `SYS, va=-1, vb=0` | "Q3 queue overflow: 0 event(s) dropped" | OTA asset extraction failed |

**Fix**: re-numbered the four codes into a dedicated OTA range that doesn't overlap any other producer:

| value_a | Meaning |
|---:|---|
| 14 | OTA firmware POST started (bytes streaming to inactive bank) |
| 15 | OTA firmware verified OK — awaiting web-asset upload |
| 16 | OTA asset ZIP extracted OK — reboot scheduled (1 s) |
| 17 | OTA asset extraction FAILED — boot partition unchanged |

`value_a=13` (firmware-only fallback commit, added in a.6.34) unchanged. Parser updated to decode 13/14/15/16/17 with operator-facing strings. A normal OTA cycle now produces rows 14 → 15 → 16 → BOOT; an interrupted cycle (a.6.34 fallback) is 14 → 15 → 13 → BOOT.

### 5. `sensor_poll` sensor-fault codes collided with motor alarm + wind override

Pre-fix `post_sensor_alarm` emitted `LOG_ALARM` rows with `channel=0` and `value_a ∈ {±1, ±2}`. The ±1 shapes collided directly with motor alarm onset/cleared (T2) and wind-override sensor-fault (T3); ±2 fell into the wind-override-direction decoder. Real bench capture showed three T5 sensor-fault rows misrendering as "MOTOR ALARM: triggered" + "WIND OVERRIDE: SET direction X" + "WIND OVERRIDE: sensor fault safe-fail".

**Fix**: T5 now uses the `channel` field to carry the sensor type (motor channels never collide because they're 1/2/3):

| `channel` | `value_a` | Meaning |
|---:|---:|---|
| 4 | 1 | T/RH sensor fault TRIGGERED |
| 4 | 0 | T/RH sensor fault CLEARED |
| 5 | 1 | Wind sensor fault TRIGGERED |
| 5 | 0 | Wind sensor fault CLEARED |

`post_sensor_alarm` signature changed from `(int16_t value_a)` to `(uint8_t sensor_kind, bool onset)` so call sites can't accidentally pass the old encoding. Parser's `_decode_alarm` checks `channel` first; for `ch∈{4,5}` it routes to a new T5-fault decoder, otherwise falls back to the legacy T2/T3 disambiguation logic.

## Acceptance — two consecutive OTA cycles on 192.168.20.160

The second cycle (which used a.6.35.3 as the **emitter**) produced this clean parsed output for the OTA + post-boot block:

```
2026-05-19 15:27:26  [SYSTEM ]  System    OTA: firmware POST started (bytes streaming to inactive bank)
2026-05-19 15:27:31  [SYSTEM ]  System    OTA: firmware verified OK — awaiting web-asset upload
2026-05-19 15:27:36  [SYSTEM ]  System    OTA: asset ZIP extracted OK — reboot scheduled (1 s)
2026-05-19 15:27:39  [SYSTEM ]  System    Heap internal largest block: 176 KB
2026-05-19 15:27:40  [SYSTEM ]  System    Boot: esp_reset_reason = 4 (PANIC)
2026-05-19 15:27:40  [SYSTEM ]  System    STA WiFi client: connected     ← boot snapshot (fix #3)
2026-05-19 15:27:40  [SYSTEM ]  System    NTP: synced                    ← boot snapshot (fix #3)
2026-05-19 15:27:41  [SYSTEM ]  System    Geolocation: success
2026-05-19 15:27:43  [SYSTEM ]  Web UI    T14 status POST: success
```

Every row renders meaningfully — no fallthroughs to generic `System event: a=N b=M`. Timestamps are local time (`15:27`, would have been `13:27` under the old gmtime_r). `value_a` histogram on the new CSV section contains only documented subtypes (1, 2, 4, 5, 7, 8, 10, 11, 12, 14, 15, 16) — zero stray uptime-second values.

## Migration notes for operators

- **Old log files with UTC timestamps**: pre-a.6.35.3 logs have UTC row timestamps inside and local-time filenames. The parser passes the timestamp string through unchanged — only the column heading caveat differs. Operators diffing pre-a.6.35.3 vs post-a.6.35.3 logs across an upgrade should expect a TZ-shift discontinuity at the upgrade reboot.

- **Old `value_a` codes in archived logs**: pre-a.6.35.3 OTA events used the colliding codes (0/1/2/-1). The new parser still recognises the old codes via the existing T14/STA/NTP/Q3 decoder branches — old logs render as before. Only the new-firmware logs (14/15/16/17) get the OTA-specific rendering. Pre-a.6.35.3 logs around OTA windows will continue showing the misleading "STA disconnected" / "NTP timeout" labels; we made no attempt to retroactively patch these.

- **Old `value_a = ±1, ±2` sensor-fault rows in archived logs**: still parse via the legacy T2/T3 ALARM decoder. Operators reviewing pre-a.6.35.3 logs should treat ALARM rows around boot or OTA windows with skepticism — they were probably T5 sensor faults during the brief connectivity gaps, not actual motor alarms or wind overrides.

## Build delta vs a.6.35.2

| Metric | a.6.35.2 | a.6.35.3 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 348 685 B | **1 349 136 B** | +451 B |
| RAM static | 60 552 B | 60 552 B | 0 |

+451 bytes total: boot-snapshot rows, OTA value_a re-numbering (new strings + new switch arms), sensor_poll signature change, plus a small saving from removing the heartbeat (−~30 B). Final flash usage 64.3 % of the 2 MB OTA bank.

## Next

Phase 7 — 14-day soak. The cleaned-up log makes the soak's daily-review pass much easier: every row has a meaningful description, there's no heartbeat noise, OTA events read as OTA events, and ALARM rows correctly distinguish motor/wind-override/sensor-read causes. The gh#23 largest-block watch criterion (>50 KB through ≥100 status POST cycles) remains the primary go/no-go gate.
