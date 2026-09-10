# Making motor travel/dwell changes take effect without a reboot

| Field | Value |
|---|---|
| Document | Implementation plan |
| Date | 2026-09-10 |
| Status | **Group A IMPLEMENTED, NOT YET VERIFIED ON HARDWARE.** All three envs build (`lolin_s3`, `lolin_s3_mbprobe`, `lolin_s3_bench`) and both sides of the notification API link in. Groups B–E not started. §8 has not been run — no claim is made that the fix works until the fail-first procedure has been executed on FDA4 |
| Issue | [gh#51](https://github.com/pe1mew/greenhouse-Controller/issues/51) |
| Trigger | Operator question 2026-09-10: "when travel time of a motor is set, when will the new value be applied?" Answer: at the next boot. The manual says next movement |
| Target | 2.4.2 (patch — no new NVS key, no new task, no new user-visible feature) |

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

## 4. Group C — the LCD reset that claims a completed action

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

### D1. `cfg_shadow_t` field names carry the wrong unit

`data_manager.h:129-132` documents `dwell_open_min[3]` / `dwell_close_min[3]` as **minutes**. They hold **seconds**: `CFG_MIN/MAX_DWELL_OPEN_S` is 0-1500, T2 multiplies by 1000, and the GUI labels the field "(s)". The misnomer is exposed in the public API as the JSON keys `dwell_open_min` / `dwell_close_min` (`web_server.cpp:1141-1142`).

Rename the C fields to `dwell_open_s[3]` / `dwell_close_s[3]` and fix the doc comments — internal, zero risk.

For the JSON keys, **emit both for one release**: add `dwell_open_s` / `dwell_close_s` alongside the existing keys, move `firmware/data/app.js:507-512` to the new names, and mark the old pair deprecated in a comment. Renaming outright would break any external `/api/config` consumer with no deprecation window. This is an asset change, so the **paired-commit invariant applies** — firmware and assets within 120 s.

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
