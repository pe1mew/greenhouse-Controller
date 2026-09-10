# Making motor travel/dwell changes take effect without a reboot

| Field | Value |
|---|---|
| Document | Implementation plan |
| Date | 2026-09-10 |
| Status | **COMPLETE and CLOSED.** All four groups shipped and hardware-verified on FDA4: A in 2.4.2, B and C in 2.4.3, D in 2.4.4. gh#51 closed 2026-09-10 |
| Issue | [gh#51](https://github.com/pe1mew/greenhouse-Controller/issues/51) |
| Trigger | Operator question 2026-09-10: "when travel time of a motor is set, when will the new value be applied?" Answer: at the next boot. The manual says next movement |
| Target | 2.4.2 (Group A, shipped) → **2.4.3** (Groups B+C). Patch, per operator 2026-09-10: this is a mandatory correction on the path to the M3 window-control rework, not a feature line of its own |

---

## 1. Why the code moves and not the manual

Two manual statements describe next-movement semantics:

- `manual/beheerderHandleiding.md:249` — reboot table: *"Motor travel- + dwell-times | Nee — geldt voor volgende beweging"*
- `manual/beheerderHandleiding.md:1479` — *"Niet strikt vereist (nieuwe waardes worden direct toegepast op de volgende beweging)"*

Both are **wrong today and correct after this change**, so the fix costs zero manual edits on the central claim. Documenting the reboot instead would mean editing five scattered locations and would leave nine defects standing (§6).

### 1.1 What actually happens now

`apply_config_update()` (`data_manager.cpp:829`) clamps, writes NVS, and updates DM's shadow `s_cfg.travel_s[i]`. It never tells T2. T2 caches its own copy once, at task entry (`relay_controller.cpp:988`):

```c
s_ch[ch].travel_ms = (uint32_t)(travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u;
```

Every motion path uses that cached field (`:439`, `:500`, `:548`, `:558`, `:621`) and NVS is never re-read. `relay_controller.cpp` is the **only** task that never calls `dm_cfg_snapshot()` — T6 and T3 snapshot once per loop iteration, which is why climate and wind parameters genuinely are live.

---

## 2. Group A — the refresh path (the core fix)

> **Implemented 2026-09-10** in `relay_controller.{h,cpp}` and `data_manager.cpp`. One deviation from the text below: `now_ms` is now sampled **after** the refresh block rather than before it, because `load_motor_timings()` performs nine NVS reads and the alarm debounce at 4a compares against that timestamp. The file already showed this discipline — it re-samples `now_ms` after `handle_alarm_clearance()` for the same reason.

### A1. `relay_controller.cpp` — extract `load_motor_timings()`

Split the boot init loop (`:968-998`) in two:

```c
/**
 * @brief (Re)load travel and dwell timings from NVS into the channel cache.
 *
 * Called at task entry and again on T2_NOTIFY_CFG_CHANGED.  Runs in T2's
 * own context, so T2 remains the only reader and writer of these fields
 * and no lock is needed.
 *
 * Deliberately touches ONLY the three timing fields.  Channel state and
 * the three deadlines are initialised by the boot path alone: resetting
 * them on a live refresh would lose window state and cancel a running
 * travel or dwell timer mid-stroke.
 */
static void load_motor_timings(void)
{
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        int32_t travel_s = 0, dwell_open = 0, dwell_close = 0;

        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_TRAVEL[ch],
                                   TRAVEL_S_DEFAULT[ch], &travel_s);
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_DWELL_OPEN[ch],
                                   DWELL_OPEN_S_DEFAULT[ch], &dwell_open);
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, NVS_KEY_DWELL_CLOSE[ch],
                                   DWELL_CLOSE_S_DEFAULT[ch], &dwell_close);

        if (travel_s < CFG_MIN_TRAVEL_S) travel_s = CFG_MIN_TRAVEL_S;
        if (travel_s > CFG_MAX_TRAVEL_S) travel_s = CFG_MAX_TRAVEL_S;
        if (dwell_open  < 0) dwell_open  = 0;
        if (dwell_close < 0) dwell_close = 0;

        s_ch[ch].travel_ms      = (uint32_t)(travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) * 1000u;
        s_ch[ch].dwell_open_ms  = (uint32_t)dwell_open  * 1000u;
        s_ch[ch].dwell_close_ms = (uint32_t)dwell_close * 1000u;

        ESP_LOGI(TAG, "CH%u: travel=%ld s  dwell_open=%ld s  dwell_close=%ld s",
                 (unsigned)(ch + 1u), (long)travel_s, (long)dwell_open, (long)dwell_close);
    }
}
```

The boot path becomes `load_motor_timings();` followed by the state-init loop that keeps `state = CH_UNKNOWN` and the three `*_deadline_ms = 0u`.

**This split is the whole risk in the change.** Those five state lines live in the same loop body today; carrying them into a live refresh would reset window state and cancel running deadlines.

### A2. `relay_controller.cpp` — check for a refresh at the top of the main loop

Insert as step 4a0, before the alarm debounce at `:1139`:

```c
/* ---- 4a0. Config refresh (T2_NOTIFY_CFG_CHANGED, posted by T4) ----
 * Non-blocking.  A change arriving while calib_close_all() is running
 * (up to ~171 s) is not lost: the notification bit persists and is
 * consumed on the next loop pass. */
uint32_t notify_bits = 0u;
if (xTaskNotifyWait(0u, 0xFFFFFFFFu, &notify_bits, 0u) == pdTRUE) {
    if (notify_bits & T2_NOTIFY_CFG_CHANGED) {
        ESP_LOGI(TAG, "config change -- reloading motor timings");
        load_motor_timings();
    }
}
```

`task.h` is already in scope (`xTaskGetTickCount`). `0xFFFFFFFFu` avoids pulling in `<limits.h>` for `ULONG_MAX`.

### A3. `relay_controller.h` — declare the notification bit

```c
/** T2 task-notification bits (mirrors T14_NOTIFY_CFG_CHANGED in status_post.h). */
#define T2_NOTIFY_CFG_CHANGED  (1u << 0)
```

### A4. `data_manager.cpp` — notify T2 from `apply_config_update()`

Inside the existing `if (updated) { ... }` block, after `xSemaphoreGive(MX4)`:

```c
/* Motor timings are cached inside T2 (relay_controller.cpp).  Nudge it so
 * the new value governs the next movement rather than the next boot --
 * which is what beheerderHandleiding.md:249 has always claimed. */
if (strcmp(ns_str, NVS_NS_MOTOR) == 0 && task_t2 != NULL) {
    xTaskNotify(task_t2, T2_NOTIFY_CFG_CHANGED, eSetBits);
}
```

No new dependency: `data_manager.cpp` already includes `relay_controller.h` (`:28`), `task_t2` is already an extern global, and T2's notification value is entirely unused today.

### A5. Why a notification and not Q1

Q1 looks natural — T2 drains it every loop — but it has two failure modes here:

1. `process_command()` **discards every Q1 message while `EG1_BIT_MOTOR_ALARM` is set** (`:821`, FR-MA03). A travel change made during a motor alarm would be silently dropped and stay stale until reboot.
2. Q1 is depth 8 and can back up behind a blocking `calib_close_all()`.

A notification bit has neither, and coalesces repeated changes into one refresh.

The rejected alternative of having T2 call `dm_cfg_snapshot()` would put a 200 ms-timeout mutex acquisition and a large struct copy into the relay task — priority 6, above T4/T5, and the task that must react to wind-safety commands. Reusing T2's existing NVS read leaves its blocking profile unchanged, and avoids depending on `cfg_shadow_t.dwell_open_min`, whose name says minutes while the value is seconds (Group D).

---

## 3. Group B — audit trail

> **Implemented 2026-09-10.** Three deviations and one bonus, all found while tracing the consumers — see §3.1.

Today `travel_mX` maps to `LOG_PARAM_NONE` (`data_manager.cpp:748`) so no SETPT row is written, and T2's boot line is `ESP_LOGI` — serial only. **The SD log contains no record of travel times at all.** That was defensible while the value was commissioning-only; once it takes effect immediately, a silent change to a motor safety timeout is not.

### B1. `app_types.h` — add the enum member

```c
/* 2.4.2 — motor travel time.  Previously unenumerated on the grounds that
 * it was commissioning-only; it now takes effect without a reboot. */
LOG_PARAM_TRAVEL = 46,  /**< motor/travel_mX (channel = motor 1/2/3) */
```

46 is the next free value in the config band (45 = `LOG_PARAM_WIND_HYST`; the ALARM band starts at 240).

### B2. `data_manager.cpp` — map it in `ns_key_to_log_id()`

Replace the `return LOG_PARAM_NONE;` fall-through for travel with a channel-stamped `LOG_PARAM_TRAVEL`, matching the existing `kdo`/`kdc` loop shape. Delete the comment that justifies the omission.

### B3. `log/logparser.py` — one line

```python
46: ("travel",          "s"),
```

Next to the existing `18: ("dwell_open", "s")`.

**`model/campaign-summer-2026/plot_daily.py` needs no change** — verified: it reads `param` only for the 240-243 ALARM band, and ignores config-band SETPOINT params.

---

### 3.1 What tracing the consumers turned up

**The parser had already fallen behind, and not because of this change.**
`_PARAM` in `logparser.py` stopped at **44**. `LOG_PARAM_WIND_HYST = 45` shipped
in **2.3.0 (gh#46)** and the parser was never taught it, so every `wind_hyst`
change logged since 2.3.0 renders as the bare `param#45` with no name and no
unit. Unknown ids degrade rather than crash (`_PARAM.get(param_id, (f"param#{id}", ""))`),
which is why nobody noticed. Fixed here alongside 46, because it is the same
one-line omission and the CLAUDE.md rule it violates is the rule this group
exists to satisfy.

**The channel-suffix set is separate from the name table.** `_decode_setpoint`
carries its own whitelist, `param_id in (18, 19)`. Adding 46 to `_PARAM` alone
rendered `travel: 171 s -> 30 s` with **no motor identified** — for travel the
channel is the entire point of the row. Two lists, one concept; 46 is now in
both. Verified by output, not by inspection.

**`wind_hyst` is unscaled.** Checked rather than assumed, because the gh#45 ALARM
rows encode speed as `×10`. `CFG_MIN/MAX_WIND_HYST` is 0..5 and it is compared
directly against `v_max` (1..30), so both are whole m/s and the parser needs no
divide.

**`design/logAnalysis.md`'s param table stops at 22.** Ids 23–45 were never added
to it. Rather than silently extend a table with a 23-row hole, the gap is now
stated in the document, pointing at `app_types.h` as the authoritative enum and
`log/logparser.md` as the complete decode table.

**Verification (parser side only).** Synthetic SETPT rows through `logparser.py`:

```
travel (M3): 171 s -> 30 s          <- param 46, channel carried
travel (M1): 21 s -> 60 s           <- param 46, second channel
wind_hyst: 10 m/s -> 15 m/s         <- param 45, previously "param#45"
dwell_open (M1): 300 s -> 600 s     <- control, unchanged behaviour
dwell_close (M2): 60 s -> 120 s     <- control, unchanged behaviour
```

The two dwell rows are controls: they exercise the path 46 now shares, so a
regression in the shared `ch_suffix`/`_fmt` logic would show up there.

**Verified on hardware 2026-09-10** (FDA4, 2.4.3), real SD rows, not synthetic:

```
2026-09-10T13:40:02,SETPT,WEB,1,46,21,25   ->  travel (M1): 21 s -> 25 s
2026-09-10T13:40:04,SETPT,WEB,1,46,25,21   ->  travel (M1): 25 s -> 21 s
2026-09-10T13:40:07,SETPT,WEB,0,45,1,2     ->  wind_hyst: 1 m/s -> 2 m/s
2026-09-10T13:40:09,SETPT,WEB,0,45,2,1     ->  wind_hyst: 2 m/s -> 1 m/s
```

Both directions, channel carried, and param 45 confirmed against real firmware
output — closing the 2.3.0 gap with evidence rather than inspection.

**Version:** recommend **2.5.0 (minor)**, not a patch. Every prior param-id
addition landed in a minor (2.1.0 `avg_win_wind`, 2.2.0 ROTA, 2.3.0 `wind_hyst`),
an older `logparser.py` renders the new rows as `param#46`, and CLAUDE.md treats
"changing the T9/T14 audit log format" as a change tooling must track. The
counter-argument is real though: the 12-byte record shape is untouched and this
is a new *value* in an existing field, which would make it 2.4.3. Operator's
call.

---

## 4. Group C — the LCD reset that claims a completed action

> **Implemented 2026-09-10** as `dm_reload_all_cfg()` + one call site. The MX4-nesting
> question flagged before writing is **settled** — see §4.1.

### 4.1 The nesting check, and the one thing it changed

**MX4 nesting: clear.** All six `nvs_load_*` helpers and `update_sun_times()` were
checked for an internal `xSemaphoreTake`/MX4 reference: **zero in all seven**. MX4 is
`xSemaphoreCreateMutex()` (`system_globals.cpp:119`) — plain, non-recursive — so a
nested take would have deadlocked T4 outright. Calling them inside the critical
section is safe.

**`nvs_load_mode()` is excluded, and that is a deliberate design decision, not an
oversight — filed as [gh#52](https://github.com/pe1mew/greenhouse-Controller/issues/52).** It only ever *sets* `EG1_BIT_STANDBY`:

```c
nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_MODE_STANDBY, 0, &v);
if (v != 0 && EG1 != NULL) { xEventGroupSetBits(EG1, EG1_BIT_STANDBY); }
```

It never clears it, because it is written for boot, where the bit starts clear.
After an erase the key reads 0, so calling it here would do **nothing**. Clearing
STANDBY properly means `dm_set_standby(false, ...)` — which posts
`CMD_RECALIBRATE` and drives a **full CLOSE_ALL sweep**. On 5C88 that is a real
three-minute window actuation, triggered from a menu whose entire distinguishing
feature is *"no reboot"*.

Three options, and the choice is the operator's:

| | Behaviour | Cost |
|---|---|---|
| **a (implemented)** | Leave mode alone | A unit in STANDBY stays in STANDBY; NVS says AUTOMATIC. Residual divergence until reboot — the same defect class, narrowed to one bit |
| b | `dm_set_standby(false, ...)` | Honest, and makes case 2 equal case 3 minus the reboot. But a reset menu silently starts a CLOSE_ALL sweep |
| c | Clear `EG1_BIT_STANDBY` directly | No actuation, but skips the recalibration `dm_set_standby` exists to perform, leaving window state unknown |

**(a) is implemented** because it is the only one that changes no greenhouse
behaviour, and because the erased key means the unit comes up AUTOMATIC on its
next boot regardless. Recorded here rather than buried: this is the one place
Group C knowingly leaves a divergence.

**WiFi and MQTT are out of reach too.** Case 2 erases those namespaces, but the
credentials are owned by other tasks and are not part of the cfg shadow, so a live
connection survives until reboot. Noted in the function's `@warning`.

### 4.2 Verification — not possible over the network

> **Run and passed 2026-09-10** — results in §4.3.

Case 2 is reached by the **physical IO0 button** on the controller. There is no web
or API route to it, so unlike Group A this cannot be exercised remotely. To verify
on FDA4, with 2.4.3 flashed:

1. Change a setting away from default and confirm it via `GET /api/config`
   (e.g. `hyst_t`, or `travel_m1` — which also re-exercises Group A).
2. Hold IO0 to reach the reset menu and select stage 2.
3. **Without rebooting**, `GET /api/config` → every value should read its factory
   default.
4. Confirm the serial log shows `cfg shadow reloaded from NVS (all namespaces);
   T2 + T14 notified` followed by T2's three `CH%u: travel=... dwell_open=...` lines.
5. Trigger a recalibration (mode standby → automatic) and confirm M1's pulse
   returns to the default 21+5 = 26 s — proving T2's cache followed, not just the
   shadow.

Step 5 is the one that matters: steps 3–4 only prove T4 reloaded.

### 4.3 Result — verified on FDA4, 2026-09-10 (2.4.3)

Run with the physical IO0 button, released at the `Reset settings?` stage.

**Setup mattered.** FDA4 was already at factory defaults for nearly everything — only
`travel_m3` (13 vs 171) and lat/lon differed — so a reset would have been almost
invisible and the test would have proved nothing. Eight settings were moved
off-default first, and a **baseline pulse measured** to confirm T2 was genuinely
running the marker value rather than merely reporting it.

| | Before reset | After reset | Factory default |
|---|---|---|---|
| `travel_m1` | 33 | **21** | 21 |
| `travel_m3` | 13 | **171** | 171 |
| `dwell_close_m1` | 42 | **0** | 0 |
| `hyst_t` | 9 | **5** | 5 |
| `t_max_day` | 31 | **28** | 28 |
| `avg_win_t` | 13 | **6** | 6 |
| `v_max` | 11 | **6** | 6 |
| `wind_hyst` | 3 | **1** | 1 |
| `poll_interval` | 45 | **30** | 30 |
| lat / lon | 52.225 / 5.964 | **52.0 / 5.0** | 52.0 / 5.0 |

11/11 reverted, at `uptime_s = 12456` — **no reboot**. That proves T4's shadow
reloaded.

**The step that actually mattered** — T2's private cache:

| | Before | After | Expected |
|---|---|---|---|
| M1 pulse | 36.5 s (travel 33) | **24.4 s** | ~26 = 21+5 |
| M3 pulse | 16.2 s (travel 13) | **174.8 s** | ~176 = 171+5 |

M3 is the decisive one: a **158-second** change in pulse length, on a channel never
touched during setup, explicable only by T2 re-reading NVS live. Had the cache not
followed, M1 would have stayed at ~38 s.

**Cost, as predicted.** The reset erased three secrets that are not readable from
the device — WiFi PSK, ROTA HMAC secret (`secret_set` went `true` —> `false`), and
the status-post secret. Everything else was restored from a pre-reset capture of
`/api/config`, `/api/ota/config` and `/api/web`. **Capture that before running this
test again.**

**Found in passing:** `POST /api/config` accepts an unrecognised key, returns
`{"ok":true}`, writes it to NVS and applies nothing — hit by using `poll_interval_s`
(the JSON field name) instead of `poll_interval` (the NVS key). Filed as
[gh#53](https://github.com/pe1mew/greenhouse-Controller/issues/53); same
false-success shape as gh#51.


IO0 menu case 2, *"Reset all NVS namespaces + PINs; no reboot"* (`ui_display.cpp:799`), erases seven namespaces and displays **"Settings Reset! / Defaults loaded"**. Nothing reloads DM's shadow or T2's cache, so afterwards three sources disagree: NVS is empty (defaults materialise on the next read), DM's shadow holds pre-reset values, T2's cache holds pre-reset timings.

This is broader than motor timings — climate and wind shadows are stale too — and unlike travel-time it asserts a **finished action**.

### C1. `data_manager.cpp` / `.h` — add `dm_reload_all_cfg()`

Mirrors the existing `dm_reload_web_cfg()` (`:1670`):

```c
void dm_reload_all_cfg(void)
{
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(500u)) != pdTRUE) {
        ESP_LOGW(TAG, "dm_reload_all_cfg: MX4 timeout -- shadow NOT refreshed");
        return;
    }
    nvs_load_climate();
    nvs_load_wind();
    nvs_load_motor();
    nvs_load_system();
    nvs_load_mode();
    nvs_load_web();
    update_sun_times();          /* lat/lon may have reverted to defaults */
    xSemaphoreGive(MX4);

    if (task_t2  != NULL) xTaskNotify(task_t2,  T2_NOTIFY_CFG_CHANGED,  eSetBits);
    if (task_t14 != NULL) xTaskNotify(task_t14, T14_NOTIFY_CFG_CHANGED, eSetBits);
}
```

T6 and T3 need no notification — they snapshot per loop iteration.

**Check before writing:** confirm none of the six `nvs_load_*` helpers takes MX4 internally. They are called at boot with the comment *"No mutex needed here"* and appear not to, but a nested take on a non-recursive mutex deadlocks T4.

### C2. `ui_display.cpp` — call it from case 2

One line after `pin_auth_init(); session_close(false);`. The reload re-reads the erased namespaces, so `nvs_cfg_get_i32_or_default` writes the factory defaults back — which is exactly the "Defaults loaded" the screen promises.

*Fallback if C1 is judged too broad:* change case 2 to reboot like case 3, or delete it. Smaller, but the menu loses its no-reboot reset.

---

## 5. Group D — naming and bounds

> **Implemented 2026-09-10** and released as **2.4.4**. Two findings changed the framing — see §5.1.

### D1. `cfg_shadow_t` field names carry the wrong unit

`data_manager.h:129-132` documents `dwell_open_min[3]` / `dwell_close_min[3]` as **minutes**. They hold **seconds**: `CFG_MIN/MAX_DWELL_OPEN_S` is 0-1500, T2 multiplies by 1000, and the GUI labels the field "(s)". The misnomer is exposed in the public API as the JSON keys `dwell_open_min` / `dwell_close_min` (`web_server.cpp:1141-1142`).

Rename the C fields to `dwell_open_s[3]` / `dwell_close_s[3]` and fix the doc comments — internal, zero risk.

For the JSON keys, **emit both for one release**: add `dwell_open_s` / `dwell_close_s` alongside the existing keys, move `firmware/data/app.js:507-512` to the new names, and mark the old pair deprecated in a comment. Renaming outright would break any external `/api/config` consumer with no deprecation window. This is an asset change, so the **paired-commit invariant applies** — firmware and assets within 120 s.

### D1a. `design/logAnalysis.md` states dwell in minutes

Rows C18/C19 (`:147`, `:148`) give the dwell value as *"Old value (min)"* / *"New value (min)"*. They are seconds. Same root misnomer as D1, propagated into a design document — found while adding param 46 to that file's table, and left for D1 so the naming work lands in one commit.

### D2. Six wrong tooltip bounds in `index.html`

Against a real ceiling of 1500 s for both:

- `:312`, `:319`, `:326` — dwell open, say *"Max 600 s"*
- `:334`, `:341`, `:348` — dwell close, say *"Max 300 s"*

Asset-only change; same paired-commit rule.

---

## 6. Issue traceability

| # | Defect | Fixed by |
|---|---|---|
| 1 | GUI shows the new value with a ✓ while T2 runs the old one | A1-A4 (value is now live, so the display is correct; convergence within one 20 ms loop tick) |
| 2 | Wind-safety `CMD_CLOSE_ALL` uses the unapplied travel pulse | A1-A4 |
| 3 | No audit trail; no way to reconstruct which value was in force | B1-B3 |
| 4 | LCD "Defaults loaded" is false for motor, climate and wind | C1-C2 |
| 5 | Commissioning needs a reboot, usually costing a full CLOSE_ALL | A1-A4 |
| 6 | Blocks §3.5 of `integrateWindowPositionSensor.md` (measured traverse cannot be applied by the action that measured it) | A1-A4, then E3 |
| 7a | Shadow/cache divergence never detected | A1-A4 — divergence is bounded to one loop tick, so there is nothing to detect. `xTaskNotify` on a valid handle cannot fail, and no config path exists before T2 spawns (T8 and T11 both start after it) |
| 7b | `dwell_open_min` / `dwell_close_min` carry seconds | D1 |
| 7c | Tooltip bounds wrong | D2 |

**Verified not a defect:** travel is not published to the remote status site — `status_post.cpp` carries no travel field, so 5C88's remote view never showed an unapplied value.

---

## 7. Group E — documentation

- **E1.** `manual/beheerderHandleiding.md:249` and `:1479` — **no edit needed.** Both become true. Add the audit-log row for `travel` to the log-parameter chapter (B1), and note in the Motors tuning procedure (~`:620-624`) that a change governs the next movement, so tuning no longer needs a power-cycle.
- **E2.** LCD reset section — document that case 2 now genuinely reloads (C2).
- **E3.** `design/integrateWindowPositionSensor.md` §3.5 — remove the constraint recorded on 2026-09-07 that a measured traverse cannot reach T2 without a reboot.
- **E4.** `changelog.md` — new `## [2.4.2]` section; `bin/2.4.2/release-notes.md`.
- **E5.** `firmware/platformio.ini:93` — `2.4.1` → `2.4.2`. The `lolin_s3_bench` env's `-DFIRMWARE_VERSION` needs the same bump (it is listed explicitly, not inherited).
- **E6.** `memory/gotcha-log.md` — a cached-at-boot config value behind a live-looking GUI is a pattern, not a one-off: the same shape produced this bug and the LCD-reset bug independently.

---

## 8. Verification — fail-first, on FDA4

### 8.1 Results — FDA4, 2026-09-10 (Group A)

No serial or LCD access was available, so the measurement was made entirely over
HTTP. **The instrument:** `POST /api/mode` standby → automatic makes
`dm_set_standby(false)` post `CMD_RECALIBRATE`; `calib_close_all()` (`:621`)
energises all three channels **unconditionally** using `s_ch[ch].travel_ms`, and
window states appear in `/api/status`. Polling at 1 Hz therefore measures the
pulse the firmware actually used. Measurements read ~2 s short of the true
duration because the poll baseline starts after the mode POSTs; the offset is
constant across runs. Script: `travel_probe.py` (scratchpad).

| Run | Firmware | `travel_m1` written | T2's boot-loaded value | Measured M1 | Reading |
|---|---|---|---|---|---|
| 1 | 2.4.1 | **40** | 21 | **24.0 s** (~26 = 21+5) | old value used — **defect reproduced** |
| 2 | 2.4.2 | **21** | 40 | **23.6 s** (~26 = 21+5) | new value used, no reboot |
| 3 | 2.4.2 | **60** | 40 | **63.3 s** (~65 = 60+5) | matches neither 26 nor 45 |

**Run 1 is the fail-first gate** and it failed as required, on the unpatched build,
with the same instrument.

**Why run 3 exists.** Runs 1 and 2 both measure ~24 s — the same number from a
defect and from a fix, separated only by what NVS held at boot. That is correct
reasoning but poor evidence: too easy to misread, and impossible to check later
without reconstructing the boot state. 60 s matches neither the cached 40 (45 s)
nor the old 21 (26 s), so only a genuine runtime reload explains it. Run 3 also
carries its own control: **M2 (unchanged, 21) stayed at 23.9 s and M3 (unchanged,
13) at 15.7 s in the same sweep** — only the channel written moved.

**Also confirmed in passing:** `/api/config` reported `travel_s = 40` while T2 was
still pulsing 26 s (§6 issue 1, on live hardware); and the Q4 hand-off has a
sub-second latency — a `GET /api/config` 0.2 s after the POST still read the old
value, the shadow updating shortly after. That latency is not the defect, but it
is why the GUI's success tick cannot be read as "in force".

**Cases 3–8 below were NOT run:** mid-stroke change, motor alarm, mid-calibration,
boot-calibration skip, audit row (Group B not implemented), LCD case 2 (Group C not
implemented).

**Post-state:** `fw_ver` and `asset_version` both 2.4.2 (read post-reboot),
`eg1 = 0x0`, AUTOMATIC, all CLOSED, `travel_s` restored to `[21, 21, 13]`.

**Incidental:** FDA4's `travel_m3` is **13 s**, not the production 171 — worth
knowing before reading any FDA4 timing measurement as production-representative.


There is no native test target for T2 (`drivers/modBus/test/` is the only test tree), so this is hardware-only. FDA4 drives no window, so the evidence is T2's own log line at `:442`/`:503`: `CH%u: → MOVING_OPEN (travel %lu ms)`.

1. **Fail-first.** On the *current* build, set travel_m1 21→40 and command a move. Confirm it still logs `26000`. If it logs `45000`, the test is not measuring what I think it is and a later pass proves nothing.
2. After the fix, the same steps log `45000` with no reboot.
3. **Mid-stroke change** — change travel during a move; confirm the stroke ends on the old deadline and the *next* one uses the new value.
4. **During a motor alarm** — assert J10, change travel, clear the alarm, confirm the new value took. This is the case Q1 would have dropped.
5. **During calibration** — change travel while `calib_close_all()` is running; confirm the refresh applies on completion rather than being lost.
6. **No state regression** — reboot with all three channels recorded CLOSED and confirm boot calibration is still skipped (`:1103`). That path shares the code being split in A1.
7. **Audit row** — change travel, pull the SD log, confirm a SETPT row with `param=46` and the correct channel, and that `logparser.py` decodes it.
8. **LCD case 2** — run the reset, then read `/api/config` and T2's log without rebooting; confirm defaults in both.

Soak on FDA4 overnight before any `promote` to 5C88.

---

## 9. Open decisions for the operator

1. **Is Group B in scope?** It changes the log format. Cheap here (one enum value, one parser line, `plot_daily.py` unaffected), but it is a format change and those have a standing rule.
2. **Group D's JSON keys** — dual-emit for one release as proposed, or rename outright? Dual-emit is safer; outright is cleaner if there are genuinely no external consumers.
3. **Group C scope** — full `dm_reload_all_cfg()`, or the one-line "make case 2 reboot" fallback?
4. **Should `travel` also appear in the boot LOG_SYSTEM row?** SETPT rows give the change history, but a log with no SETPT row leaves the in-force value implicit. Probably unnecessary; noted rather than assumed.

### 5.1 Two things the survey changed

**The `_min` suffix is a real convention, not a typo.** `session_timeout_min` and
`ap_timeout_min` sit a few lines below in the same struct and the same JSON object,
and both genuinely are minutes (defaults 5 and 30). The dwell pair followed the
convention while meaning seconds, which is precisely why it survived this long. The
name was ambiguous a second way too: the doc comment opens *"**Min** hold at
OPEN…"*, so `_min` could be read as "minimum". `_s` kills both readings and lines
up with `DEF_DWELL_OPEN_M1_S` and `CFG_MAX_DWELL_OPEN_S`.

**The dwell-close tooltip was the worse of the six.** It claimed *"Max 300 s"*
against a real ceiling of 1500 — and M3's own factory default for dwell-close is
**600 s**. An operator reading that tooltip would have concluded the shipped default
was out of range. The dwell-open trio said 600 against M3's 1500 default, same shape.

**Checked before adding to the JSON:** `/api/config` builds into a 1536-byte buffer
with no truncation guard on the `snprintf` return — it sends `n` bytes, which would
over-read if it ever truncated. The live response is 620 bytes and the two new arrays
add ~60, so this lands at 44% of cap. Ample, but the missing guard is worth knowing
about before anyone adds a large field there.

### 5.2 Group D verified — FDA4, 2.4.4, 2026-09-10

| Check | Result |
|---|---|
| `/api/config` dual-emits `_s` and `_min`, identical values | `[300,300,1500]` / `[0,0,600]` |
| `app.js` parses; fallback logic evaluated in the live page | `[1500, 600]` for new **and** old firmware shapes |
| Six tooltips | 6x `Max 1500 s (25 min)`, 0 stale |
| Motors tab populates all six dwell fields | confirmed visually |
| Dwell round-trip emits its audit row | `dwell_open (M1): 300 s -> 420 s` and back |

`app.js` was the check that mattered: it is an **asset**, so the firmware build never
parses it and a syntax error would have left the Motors tab blank with everything else
looking normal. `/api/config` came out at **676 bytes** against the 1536 buffer.

**The tooltips had been contradicting the control beside them.** Sliders always took
their bounds from `/api/config/limits` (1500), so a dwell-open slider ran to 1500 while
its own tooltip claimed a 600 s maximum — and for dwell-close the tooltip said 300
against M3's *factory default* of 600.

### 5.3 Left open deliberately

- **gh#52** — `nvs_load_mode()` sets `EG1_BIT_STANDBY` and never clears it.
- **gh#53** — `POST /api/config` accepts an unknown key, returns `ok:true`, writes junk
  to NVS and applies nothing.
- **Deprecation:** drop the `dwell_open_min` / `dwell_close_min` JSON aliases in the next
  minor.
- **Not this issue:** the ROTA soak channel offers 2.4.1 while FDA4 runs 2.4.4 — 2.4.2
  through 2.4.4 were push-OTA'd and never published.
