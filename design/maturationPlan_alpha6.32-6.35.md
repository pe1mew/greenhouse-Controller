# Firmware maturation plan — alpha.6.32 → alpha.6.35

**Status:** complete — all four alphas (a.6.32, a.6.33, a.6.34, a.6.35) shipped + bench-verified on 192.168.20.160. Phase 7 (14-day soak) now unblocked.
**Branch:** `dev/2.0.0-esp-idf`.
**Predecessor:** alpha.6.31 (T10 admin-toggled AP + STA back-off + IO0 debounce).
**Successor:** Phase 7 (14-day soak → 2.0.0-rc.1), deferred until this plan completes.

This document supersedes the supplements + addenda discussed during planning and is now the single source of truth for the four maturation alphas. It diverts from the migration plan's Phase 7 sequencing in `~/.claude/plans/do-not-make-changes-toasty-salamander.md` — the soak is paused while we close the four feature gaps the audit surfaced.

---

## Executive summary

The alpha.6.31 audit identified four operationally-meaningful gaps that should land before the 14-day soak begins. None block boot; each weakens either operator visibility (T1 heap rows, T10 audit events) or operator-experience parity vs the 1.20.x documented behaviour (T13 firmware-only fallback, T14 status reporting + log upload).

| Alpha | Item | Scope (one-line) | Risk |
|---|---|---|---|
| 6.32 | T1 watchdog | NeoPixel + heap rows + heap-integrity sweep + stack-HWM | medium |
| 6.33 | T10 network | 24 h NTP resync + LOG_SYSTEM event_a=3 (AP) + event_a=4 (geo) | low |
| 6.34 | T13 OTA | 120 s firmware-only fallback timer | low |
| 6.35 | T14 status | Shared secret + canonical JSON + SD log upload + 4 supplements | high |

Total estimated delta: **+12–13 KB flash, +220 B RAM, ~10 files touched**.

## Sequencing rationale

T1 instrumentation first because the alpha.6.35 work will stress the mbedTLS heap — having heap-row and largest-block telemetry on SD before the high-risk T14 changes makes the gh#23 watch item (carried forward from the migration plan) actually measurable. T10 and T13 are small + low-risk and slot in between to spread acceptance load across days. T14 last because it's the biggest and benefits from everything earlier being known-good.

Each alpha ends with a tagged release in `bin/2.0.0-alpha.6.NN/`, a paired asset bundle, the canonical bench-unit acceptance suite, and a go/no-go gate before the next starts. **No skipping the verification step** — heap behaviour is the whole reason the soak exists.

---

## Alpha 6.32 — T1 Watchdog full instrumentation

### Scope

Restore the four features deferred when alpha.6.22 first shipped T1 minimal. The 1.20.3 reference is at `git show 3d24709^:firmware/src/main.cpp` lines 126–300 (`task_watchdog_heartbeat`).

| Feature | Behaviour |
|---|---|
| **NeoPixel day/night colour** | Single WS2812B on `PIN_RGB_LED` reflects EG1 priority: RED on `MOTOR_ALARM|WIND_OVERRIDE`, AMBER on `SENSOR_FAULT_T|SENSOR_FAULT_W`, BLUE on `CALIBRATING`, GREEN otherwise. Brightness scaled by `cfg.led_day_brt` (200) or `cfg.led_nite_brt` (20) depending on local hour vs `cfg.led_nite_from` (22) / `cfg.led_nite_to` (6) |
| **60 s heap-row LOG_SYSTEM** | Three rows per 60 s cycle: `value_a=7` (free internal kB), `value_a=8` (free PSRAM kB), `value_a=12` (largest internal block kB). Posted to Q3 → T9 → SD CSV |
| **30 s-offset heap-integrity check** | `heap_caps_check_integrity_all(panic=false)` every 60 s, offset 30 s from the heap-row tick. Failure logs `value_a=9, value_b=0`. Device keeps running on corruption — T15 (when re-enabled in a future alpha) handles escalation |
| **10-minute stack HWM sweep** | `uxTaskGetStackHighWaterMark()` over all task_tN handles (T1–T15, skip NULL). Serial-log only — `LOGI` if hwm ≥ 1024 B, `LOGW` if below. Not posted to Q3 (too noisy for SD) |

