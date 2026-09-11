# Release 2.4.7

**Date:** 2026-09-11
**Built on:** 2.4.6
**Fixes:** a 2.4.6 regression — the WiFi-AP toggle stopped working — plus one latent ordering defect, found by a full 2.3.1-vs-2.4.6 comparison

## The regression

Within minutes of 2.4.6 landing on FDA4 the operator reported that the LCD could not
enable the WiFi AP. Live proof on the unit:

```
POST /api/config {"ns":"wifi","key":"ap_enable","value":0}  ->  400 {"ok":false,"err":"unknown ns/key"}
```

`wifi/ap_enable` has **no `cfg_shadow_t` field at all** — T10 polls it straight out of
NVS every 5 s (`network_manager.cpp:1185`). For that key the NVS write *is* the
mechanism. The gh#53 fix in 2.4.6 defined "known" as "has a shadow-ladder arm" and
rejected it before the write. Three producers went silent:

- `ui_display.cpp` `handle_menu_system()` key '1' — the LCD System-menu AP toggle.
  **This is what the operator hit.**
- `network_manager.cpp` `poll_ap()` — T10 clearing the stale flag on AP timeout. With
  the clear rejected, NVS keeps `ap_enable=1`; `s_ap_enable_nvs` initialises to −1, so
  the next boot sees 1 and **restarts the AP**. A stale flag becomes permanent.
- `POST /api/config` — 400.

The AP is the recovery path for a unit that has lost its station credentials. On 5C88,
behind NAT with no push path, it is the only way in. **This blocked promoting 2.4.6.**

### Why it was missed

The 2.4.6 verification enumerated the web GUI's 33 posted keys out of `app.js`, then
*reasoned* about the LCD and T10 instead of listing them — the code comment even
asserted the predicate was "defence in depth for the LCD, which is the other Q4
producer". The LCD *parameter* tables (12 climate + 2 wind) were fine. The System-menu
toggle and T10's `post_q4()` were not in any list.

The rule that follows is in the gotcha log: when a change gates a shared queue, table or
key set, **enumerate every producer and consumer with `grep`** and put the list in the
release notes. "I checked the callers" without the list is not a check.

## The fix

The boolean is replaced by a three-way classification, `cfg_key_kind()`:

| kind | meaning | action |
|---|---|---|
| `UNKNOWN` | nothing in this firmware writes it via Q4 | reject (400 / Q4 drop) — as 2.4.6 |
| `SHADOW` | has a `cfg_shadow_t` field | NVS + shadow + audit row — as before |
| `NVS_ONLY` | legitimate Q4 traffic, consumer reads NVS itself | NVS write only, no audit row |

`wifi/ap_enable` is the one NVS-only entry. The table names its producers and its
consumer, and the key is now range-clamped 0/1 as well (`/api/config` accepts it from an
admin). `dm_cfg_key_is_known()` keeps its signature and means "not UNKNOWN".

**Every Q4 producer in the tree, enumerated** (`grep -rn 'xQueueSend(Q4\|post_q4('`):

| producer | ns/key | kind |
|---|---|---|
| `web_server.cpp:1395` — `POST /api/config` | whatever the GUI posts: 33 pairs, all in the shadow ladders | SHADOW |
| `ui_display.cpp:658` — `apply_param_change()` | `CLIMATE_PARAMS` (12) + `WIND_PARAMS` (2) | SHADOW |
| `ui_display.cpp:1216` — System menu '1' | `wifi/ap_enable` | **NVS_ONLY** |
| `network_manager.cpp:924-927` — geolocation | `system/lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` | SHADOW |
| `network_manager.cpp:1208` — AP timeout | `wifi/ap_enable` | **NVS_ONLY** |

## The latent defect

`dm_reload_all_cfg()` restored the operating mode (which may post `CMD_RECALIBRATE`)
*before* posting `T2_NOTIFY_CFG_CHANGED`. T2 consumes task notifications at the top of
its loop (`relay_controller.cpp:1201`) before it drains Q1 (`:1253`), so notify-first is
sufficient to guarantee a queued sweep runs on the reloaded timings. 2.4.6 had it the
wrong way round. Unreachable today — the only caller's `session_close()` clears STANDBY
first, so the restore early-returns — but the next caller would have hit it.

The same function now takes `(initiator, channel)` from its caller. 2.4.6 hard-coded
`LOG_BY_SYSTEM`/0 for the mode row, which is byte-for-byte a T6 vent-step row on the
shared `LOG_MODE_CHANGE` type (gh#54). The IO0 reset passes `LOG_BY_ADMIN`, 1.

## Known limitation (documented, not fixed)

On the IO0 stage-2 reset the recalibration is queued by `session_close()` *before*
`dm_reload_all_cfg()`, so it sweeps on the pre-erase timings. Seen on FDA4: M3 swept in
18 s (cached 13 s) while NVS already held the 171 s default. Benign on production; safe
direction on the rig. Reordering changes post-reset dwell semantics (the calibration
sets `dwell_close` deadlines; a `T2_NOTIFY_CLEAR_DWELL` arriving mid-sweep would wipe
them), so it is deferred to the gh#54 work rather than done in a hotfix.

## The 2.3.1 → 2.4.6 comparison

`design/releaseComparison_2.3.1_vs_2.4.6.md` lists every functional surface at both
versions — tasks, routes, GUI keys and endpoints, LCD handlers and tables, all five
config-key tables and their cross-references, log types/params and all three parser
consumers, the Modbus driver, network/OTA/ROTA/status/storage, build envs — extracted
mechanically and diffed, with the control-path source read as full diffs. Nine findings;
the two above are the only new defects. T3, T5, T6, T10, T13, T14, T16, the logger and
storage have **empty** non-comment diffs against production.

Also recorded there, pre-existing and identical at both versions: 11 shadow keys with no
range clamp on the raw API path (`cr_priority`, `rh_ctrl_en`, `wind_prot_en`, the four
`lat/lon` parts, the four `led_*`) — the LCD and GUI constrain their own input, so only a
hand-written POST reaches it. `vent_resolve_conflict()` has a `default:` arm, so an
out-of-range `cr_priority` degrades to a defined branch rather than undefined behaviour.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`).

**To verify on FDA4 after the OTA** (both `fw_ver` and `asset_version` must read 2.4.7
post-reboot):

1. `POST /api/config {"ns":"wifi","key":"ap_enable","value":0}` → **200** (was 400).
2. From the LCD: System menu → '1' → the AP comes up (`Greenhouse-FDA4` visible); '1'
   again → it goes down. **This is the operator's original report.**
3. `POST /api/config {"ns":"system","key":"poll_interval_s","value":45}` → still **400**
   — the gh#53 fix must survive.
4. `POST /api/config {"ns":"system","key":"poll_interval","value":45}` → 200 and applied;
   restore to 30.
5. Soak ≥ overnight, then `rota_release.py release 2.4.7` → soak channel → promote.
   **Do not publish 2.4.6.**

## Upgrade notes

No NVS migration, no partition change. `dm_reload_all_cfg()` gained two parameters; it
has one caller. Anyone scripting `/api/config`: `wifi/ap_enable` is accepted again
(admin, 0/1); every other rule from the 2.4.6 notes stands.
