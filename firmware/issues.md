# Open firmware items

This file is the canonical in-repo TODO list for the greenhouse-Controller
firmware. Add new items at the end of the numbered list. Items that need
outsider visibility (farmer-facing behaviour, public bug reports, RFC-style
design discussions) should go on **GitHub Issues** instead:
[pe1mew/greenhouse-Controller/issues](https://github.com/pe1mew/greenhouse-Controller/issues).
Cross-reference here with `→ gh#N` when an item has a GitHub thread.

The `changelog.md` file is for what's *done*; this file is for what's *open*.

## Status flags

- `(open)`           — to do, not started
- `(in-progress)`    — being worked on this session
- `(blocked: …)`     — needs external input (decision, data, third party)
- `(decision-needed)` — design choice has to be made before implementation
- `~~RESOLVED~~`     — done; include a closing reference (commit SHA, version, gh#N)

## Items

1. `(open — soak-watch)` → [gh#7](https://github.com/pe1mew/greenhouse-Controller/issues/7)
   **Serial-port use causes controller freeze.** The Arduino-era `Serial`
   API that this issue's original hypothesis depended on was removed in
   the 2.0.0 ESP-IDF migration (`framework = espidf`). All logging now
   goes through `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` to the UART driver
   directly; the `Serial.setTxTimeoutMs(0)` workaround is no longer
   present (or needed). The bench unit has been running through rc.1 →
   rc.1.3 with continuous `pio device monitor` usage and zero freeze
   events. **Leaving open as a watch-item** for the Phase 7 soak + the
   7-day Unit 2 / Unit 1 production observation. If neither produces a
   freeze, close as "no longer reproducible after ESP-IDF migration".

2. `(deferred — gated on T15 re-enable)` → [gh#27](https://github.com/pe1mew/greenhouse-Controller/issues/27)
   **T15 heap-drop sampling — defer to 100 ms post-`do_status_post()`
   return.** Cheap design polish that catches transient holds correctly.
   No current landing site: T15 (`firmware/src/status_post_supervisor/`)
   is on disk but not in `firmware/src/CMakeLists.txt` SRCS as of rc.1.3.
   The rc.1 release notes call T15 *"dormant. Re-enabled paired with
   a.6.36 only if gh#23 mitigations land"*. With gh#23 closed (the
   triggering mbedTLS fragmentation pattern is gone), T15's
   planned-reboot path may not need to be re-enabled at all. Decision
   deferred until the Phase 7 soak completes (day 14 = 2026-06-03):
   - **Path A**: close gh#27 as obsolete (T15 stays dormant; gh#23 fix
     made the planned-reboot trigger structurally unreachable).
   - **Path B**: re-enable T15 + land this patch as design polish.
   - **Path C**: land this patch preemptively before any future T15
     re-enable.

## Closed (most recent first)

### 2.0.0 — ESP-IDF migration era

- ~~T14 chunk-reader heap-overrun panic at log-upload trigger~~ —
  resolved in **2.0.0-rc.1.2.1** (one-line allocation bump
  `LOG_UPLOAD_CHUNK_BYTES` → `+1u` so `storage_sd_read`'s NUL terminator
  lands inside the allocation, not one byte past the TLSF block header).
  Fix demonstrated end-to-end against the production status server: the
  524 294 B file that crashed rc.1.2 at 03:15 streamed cleanly at 08:35
  on rc.1.2.1. See `bin/2.0.0-rc.1.2.1/release-notes.md`.

- ~~OTA reboot stack-overflow in FreeRTOS timer-service task~~ — resolved
  in **2.0.0-rc.1.2** (`reboot_worker_task` carve-off in
  `ota_manager.cpp`: `esp_restart()`'s WiFi-teardown chain consumes
  several KB of stack — more than the timer-service task's ~2 KB
  allotment — so the call is now made from a dedicated 4 KB-stack
  worker spawned by the timer callback). See `bin/2.0.0-rc.1.2/release-notes.md`.

- ~~Web GUI shows the instant wind direction instead of the
  sliding-window average~~ — resolved in **2.0.0-rc.1.1**. LCD and GUI
  now both render `direction_avg_deg`; the "Variation" field now
  displays the half-arc with a leading ± (`direction_variation_deg / 2`)
  to match the natural "±N° around the average" operator reading. See
  `bin/2.0.0-rc.1.1/release-notes.md`.

- ~~Heap fragmentation in T14 status-POST loop — fix the root cause~~ →
  [gh#23](https://github.com/pe1mew/greenhouse-Controller/issues/23) —
  closed 2026-05-20 (completed) by the 2.0.0 ESP-IDF migration. The
  arduino-esp32 / WiFiClientSecure / mbedTLS-via-Arduino stack that
  drove the per-handshake fragmentation pattern is gone, replaced by
  `esp_http_client` + `esp_tls` with directly-configured mbedtls
  (`keep_alive_enable = true`, `buffer_size = 1024`,
  `buffer_size_tx = 1024`). rc.1.x soak demonstrates the fix
  end-to-end: largest_block steady at 31 KB across 24+ h, zero T15
  planned-reboot triggers, the 8:35 log upload streamed 524 KB clean
  on the production server. Closure comment on gh#23 has the full
  verification trail.

- ~~NVS event-log ring buffer~~ → [gh#22](https://github.com/pe1mew/greenhouse-Controller/issues/22) —
  closed 2026-05-20 (completed) by **2.0.0-a.6.5** retiring the
  NVS-backed ring. The SD-side CSV log (T9) is now the sole event
  journal — vastly better retention, no flash-wear cost, matches
  exactly what the issue proposed. See `firmware/platformio.ini`
  comment around the removed `-DCONFIG_NVS_LOG_CAPACITY` build flag.

### 1.18.x – 1.20.x era

- ~~T15 planned_reboot() calls esp_restart() without unmounting SD~~ →
  [gh#26](https://github.com/pe1mew/greenhouse-Controller/issues/26) —
  resolved in **1.20.2** (T15 now calls `event_logger_sd_unmount()` +
  waits for the f_sync before invoking `esp_restart()`).

- ~~T14 log-upload dedup latch doesn't advance on bad-file failures~~ →
  [gh#25](https://github.com/pe1mew/greenhouse-Controller/issues/25) —
  resolved in **1.20.x** (dedup latch now advances on every recognised
  outcome, including bad-file).

- ~~T15 heap-drop accumulator integrates jitter; trips planned reboot
  every few hours without a real leak~~ →
  [gh#24](https://github.com/pe1mew/greenhouse-Controller/issues/24) —
  resolved in **1.20.x** (signed-balance accumulator in
  `record_heap_drop()` only counts confirmed monotonic drops).

- ~~lwIP-init race on T11 fast-boot path — `Invalid mbox` assert after
  planned reboot~~ → [gh#21](https://github.com/pe1mew/greenhouse-Controller/issues/21) —
  resolved in **1.19.0** (`EG1_BIT_NETIF_READY` event-bit gate). The
  2.0.0 ESP-IDF migration makes the underlying class of race
  structurally impossible (esp_netif's event handler only fires after
  the netif is fully initialised).

- ~~1.18.2 defensive pass — platform pin + heap-fragmentation probe +
  TLS audit~~ → [gh#20](https://github.com/pe1mew/greenhouse-Controller/issues/20) —
  closed 2026-05-14. All three additions behaved as designed in the
  first production exercise. The actual fix work continued under gh#23
  (now also closed).

- ~~Document the WDT-subscriber design rule~~ →
  [gh#19](https://github.com/pe1mew/greenhouse-Controller/issues/19) —
  closed 2026-05-14. Rule written into `design/tasks.md` §6
  *Watchdog-subscriber discipline*.

- ~~Bulkhead policy: secondary network activity must not affect primary
  climate control~~ → [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18) —
  closed 2026-05-14. All four phases shipped (HTTPS hardening,
  persistent circuit breaker, NVS-persisted window state, T15
  supervisor task).

- ~~Unique unit identifier derived from MAC~~ →
  [gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17) —
  closed 2026-05-14, shipped **1.18.3** with the 2-byte short form
  (`Greenhouse-XXXX` AP-SSID convention). Surfaced on Phase 0 serial,
  LOG_SYSTEM value_a=11, canonical status JSON, AP SSID.

- ~~Unit-2 reboots: PANIC + INT_WDT within 3 minutes on 1.17.32~~ →
  [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16) —
  closed 2026-05-14 without proven root cause but structurally bounded
  by gh#18 (bulkhead) and ultimately resolved by gh#23 (heap
  fragmentation fix).

- ~~User-configurable LCD contrast + backlight brightness~~ →
  [gh#15](https://github.com/pe1mew/greenhouse-Controller/issues/15) —
  closed 2026-05-15 after hardware investigation. Driver-side API
  `lcd_set_contrast()` shipped in 1.17.33 but the LCD module's V0 is
  hardware-overridden (external trimmer or non-AiP31068L controller)
  so no operator-side change is visible. Out of firmware's reach
  without a hardware swap.

- ~~Web GUI shows 'Mounted, 0 MB' after unmount + physical card
  removal~~ → [gh#14](https://github.com/pe1mew/greenhouse-Controller/issues/14)
  — closed in **1.17.x**.

- ~~Tier-1/2 hardening + 5 MB streaming refactor~~ →
  [gh#13](https://github.com/pe1mew/greenhouse-Controller/issues/13) —
  closed 2026-05-13, resolved in **1.17.29**.

- ~~Unexpected reboot investigation — 1.17.27 esp_reset_reason
  instrumented~~ → [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12)
  — closed after the instrumentation chain (gh#16 → gh#18 → gh#23)
  identified mbedTLS fragmentation as the actual cause and resolved
  it via the ESP-IDF migration.

- ~~Forward-port `esp_reset_reason()` boot reason to web GUI Clock
  card~~ → [gh#11](https://github.com/pe1mew/greenhouse-Controller/issues/11)
  — closed. Boot reason now surfaces in canonical status JSON via
  `system.last_reset_reason` (string).

- ~~Archive/images/*.jpg blob bloat~~ →
  [gh#10](https://github.com/pe1mew/greenhouse-Controller/issues/10) —
  closed 2026-05-13 with decision (a): leave the photos in history.

### 1.17.x era

- ~~`firmware/data/index.html` placeholder regression — pre-commit
  guard needed~~ → [gh#9](https://github.com/pe1mew/greenhouse-Controller/issues/9)
  — closed. Pre-commit hook (`.githooks/pre-commit`) now refuses to
  commit `firmware/data/manifest.json` unless it is the canonical
  placeholder `{"asset_version":"{{ASSET_VERSION}}","checksum":""}`.
  (The original 2026-05-13 plan was to hook `index.html`; the
  placeholder discipline ultimately landed on `manifest.json` instead
  because that's the file the build pipeline stamps.)

- ~~Daily log upload: design decision — force-rotate at slot, status
  quo, or stream active?~~ → [gh#8](https://github.com/pe1mew/greenhouse-Controller/issues/8)
  — closed 2026-05-13, resolved in **1.17.28** with option (b):
  `event_logger_force_rotate()` before the daily upload's
  `event_logger_newest_closed()`.

- ~~Tier-1/2 hardening + 5 MB streaming refactor~~ — resolved in
  **1.17.29** (four phases: compile-flag warning surface, `pio check`
  config, T1 heap-free + integrity + stack-HWM instrumentation +
  WDT subscription for 8 tasks, streaming `SDFileChunkedStream`
  adapter). Tracked as gh#13 above.

- ~~LCD Wind status row 2 missing space after `Dir:`~~ →
  [gh#6](https://github.com/pe1mew/greenhouse-Controller/issues/6) —
  closed 2026-05-13, resolved in **1.17.26** (one-character format-
  string fix).

- ~~NTP resync overflow — "24 h" actually firing every 8 min~~ —
  resolved in **1.17.27** (`pdMS_TO_TICKS` overflow inside the macro;
  computing tick count directly avoids it).

- ~~No `esp_reset_reason()` boot log~~ — resolved in **1.17.27**
  (`main.cpp setup()` posts `LOG_SYSTEM value_a=5` with the reason
  code on every boot).

- ~~Daily-fallback silently no-ops when no closed file exists~~ —
  resolved in **1.17.27** (slot emits `SYSTEM,WEB,0,0,0,2` or
  `…,0,3` so the SD log records the fact). Diagnostic floor under
  the gh#8 design question.