### Files to change

| File | Change |
|---|---|
| `firmware/src/watchdog/watchdog.cpp` | +~200 lines body. Tick-modulo dispatch table: heap rows at `tick % 120 == 0`, integrity at `tick % 120 == 60`, stack HWM at `tick % 1200 == 0`, LED + ota_mark_healthy every tick |
| `firmware/src/watchdog/watchdog.h` | Update stack hint comment 4 KB → 6 KB; document the new instrumentation |
| `firmware/src/main.cpp` (T1 spawn) | xTaskCreatePinnedToCore stack arg `4096 → 6144` |
| `firmware/components/led_strip/` | New managed-component dependency: `espressif/led_strip@^2.5.3` |
| `firmware/platformio.ini` | `lib_deps += espressif/led_strip` pin |
| `firmware/sdkconfig.defaults` | Add `CONFIG_RMT_SUPPRESS_DEPRECATE_WARN=y` if needed by led_strip |

### Design decisions

- **NeoPixel driver: `espressif/led_strip` managed component** over rolling our own RMT-based driver. Reasons: IDF-blessed, maintained, hides RMT v4 / v5 API differences. Cost: ~3 KB flash. The 1-LED-only case could be done in ~80 lines of raw RMT but the maintenance burden isn't worth the 2 KB saved.
- **Colour ↔ EG1 priority mapping** ports verbatim from `ui_display.cpp::status_colour_for_bits` (the LCD backlight uses the same rules — keeping the LED and the LCD in sync at all times is the single most important UX-consistency choice).
- **Day/night brightness scaling at the R/G/B level**, NOT via `led_strip_set_brightness`. The global-brightness API re-scales the internal pixel buffer on every change; over many day↔night cycles the stored values degrade. Computing `scaled = (raw * dim) >> 8` on each tick at the application layer preserves the source values forever (documented in the 1.20.3 file header).
- **Heap-row tick offsets**: rows at `% 120 == 0`, integrity at `% 120 == 60`. Spreads cost so the worst-case tick has only one heavy op (~10 ms for heap_caps_check_integrity_all).
- **Stack HWM at `% 1200 == 0` (10 min)**: read-only, no locking needed. `uxTaskGetStackHighWaterMark` is a FreeRTOS internal that doesn't take MX1..MX5.
- **WDT subscription unchanged**: T1 already calls `esp_task_wdt_add(NULL)` + `esp_task_wdt_reset()` per tick. Heap walk + stack walk both fit easily inside the 5 s TWDT window.
- **`task_tN` handles**: T12 (MQTT, out of scope for 2.0.0) and T13 (OTA, on-demand) will be NULL — skip silently. All other handles are populated by main.cpp's spawn block.

### Integration risks

| Risk | Mitigation |
|---|---|
| `led_strip` managed component version drift vs IDF 5.5 | Pin to specific version; smoke-test before merging |
| `heap_caps_check_integrity_all(panic=false)` returns false on transient false-positive | Already non-panicking; only logs `value_a=9`. T15 (future) handles escalation if it persists |
| Stack HWM walk panics if a task handle has been freed mid-walk | Tasks are never deleted in this firmware; T13 (the on-demand task) sets its handle to NULL on exit. Check `if (h != NULL)` before reading. |
| RGB LED visible during sleep / power down | Not applicable — no low-power path in this firmware |
| Day/night default values mismatch operator expectations | Defaults match 1.20.3 (200/20 with 22:00→06:00 night window). T4 loads from NVS; operator overrides via GUI persist |

### Acceptance test

Bench unit @ 192.168.20.160:

1. After 70 s of uptime: visual check — amber LED during ~30 s boot calibration, green steady afterwards
2. `GET /api/log/files` → today's CSV present
3. Download today's CSV via `/api/log/download?file=YYYYMMDDHHMMSS.csv`; grep for `LOG_SYSTEM,…,value_a=7`, `=8`, `=12` — expect three rows per 60 s
4. After 10 min uptime: serial log contains `stack T7 hwm=N B` and similar lines for every spawned task; warn on any < 1024 B
5. Power-cycle: LED transitions amber → green within the calibration window
6. Optional: trigger sensor fault (disconnect S200), verify LED goes amber

