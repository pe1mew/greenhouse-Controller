# 2.0.0-rc.1.5.1 — release notes

**Date built:** 2026-05-26 (rev. 2 — bundles three operator-feedback fixes into one patch before any OTA)
**Built on top of:** 2.0.0-rc.1.5.0 (gh#28 STANDBY + gh#29 admin manual motor control)

**Three operator-visible improvements bundled into this patch:**
1. **gh#29 manual-menu UX fix** — *"climate control kicks-in after closing a window"*: the 10 s idle dismiss in rc.1.5.0 let T6 undo manual actions. Now the menu auto-enters STANDBY for the full session.
2. **WINDOW_CAL select alignment** — *"webgui standby setting and behaviour shall follow the LCD behavior with respect to window cal"*: during a calibration cycle the Climate-tab Mode select now shows `Calibrating windows...` instead of misleadingly showing `Normal (autonomous)`.
3. **Standby badge in Alarms shield** — *"in web gui when in standby a amber badge shall appear in alarms shield"*: new amber `Standby` badge in the local web GUI Alarms card and on the public dashboard.

**Scope:** firmware + web GUI (app.js + status_json.cpp). All three fixes are bundled into the same rc.1.5.1 build because none of them have been OTA'd yet; folding them avoids a version-number proliferation.

---

## Why a patch bump (1.5.0 → 1.5.1)

Three observable improvements from operator feedback after the rc.1.5.0 OTA, all in the same code surface and shipped together. No new HTTP route, no SD-log format change, no breaking API change — so a patch bump is the right granularity.

The one new internal API (`dm_set_standby_ex()`) is a thin extension for `data_manager.cpp`; it doesn't appear in any HTTP route or operator-facing surface. The original `dm_set_standby()` keeps its exact prior semantics via a wrapper.

---

## What changed

### Observed behaviour (rc.1.5.0, reported by operator after the 11:01:57 OTA)

> "after manually opening, auto rotate of displays starts. this is undesired and should only start after user stepped back to auto rotate windows or at timeout of admin session. when going to manually control windows, unit should change from auto to standby because, climate control kicks-in after closing a window. at a manual operation, the unit should step back to auto at the timeout of the session of the admin."

### Root cause

rc.1.5.0 used a separate, RAM-only EG1 bit (`EG1_BIT_MANUAL_SESSION`, bit 8) to gate T6 while the admin was in the manual-motor menu. The bit was set on menu entry and cleared on every menu exit — including the `MANUAL_MENU_IDLE_TICKS = 100u` (10-second) idle dismiss.

In physical testing, the admin would:
1. Open Scherm 6 → `#` → enter admin PIN → land in `UI_MOTOR_PICK`
2. Pick a motor → issue an OPEN/CLOSE command → land back on `UI_MOTOR_ACTION` waiting for next input
3. Pause for >10 seconds (looking at the windows, talking to a colleague, thinking)
4. Menu auto-dismissed at the 10 s mark → `EG1_BIT_MANUAL_SESSION` cleared → LCD started auto-rotating status screens
5. T6 woke up on its next decision tick (~30 s later), still saw the hot greenhouse, re-issued an OPEN command that undid the admin's manual CLOSE

The behaviour matches the design as shipped — but the design itself was wrong: a 10-second idle window is far too short for human deliberation, and the transient bit semantics were structurally weaker than full STANDBY.

### Fix (rc.1.5.1, this release)

| Change | Mechanism |
|---|---|
| **Menu entry auto-enters STANDBY** for the full menu lifetime | `dm_set_standby_ex(true, LOG_BY_ADMIN, 1, false)` called from both # dispatch paths (direct admin and PIN-success), only if STANDBY wasn't already on |
| **No more 10-second idle dismiss** for the manual menu | `MANUAL_MENU_IDLE_TICKS` define removed; manual-menu states exempted from the menu-auto-return tick |
| **No more 5-minute generic menu-auto-return** for the manual menu either | `UI_MOTOR_PICK` / `UI_MOTOR_ACTION` skip the auto-return idle counter increment |
| **Only two exit triggers**: explicit `*=back` from `UI_MOTOR_PICK`, or admin PIN session timeout (default 5 min) | Both call `go_status()` which clears the auto-set STANDBY |
| **Menu exit clears STANDBY without recalibration** | `dm_set_standby_ex(false, LOG_BY_ADMIN, 1, false)` — the `recalibrate_on_clear=false` argument suppresses the CLOSE_ALL Q1 post; the admin's manual per-channel positions are preserved (FR-MM07 intent) |
| **Pre-existing STANDBY is respected** | A new `s_manual_set_standby_on_entry` flag tracks whether the menu set STANDBY; only cleared if true. STANDBY set independently via Scherm 3 or web survives the manual session. |
| **`EG1_BIT_MANUAL_SESSION` removed** | Bit 8 of EG1 reserved. T6's inhibit mask drops to `MOTOR_ALARM | WIND_OVERRIDE | SENSOR_FAULT_T | STANDBY` — STANDBY is the single gate. |

### Why STANDBY-exit-without-recal for the manual menu, but STANDBY-exit-WITH-recal for Scherm 3 / web?

Locked decision from the operator on 2026-05-26: the two surfaces have different semantics.

- **Scherm 3 / web GUI STANDBY** is the operator saying "pause climate control entirely; I'm not touching the windows right now (or I'm using the motorbox handbedienings-schakelaars to do something the controller can't see)". When the operator un-pauses, a clean CLOSED baseline is the right starting state for T6 because the controller can't trust any per-channel position it remembers.
- **gh#29 manual-menu STANDBY** is the operator saying "T6, hold still while I deliberately set each window where I want it". When the operator exits, T6 should pick up from the per-channel positions the admin just set — closing them all just to re-open them would undo the admin's work.

The asymmetry is encoded in `dm_set_standby_ex(..., recalibrate_on_clear)` — `dm_set_standby()` (the existing API) defaults to `true` so all existing callers keep their behaviour; only the gh#29 paths pass `false`.

---

## Standby badge in Alarms shield

**Observed gap:** when STANDBY was active in rc.1.5.0, the only visual indicator was the "Standby" pill in the Status-tab Mode tile. The Alarms tile — the operator's at-a-glance "what's happening?" panel — showed only the green OK badge, hiding the fact that climate control was deliberately paused. Same gap on the public dashboard.

**Fix:**
- Firmware: `firmware/src/status_post/status_json.cpp` — added `{ EG1_BIT_STANDBY, "standby" }` to the `EG1_FLAGS` table. The canonical status JSON's `mode.flags[]` array now includes `"standby"` whenever EG1_BIT_STANDBY is set. Same flag string lands at both `/api/status` (local GUI) and the T14 status-website POST (public dashboard).
- Web GUI: `firmware/data/app.js` — added `standby: '<span class="badge warn">Standby</span>'` to the `flagBadges` mapping. The Alarms card now renders an amber "Standby" badge whenever the flag arrives.

**Badge styling:** amber (`badge warn` CSS class), same colour as `Wind protect off` and `Humidity ctrl off` — operator-initiated state, not a fault.

## WINDOW_CAL select alignment with LCD

**Observed gap:** during a CLOSE_ALL recalibration triggered from the Climate-tab "Mode" select (when you switch from Standby back to Normal), the controller spends ~3 minutes in `Mode: WINDOW_CAL` while M3 closes. The LCD's Scherm 3 shows `Mode: Window Cal.` for the duration — clear. But the web GUI's Climate-tab Mode select flipped back to "Normal (autonomous)" immediately (greyed out, but the label was misleading: you thought the toggle had taken effect, instead the controller was still mid-sweep).

**Fix:** `firmware/data/app.js` — when `mode.flags[]` contains `"calibrating"`:
- Dynamically insert a transient option `<option value="_calibrating">Calibrating windows...</option>` at the top of the select
- Set it as the selected value, lock the select + Apply button
- On the next status push where `calibrating` is no longer in flags: remove the transient option, restore the live selection based on `mode.current`

The underscore-prefixed value never makes it to the server contract — even if someone forced a POST with it, `/api/mode` would 400 on `bad_mode`. The button is disabled anyway.

**Consistency:** the operator now sees the same state-name on both surfaces during the 3-minute recalibration window — LCD shows `Mode: Window Cal.`, web shows `Calibrating windows...` in the select where the mode choice would normally appear.

## API delta (data_manager)

| Symbol | Status |
|---|---|
| `dm_set_standby(bool standby, log_initiator_t init, uint8_t channel)` | Unchanged signature, unchanged behaviour (now a thin wrapper around `dm_set_standby_ex(..., true)`) |
| **`dm_set_standby_ex(bool standby, log_initiator_t init, uint8_t channel, bool recalibrate_on_clear)`** | **NEW** — used by the gh#29 paths with `recalibrate_on_clear=false` |
| `dm_get_standby()` | Unchanged |

---

## Operator-visible behaviour

| Scenario | rc.1.5.0 | rc.1.5.1 |
|---|---|---|
| Admin opens Scherm 6 → # → enters PIN → menu | `EG1_BIT_MANUAL_SESSION` set; mode still shows "AUTO" | `EG1_BIT_STANDBY` set; mode shows "STANDBY" on Scherm 3 |
| Admin issues OPEN/CLOSE, waits 15 sec | Menu auto-dismissed; LCD auto-rotates; T6 may re-open/close | Menu stays open; T6 stays paused; LCD stays on `UI_MOTOR_ACTION` |
| Admin presses `*=back` from `UI_MOTOR_PICK` | Menu dismissed; T6 resumes immediately; manual positions held briefly then re-evaluated | Menu dismissed; STANDBY cleared (no recal); T6 resumes from actual per-channel state — admin's manual positions are the baseline |
| Admin walks away; 5-min session timeout fires | Session closes; `EG1_BIT_MANUAL_SESSION` cleared; T6 resumes (mid-thought) | Session closes; menu auto-exits; STANDBY cleared (no recal); T6 resumes from actual per-channel state |
| Admin had STANDBY already on via Scherm 3, then enters manual menu, then exits | `EG1_BIT_MANUAL_SESSION` cleared (it was set on menu entry); STANDBY stays on (because Scherm 3 set it) | `EG1_BIT_STANDBY` stays on (admin had it before menu entry; `s_manual_set_standby_on_entry` records that the menu didn't set it, so menu exit leaves it alone) |

---

## What did NOT change

- **`POST /api/mode`** behaviour (gh#28) — Web Standby toggle still triggers CLOSE_ALL recalibration on exit. The rc.1.5.0 verification trail for `/api/mode` is unchanged.
- **LCD Scherm 3 mode-toggle** PIN flow and render — unchanged.
- **rc.1.4.0 SD-log format** (`LOG_SENSOR_HR` triplet, `LOG_SUN` change-detect, 1 MB rotation cap, 30-file retention) — preserved byte-for-byte.
- **rc.1.3.3 NTP-resync fix** preserved.
- **Web GUI** — `index.html`, `app.js`, `style.css`, `manifest.json` content is byte-for-byte identical. Only the manifest's `asset_version` string bumps from `2.0.0-rc.1.5.0` to `2.0.0-rc.1.5.1`.
- **LOG_MODE_CHANGE row format** — still `initiator | channel (0=web, 1=LCD) | value_a (0=leave, 1=enter)`. The same audit format covers both gh#28 (Scherm 3 / web) and gh#29 (manual menu) STANDBY transitions; the channel field differentiates the surface.
- **`LOG_RELAY` row format** for manual commands — still produced by T2 when the relay is actually energised; the `cmd_source_t.source = SRC_OPERATOR_MANUAL` field threads admin attribution through T2's per-command log line as before.

---

## Build delta vs rc.1.5.0

| File | rc.1.5.0 | rc.1.5.1 | Δ |
|---|---:|---:|---:|
| `greenhouse-controller-*.bin` | 1 356 928 B | 1 357 136 B | **+208 B** |
| `web-assets-*.zip` | 103 864 B | 105 884 B | **+2 020 B** (app.js Standby badge + WINDOW_CAL select logic) |
| Bootloader | 22 528 B | 22 528 B | 0 |
| Partition table | 3 072 B | 3 072 B | 0 |
| `firmware-*.elf` (debug) | 13 071 196 B | ~13 072 KB | +small |

Link-report usage:
- RAM: 18.9 % (62 016 / 327 680 B) — unchanged
- Flash: 64.7 % (1 356 729 / 2 097 152 B) — essentially unchanged

Build is clean: **zero source-tree warnings**.

---

## SHA-256

```
60b62190507092c8fd956e6ef13ce09dbb929eab9b22c467b458f61a637adf1b  greenhouse-controller-2.0.0-rc.1.5.1.bin
50ce7f58bbd2b2bbb49971a0ed197be1bdb7bfb67ca031801869f2982f05b522  web-assets-2.0.0-rc.1.5.1.zip
```

---

## Deployment record

| Step | Status |
|---|---|
| Build artifacts produced | ✅ 2026-05-26 (this directory) |
| SHA-256 captured | ✅ |
| OTA-flashed to soak unit (`192.168.20.160`, unit 2344) | ⏳ awaiting operator instruction |
| Phase 7 soak clock restart | ⏳ pending OTA |

The rc.1.5.0 soak clock that started at 2026-05-26 11:01:57 is superseded by this release. Day-14 for rc.1.5.1 starts at the moment of the rc.1.5.1 OTA.

---

## Verification checks (post-OTA, physical access required for gh#29 path)

### Smoke (web only — admin can do remotely)

1. **Boot + status.** `GET /api/status` shows `fw_ver = 2.0.0-rc.1.5.1`, `asset_version = 2.0.0-rc.1.5.1`, bank flipped, `accepted=true` after T1 marks healthy.
2. **Web Standby unchanged.** `POST /api/mode {"mode":"standby"}` → 200 → mode=STANDBY. `POST /api/mode {"mode":"automatic"}` → 200 → mode=WINDOW_CAL for ~3 min → AUTOMATIC. SD log has the `MODE,WEB,0,0,1,0` and `MODE,WEB,0,0,0,0` pair. Same as rc.1.5.0.
2a. **Standby badge visible while STANDBY active.** With STANDBY on: open `/api/status` directly (or watch the GUI Alarms card) — `mode.flags` must contain `"standby"`. The local-GUI Alarms card shows an amber **Standby** badge alongside `Wind protect off` / `Humidity ctrl off` / etc. if those are also active. Public dashboard renders the same badge.
2b. **WINDOW_CAL select alignment.** While the recalibration sweep runs (the ~3 min between `POST /api/mode {"mode":"automatic"}` and the mode flipping back to `AUTOMATIC`): the Climate-tab Mode select shows `Calibrating windows...` as the visible/selected option (greyed out). Apply button greyed. After calibration completes the transient option is removed and the select shows `Normal (autonomous)` again.

### gh#29 fix (physical access required — admin at the LCD)

3. **Enter menu auto-sets STANDBY.** Press D until Scherm 6; press `#`; enter admin PIN. SD log gets `MODE,ADMIN,1,0,1,0` (entry from LCD). Scherm 3 (advance with D) shows `Mode: STANDBY`. `/api/status` shows `mode.current = STANDBY`.
4. **Menu does NOT auto-dismiss on idle.** Stay on `UI_MOTOR_ACTION` for >2 minutes without pressing keys; LCD stays on the action screen; T6 stays paused (no window movement); the auto-rotate does NOT kick in.
5. **Issue manual command; T6 does NOT undo it.** Press `2` (Close) on M1 → wait >1 minute. Window stays closed; T6 does not re-open it (because STANDBY is set). Compare with rc.1.5.0 where after ~10 s + a sensor tick T6 would re-open.
6. **Exit via `*=back` preserves positions.** Press `*` to back; press `*` again. SD log gets `MODE,ADMIN,1,0,0,0` (leave from LCD). LCD returns to auto-rotation. Windows stay at the manual positions; T6 takes its next decision from the actual state. NO CLOSE_ALL recalibration. `Mode: STANDBY` on Scherm 3 flips to `Mode: AUTO`.
7. **Exit via session timeout also preserves positions.** Re-enter manual menu, issue a command, walk away. After `cfg.session_timeout_min` (default 5 min): LCD shows "Session timeout / Returning home..", auto-returns. SD log gets `MODE,ADMIN,1,0,0,0`. Windows stay at the manual positions. NO recalibration.
8. **Pre-existing STANDBY survives.** Set STANDBY on via Scherm 3 (FARMER PIN OK). Enter manual menu via Scherm 6 + admin PIN. Exit menu. STANDBY stays on (not auto-cleared). `Mode: STANDBY` persists; clearing only happens via explicit Scherm 3 / web action.

---

## Phase 7 soak acceptance criteria (rc.1.5.1)

Same shape as prior releases. 14-day clean criterion applies independently; clock starts at OTA.

| Criterion | Status (pre-OTA) |
|---|---|
| Zero PANIC / WDT during 14-day clean window | n/a (clock starts at OTA) |
| Zero SW reboots | n/a |
| Zero motor-SSR alarms (ch 1/2/3) | inherited clean from rc.1.3.3 → rc.1.5.0 |
| Zero unexpected sensor-fault clusters | inherited clean |
| Heap not bleeding (min internal ≥ 40 KB, median ≥ 90 KB) | inherited clean |
| gh#28 — web Standby round-trip | verified on rc.1.5.0 — no behaviour change in rc.1.5.1 |
| gh#29 — admin manual menu auto-enters STANDBY on entry | ⏳ verify in week 1 (physical) |
| gh#29 — menu does NOT auto-dismiss; only *=back or session timeout exits | ⏳ verify in week 1 (physical) |
| gh#29 — T6 does NOT undo manual commands within session | ⏳ verify in week 1 (physical) |
| gh#29 — menu exit preserves manual positions (no recal) | ⏳ verify in week 1 (physical) |
| gh#29 — pre-existing STANDBY survives manual menu | ⏳ verify in week 1 (physical) |

---

## Analyst follow-on (for the operator)

| Item | Status |
|---|---|
| `firmware/issues.md` — gh#28 and gh#29 status flags | Already `(implemented in 2.0.0-rc.1.5.0; ready for soak)` from the previous release; no change needed for the patch |
| TSDS update — T6 inhibit mask drops MANUAL_SESSION; Scherm 6 # flow rewritten | ✅ done in this release |
| FRS update — FR-MM03 + FR-MM07 reworded | ✅ done in this release |
| Beheerder-manual §10.11 update (semantics: auto-STANDBY, no idle dismiss, exit preserves positions) | ✅ done in this release |
| Boer-manual update | ✅ done in this release — §10.4 *Visuele bevestiging dat Standby actief is* added (amber badge + WINDOW_CAL select label); §5.3 amber-LED summary updated to mention operator-Standby; v1.15 history row added |
| Quick-ref update | ✅ done in this release — new FAQ row for the amber Standby badge |
| Beheerder-manual badges catalog | ✅ done in this release — §6 Alarms-tegel table gains the **Standby** row (amber, gh#28 surface, recalibration-on-clear caveat) |
| Changelog entry for rc.1.5.1 | ✅ done in this release |
| logparser.py / plot_daily.py change required? | ❌ no — log format and types unchanged |
| OTA-flash decision | ⏳ user choice |

---

## Files in this directory

| File | Purpose |
|---|---|
| `bootloader-2.0.0-rc.1.5.1.bin` | 22 528 B — bootloader (unchanged shape vs rc.1.5.0; rebuilt for consistency) |
| `greenhouse-controller-2.0.0-rc.1.5.1.bin` | 1 357 120 B — firmware image (rc.1.5.1 main artifact) |
| `web-assets-2.0.0-rc.1.5.1.zip` | 103 864 B — STORE-only ZIP of `/data` for `POST /api/ota/assets`. Content byte-identical to rc.1.5.0 except for the manifest version string |
| `partitions-2.0.0-rc.1.5.1.bin` | 3 072 B — partition table (unchanged shape; rebuilt) |
| `firmware-2.0.0-rc.1.5.1.elf` | 13 072 024 B — debug ELF for `addr2line` if a crash needs investigation |
| `release-notes.md` | this file |
