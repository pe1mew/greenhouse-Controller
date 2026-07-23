# Wind-alarm improvement plan — log discriminator (gh#45 Part 2) + speed hysteresis

**Status:** IMPLEMENTED in 2.3.0 (2026-07-23) — operator approved all four recommendations (D1: default 1 m/s; D2: single release; D3: B2 deferred; D4: gh#46 filed). This document is the design record; deviations from the draft are marked inline. Notable execution findings: the FRS already contained **FR-WS08** (hysteresis *timer* = the deferred B2) — the dead band was added as **FR-WS12** beside it; the T2 motor-alarm `(0,0)` ambiguity is resolved on 2.3.0+ data as a side effect of the param band; `plot_daily.py`'s ALARM_W tuple gained the `kind` element its own comment had always promised.
**Target release:** 2.3.0 (both parts are feature-bearing: new log encoding + new NVS key → minor bump, patch resets to 0 per CLAUDE.md SemVer rule).
**Origin:** 5C88 wind events of Sunday 2026-07-19, analysed 2026-07-23. Three speed episodes at `v_max = 6.0 m/s`; two decoded wrongly as direction events (gh#45, parser Part 1 fix already applied); episodes 2+3 chattered SET→CLEAR→SET within ~3 minutes because set and clear share one threshold.

Safety context: **T3's protection worked correctly** on Jul 19 — windows closed under gusts to 9.2 m/s, resumed when safe. Neither part changes when the alarm *engages*. Part A changes only what is *logged*; Part B only delays *re-opening* (the fail-safe direction).

---

## Part A — self-identifying wind-event log rows (gh#45 Part 2)

### Problem

`make_wind_log()` (`safety_monitor.cpp:107`) hardcodes `channel=0, param_id=LOG_PARAM_NONE` for every wind event. Speed-SET packs `(va=speed×10, vb=v_max×10)`, direction-SET packs `(va=dir°, vb=excl_low°)` — same row shape, overlapping value ranges, no type tag. Three consumers guess:

| Consumer | Today |
|---|---|
| `log/logparser.py` `_decode_alarm` | magnitude heuristic; boundary case fixed (Part 1) but `direction > excl_low ≤ 200` still reads as speed |
| `model/campaign-summer-2026/plot_daily.py` (~:159) | own comment: *"We can't perfectly distinguish onset from clearance from the row alone"* |
| Humans reading raw CSV | no chance |

Bonus defect the discriminator also fixes: wind CLEAR-while-disabled logs `(0,0)`, byte-identical to motor-alarm CLEAR (`logparser.py` returns the ambiguous *"Motor alarm / wind override: CLEARED"*). And because speed/direction SET are separate `if`s (`safety_monitor.cpp:252,265`), a simultaneous speed+direction trigger posts **two** same-timestamp rows — undecodable without a tag.

### Design

Populate `param_id` per event type. `param_id` is free on ALARM rows (only SETPT rows use it today), and it avoids `channel`, which T2 motor-alarm shares (`ch=0`) and T5 owns (`ch≥4`).

New enum values in `app_types.h`, placed in a reserved event-subtype band **240–243**, far above the config C-number space (highest today: 44; Part B adds 45):

```c
/* ── ALARM event-subtype discriminators (gh#45) — NOT config C-numbers.
 *    Reserved band 240..254. LOG_PARAM_NONE on an ALARM row = legacy
 *    pre-2.3.0 encoding (decode by heuristic). ── */
LOG_PARAM_ALARM_WIND_SPEED = 240,  /**< wind SET, speed: va=speed×10, vb=v_max×10 */
LOG_PARAM_ALARM_WIND_DIR   = 241,  /**< wind SET, direction: va=dir°, vb=excl_low° */
LOG_PARAM_ALARM_WIND_CLEAR = 242,  /**< wind CLEAR: va=speed×10|0, vb=dir°|0 (also disabled-while-active 0,0) */
LOG_PARAM_ALARM_WIND_FAULT = 243,  /**< wind SET, sensor-fault safe-fail: va=-1, vb=0 */
```

Motor-alarm rows keep `param=NONE` — which now *itself* disambiguates the `(0,0)` alias: `param=242` → wind clear, `param=NONE` → motor alarm clear (new firmware) or legacy-ambiguous (old logs).

### Touch points

| File | Change |
|---|---|
| `firmware/src/types/app_types.h` | 4 enum values (band 240–243) + comment reserving 240–254 |
| `firmware/src/safety_monitor/safety_monitor.cpp` | `make_wind_log(ts, param, va, vb)` gains the param arg; 5 call sites pass their code (:192 disabled-clear→242, :247 fault→243, :255 speed→240, :269 dir→241, :292 clear→242) |
| `firmware/src/safety_monitor/safety_monitor.h` | va/vb catalogue comment gains the param column |
| `firmware/src/event_logger/event_logger.h` | value_a/value_b catalogue: ALARM section documents the param band |
| `log/logparser.py` `_decode_alarm` | read `row["param"]` **first**: 240–243 → exact decode, no guessing; `param==0/NONE` → existing legacy heuristic (with the Part 1 `>=` fix) unchanged for old logs |
| `model/campaign-summer-2026/plot_daily.py` | `ALARM_W` classifier: param-first, legacy fallback; removes the "can't perfectly distinguish" caveat for new logs |
| `log/logparser.md` | document the new encoding + legacy fallback |
| `test/softwareTestPlan.md` | new IT case: boundary `speed == v_max` row carries param 240 and decodes as speed |

Per CLAUDE.md hard rule: **logparser learns the new encoding in the same changeset as the firmware change.** `app.js` needs nothing — the GUI badge reads EG1 bits from status JSON, not log rows.

---

## Part B — wind-speed hysteresis dead band

### Problem

SET and CLEAR share one threshold: SET at `avg ≥ v_max` (`safety_monitor.cpp:219`), CLEAR the moment `avg < v_max` (:281). Wind hovering at `v_max` chatters — Jul 19: SET 6.0 → CLEAR 5.8 (58 s) → SET 6.2 → CLEAR 5.3. Each toggle posts `CMD_CLOSE_ALL` then `CMD_RESUME` to Q1; with M3's 171 s travel a sub-minute reversal is real motor wear. The codebase already has the pattern: `DEF_HYST_T = 5` — *"wider band reduces window oscillation"* (`cfg_defaults.h:50`). Wind has no equivalent.

### Design

New admin-only config `wind_hyst` (m/s, integer — same unit style as `v_max`), NVS namespace `wind`:

- **SET (unchanged):** `avg ≥ v_max`
- **CLEAR (new):** all-safe only when `avg < (v_max − wind_hyst)` **and** direction outside the exclusion zone
- `wind_hyst = 0` → exactly today's behaviour (dead band off)

Implementation shape in the T3 loop — threshold depends on the current state:

```c
if (cfg.v_max > 0) {
    int32_t eff_hyst = alarm_active ? (int32_t)cfg.wind_hyst : 0;
    if (eff_hyst >= (int32_t)cfg.v_max) eff_hyst = (int32_t)cfg.v_max - 1;  /* alarm must stay clearable */
    speed_unsafe = ((int32_t)meas.wind_speed_avg_ms10 >=
                    ((int32_t)cfg.v_max - eff_hyst) * 10);
}
```

Documented subtlety (accepted, conservative): while the alarm is active — even if it was *direction*-triggered — the lowered speed threshold applies, so speed hovering in the band `[v_max − hyst, v_max)` holds the alarm until it drops below the band. Single-cause tracking was considered and rejected: more state in a safety task for a marginal case.

Runtime guard `eff_hyst ≤ v_max − 1` (above) protects against a mis-set pair (e.g. operator lowers `v_max` after raising `wind_hyst`) that would otherwise make the alarm un-clearable; the static clamp (below) is the first line, this is the belt-and-braces.

### Touch points (clones the `avg_win_wind` / `hyst_t` pattern exactly)

| File | Change |
|---|---|
| `firmware/config/cfg_defaults.h` | `DEF_WIND_HYST` (see decision D1) + `CFG_MIN_WIND_HYST 0` / `CFG_MAX_WIND_HYST 5` |
| `firmware/src/types/app_types.h` | `LOG_PARAM_WIND_HYST = 45` (next C-number) |
| `firmware/src/data_manager/data_manager.h` | `int16_t wind_hyst;` in `cfg_shadow_t` wind block |
| `firmware/src/data_manager/data_manager.cpp` | `K_WIND_HYST` constant; `nvs_load_wind`; `cfg_clamp` → `_CLAMP(CFG_MIN_WIND_HYST, CFG_MAX_WIND_HYST)`; `ns_key_to_log_id` → 45; `apply_config_update` |
| `firmware/src/safety_monitor/safety_monitor.cpp` | state-dependent threshold (above); header comment |
| `firmware/src/web_server/web_server.cpp` | config JSON field + snprintf arg + limits entry. **NOT** added to `FARMER_WIND_KEYS[]` (:1067) → admin-only by omission, farmer POST → 403 (same mechanism as `avg_win_wind`) |
| `firmware/data/index.html` | Wind-tab `slider-row admin-only` (clone the `avg_win_wind` row; tooltip explains dead band + motor-wear rationale) |
| `firmware/data/app.js` | `loadConfig` setVal + `linkAllSliders` entry |
| `manual/beheerderHandleiding.md` | Wind-tab "Alleen Beheerder" table row + NL explanation (windows close at `v_max`, reopen only below `v_max − wind_hyst`; protects motors when wind hovers at the limit) |
| `manual/boerHandleiding.md` | one line: hysteresis exists, admin-only (per the boer-manual-sync rule: note the boer *cannot* set it) |
| `design/functionalRequirementsSpecification.md` | new **FR-W05**: "A wind override raised on speed **shall** clear only after the averaged wind speed falls below `v_max − wind_hyst`; `wind_hyst = 0` disables the dead band." (FR-W01–04 exist; none covers thresholds today) |
| `design/technicalSoftwareDesignSpecification.md` | T3 row (~:236) + T3 state-machine description gain the dead band. Also fix in passing: `safety_monitor.cpp:209` cites "TSDS §5.12" for safe-fail, but §5.12 is the RGB LED — stale reference, correct it |
| `test/softwareTestPlan.md` + `test/test_04_wind_override.py` | see verification |

No LCD change: boer LCD wind menu exposes only Wnd-max / Wnd-prot; `wind_hyst` is web+admin only (mirrors `avg_win_wind`).

### B2 — minimum-hold-time before RESUME (designed, deferred)

A `wind_clear_hold_s` timer (clear condition must hold continuously for N s before `CMD_RESUME`) would debounce even a wind that dips *through* the dead band. Sketch: on clear-condition-true record `clear_since`; transition only when `now − clear_since ≥ hold`; any unsafe sample resets it. **Deferred**: ship the dead band first; add the timer only if soak still shows chatter. Two new knobs at once on a safety task is one too many.

---

## Decision points (operator call before implementation)

| # | Question | Recommendation |
|---|---|---|
| **D1** | `DEF_WIND_HYST`: `1` (dead band active after OTA) or `0` (opt-in, zero behaviour change on update)? | **1 m/s.** The change is fail-safe-directional only (never delays closing, only re-opening), and default 0 means production keeps chattering until someone remembers to configure it. Precedent cuts both ways (`avg_win_wind` chose back-compat), so flagging rather than assuming. |
| **D2** | One release (2.3.0 = A+B) or two? | **One.** Same subsystem, same soak exercises both, halves the ROTA cycles. |
| **D3** | Include B2 hold timer now? | **No** — deferred contingency as above. |
| **D4** | File a GitHub enhancement issue for Part B before implementing? | **Yes** (next free number), so the changelog/release-notes have an anchor like gh#45 for Part A. |

## Verification

Bench, deterministic (the `test_04_wind_override.py` emulator fixture already drives synthetic wind through tc07–tc10 — extend it):

- **tc11:** emulated avg exactly `v_max` → override SET, log row `param=240`, decodes "speed" (the Jul 19 boundary case, now regression-locked end-to-end)
- **tc12:** with `wind_hyst=1`: avg `v_max − 0.5` after SET → override **holds** (no RESUME on Q1)
- **tc13:** avg `v_max − 1.5` → override clears, row `param=242`
- **tc14:** `wind_hyst=0` → legacy single-threshold behaviour (regression)
- **tc15:** direction-zone SET row carries `param=241`; farmer POST of `wind_hyst` → 403
- **Parser replay:** run new `logparser.py` over the full 5C88 campaign archive (legacy rows) — byte-identical parsed output to Part-1 parser (proves the fallback); then over a bench log with new rows — exact decode, no heuristic hits.
- **Jul-19 thought-check (design validation, already done):** with `wind_hyst=1`, episode 2's CLEAR at 5.8 m/s would not fire (needs < 5.0), so episodes 2+3 merge — one close/open cycle instead of two, no sub-minute reversal.

Release + soak (standard cycle): build 2.3.0 → changelog + `bin/2.3.0/release-notes.md` → user commits → `rota_release.py release 2.3.0` → soak on 2344 + FDA4 (emulator tests + ≥ overnight; ideally a windy day) → verify `fw_ver` **and** `asset_version` (index.html/app.js changed → assets must pair) → `promote` to mainstream → 5C88 applies in its night window.

## Risks

| Risk | Mitigation |
|---|---|
| Hysteresis holds windows closed longer than expected in hot weather (ventilation lost while wind hovers in the band) | Band is small (1 m/s default, max 5); T6 resumes the moment RESUME posts; beheerder manual documents the trade-off; operator can set 0 |
| `wind_hyst ≥ v_max` config combination | static clamp 0–5 + runtime `eff_hyst ≤ v_max − 1` guard |
| Old logs mis-decode after parser change | param-first dispatch only fires on 240–243; `param=NONE` path is byte-identical to Part-1 behaviour, proven by campaign-archive replay |
| A consumer of ALARM rows missed | consumers enumerated: logparser, plot_daily, GUI (reads EG1, not rows — no change). `grep -rn "ALARM" ` across repo before release as a final sweep |
| T3 stack/WDT budget | change adds one int32 compare, no new blocking; stack note at `safety_monitor.cpp:144` unaffected |