### Build delta estimate

- Flash: **+6 KB** (led_strip ~3 KB, T1 body ~2 KB, tick-dispatch + log_post + brightness calc ~1 KB)
- RAM static: **+~100 B** (led_strip instance + new T1 statics)
- Final: 1.327 MB

### Tag

`2.0.0-alpha.6.32` — "T1 full instrumentation (NeoPixel + heap rows + integrity + stack HWM)"

---

## Alpha 6.33 — T10 24h NTP resync + LOG_SYSTEM events

### Scope

Two related additions:

**A. Periodic 24h NTP resync** (deferred-to-2.0.1 item from the migration plan, pulled forward).

| Aspect | Implementation |
|---|---|
| Cadence | `NTP_RESYNC_INTERVAL_S = 86400u` |
| State | `static int64_t s_last_ntp_sync_us = 0`; set on every successful sync (boot + resync) |
| Trigger | In T10 main loop: when STA connected AND `(esp_timer_get_time() - s_last_ntp_sync_us) > NTP_RESYNC_INTERVAL_US`, call `run_ntp_resync()` |
| Method | `esp_sntp_*` directly. `esp_sntp_set_sync_mode` + `esp_sntp_init` if not already initialized; `esp_sntp_restart` if init'd. Wait up to 10 s for plausible time |
| TZ preservation | After resync, re-apply `cfg.tz_str` via `setenv("TZ", …) + tzset()` — `esp_sntp_init` resets TZ to UTC. Skip the setenv call if TZ is already correct |
| Geo | NOT re-fetched (location is stable; matches 1.20.3) |
| Side effect | `xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits)` so T4 re-writes DS1307 |
| Failure handling | Single-attempt resync; on timeout, leave `s_last_ntp_sync_us` unchanged so retry happens on next iteration |

**B. LOG_SYSTEM audit events** (the gap identified in the alpha.6.31 audit).

| Event | When | Code |
|---|---|---|
| AP start | `start_ap()` after `esp_wifi_set_config(WIFI_IF_AP, &cfg)` returns OK | `value_a=3, value_b=1` |
| AP stop | `stop_ap()` before returning (any cause: admin disable, auto-shutdown, deferred-reboot teardown) | `value_a=3, value_b=0` |
| Geo sync success | `do_geo_sync()` after the four `post_q4` writes complete | `value_a=4, value_b=1` |

### Files to change

| File | Change |
|---|---|
| `firmware/src/network_manager/network_manager.cpp` | +~50 lines: `s_last_ntp_sync_us` static; `run_ntp_resync()` helper; `log_sys(value_a, value_b)` static helper wrapping `log_post`; 3 `log_sys` calls in start_ap / stop_ap / do_geo_sync |
| Includes added | `event_logger.h` (for `log_post` + `log_event_t`), `esp_sntp.h` (for periodic resync) |

### Design decisions

- **Reuse the existing event_logger Q3 path**. T9 already drains Q3 → SD CSV; no new infrastructure.
- **`esp_sntp_*` directly**, not via `wifi_tickle_run()`. `wifi_tickle_run` is a boot-time helper that reinitializes the event loop + netif — wrong tool here.
- **Cadence check inside the existing 5-s polling loop** — no new task or timer.
- **Don't audit-log STA disconnects** — too frequent (every brief AP wobble) for SD CSV. Belongs in serial log only, where it already is.
- **`event_a` codes are stable across firmware versions** — `=3` for AP, `=4` for geo. They predate the 2.0 migration; operator reports / analytics already consume them.

### Integration risks

