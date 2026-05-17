# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.20.3] — 2026-05-17

**Note: active development has moved to dev/2.0.0-esp-idf. The 1.20.x line continues to receive critical bug fixes via cherry-pick from that branch.**

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

