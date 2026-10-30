# Release comparison: 2.3.1 (production, 5C88) vs 2.17.0 (candidate)

**Date:** 2026-10-27
**Baseline:** tag `v2.3.1`, what 5C88 has run on the `mainstream` channel since 2026-07-29.
**Candidate:** tag `v2.17.0` (`d7bc3c3`), ROTA seq 64 on the soak channel, on 2344 since 2026-10-11. `HEAD`
(`a098f38`) is byte-identical to the tag under `firmware/` and `drivers/`.
**Why:** the operator asked what an update of 5C88, which has no position sensor, to 2.17.0 would do. The
project's rule for anything headed to `mainstream` is this document, regenerated against the production tag:
the template is [releaseComparison_2.3.1_vs_2.4.6.md](releaseComparison_2.3.1_vs_2.4.6.md).

**Method.** Every surface was extracted from both trees (`git show v2.3.1:<file>` against `git show
v2.17.0:<file>`) and diffed with comments stripped. The control-path files were then read as full diffs, and
every change was judged for **5C88's configuration**, not the rig's: no position sensor (`motor/wpos_fitted_m3`
absent, so 0), `travel_m3` 171 s, `cr_priority` 0, a real FG6485A (address 1) and S200 (address 44), behind NAT,
updated only by ROTA pull. The changelog was read afterwards, as a cross-check only. Five read-only passes did the
extraction in parallel (tasks and control path; configuration and NVS; HTTP API, GUI and payload; LCD, log and SD;
network, OTA, build and heap). Their evidence (file:line at `v2.17.0` and the command for each claim) is condensed
here. Nothing in §2 is inferred from release notes.

**Footprint.** 278 commits. Under `firmware/`, `drivers/`, `log/` and `webUiMock/`: 158 files, +29 322 / −2 931
lines. The firmware delta proper is **41 modified and 20 added** files under `firmware/src`, `firmware/config`,
`firmware/components`, `firmware/data` and the build files. The partition table is byte-identical.

**Evidence from hardware** (beyond the extraction): 2.17.0's release image ran a **24.25 h soak in 5C88's
configuration** on 2344 (2026-10-25/26: no sensor, `cr_priority` 0, the law `stepped v2`; 0 FAIL, 14 of 14
decisions conform; `bin/2.17.0/release-notes.md`). That soak's T/RH and wind come from the **emulator**, not
real sensors (finding 6).

**Outcome (2026-10-30).** The operator put 2.17.0 on 5C88 by a push on site around 2026-10-28, not by ROTA, so `mainstream` still points at 2.3.1. A failed RS485 transceiver was replaced, and the bus then needed the window sensor's 120 Ω removed (gotcha log, 2026-10-30). With that, a real FG6485A and S200 run clean on 2.17.0's driver, which closes the unknown in finding 6. `travel_m3` stays 171 by the operator's decision. Heap at 78 min of uptime: 68 KB free, 24 KB largest, floor **5 KB**, below finding 7's expected 10-18 KB, though in line with 2.16.2's soak floor of 4-5 KB. Everything below is the comparison as written before the update.

---

## 1. Findings

Severity is for 5C88. Ordered by severity, then by what an operator meets first.