| Risk | Mitigation |
|---|---|
| Resync starts SNTP while a previous SNTP is still in progress | Guard with `esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_IN_PROGRESS` |
| `setenv("TZ", …) + tzset()` on every resync is wasteful if TZ hasn't changed | Compare against current `getenv("TZ")`; skip if identical |
| Q3 full → log_post drops silently | Acceptable; matches 1.20.3. The event is diagnostic, not safety-critical |
| 24 h period unverifiable during normal acceptance | Add a `#define NTP_RESYNC_TEST_60S` compile-time flag for one verification build; production builds use 86400 |

### Acceptance test

1. Test-build with 60 s resync: deploy, run for 5 min, download SD CSV. Verify multiple resyncs visible (T4 RTC-readback log lines + `s_last_ntp_sync_us` increments past 60 s observable via serial). Verify TZ stays correct (`/api/status` `time_iso` shows local time, not UTC).
2. Production build (24 h period) — manual verification deferred to the soak; not gating for the alpha.
3. Toggle AP enable→disable via GUI; download today's CSV; grep for two new rows: `value_a=3,value_b=1` (start) then `value_a=3,value_b=0` (stop)
4. After geo sync runs on a fresh boot, grep CSV for `value_a=4,value_b=1`

### Build delta estimate

- Flash: **+1.2 KB**
- RAM: **+~12 B** (`s_last_ntp_sync_us` + helper static)
- Final: 1.328 MB

### Tag

`2.0.0-alpha.6.33` — "T10 24h NTP resync + audit events"

---

## Alpha 6.34 — T13 firmware-only fallback timer

### Scope

Implement what `ota_manager.cpp:287-288` currently disclaims: after `ota_firmware_end()` succeeds and `OTA_STATE_FW_DONE` is entered, start a 120 s one-shot timer. If `ota_assets_begin()` is called within the window, cancel the timer. If the timer expires first, commit the firmware-only update (`esp_ota_set_boot_partition(s_ota_part)` + 1 s deferred `esp_restart`).

This restores the 1.20.3 documented behaviour: firmware-only OTA is a valid operation; the operator isn't forced to also push assets just to commit the firmware update.

### Files to change

| File | Change |
|---|---|
| `firmware/src/ota_manager/ota_manager.cpp` | Add `s_fw_done_timer` static + `fw_done_fallback_cb` callback + lifecycle hooks in `ota_firmware_end()` (start) and `ota_assets_begin()` (cancel) |
| `firmware/src/ota_manager/ota_manager.h` | Update `ota_firmware_end()` doc-comment — it already mentions a 120 s window; the implementation just needs to honour it |

### Design decisions

- **Timer period**: `FW_DONE_FALLBACK_MS = 120000` — matches the 1.20.3 reference and the existing doc-comment intent.
- **Cancel semantics**: `xTimerStop` only. FreeRTOS guarantees the callback won't fire after a successful stop.
- **State check inside the callback** (under `s_mx`): if `s_state != OTA_STATE_FW_DONE`, bail out without committing. Covers the race where the timer fires concurrently with `ota_assets_begin()`.
- **Boot partition commit**: `esp_ota_set_boot_partition(s_ota_part)`. Errors logged via `set_error_locked`, no reboot in that case (the device stays on the current firmware, which is safe).
- **Reboot**: reuse the existing `schedule_reboot(1000)`.
- **Log to Q3**: post `value_a=10, value_b=1` (firmware-only commit) on success. New event code, documented inline.

### Integration risks

| Risk | Mitigation |
|---|---|
| Race: timer fires concurrently with `ota_assets_begin()` | State check under `s_mx` in callback — sees `ASSETS_BUFFERING` and bails |
| Repeated firmware OTA before the timer cancels the previous one | `ota_firmware_begin()` already rejects when state is not IDLE/ERROR. Idempotent |
| User cancels OTA mid-window (no API exists today) | Not applicable — error path sets state to ERROR; callback won't commit |
| Boot partition flipped to a partition with un-paired assets → MISMATCH badge fires post-reboot | Expected and documented. Operator can push assets afterward to clear the badge. The mismatch flag is the SIGNAL that the firmware-only path was taken |

### Acceptance test

