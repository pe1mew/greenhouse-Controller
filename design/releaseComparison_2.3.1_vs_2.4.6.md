# Release comparison — 2.3.1 (production, 5C88) vs 2.4.6 (candidate)

**Date:** 2026-09-11
**Baseline:** tag `v2.3.1` — what 5C88 has run on the `mainstream` channel since 2026-07-29.
**Candidate:** `e034144` (`release: 2.4.6`) — first release from the re-split `main`.
**Why this exists:** 2.4.6 shipped a regression that the operator found by hand within
minutes (the LCD WiFi-AP toggle stopped working, see §1 #1). The 2.4.6 verification had
enumerated the web GUI's inputs and not the LCD's. This document is the enumeration
that should have been done: every functional surface listed at both versions, side by
side, and every difference checked for a defect.

**Method.** Each surface below was extracted mechanically from both trees (`git show
v2.3.1:<file>` against the working tree at `e034144`) and diffed; the control-path
source files were then read as full diffs, not summaries. "Identical" means the
extracted list matched byte-for-byte. Nothing in this document is inferred from the
changelog — the changelog was read *afterwards* to check that every declared change
appears in the mechanical delta, and every mechanical delta is declared.

**Footprint.** 114 files changed, of which 61 are `model/` campaign artefacts (logs,
plots). The firmware delta is **14 modified + 4 added** source files, plus 4 in
`drivers/modBus`, 2 web assets, 2 parser files, 1 manual. Every one is accounted for in
§2.

---

## 1. Findings

| # | Finding | 2.3.1 | 2.4.6 | Severity | Status |
|---|---|---|---|---|---|
| **1** | **`wifi/ap_enable` is rejected as an unknown key.** It has no `cfg_shadow_t` field by design — T10 polls it from NVS every 5 s — so for this key the NVS write *is* the mechanism. 2.4.6's gh#53 fix defined "known" as "has a shadow field" and rejected it before the write. Three producers silently broken: the LCD System-menu AP toggle (`ui_display.cpp` `handle_menu_system` '1'), T10's own stale-flag clear on AP timeout (`network_manager.cpp` `poll_ap()`), and `POST /api/config`. The AP is the recovery path for a unit that has lost its station credentials; on 5C88 (behind NAT, no push path) it is the only way in. | works | **broken** | **HIGH** | **Fixed 2.4.7** — three-way classification `UNKNOWN / SHADOW / NVS_ONLY`; `wifi/ap_enable` is the one NVS-only entry and is now clamped 0/1 as well. Found by the operator on FDA4, 2026-09-11. |
| **2** | **`LOG_MODE_CHANGE` has two emitters and one decoder.** T6's `post_log_mode()` (vent step, `value_b` = packed demands) and `dm_set_standby_ex()` (STANDBY enter/leave, `value_b` = 0) share the event type; `logparser.py`, `plot_daily.py` and `vent_step_replay.py` all decode only the first, so every STANDBY transition renders as a ventilation decision that never happened. 2.4.6 made it worse: the new mode-restore call in `dm_reload_all_cfg()` hard-coded `LOG_BY_SYSTEM`/channel 0, which is byte-for-byte a vent-step row's signature. | parser wrong | parser wrong **and** a collision reachable by the next caller | MED | **gh#54.** 2.4.7 removes the collision on the only path by making the caller attribute the row (`dm_reload_all_cfg(initiator, channel)`); the parser fix and a distinct `param_id` remain gh#54's. |
| **3** | **A recalibration queued by `dm_reload_all_cfg()` could run on stale timings.** The mode restore (which may post `CMD_RECALIBRATE`) ran *before* `T2_NOTIFY_CFG_CHANGED`; T2 consumes notifications at loop-top before draining Q1, so notify-first is sufficient and the 2.4.6 order was the wrong way round. Unreachable today (the only caller's `session_close()` clears STANDBY first, so the restore early-returns) — it would have bitten the next caller. | n/a | latent | LOW | **Fixed 2.4.7** — internal reorder. |
| **4** | **IO0 stage-2 reset: the sweep queued by `session_close()` runs on pre-erase motor timings.** Since 2.4.5 `session_close()` recalibrates; since 2.4.3 the shadow/T2 reload happens *after* it. Verified on FDA4 2026-09-11: M3 swept in 18 s (cached 13 s + overhead) while NVS already held the 171 s default. On production the pre-reset values are almost always the defaults, so the effect is nil; on the rig it happened to be the safe direction. Reordering the reset sequence changes post-reset dwell semantics (the calibration sets `dwell_close` deadlines; a `T2_NOTIFY_CLEAR_DWELL` arriving mid-sweep would wipe them) and is not a hotfix change. | n/a (2.3.1 neither reloads nor recalibrates on session close) | present | LOW | **Known limitation, documented in 2.4.7.** Revisit with gh#54. |
| **5** | **11 shadow keys have no range clamp on the raw API path**: `cr_priority`, `rh_ctrl_en`, `wind_prot_en`, `lat_deg`, `lat_frac`, `lon_deg`, `lon_frac`, `led_day_brt`, `led_nite_brt`, `led_nite_from`, `led_nite_to`. The LCD tables clamp their own input and the GUI uses selects/limits, so only a hand-written `POST /api/config` reaches it. `cr_priority` is consumed by a `switch` (see note below); lat/lon feed `update_sun_times()` unchecked; `led_*` feed the status-LED PWM. | present | present | LOW | Pre-existing, identical. Belongs in the descriptor-table unification (finding 7), not a patch. |
| **6** | **`GET /api/config` field names are not always the NVS keys** (`poll_interval_s` vs `poll_interval`; the status-post interval is `interval_s` on `/api/web` vs `status_intv_s` in NVS). Reading the API and POSTing it back half-worked silently in 2.3.1; it now fails loudly with 400. | silent half-work | hard 400 | LOW | Documented in gh#53 and the 2.4.6 upgrade notes. Align in the next minor. |
| **7** | **The int32 key set exists in three near-miss tables**: `cfg_clamp()` (32 keys, passes unknowns through, carries the 4 `ota_*` keys `/api/ota/config` owns), `ns_key_to_log_id()` (36, returns NONE for 7 genuinely known keys), and since 2.4.6 `cfg_key_kind()` (46 shadow + 1 NVS-only). Finding 1 is what drift between them looks like. | 2 tables | 3 tables | LOW | Follow-up: one descriptor table. Drift is at least detectable now (a SHADOW key with no ladder arm logs ERROR). |
| **8** | `dwell_open_min` / `dwell_close_min` still emitted on `/api/config` as deprecated aliases of `dwell_*_s`. | n/a | emitted | INFO | Drop in the next minor, as 2.4.4 promised. |
| **9** | **2.4.2 → 2.4.6 were never published to ROTA** (`manifest-*.json` exists only up to 2.4.1). 5C88 can only ever pull what is published. | — | — | INFO | Publish 2.4.7 after its soak; do not publish 2.4.6. |

Note on finding 5: `vent_resolve_conflict()` switches on `cr_priority` with `case 0:
default:` sharing an arm (`climate_control.cpp:285`), so an out-of-range value degrades
to temperature-first rather than undefined behaviour. The lat/lon and `led_*` paths have
no such guard.

**What the comparison did *not* find.** No change of any kind in T6 climate control, T3
safety, T5 sensor polling, T10 network, T14 status-post, T16 ROTA client, the OTA
manager, the event logger or SD storage: their non-comment diffs are empty. The task
table (T1–T16, priorities, stacks) is identical. The NVS key set (57) is identical.
The web GUI posts the identical 33 `ns/key` pairs to the identical endpoints. The LCD
parameter tables (12 climate + 2 wind) and menu handler set are identical. The IO0 reset
erases the identical seven namespaces at all three stages.

---

## 2. Parallel feature table

Legend for Δ: **=** identical · **+** added · **~** changed · **−** removed.
"Defect?" is the result of reading the change, not just noting it.

### 2.1 FreeRTOS tasks

| Task | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| T1 watchdog / heartbeat (prio 1) | present | present | = | — |
| T2 relay controller (prio 6) | travel/dwell read **once at task entry**; deferrals at `ESP_LOGD` | `load_motor_timings()` re-run on `T2_NOTIFY_CFG_CHANGED`; deadlines zeroed on `T2_NOTIFY_CLEAR_DWELL`; deferrals at `ESP_LOGI`, latched per episode (`dwell_defer_logged`) | ~ | Read in full. `relay_deadline_ms` is computed at stroke start (`:447`, `:515`, calibration `:638`), so a reload mid-stroke cannot shorten or extend a running stroke. `CLEAR_DWELL` zeroes all three channels including any T6-set debt — the documented 2.4.5 decision. No defect. |
| T3 safety monitor (prio 6) | present | present | = | non-comment diff empty |
| T4 data manager (prio 5) | `nvs_load_mode()` boot seed; `apply_config_update()` writes NVS for any key | `nvs_restore_standby_at_boot()`; `dm_reload_all_cfg()`; key classification before the NVS write; T2 notified on any `motor` write; `LOG_PARAM_TRAVEL` audit row | ~ | Findings 1, 3 (both fixed 2.4.7). `update_sun_times()` verified free of MX4/`dm_get_*` calls, so calling it under MX4 in `dm_reload_all_cfg()` cannot deadlock. |
| T5 sensor manager (prio 5) | present | present | = | non-comment diff empty |
| T6 climate control (prio 5) | present | present | = | non-comment diff empty |
| T7 keypad (prio 4) | present | present | = | — |
| T8 LCD UI (prio 4) | `session_close()` clears STANDBY **without** recalibrating; IO0 stage 2 does not reload the shadow | `session_close()` posts `T2_NOTIFY_CLEAR_DWELL` and recalibrates; IO0 stage 2 calls `dm_reload_all_cfg()` | ~ | Findings 1 (AP toggle), 4. Manual positions no longer survive the session — operator-visible, documented in 2.4.5. |
| T9 event logger (prio 4) | present | present | = | non-comment diff empty |
| T10 network manager (prio 3) | polls `wifi/ap_enable` from NVS; clears it via Q4 on AP timeout | same code | = | **Its Q4 write is finding 1** — unchanged code, broken by the consumer. |
| T11 web server (prio 4) | 31 routes | 32 routes (`POST /api/diag/modbus`, `MODBUS_BENCH` builds only) | ~ | see §2.3 |
| T13 OTA manager (on demand) | present | present | = | non-comment diff empty |
| T14 status post (prio 3) | present | present | = | non-comment diff empty |
| T15 (retired) | — | — | = | — |
| T16 ROTA client (prio 3) | present | present | = | non-comment diff empty |
| T17 window position | — | — | = | **Not in 2.4.6.** Lives on `ropeSensor` only. |

### 2.2 Config pipeline (`POST /api/config` → Q4 → T4)

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| NVS key constants `K_*` | 57 | 57 | = | — |
| `cfg_shadow_t` fields | 72 | 72, two renamed: `dwell_*_min[3]` → `dwell_*_s[3]` (they were always seconds) | ~ | Rename only; every reader updated (`nvs_load_motor`, ladder, `/api/config`, GUI). |
| Q4 producers | web `/api/config`; LCD param menu (`apply_param_change`); LCD System menu `wifi/ap_enable`; T10 `system/lat_*`,`lon_*` and `wifi/ap_enable` | identical set | = | The LCD and T10 producers were **not enumerated** when the 2.4.6 predicate was written. Finding 1. |
| Key recognition | none — any key written to NVS, applied only if a ladder arm exists | `cfg_key_is_known()`: 46 shadow keys; everything else 400 / dropped | + | Finding 1: the set lacked the NVS-only category. 2.4.7 → `cfg_key_kind()`. |
| Shadow ladder keys | 46 | 46 | = | — |
| `cfg_clamp()` keys | 32 | 32 | = | Finding 5 (11 shadow keys unclamped; 4 `ota_*` clamped but not appliable here). |
| `ns_key_to_log_id()` keys | 36 | 36 + `travel_m1..3` → `LOG_PARAM_TRAVEL` | ~ | — |
| Farmer-writable keys | 11 climate + `wind_prot_en` | identical | = | — |
| String path | any `str_value` written to NVS; only `tz_str` acted on | `tz_str` only; others 400 | ~ | Correct: `status_*` belong to `/api/web`, `ota_*` to `/api/ota/config`. |
| `GET /api/config` fields | 73 | 75 (+`dwell_open_s`, `dwell_close_s`; `_min` kept as aliases) | + | Finding 6 (pre-existing name mismatch), finding 8. |
| Motor-key write → T2 | takes effect at next **reboot** | `T2_NOTIFY_CFG_CHANGED` → next **movement** (gh#51) | ~ | Hardware-verified fail-first (M3 16.2 s → 174.8 s). |
| IO0 stage-2 "Defaults loaded" | screen lied: shadow and T2 kept pre-reset values | `dm_reload_all_cfg()` makes it true | ~ | Finding 4 (sweep on stale timings). |
| Operating-mode restore on reload | n/a | via `dm_set_standby_ex()` outside MX4 | + | Findings 2, 3 (both addressed 2.4.7). |

### 2.3 HTTP API and web GUI

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| Routes | 31 | 31 + `POST /api/diag/modbus` (`#ifdef MODBUS_BENCH`; **absent from the release binary**) | + | Bench handler refuses writes to any address but two (`MODBUS_BENCH_WRITE_ADDR_A/B`), verified in `diag/modbus_bench.cpp:74`. |
| `POST /api/config` response for an unappliable key | 200 `ok:true` (junk NVS write) | 400 `{"ok":false,"err":"unknown ns/key"}` | ~ | Finding 1 (over-rejection), finding 6. |
| Auth / session model | unchanged | unchanged | = | — |
| `/api/ota/*`, `/api/web`, `/api/wifi`, `/api/pin`, `/api/sd/*`, `/api/mode`, `/api/history`, `/api/log/*`, `/api/coredump/*` | present | present, handlers unchanged | = | — |
| GUI: `ns/key` pairs posted | 33 | 33, identical | = | Every one classifies SHADOW under 2.4.6 and 2.4.7. |
| GUI: endpoints called | 25 | 25, identical | = | — |
| GUI: footer / tab title | unit ID lost on login and every 5 s refresh (gh#50) | `renderIdentity()` caches `fw_ver` + `unit_id`; tab title `<ID> · Greenhouse Controller` | ~ | Falls back to the generic title when `unit_id` is absent (older firmware). No defect. |
| GUI: dwell fields | read `dwell_*_min` | read `dwell_*_s` with `_min` fallback | ~ | Works against 2.3.1 firmware and through the paired-commit window. |
| GUI: Motors-tab tooltips | "Max 600 s" (open) / "Max 300 s" (close) — **wrong**, limit is 1500 | "Max 1500 s (25 min)" | ~ | Was contradicting the slider bounds beside it. |
| Failure rendering | `feedback()` shows "✗ Error" on non-OK | same | = | So the new 400 is visible without an asset change. |

### 2.4 LCD / keypad (T8)

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| Menu handlers (root, climate, browse, access, pin, mode, motor pick/action, edit, date, time, system, status) | 18 | 18, identical set | = | — |
| Climate / wind parameter tables | 12 + 2 | 12 + 2, identical | = | All classify SHADOW. |
| System menu '1' — WiFi AP toggle | works | **broken** | ~ | **Finding 1.** |
| IO0 stage 1 (PINs) | erases `access` | same | = | — |
| IO0 stage 2 (all, no reboot) | erases 7 namespaces; `session_close(false)` | same + `dm_reload_all_cfg()` | ~ | Finding 4. Leaves **no SD-log trace** at either version. |
| IO0 stage 3 (all + reboot) | same 7 + restart | same | = | — |
| Admin manual motor session end (logout or 5-min timeout) | STANDBY cleared, **positions kept**, T6 then refused by inherited dwell debt for up to 25 min (rc.1.5.2 regression) | STANDBY cleared, dwell debt cleared, CLOSE_ALL recalibration | ~ | Hardware-verified 2026-09-10 and again 2026-09-11 (one sweep, one MODE row). |
| Farmer session end | no recalibration (never in STANDBY) | same (`dm_set_standby_ex` early-returns) | = | — |

### 2.5 Logging and its consumers

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| Event types | 14 | 14, identical | = | — |
| `LOG_PARAM_*` ids | 50 (0–45) | 51 (+`LOG_PARAM_TRAVEL` = 46) | + | — |
| `LOG_MODE_CHANGE` emitters | T6 vent step; `dm_set_standby_ex()` | same two | = | **Finding 2** — pre-existing decoder gap; 2.4.6 added a colliding call. |
| `logparser.py` param table | 45 entries — **`wind_hyst` (45) missing** since 2.3.0 | 47 (+45 `wind_hyst`, +46 `travel`) | ~ | 2.3.1 production logs with a `wind_hyst` change show an unknown-param row. |
| `logparser.py` MODE decoder | vent-step only | vent-step only | = | Finding 2 / gh#54. |
| `plot_daily.py`, `vent_step_replay.py` | vent-step only | vent-step only | = | Finding 2 / gh#54. |
| Dwell deferral visibility | `ESP_LOGD`, serial only | `ESP_LOGI`, one line per episode | ~ | Still serial-only; not in the SD log. |
| `LOG_SYSTEM` rows | unchanged | unchanged | = | — |

### 2.6 Modbus / RS-485 driver

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| Bus mutex | **documented in `modbus_rtu.h`, never implemented** (gh#49) | `xSemaphoreCreateMutex()` in `modbus_init()`; every public transaction wrapped; 500 ms bounded acquire → `MODBUS_ERR_BUSY` | + | Hardware-verified fail-first: 0/1000 clean under two callers unpatched, 1000/1000 locked. Single-caller remains policy (T5 only in 2.4.6). |
| `modbus_write_multiple_registers()` | unlocked | `write_multiple_locked()` wrapper | ~ | — |
| `UART_SCLK` selection | unconditional | guarded on `ESP_IDF_VERSION` | ~ | Build compatibility only. |
| Re-init during another task's transaction | use-after-delete hazard | unchanged hazard (no second caller in 2.4.6) | = | Documented trap; not exercised on `main`. |
| Loopback hardware test | 8 tests | 12 (+4 concurrency) | + | Needs the 12F0 bench board. |
| Status strings (`main.cpp`) | — | + `MODBUS_ERR_BUSY` | + | — |

### 2.7 Network, OTA, ROTA, status-post, storage

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| T10 STA/AP handling, geolocation → Q4 | present | identical code | = | AP toggle broken only via finding 1. |
| T13 push OTA (`/api/ota/firmware` + `/assets`), paired-commit `FW_DONE` timer | present | identical | = | — |
| T16 ROTA pull, quiet gate, night window, cert pinning | present | identical | = | gh#41 (quiet gate vs operator session) still open at both. |
| ROTA ledger `manifest-<v>.json` | up to 2.3.1 | up to **2.4.1** | ~ | Finding 9: 2.4.2–2.4.6 unpublished. |
| T14 status POST / daily log upload | present | identical | = | gh#44 (T15 stubs) still open at both. |
| SD logging, rotation at 1 MB, LittleFS assets, coredump | present | identical | = | — |
| NVS namespaces (7) | same | same | = | — |

### 2.8 Build, versions, documentation

| Surface | 2.3.1 | 2.4.6 | Δ | Defect? |
|---|---|---|---|---|
| `platformio.ini` envs | `lolin_s3` | + `lolin_s3_mbprobe` (`-DMODBUS_CONCURRENCY_PROBE`), + `lolin_s3_bench` (`-DMODBUS_BENCH`, `-bench` version suffix) | + | Both compile to nothing in the release env; `diag/*.cpp` are `#ifdef`-guarded. |
| `firmware/src/diag/` | — | `modbus_probe.{cpp,h}`, `modbus_bench.{cpp,h}` | + | Dev-only. |
| Partition table, `sdkconfig.defaults` | — | unchanged | = | — |
| Version string | `2.3.1` | `2.4.6` | ~ | 2.4.0 was a rebuild of 2.3.1 sources (verified: only `platformio.ini` differs). |
| `boerHandleiding.md` | 4 places show the unit ID | 5 (tab title) | ~ | — |
| `README.md` | referenced retired `BRANCH_NOTES.md`, `LCD_GUI_Design.md`, `firmwareImplementationPlan.md` | pointers corrected; `BRANCH_NOTES.md` deleted | ~ | Docs only. |
| `changelog.md` vs mechanical delta | — | every 2.4.x declared change appears in the delta; every delta is declared | = | Cross-checked both directions. |

---

## 3. What this comparison changes about how releases are verified

The 2.4.6 regression was not a subtle bug. It was a **missing enumeration**: the
verification listed one producer of Q4 traffic (the web GUI) and reasoned about the
other two (LCD, T10) instead of listing them. The rule that follows, now in the gotcha
log: **when a change gates a shared queue, table or key set, enumerate every producer
and consumer with `grep`, and put the list in the verification section of the release
notes.** "I checked the callers" without the list is the claim this document exists to
retire.

Two consequences for the release cycle, both cheap:

1. **`bin/<v>/release-notes.md` § Verification must name the producer/consumer
   enumeration** for any change to `/api/config`, Q4, `cfg_shadow_t`, the `LOG_PARAM`
   table or the `LOG_*` event types.
2. **This document is the template** for the next production candidate: regenerate §2
   mechanically (`git show <tag>:<file> | grep …` per surface), then read the deltas.