| # | Finding | 2.3.1 | 2.17.0 | Severity | Status / action |
|---|---|---|---|---|---|
| **1** | **The update is forward-only.** T16 refuses any manifest seq at or below its persisted high-water mark, so 5C88 can never be offered 2.3.1 (seq 44) again. The 3-fail rollback (`ota_check_rollback()`, identical at both tags) covers only a crash in the first 30 s (`OTA_HEALTHY_MS`). Any later fault can be fixed only by a newer release (one night window) or on site. **Worst case:** a fault that makes the quiet gate refuse every apply also blocks the cure. A lasting wind-sensor fault does exactly that: T3 holds the wind override, and the gate refuses (finding 6) | — | — | **MED** | Promote with a site visit planned, or accept a visit as the recovery path |
| **2** | **A T13 failure after it has started makes 5C88 refuse seq 64 for good.** 2.3.1's `rota_apply()` writes `fw_hiwater = 64` as soon as `ota_assets_end()` returns, i.e. when T13 has been *spawned*, not when it has installed (`v2.3.1 ota_client.cpp:607,617`). If T13 then fails (LittleFS, zip, bank switch), the unit stays on 2.3.1, paired, but treats 2.17.0 as a replay (`dl` 3). A spawn failure itself (gh#94) returns before the write, and so is clean. 2.17.0 keeps the same ordering | latent | latent | **MED** | Recovery: publish a new release (seq ≥ 65); no site visit needed. Worth fixing in the client: write the mark when the new image has booted |
| **3** | **The LCD's M3 keys mean something different** (gh#93, not gated on a sensor). A tap under 1 s is a full stroke that starts on **release**. A 1–2 s press does nothing (the LCD says why). A hold of 2 s or more moves M3 until release, then stops it **part-open, by time**: the controller does not know how far, and persists it as UNKNOWN. M1 and M2 are unchanged. A release never stops a wind close | full stroke on press | tap / hold | **MED** (operator-visible) | Brief the admin; the beheerder manual 1.31 §10.11 has it. The part-open state ends at the session's recalibration (finding 4) |
| **4** | **Every admin LCD manual session now ends with a full recalibration** (2.4.5, gh#65): dwell debt dropped, CLOSE_ALL sweep (176 s on M3, T6 paused), then T6 from step 0. The sweep re-arms M3's close dwell (600 s), so **M3 stays shut for about 13 minutes after any LCD session**, even in heat. The manual positions no longer outlive the session; a reboot no longer strands Standby (gh#65) | positions kept, no sweep | sweep at session end | **MED** (operator-visible) | Declared since 2.4.5. Brief the admin |
| **5** | **The farmer's LCD setpoint browse has four items, not three.** T-min day/night is inserted second (`ui_display.cpp:183`), so a trained B-sequence lands on a different setpoint, and a farmer can now edit T-min from the LCD. The CR-priority edit offers 0/1 (a stored 2 shows as 1; confirming writes 1) | 3 items | 4 items | **MED** (operator-visible) | Brief the farmer; check the boer manual's key sequences |
| **6** | **The Modbus driver under T5 changed materially, and has never polled real field sensors.** It gained a bus mutex (gh#49) and a locked re-init (gh#69). DE/RE moved from task-driven GPIO to the UART's RS-485 half-duplex RTS (gh#70), and the echo discard went with it. The inter-frame gap went from 4 ms to **20 ms**. Every rig test since at least 2026-09-14 polled **emulated** slaves at addresses 1 and 44 (CLAUDE.md), and the real encoder at 40. Not established: that 5C88's transceiver and wiring match the rig's. **If S200 replies were lost:** the wind fault leads T3 to hold the override (windows closed) and the quiet gate to refuse every update. No crash, so no rollback: finding 1's worst case | task GPIO, 4 ms | UART RTS, 20 ms | **MED** (impact) / LOW (likelihood) | The strongest reason to promote with someone able to reach 5C88. Optional before that: one run of 2.17.0 against a real FG6485A and S200 |
| **7** | **About 11 KB less internal heap.** Static internal RAM grew by about 6.2 KB (DRAM +3 148, IRAM +3 072, mostly `UART_ISR_IN_IRAM`), and T17's 4 KB stack adds the rest. Expect about **70 KB free** on 5C88 (2.3.1: 81 KB at 15 d), the **largest block unchanged at 31 KB**, and the floor falling from 33 KB towards 10–18 KB. TLS records for T14 and T16, and T13's stack, each need about 16.4 KB contiguous (gh#81, #94, #95) | 81 / 31 / 33 KB | ≈70 / 31 / 10–18 KB | **MED** | The 24 h soak in this configuration had 0 failed status POSTs and a flat 31 KB block. Not measured: a 15-day trend on 2.17.0 in this configuration (2344 itself held 70 / 31 KB after 14 days on 2.17.0 with its sensor) |
| **8** | **`motor/ctrl_mode_m3` defaults to 1 (Linear) on 5C88.** The key is absent from its NVS, so the first boot writes the default. Control is unaffected: with no sensor the effective mode is TIMED (proof in §2.1). But the Motors tab permanently shows the warning *"Linear is set but NOT in force…"*, the payload reports `M3_ctrl_reason: no_position`, and if a sensor were fitted later, Linear would start with no further choice. The comment in `cfg_defaults.h:136-155` says the default reaches only a factory-reset unit, which is wrong for a unit that never had the key | — | 1 | **MED** (found by three passes) | After the update set `ctrl_mode_m3` 0 (one admin write), or change the default to 0 for units without a sensor |
| **9** | **SD retention starts trimming 5C88's card history.** The first boot deletes nothing. From the first 1 MB rotation after it (about 2 days), up to 5 of 5C88's oldest files go per rotation until 30 remain, with no check of upload state (gh#82, 2.12.2). **Where the data is:** 5C88 has uploaded every rotated file since at least 2026-07-28 with no gap over 2.3 days (51 files on the status site, which keeps 90 days), and the repo's `model/campaign-*` folders hold its uploads continuously from **2026-06-10**. Only files from before 2026-06-10 would exist on the card alone | count saturated | per-unit, 30 | **LOW** | Copy the card at the site visit if the May/early-June logs matter |
| **10** | **The web GUI polls `/api/diag/commission` every second, unconditionally** (`app.js:622`). Each poll renews the server session (the admin gate renews before it checks the role), against the GUI's own intent that background polls do not extend a session. A poll sent without a cookie while a login is in flight comes back 401 and bounces the operator to the login screen. Not reproduced on hardware | — | 1 s poll | **LOW** | Gate the poll on the Motors tab and a fitted sensor |
| **11** | **API changes a script would notice:** `GET /api/config` no longer has `dwell_open_min` / `dwell_close_min` (use `_s`). An admin route answers an expired session with **401** `session_expired` instead of 403. `POST /api/pin` without a valid role is now 400 (in 2.3.1 it silently changed the farmer PIN: a fix) | — | — | LOW | No in-repo client depends on the old forms |
| **12** | **Stored values are not clamped at load**, only on write. Bounds that narrowed or are new: `poll_interval` max 300 → 120; `cr_priority`, `rh_ctrl_en`, `wind_prot_en`, lat/lon, `ap_enable`. A 5C88 value outside them would be used as stored (as in 2.3.1) and shown outside the published limits | — | — | LOW | Read 5C88's `GET /api/config` on site before the update; no config dump of 5C88 exists in the repo |
| **13** | **IO0 reset stages 2 and 3 now also erase ESP-IDF's own WiFi credentials** (gh#56), as intended. As at 2.3.1, they also erase `ota_url`, `ota_secret`, `ota_cert` and `fw_hiwater` | — | — | LOW | Behind NAT: a stage 2/3 reset ends ROTA and WiFi until re-provisioned on site, as before |
| **14** | **T6's stack is unchanged at 6144 B while its frames grew**: up to three `cfg_shadow_t` copies of about 700 B each per cycle, plus the law's in/out structs. T17 and T16 are missing from T1's stack high-water sweep (`watchdog.cpp:242-248`) | — | unmeasured | LOW | Read T6's high-water mark on 2344; add T17 and T16 to the sweep |
| **15** | **One admin-only call still reaches address 40 on a unit without a sensor:** `POST /api/diag/commission {"action":"abort"}`, exempt from the not-fitted gate (`web_server.cpp:3530-3535`). It writes and reads address 40 from T11, so a few 200 ms timeouts land on the bus. T5 sees BUSY within its 60 s grace | — | reachable | LOW | Gate `abort` on `wpos_fitted_m3` too |
| **16** | **`logparser.py` misreads four `LOG_SYSTEM` rows**: an SD write failure (−1, 0) reads as "Q3 overflow 0"; the Q3-overflow row carries its count in `value_a` and collides with codes 1–32; T10's code 19 (DHCP renew) reads as a legacy heartbeat; −2 (SD low space) is not decoded | same | same | LOW | Pre-existing, identical at both tags |
| **17** | **Retired `system/led_*` keys** stay in 5C88's NVS unread; the LED uses constants equal to the old defaults. A POST of them now gets 400 | read | orphaned | INFO | Behaviour changes only if 5C88 held non-default values, which only a raw POST could have written |
| **18** | **New SD rows on 5C88:** SYSTEM 29 (a T6 move deferred on dwell, one per episode, frequent on M3), 27 (a command discarded in a motor alarm), 31 (hourly bus counters for 1 and 44), MODE params 54 and 56 at boot, `PIN_AUTH`, web `SESSION` rows; `SENSOR_HR` ch 2 `value_b` becomes −1 (no M3 position) | — | — | INFO | The parser at 2.17.0 decodes all of them. Any other consumer of the CSV should be checked for ch 2's −1 |
| **19** | **Documentation drift found on the way:** the changelog has no `## [2.15.0]` heading (its text sits inside 2.15.1); the 2.17.0 entry's "no key, payload or log change" is true of 2.17.0 alone, not of the span; `modbus_rtu.cpp:180` still calls `MODBUS_IFG_US` 20 000 an "EXPERIMENT, not a shipping value"; `NVS_SCHEMA_VERSION` is still 1 although keys were added and retired (harmless: nothing renamed); `status_post.cpp:730` and `event_logger.cpp:26` say `SD_MAX_FILES` is 10 (it is 30); the limits handler says 40 keys (41); `law` is missing from the status-site contract | — | — | INFO | Docs only |

**What 2.17.0 fixes that 5C88 has today:**
- **gh#79:** T6 no longer fills Q1 during a calibration sweep, which could drop T3's single CLOSE_ALL. T3 now retries the CLOSE_ALL until it is accepted. T2 reads the clock per command, so a command drained after a 176 s sweep is no longer timed from before it.
- **gh#65:** a reboot during an LCD session no longer leaves ventilation paused indefinitely.
- **The heartbeat** no longer steals key events from T8.
- **gh#89:** LittleFS is formatted only when its contents are corrupt, not on any mount failure.
- **Payload:** temperatures between −0.9 and −0.1 °C keep their minus sign (2.3.1 sent −0.5 °C to the status site as +0.5).
- **`/api/history`:** the same sign fix.
- **gh#56:** the IO0 reset really forgets the WiFi credentials.

**What the comparison did *not* find.**
- **Boot:** the partition table is byte-identical; the toolchain is the same (espressif32@6.12.0, ESP-IDF 5.5.0); the two bootloaders differ only by the removed compile-time log line and path strings (48 bytes). An app built for 2.17.0 boots behind 2.3.1's bootloader.
- **ROTA:** 2.3.1's client accepts the 2.17.0 manifest (every field, sizes under both limits, seq 64 > 44) and can install its assets zip (four STORE entries, the same four names as 2.3.1).
- **Control:** no window decision differs at `cr_priority` 0; no path issues a position target or a position stop to M3 without a sensor; T17 sends nothing on the bus when not fitted; T3's wind thresholds and hysteresis are byte-identical.
- **NVS:** no key renamed, no unit changed, no stored value misread; the value 0 of `cr_priority` resolves identically.
- **Log:** `log_type_t` and `LOG_PARAM_*` were only appended to; the CSV format is byte-identical.
- **Status payload:** no key removed, renamed or retyped. The worst case for 5C88 is 1 374 B against T14's 2 048 B.
- **Status site:** it already reads every key and flag 2.17.0 emits, except `law`, which it ignores.

---

## 2. Parallel feature table

Legend for Δ: **=** identical · **+** added · **~** changed · **−** removed. "Defect?" is the result of reading the
change for 5C88's configuration.

### 2.1 FreeRTOS tasks and the window-control path

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| Task table (name, priority, stack) | T1 1/6144, T2 6/8192, T3 6/6144, T4 5/8192, T5 5/8192, T6 5/6144, T7 4/3072, T8 4/8192, T9 4/6144, T10 3/6144, T11 4/6144, T14 3/8192, T16 3/16384, heartbeat 5/4096 | identical, **+ T17 4/4096, created unconditionally** (`main.cpp:1560`) | + | T17 idles every 500 ms without a sensor. Finding 14 |
| On-demand tasks (T13 16 KB, `ota_reboot`, `ota_fb_commit`, `wifi-restart`, `ws_push`) | present | identical | = | Bench-only tasks are compiled out |
| Heartbeat | drained Q2 every 5 s | no drain | ~ | Fix |
| T2 dwell | T6 deferred silently | T6 deferred, `ESP_LOGI` + SYSTEM 29 per episode | ~ | Rows only (finding 18) |
| T2 dwell length (`ch_dwell_ms`) | per-channel dwells | `min_intv_m3` **only when the effective mode is linear** | ~ | 5C88 keeps its own dwells (1500/600 s M3) |
| T2 motor-key reload | at boot only | on `T2_NOTIFY_CFG_CHANGED` (gh#51); `T2_NOTIFY_CLEAR_DWELL` | + | Takes effect at the next stroke |
| T2 Q1 clock | one per pass | per command (gh#79) | ~ | Fix |
| `CMD_TARGET`, position stop, `CMD_PULSE` | — | M3 only, refused without a trusted position and a band | + | **Unreachable on 5C88**: no release producer passes the guards |
| `CMD_STOP`, `drive_src`, `PART_OPEN` | — | M3; refused unless the drive is the sender's; a T3 command takes the drive | + | Reachable from the LCD hold. Finding 3 |
| Boot CLOSE_ALL skip from persisted state | present | same; PART_OPEN persists as UNKNOWN | = | — |
| Motor alarm | onset / clear; commands discarded silently | same + SYSTEM 27 per discarded command | ~ | Rows only |
| T6 law | inline stepped | `stepped v2` (`drivers/ventModel`), `graded v2` only in effective mode 2 | ~ | Decision-identical at `cr_priority` 0 (below) |
| T6 effective mode | — | `dm_m3_ctrl_mode_eval()`: linear only if wanted **and** a fitted, trusted position | + | **TIMED on 5C88**, proven below |
| T6 inhibit mask | wind, motor alarm, T-sensor fault, Standby | **+ calibrating** (gh#79) | ~ | Fix |
| T6 log | MODE p0 on a step change | + p54 (effective mode) and p56 (law) at boot and on a change | + | One-off rows on 5C88: p54 = TIMED / `no_position`, p56 = stepped 2 |
| T3 wind (thresholds, hysteresis, direction, `wind_prot_en`) | as shown | identical | = | — |
| T3 CLOSE_ALL | one send, lost if Q1 full | retried each pass until accepted (gh#79) | ~ | Fix |
| T4 explicit Standby | persisted; recalibrate on clear | same, serialised; an explicit choice ends every hold | ~ | — |
| T4 LCD manual-menu Standby | persisted to NVS | a RAM-only hold (gh#65) | ~ | Fix |
| T4 LCD session end | Standby cleared, positions kept | dwell cleared, Standby cleared, CLOSE_ALL | ~ | Finding 4 |
| T4 bus KPI | — | hourly counters; address 40 skipped when not fitted | + | No bus I/O |
| T17 with `wpos_fitted_m3` 0 | — | publishes TIMED / NOT_FITTED, no probe, no bus access | + | Proven (`window_pos_task.cpp:1333-1336`, `:1467-1475`). Finding 15 is the only path to 40 |
| T5 poll sequence, retries, averaging | as shown | identical; BUSY gets a 60 s no-fault grace; fault rows carry the driver status | ~ | BUSY unreachable on 5C88 except finding 15 |

**Proof that 5C88 runs M3 timed under `stepped v2`.**
1. `wpos_fitted_m3` is absent, so it is 0. `ctrl_mode_m3` is absent, so it is **1** (finding 8).
2. `dm_m3_ctrl_mode_eval()`: `available = wanted && dm_m3_position(&pos) && pos.position_ctrl`. `dm_m3_position()` returns false when not fitted (`data_manager.cpp:1931-1940`), so `available` is false and the mode is TIMED, reason `no_position` (`:1765-1768`).
3. Promotion needs `available`, so it cannot happen while `wpos_fitted_m3` is 0.
4. T6 takes `k_laws[0]`, `vent_model_stepped`. T2's `dm_m3_ctrl_mode()` returns the same cached value, so the classic dwells apply.

**Decision rules, 2.3.1's inline law against `stepped v2`:**

| Rule | 2.3.1 | `stepped v2` | At `cr_priority` 0 |
|---|---|---|---|
| Step table, step width, close guard on temperature | as shown | byte-equivalent | same |
| Humidity vote above `rh_max` | `step_from_deviation` | same | same |
| Humidity close guard (new) | none | while the previous vote > 0 and RH > `rh_max` − max(hyst_rh/3, 1): vote 1 | same: the vote never decides alone at priority 0 |
| Dry (RH < `rh_min`) against heat | cr 0: temperature; **cr 1: 0 (closes a hot house)**; cr 2: max | always temperature | same |
| Humidity alone | cr 0: 0; cr 1/2: up to step 3 | cr 0: 0; cr 1/2: M1 at most, from `t_min` + 2 | same (0) |
| Both open | max | max | same |
| Out-of-range priority | temperature | temperature | same |

So at `cr_priority` 0 no window decision differs. The changes are log content, new rows and the calibration
inhibit. The 24 h soak in this configuration judged 14 decisions, all conforming.

### 2.2 Configuration pipeline and NVS

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| Boot load | five `nvs_load_*` ladders, every key through `nvs_cfg_get_i32_or_default` | one table, `cfg_desc.inc` (52 rows), same call | ~ | 2.3.1 wrote every default at boot, so 5C88 holds a stored value for every 2.3.1 key: **only the five new keys take defaults** |
| NVS keys `K_*` | 57 | 58: −4 `led_*`, +5 M3 keys | ~ | Finding 17. No renames: `git log -S` finds none of the new names in 2.3.1's history |
| Full inventory | 82 | 84 (+`motor/m3char` blob) | ~ | `t2_st_ch0..2` and `mode_standby` encoding identical |
| `cfg_shadow_t` | `dwell_*_min[3]` (seconds), `led_*` | `dwell_*_s[3]` (rename only, same NVS key, same unit), +5 M3 fields | ~ | No reinterpretation |
| New keys on 5C88 | — | `deadzone_m3` 20, `deadzone_src_m3` 1, `wpos_fitted_m3` 0, `ctrl_mode_m3` **1**, `min_intv_m3` 600 | + | All inert without a sensor; `ctrl_mode_m3` is finding 8 |
| Re-meant values | — | `cr_priority` 1 and 2 (gh#84); `t_min_*` as the humidity floor under 1/2 | ~ | 0 identical (§2.1) |
| Defaults of existing keys | — | no numeric change | = | — |
| Clamp on write | 39 pairs; 11 shadow keys and `ap_enable` unclamped | all 52 rows bounded | ~ | Finding 12 (load is not clamped) |
| Q4 producers | web `/api/config`; LCD `apply_param_change`; LCD System `wifi/ap_enable`; T10 geolocation (lat/lon ×4); T10 `ap_enable` = 0 | identical set (`web_server.cpp:1519`, `ui_display.cpp:714`, `:1304`, `network_manager.cpp:870/945-948/1229`) | = | — |
| Key recognition | any key written | 47 SHADOW + 1 NVS-only (`wifi/ap_enable`); others 400 / SYSTEM 30 | + | `led_*` POSTs now 400 |
| Farmer-writable keys | 10 climate + `wind_prot_en` | identical | = | LCD `cr_priority` range 0–2 → 0/1 (finding 5) |
| String keys | any | `system/tz_str` only | ~ | — |
| `GET /api/config` | 34 fields | 39 (−`dwell_*_min`, +`dwell_*_s`, +5 M3) | ~ | Finding 11 |
| `GET /api/config/limits` | 29 keys, hand-written | 41, generated from the table; `poll_interval` [30,300] → [15,120] | ~ | Finding 12 |
| IO0 stage 1 / 2 / 3 | `access` / 7 namespaces / 7 + restart | same + WiFi credentials, `dm_reload_all_cfg()` at stage 2, a SYSTEM 26 row | ~ | Finding 13 |
| `NVS_SCHEMA_VERSION` | 1 | 1 | = | Finding 19. A rollback to 2.3.1 would be NVS-safe |

### 2.3 HTTP API, web GUI and status payload

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| Routes, release build | **34** (the 2.4.6 comparison's "31" was a miscount; 2.3.1's own banner says 34) | 36 (+`GET`/`POST /api/diag/commission`, admin) | + | 10 bench routes compiled out |
| Unauthenticated routes | static files, whoami, login, logout, status, history, config/limits, sd/status, `/ws` | identical | = | — |
| Farmer or admin | `GET`/`POST /api/config` (farmer keys), `/api/mode`, `GET /api/ota/status` | identical | = | — |
| Admin-only | wifi, pin, sd, log, coredump, ota, web, ota/config, ota/check | same + diag/commission | + | Expired session now 401 (finding 11) |
| Handlers present at both | — | whoami, status, wifi, mode, sd/*, log/download, coredump/*, ota/status, web, ota/config, ota/check, ws, static: no non-comment change | = | — |
| `/api/login`, `/api/logout` | no audit row | `SESSION` rows | ~ | — |
| `/api/history` | lost the minus sign between −1 and 0 | fixed | ~ | Fix |
| `/api/log/files` | `{"sd_files":[…]}` | + `current`, `on_card`; sorted by unit-aware timestamp | ~ | Keys added only |
| `/api/ota/firmware`, `/assets` | retried a timeout forever | 30 s stall → 408 and abort | ~ | Push OTA is on-site only for 5C88 |
| GUI endpoints | 25 | 26 (+diag/commission) | + | Finding 10 |
| GUI `ns/key` pairs | 37 | 42 (+5 M3 keys) | + | All recognised |
| GUI without a sensor | — | Linear block greyed, with the reason above it ("grey, never hide") | + | Compliant; finding 8's warning line sits outside the block |
| New cards | — | Modbus bus (public), commissioning (Motors tab) | + | — |
| Status JSON `climate`, `wind`, `sun`, `system`, `mode.current` values, EG1 bits | as shown | identical keys | = | Sign fix in `climate` |
| `windows` | M1–M3 | + `M3_ctrl_mode`, `law`, `M3_ctrl_reason`, `M3_pos_gate`; position fields only when fitted | + | On 5C88: TIMED, `stepped v2`, `no_position`, `not_fitted` |
| `mode.flags` | 12 | 16 (+4 position flags, emitted only when fitted) | + | Never on 5C88 |
| `bus[]` | — | per-slave counters, address 40 left out when not fitted | + | Rides on expose bit 5 |
| `status_expose` | bits 0–5, default 0x3F | identical | = | — |
| Payload size, 5C88 | typical about 795 B, worst 1 121 B | typical about 1 000 B, worst **1 374 B** (two slaves) | ~ | T14 buffer 2 048 B (unchanged); local 4 096 B |
| Status site (`greenhouse-Controller-status-website`) | reads `climate`, `wind`, `windows`, `mode`, `sun`, `system` | already handles every 2.17.0 key and flag except `law` (ignored) | = | No dependency broken. `law` is not in the contract (finding 19) |

### 2.4 LCD and keypad (T7, T8)

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| UI states, menu handlers | 16 / 17 | identical | = | — |
| Status pages | 7, 5 s rotation | identical; page 5 gains `PART` | = | — |
| `#` per page, root menu keys | as shown | identical | = | — |
| Climate parameter table | 12 entries; day/night browse 3 items | 12 entries; browse **4 items** (T-min added) | ~ | Finding 5 |
| CR-priority edit | 0–2 | 0/1 | ~ | Finding 5 |
| System menu AP toggle | works | works (the 2.4.6 regression is absent) | = | — |
| Manual motors M1/M2 | stroke on press | identical | = | — |
| Manual motor M3 | stroke on press | tap / hold (gh#93) | ~ | Finding 3 |
| Manual-menu Standby, session end | persisted; no sweep | hold; sweep at the end | ~ | Finding 4 |
| T7 key events | press, repeat | + release (gh#93); repeat timing and Q2 depth unchanged | + | Releases are consumed before any handler |
| LCD driver | — | modes re-asserted every 10 s (gh#80) | + | — |
| PIN entry | — | failed attempts logged (`PIN_AUTH`) | + | — |

### 2.5 Log and its consumers

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| `log_type_t` | 9 | 10 (`LOG_PIN_AUTH` = 9 appended) | + | Append-only |
| `LOG_PARAM_*` | 50 (0–45, 240–243) | 72 (+46–57, +244–253) | + | None renumbered |
| CSV header and line format | `timestamp,type,initiator,ch,param,value_a,value_b` | byte-identical | = | — |
| File naming | `/<UNIT>_YYYYMMDDHHMMSS.csv` | identical | = | — |
| `LOG_SYSTEM` codes | −2, −1, 0–5, 7–24, 100–599 | + 25–32; 6 retired | + | Finding 16 (pre-existing parser gaps) |
| `LOG_MODE_CHANGE` emitters | 2, both param 0 (gh#54) | 4, told apart by param (0, 47, 54, 56) | ~ | gh#54 resolved |
| `LOG_ALARM` | T2, T3, T5 | + T5 status in `value_b`; T17 (ch 6) and characterisation params | + | T17 rows never on 5C88 |
| `SENSOR_HR` | ch 0–2 (ch 2 `value_b` 0) | + ch 3; ch 2 `value_b` = M3 % or −1 | ~ | Finding 18 |
| `log/logparser.py` | — | decodes every 2.17.0 emitter, with finding 16's four exceptions | ~ | — |

### 2.6 SD storage

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| Rotation, file I/O | 1 MB; append per line | identical, now under the driver lock (gh#90) | = | — |
| Directory scan | into an 871 B list that silently truncates | `storage_sd_foreach_csv()`, cannot truncate (gh#82) | ~ | — |
| Retention | 30, all units, from the truncated list, 1 per rotation | 30 **of this unit's files**, up to 5 per rotation | ~ | Finding 9 |
| Free-space guard | < 4 MB: delete oldest while > 5 | same, own files | = | — |
| Boot | resume the largest listed name if < 1 MB | resume the true newest own file; **no deletion at boot** | ~ | — |
| Upload enumeration (T14) | over the truncated list | full scan, timestamp against `log_last_up` | ~ | Files older than the watermark are never offered again. 5C88's uploads have been continuous since at least 2026-07-28 |
| Unmount | remounted within 30–60 s | latched until `/api/sd/mount` or reboot (gh#61) | ~ | An admin who reinserts without "mount" loses logging silently |
| Unmount during a write | could panic (gh#90) | one recursive lock | + | Fix |
| LittleFS on a mount failure | always formatted | formatted only when the contents are corrupt (gh#89) | ~ | Fix |

### 2.7 Network, OTA, ROTA, status post, Modbus

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| T16 manifest parser, seq / `min_version` gate, size ceilings | as shown | identical | = | `manifest-2.17.0.json` passes 2.3.1's parser; 64 > 44 |
| T16 download | pinned cert, 15 s, PSRAM body, `MX_TLS` | identical | = | gh#95 applies to this download (it retries hourly; nothing is written before both verify) |
| T16 quiet gate | windows still, no override/alarm/calibration, no session | + no teach or characterisation running | + | Never true on 5C88 |
| T16 high-water write | at T13 spawn | at T13 spawn | = | Finding 2 |
| T13 zip support | STORE only, basenames | same + gh#89 rules, `VFS_MAX_COUNT` 12 | ~ | The 2.17.0 zip: 4 STORE entries, 173 456 B, the same four names |
| T13 spawn | 16 KB internal stack | same | = | gh#94: on the ROTA path it fails clean |
| 3-fail rollback | identical body | identical; healthy after 30 s | = | Finding 1 |
| T10 network | STA/AP, SNTP, geolocation | + `nm_wifi_erase_persistent()` on IO0 2/3 | + | Finding 13 |
| T14 status post | — | comment-only diff; 2 048 B buffer | = | — |
| Modbus driver | no mutex; task GPIO DE/RE with echo discard; IFG 4 ms | mutex (gh#49), locked re-init (gh#69), UART RTS DE/RE in IRAM (gh#70), IFG **20 ms**, per-slave counters | ~ | Finding 6 |

### 2.8 Build, boot compatibility, heap

| Surface | 2.3.1 | 2.17.0 | Δ | Defect? |
|---|---|---|---|---|
| `partitions.csv` / `partitions-<v>.bin` | blob 7d5be97 / sha 18fbe59a… | identical bytes | = | — |
| Platform, ESP-IDF | espressif32@6.12.0, IDF 5.5.0 | identical | = | — |
| `sdkconfig.defaults` | — | +5, none removed: `UART_ISR_IN_IRAM`, `VFS_MAX_COUNT` 12, `APP_COMPILE_TIME_DATE` n, `BOOTLOADER_COMPILE_TIME_DATE` n, `APP_REPRODUCIBLE_BUILD` | + | Only the bootloader's banner is touched, and the bootloader is not delivered by OTA |
| `sdkconfig.lolin_s3` | untracked | untracked | ? | Not diffable; inferred from the identical defaults and the identical bootloader string set |
| App image header | mode 2, 16 MB, 80 MHz, S3, rev 0–65535 | identical | = | — |
| App `secure_version` | 0 | 0 | = | Anti-rollback not involved |
| App size | 1 379 152 B | 1 435 200 B | ~ | Fits the 2 MB bank and `ROTA_FW_MAX` |
| Bootloader | 22 528 B | 22 480 B | ~ | 48 bytes: the compile-time log line and path strings; functionally the same |
| Internal static RAM | DRAM 62 752 B, IRAM 118 528 B | DRAM 65 900 B, IRAM 121 600 B | ~ | Finding 7 |
| Task stacks | — | + T17 4 KB | + | Finding 7 |
| mbedTLS | dynamic buffers, internal | identical | = | gh#95 |

**Heap, as TC-09 asks (steady-state free and largest block at a stated uptime, not the floor alone):**

| Unit | Firmware | Configuration | Uptime | Free | Largest | Floor since boot |
|---|---|---|---|---|---|---|
| 5C88 | 2.3.1 | production | 15.0 d (2026-10-27 07:42) | 81 KB | 31 KB | 33 KB |
| 2344 | 2.17.0 release | rig, sensor fitted, mode 2 | 14.4 d (2026-10-25 19:46) | 70 KB | 31 KB | 18 KB |
| 2344 | 2.17.0 release | **5C88's configuration** | 24 h soak (2026-10-25/26) | 70 KB, first hour to last | 31 KB, flat | 18 KB |

---

## 3. Before a promote to `mainstream`

Nothing here is a defect that should stop 2.17.0. The risk sits in the recovery path (findings 1, 2 and 6): a
fault that slips past the 30-second rollback can be cured only by a newer release or on site. So the promote
belongs to a **planned site visit**, and the visit is the cheap moment for the things only physical access gives:

1. **Before the update, on site:**
   - read `GET /api/config` (finding 12) and `GET /api/web` (`log_last_up`);
   - copy the SD card if logs from before 2026-06-10 matter (finding 9).
2. **The update:**
   - `python bin/rota_release.py promote 2.17.0` (operator instruction needed);
   - it applies in 5C88's night window, or force a check on site and log out at once (the quiet gate).
3. **After the reboot:**
   - `fw_ver` and `asset_version` both 2.17.0;
   - `windows.law` = `stepped v2`, `M3_ctrl_mode` TIMED;
   - the `bus[]` counters for addresses 1 and 44 rising with no errors over the first hour (finding 6);
   - set `ctrl_mode_m3` 0 (finding 8).
4. **Brief the farmer and the admin** on findings 3, 4 and 5.

Fixes worth making before or after (each its own issue, none blocking):
- the client's high-water write (finding 2);
- `ctrl_mode_m3`'s default and comment (finding 8);
- the 1 s poll (finding 10);
- the `abort` gate (finding 15);
- T6's stack and the high-water sweep (finding 14);
- the four parser gaps (finding 16);
- the documentation drift (finding 19).