1. Upload firmware-only via `POST /api/ota/firmware` (no follow-up asset upload). Within 120 s: device reboots; new `fw_ver` reported via `/api/status`. `asset_version` stays at the prior version → MISMATCH badge fires (correct).
2. Cancel test: upload firmware, then within 30 s upload assets via `POST /api/ota/assets`. Verify exactly ONE reboot via the normal asset-OTA path. Verify `fw_ver == asset_version` post-reboot.
3. Verify SD CSV contains `value_a=10, value_b=1` after the fallback commit

### Build delta estimate

- Flash: **+0.4 KB**
- RAM: **+~50 B** (timer struct)
- Final: 1.328 MB

### Tag

`2.0.0-alpha.6.34` — "T13 firmware-only fallback timer"

---

## Alpha 6.35 — T14 Status POST hardening

**Largest sub-phase. Highest gh#23 exposure risk.** Seven discrete items (A–G).

### Scope

**A. Shared secret in `sourceidentifier` header from NVS**

| Aspect | Implementation |
|---|---|
| NVS key | `system/status_secret` (already in `cfg_shadow_t::status_secret[65]`) |
| Read path | `dm_cfg_snapshot(&cfg)` once per POST cycle |
| HTTP header | `esp_http_client_set_header(client, "sourceidentifier", cfg.status_secret)` after `esp_http_client_set_header(client, "Content-Type", "application/json")` |
| Secret length | Already validated by T11's `POST /api/web` handler (`CFG_MIN_SECRET_LEN ≤ len ≤ CFG_MAX_SECRET_LEN`) |
| Empty-secret behaviour | Skip the header. Server-side reject is the server's problem |

**B. Canonical status JSON shape via `build_canonical_status_json`**

| Aspect | Implementation |
|---|---|
| Current minimal | `build_min_status_json` → `{unit_id, fw_version, uptime_s, free_heap}` ~150 B |
| New shape | `build_canonical_status_json(buf, cap, snap, cfg.status_expose, /*include_disabled_setpoints=*/false)` |
| Buffer | 2 KB heap-allocated per POST. Matches T11's 4 KB cap with safety margin |
| `include_disabled_setpoints=false` | Public dashboard sees no inert setpoints when operator disables RH control — matches 1.20.3 |
| Sections | climate / wind / windows / mode / sun / system, each filterable via expose mask |

**C. SD-CSV log upload (daily + T9 rotation triggers)**

| Aspect | Implementation |
|---|---|
| **Daily trigger** | T14 main loop polls local time each iteration. When `tm_hour == cfg.log_upload_h && tm_min == cfg.log_upload_m && cfg.log_last_up != today's filename`, trigger upload |
| **Rotation trigger** | T9 finishes a day's CSV → sends `xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits)` carrying the just-closed filename via a tiny mutex-protected static in event_logger. T14 wakes via `xTaskNotifyWait` (timeout = next-cycle delay) |
| **Streaming upload** | `esp_http_client_open(fsize)` → 4 KB-chunk loop via `storage_sd_read` → `esp_http_client_write(chunk, n)` → `esp_http_client_fetch_headers()` → status check → `esp_http_client_cleanup()` |
| **Endpoint** | `<cfg.status_url>?action=log&file=<filename>` (T14 appends query string; matches 1.20.3) |
| **Server identity** | Same `sourceidentifier` header as the status POST (item A) |
| **Chunk size** | **4 KB**. Larger chunks blow up mbedTLS per-handshake heap (gh#23 trigger); smaller chunks waste TLS overhead. 4 KB aligns with the gh#23 follow-on `max_frag_len=1024` tuning that lands in a later alpha |
| **gh#25 dedup latch** | On 2xx: write `cfg.log_last_up = filename` via Q4 → T4 → NVS. Next daily/rotation trigger sees the same filename → skips. Same filename within 24 h = no-op |
| **Failure handling** | Don't retry on HTTP non-2xx. Log via Q3 with `value_a=5, value_b=0` (failure). Successful upload: `value_a=5, value_b=1`. Update `s_last_log_str` (see item F) |

**D. Respect `status_enable` master flag** (audit gap)

| Location | Change |
|---|---|
| `status_post.cpp::task_status_post` main-loop gate | Change `if (cfg.status_url[0] == '\0' || cfg.status_interval_s <= 0)` to `if (!cfg.status_enable || cfg.status_url[0] == '\0' || cfg.status_interval_s <= 0)` |
| Behaviour when disabled | Task idles (re-checks every `STATUS_REPOLL_S`); `s_last_str = "DISABLED"`; GUI Web tab shows it correctly |
| Apply on next cycle | `dm_cfg_snapshot` reload each iteration — flipping `status_enable=0` via GUI takes effect at the start of the next cycle, no restart |

**E. Respect `log_upload_rot` rotation flag** (audit gap)

| Location | Change |
|---|---|
| T14 `T14_NOTIFY_LOG_ROTATED` handler | Wrap upload trigger in `if (cfg.log_upload_rot != 0) { … }`. If 0, T14 silently consumes the notification and waits for the next daily window |
| Daily-window upload | Independent of `log_upload_rot` — fires at `log_upload_h:log_upload_m` regardless |
| Admin policy | `rot=0` = daily-only, `rot=1` = daily + on-rotation. GUI Web tab tooltip already explains |

**F. Update `s_last_log_str` after each upload attempt** (audit gap)

| Location | Change |
|---|---|
| `do_log_upload` after server response | Format `"OK <YYYY-MM-DD HH:MM:SS>"` or `"FAIL <YYYY-MM-DD HH:MM:SS> code=<N>"` into `s_last_log_str` (mirroring `s_last_str`) |
| Persist last-uploaded filename | On 2xx, write `cfg.log_last_up = filename` via Q4 (already covered by gh#25 dedup latch — same write) |

**G. Tighten `POST /api/web` URL validator (T11) to `https://` only**

| File | Change |
|---|---|
| `firmware/src/web_server/web_server.cpp::web_post_handler` URL-validation block | Reject `http://` scheme with HTTP 400 + `{"ok":false,"err":"URL must use https:// — plain HTTP exposes the shared secret on the wire"}`. Keep `?`/`#` rejection + `api.php` suffix check unchanged |
| `webUiMock/mock_server.py::web_post` | Mirror the same validation for parity |
| `firmware/data/index.html` | Confirm Web tab placeholder is `https://example.org/api.php` (no `http://` examples) |

### Files to change

| File | Change |
|---|---|
| `firmware/src/status_post/status_post.cpp` | +~200 lines: swap `build_min_status_json` → `build_canonical_status_json`; add `sourceidentifier` header; new `do_log_upload` streaming function; daily-trigger logic; `xTaskNotifyWait` for rotation; `status_enable` gate; `s_last_log_str` updates |
| `firmware/src/status_post/status_post.h` | Add `T14_NOTIFY_LOG_ROTATED` bit definition |
| `firmware/src/event_logger/event_logger.cpp` | Add `xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits)` on rotation; mutex-protected last-rotated-filename buffer |
| `firmware/src/event_logger/event_logger.h` | Add accessor `event_logger_last_rotated_filename(buf, cap)` |
| `firmware/src/web_server/web_server.cpp` | Tighten URL validator (item G) |
| `webUiMock/mock_server.py` | Mirror URL validator |
| `firmware/data/index.html` | Verify Web tab placeholder uses `https://` |
| `firmware/data/app.js` | If any client-side validation accepts `http://`, tighten to `https://` |

### Design decisions

- **gh#23 mbedTLS mitigations** (max_frag_len 1024, single cipher TLS_ECDHE_RSA_AES_128_GCM_SHA256, session-ticket persistence) **are out of scope for this alpha**. The user-asked items are: secret, canonical JSON, log upload. gh#23 mitigations land as a separate alpha (~6.36) that pairs with re-enabling T15 supervisor.
- **`build_canonical_status_json` is already a callable function** — no rewrite needed. Just swap the call.
- **`include_disabled_setpoints=false` for T14** (sends to public dashboard) — distinct from T11's local-GUI path which uses `true`. Two callers, two policies.
- **T9 → T14 notify contract**: single notify bit + mutex-protected last-rotated-filename string in event_logger. No new queue (no Q-numbered queue spec in TSDS for this signal).
- **gh#25 dedup latch belongs with the log upload**, not as a separate item — it's the natural completion of the upload path.
- **Race protection for daily + rotation triggers firing simultaneously**: dedup latch sees the same filename and skips the second attempt. No device-side race.
- **HTTPS via URL scheme** — `esp_http_client` picks transport from URL. `transport_type = HTTP_TRANSPORT_OVER_SSL` stays as belt-and-braces (already in minimal T14). Tightening the T11 validator to `https://` only (item G) closes the policy gap so the operator can't accidentally configure a plain-HTTP endpoint that leaks the shared secret.

### Integration risks

| Risk | Mitigation |
|---|---|
| **gh#23 heap fragmentation surfaces here** — the whole point of landing T1 instrumentation first (alpha.6.32) is to be able to see this | Watch `value_a=12` (largest-block) trend over 24 h after alpha.6.35 lands. Don't ship rc.1 until largest-block stays > 50 KB through 100 status-POST cycles. If it drops below 30 KB → alpha.6.36 lands gh#23 mitigations |
| Streaming SD read + HTTPS write blocks T14 for several seconds | T14 already low priority. Status POST cycle is paused during a log upload (single-task model, matches 1.20.3) |
| Big CSV files (~1 MB after a busy day) push the upload past mbedTLS per-handshake heap budget | 4 KB chunks bound per-write heap. Total transfer size doesn't matter; only per-write does |
| `task_t14` referenced from event_logger at compile time | Already extern in `app_types.h:72`. Populated by main.cpp's spawn block |
| Daily trigger fires at boot if NVS `log_last_up == yesterday` and local clock just crossed midnight | Add guard: only upload if filename ≠ today's open file (compare against `event_logger_current_filename(buf, cap)`) |
| `cfg.status_expose = 0` (all bits cleared) → degenerate JSON | Builder emits system block unconditionally; expose mask only filters climate/wind/windows/mode/sun. Safe |
| Reading `cfg.status_secret` on every POST cycle (~10 μs MX4 take) | Negligible |
| T11 URL validator change rejects existing `http://` configs | Operator must re-enter URL with `https://`. Document in release notes. Existing NVS value is not auto-migrated |

### Acceptance test

For a unit pointed at a real status server (or the webUiMock):

| Test | Expected |
|---|---|
| **A. Secret header** | Capture POST via mock logs or `tcpdump`; verify `sourceidentifier: <16+ char secret>` |
| **B. Canonical JSON** | Body matches `build_canonical_status_json` output. Toggle `cfg.status_expose=0x3F` (full) vs `0x01` (climate only); verify section filtering |
| **C. Daily upload** | Configure `log_upload_h:m = (now + 2 min)`; wait; verify upload reaches server; verify `cfg.log_last_up` updates via `GET /api/web` |
| **C. Rotation upload** | Force T9 rotation (clock-hack OR wait until 03:14 local); verify upload fires within ~5 s |
| **C. Dedup latch** | Force same upload twice in 10 min; verify second attempt skipped via SD-log `value_a=5, value_b=0` |
| **D. `status_enable=0` gate** | Set `status_enable=0` via GUI; verify no POST hits the server for 5 min; verify `s_last_str = "DISABLED"` via `GET /api/web` |
| **E. `log_upload_rot=0` gate** | Set `log_upload_rot=0`; force rotation; verify NO upload. Set `=1`; force rotation; verify upload fires |
| **F. `s_last_log_str` updates** | After successful upload, `GET /api/web::last_log_up` shows `"OK <ts>"`. After forced failure, shows `"FAIL <ts> code=<N>"` |
| **G. URL validator** | `POST /api/web` with `http://example.com/api.php` → 400 with `"URL must use https://"`. Same payload with `https://...` → 200 |
| **gh#23 watch** | After 24 h: `value_a=12` (largest-block) stays > 50 KB through ≥100 POST cycles. If not, alpha.6.36 lands gh#23 mitigations |

### Build delta estimate

- Flash: **+6 KB** (streaming-upload + query-string + dedup + daily-trigger + canonical JSON linkage + status_enable gate + s_last_log_str formatting + URL validator)
- RAM: **+~60 B** (notify bits, time tracking, rotation-filename buffer)
- Final: 1.334 MB (over migration plan's 1.30 MB target by ~34 KB — target needs re-baselining)

### Tag

`2.0.0-alpha.6.35` — "T14 secret + canonical JSON + SD log upload + status_enable + log_upload_rot + https-only"

---

## Cross-cutting concerns + open items

| Item | Status |
|---|---|
| **gh#23 mbedTLS mitigations** | OUT OF SCOPE for this plan. Lands in a follow-on alpha (~6.36) paired with re-enabling T15 supervisor. Triggered by alpha.6.35's largest-block watch test (acceptance gh#23 watch) |
| **T15 status_post_supervisor** | OUT OF SCOPE. Stays dormant (source-on-disk, not in CMakeLists.txt SRCS) until paired with gh#23 mitigations. The 1.20.3 wedge/leak/respawn-storm protections are absent from the soak — flagged as a known risk |
| **Phase 7 14-day soak** | DEFERRED until alpha.6.35 lands. The soak measures the full mature firmware including gh#23 watch under canonical JSON + log upload load |
| **Migration plan flash budget** | Target was ≤ 1.30 MB. Current 1.31 MB; estimated post-6.35 1.334 MB. Need to re-baseline target — the audit + maturation items have valid value, the budget needs to absorb them |
| **Migration plan Phase 6 status** | Officially complete (per the plan's phase table). This maturation pass is "phase 6 hardening" not new phase work |
| **Test unit re-flash count** | High (~15 reflashes during the recent debug cycle). The unit's flash endurance is rated at ~100k cycles per sector — we're nowhere near, but worth tracking |

## Per-alpha go/no-go gates

Each alpha must pass before the next starts:

1. **Build** succeeds with no new warnings (existing -Wvolatile noise is fine)
2. **Bench flash** completes, hash verified
3. **Boot** reaches `/api/whoami` returning 401 within 20 s
4. **Acceptance tests** for THAT alpha all PASS (see per-alpha lists)
5. **Heap rows** (post-alpha.6.32) show no degradation vs baseline
6. **Release artifacts** staged in `bin/2.0.0-alpha.6.NN/`: firmware-X.bin, firmware-X.elf, partitions.bin, bootloader.bin, paired web-assets-X.zip, release-notes.md
7. **Changelog entry** added with acceptance evidence inline
8. **24h+ delay** between alphas so heap trends are observable on SD logs

If any gate fails, halt and diagnose. Don't stack alphas on a broken baseline.

## Total scope estimate

| Alpha | Net flash | Net RAM | Files touched | Risk |
|---|---:|---:|---:|---|
| 6.32 | +6 KB | +100 B | 5 | medium (RMT) |
| 6.33 | +1.2 KB | +12 B | 1 | low |
| 6.34 | +0.4 KB | +50 B | 2 | low |
| 6.35 | +6 KB | +60 B | 8 | high (gh#23 surface) |
| **Total** | **+13.6 KB** | **+222 B** | **~16** | |

Final flash after alpha.6.35: **~1.334 MB** (62.5 % of the 2 MB OTA bank). Comfortable headroom for the gh#23 mitigations (~+8 KB estimate) + T15 re-enable (~+5 KB).

## Suggested commit cadence

One commit per alpha. Each commit message references this plan doc + the per-alpha acceptance evidence inline (curl results, SD-log grep output). Stagger flashes by ≥ 24 h on the bench unit so heap trends are observable between landings.

---

## Document control

| Field | Value |
|---|---|
| Created | 2026-05-18 |
| Author | Claude (codenamed planning round after the alpha.6.31 audit) |
| Branch | `dev/2.0.0-esp-idf` |
| Supersedes | The earlier conversational plan + supplements; this doc is the single source of truth |
| Next-review trigger | Completion of alpha.6.35 acceptance — at that point reassess Phase 7 readiness AND gh#23 / T15 re-enable timing |
