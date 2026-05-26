# 2.0.0-rc.1.5.2 — release notes

**Date built:** 2026-05-26
**Built on top of:** 2.0.0-rc.1.5.1 (gh#29 UX fix + Standby badge + WINDOW_CAL select)
**Closes:** physical-test feedback on rc.1.5.1 gh#29 (*"during manual operation climate control kicked in and took over"*) — log analysis confirmed T6 acted exactly 7 seconds after `*=back`, undoing the admin's manual positions on the next sensor poll
**Scope:** single-file firmware change in `ui_display.cpp`; web assets byte-identical to rc.1.5.1

---

## Why a patch bump (1.5.1 → 1.5.2)

Single behaviour adjustment to one of the gh#29 surfaces. No API addition, no new HTTP route, no SD-log format change, no web-asset content change. Patch bump is the right granularity.

---

## What changed

### Observation (operator, after rc.1.5.1 OTA at 12:03 local)

> "after manually opening, auto rotate of displays starts. this is undesired"  *(addressed in rc.1.5.1)*
>
> "during manual operation climate control kicked in and took over. is this expected behaviour (see log) or wrong?"  *(this release)*

### Log analysis (active SD file `20260525183425.csv` at 12:13)

```
12:10:27  SESSION,ADMIN,2 + MODE,ADMIN,1,0,1,0   ← admin enters menu → STANDBY auto-set ✓
12:10:41  RELAY,SYS,1,0,2,0                       ← admin opens M1
12:10:48  RELAY,SYS,2,0,2,0                       ← admin opens M2
12:11:05  RELAY,SYS,3,0,2,0                       ← admin opens M3
12:11:07  RELAY M1 → OPEN (travel-timer)
12:11:13  RELAY M2 → OPEN
12:12:09  RELAY,SYS,1,0,4,0                       ← admin closes M1
12:12:14  RELAY,SYS,2,0,4,0                       ← admin closes M2
12:12:19  RELAY,SYS,2,0,2,0                       ← admin re-opens M2 (reversal)
12:12:35  RELAY M1 → CLOSED
12:12:45  RELAY M2 → OPEN (after reversal travel)
12:12:52  MODE,ADMIN,1,0,0,0                      ← admin presses *=back → STANDBY auto-CLEARED (rc.1.5.1)
12:12:59  RELAY,SYS,1,0,2,0                       ← T6 immediately re-opens M1 (greenhouse is hot)
```

T6 correctly stayed gated for the full admin menu session (12:10:27 → 12:12:52). The 12:12:59 RELAY is T6 reacting **7 seconds after admin pressed `*=back`**, on the next sensor poll. From T6's perspective: "STANDBY just cleared, T = 36.6 °C ≫ T_max = 28 °C, need step 3 vent, M1 is CLOSED — issue OPEN on M1". The admin's deliberate CLOSE on M1 was undone almost immediately.

### Locked design decision (2026-05-26 follow-up)

> *"Respect window, T6 acts after the session timeout from admin at LCD."*

The admin's manual positions deserve a **respect window** during which T6 cannot override them. The window is the admin PIN-session timeout — `cfg.session_timeout_min`, default 5 minutes from last keypress. After the admin's last interaction, manual positions hold for the full session lifetime; only when the session ends (timeout or explicit logout) does T6 resume.

### Fix

| Change | Implementation |
|---|---|
| **`*=back` no longer clears STANDBY** | `handle_motor_pick` `*=back` case just calls `go_status()`; no `dm_set_standby_ex` call |
| **`go_status()` no longer auto-clears STANDBY** | The block that called `dm_set_standby_ex(false, ..., false)` based on `s_manual_set_standby_on_entry` has been removed |
| **`session_close()` now auto-clears STANDBY** | When called (timeout or logout), if `s_manual_set_standby_on_entry == true`, calls `dm_set_standby_ex(false, LOG_BY_ADMIN, 1, false)` (no recal) and resets the flag |
| **Menu re-entry preserves the flag** | Menu-entry logic changed from `s_manual_set_standby_on_entry = !was_already_standby` (which would overwrite to false on a re-entry where STANDBY was already on from previous entry) to a guarded set: only flip to `true` if menu actually sets STANDBY now; otherwise leave alone |

### Operator-visible behaviour change

| Scenario | rc.1.5.1 | rc.1.5.2 |
|---|---|---|
| Admin enters menu, opens M1+M2+M3, presses `*=back`, walks away | T6 reopens what admin closed within ~30 s | T6 stays paused for 5 min. After 5 min: session timeout, STANDBY clears, T6 resumes from current state |
| Admin enters menu, presses `*=back`, presses D to scroll status screens, then D back to Scherm 6, presses # to re-enter menu | Menu auto-set STANDBY again (re-entry); same loop | Menu re-enters; STANDBY already on; flag stays true; session-timeout-clock keeps resetting with each keypress |
| Admin enters menu, presses `*=back`, explicitly logs out via `Access → Logout` | STANDBY cleared at *=back | STANDBY cleared at the explicit logout call |
| Admin entered menu with STANDBY already on (set via Scherm 3 or web) | STANDBY survives menu exit (rc.1.5.1 logic) | Same — flag stays false, session-end leaves STANDBY alone |
| Admin enters menu, presses `*=back`, opens web GUI Climate-tab, sees "Standby (paused)" select | Select shows "Normal" (STANDBY was already cleared) | Select shows "Standby (paused)"; admin can manually toggle to "Normal" early if they want T6 to resume sooner |

---

## What did NOT change

- `POST /api/mode` (gh#28) behaviour — Scherm 3 / web Standby exits still trigger CLOSE_ALL recalibration.
- LCD Scherm 3 mode-toggle PIN flow and render — unchanged.
- Amber Standby badge in Alarms shield (rc.1.5.1) — unchanged. Now visible for longer when admin uses the manual menu.
- WINDOW_CAL select treatment in Climate-tab (rc.1.5.1) — unchanged.
- Manual menu structure (UI_MOTOR_PICK → UI_MOTOR_ACTION) — unchanged.
- Safety gates (`WIND_OVERRIDE` blocks OPEN; `MOTOR_ALARM` and `CALIBRATING` block all manual commands) — unchanged.
- T6 inhibit mask (`MOTOR_ALARM | WIND_OVERRIDE | SENSOR_FAULT_T | STANDBY`) — unchanged.
- rc.1.4.0 SD-log format — preserved.
- Web-asset content — byte-identical to rc.1.5.1 (only the manifest version string bumps).

---

## Build delta vs rc.1.5.1

| File | rc.1.5.1 | rc.1.5.2 | Δ |
|---|---:|---:|---:|
| `greenhouse-controller-*.bin` | 1 357 136 B | 1 357 216 B | **+80 B** |
| `web-assets-*.zip` | 105 884 B | 105 884 B | 0 (manifest-only delta) |
| Bootloader | 22 528 B | 22 528 B | 0 |
| Partition table | 3 072 B | 3 072 B | 0 |

Link-report usage: RAM 18.9 %, Flash 64.7 % — both unchanged.

Build is clean: **zero source-tree warnings**.

---

## SHA-256

```
085c744a5520f844c4a145fbeaf8ef47e13e0ba6fcd34149456b09a27bab1a25  greenhouse-controller-2.0.0-rc.1.5.2.bin
3e479f2cc0f2c2d80aa8963452ff89bd81b918b420144d9f39c7a99e36da0c7b  web-assets-2.0.0-rc.1.5.2.zip
```

---

## Deployment record

| Step | Status |
|---|---|
| Build artifacts produced | ✅ 2026-05-26 (this directory) |
| SHA-256 captured | ✅ |
| OTA-flashed to soak unit | ⏳ awaiting operator instruction |
| Phase 7 soak clock restart | ⏳ pending OTA |

---

## Verification checks (post-OTA, physical access required for full gh#29 path)

### Smoke (web only, can do remotely)

1. **Boot + status.** `GET /api/status` shows `fw_ver = 2.0.0-rc.1.5.2`, `asset_version = 2.0.0-rc.1.5.2`, bank flipped, `accepted=true` after T1 marks healthy.
2. **Web Standby unchanged.** `POST /api/mode {"mode":"standby"}` → 200 → mode=STANDBY. `POST /api/mode {"mode":"automatic"}` → 200 → mode=WINDOW_CAL for ~3 min → AUTOMATIC. Standby badge appears/disappears in the Alarms card. Same as rc.1.5.1.

### gh#29 fix (physical access required — admin at the LCD)

3. **Enter menu auto-sets STANDBY.** Press D to Scherm 6; press `#`; enter admin PIN. SD log gets `MODE,ADMIN,1,0,1,0`. Scherm 3 shows `Mode: STANDBY`. `/api/status` mode = STANDBY; flags = `["standby"]`. Same as rc.1.5.1.
4. **`*=back` does NOT clear STANDBY.** Issue a command (e.g. CLOSE M1). Press `*=back` to UI_MOTOR_PICK. Press `*=back` again to dismiss to status screens. Wait 30 sec without pressing keys. Confirm:
   - Scherm 3 still shows `Mode: STANDBY`
   - `/api/status` mode still `STANDBY`, flags still `["standby"]`
   - **M1 stays CLOSED** — T6 does NOT reopen it (this is the rc.1.5.2 fix)
   - **No new `MODE,...,0,0` row in the SD log** (STANDBY didn't clear)
5. **Re-enter menu — flag preserved.** Press D until Scherm 6, press `#` (no PIN needed — session still active). Menu opens directly. Issue another command. Press `*=back`. Mode stays STANDBY.
6. **Session timeout auto-clears.** From the moment of your last keypress, wait `cfg.session_timeout_min` (default 5 min). LCD shows "Session timeout / Returning home..". `MODE,ADMIN,1,0,0,0` row in SD log. Scherm 3 shows `Mode: AUTO`. T6 takes its next decision from the actual current state; if greenhouse is hot, T6 will issue new commands.
7. **Explicit logout also auto-clears.** Re-do steps 3 and 4. Then navigate hoofdmenu → 3 (Access) → 3 (Logout). `MODE,ADMIN,1,0,0,0` row immediately. Same exit behaviour as session timeout.
8. **Pre-existing STANDBY survives.** Set STANDBY on via Scherm 3 (Farmer PIN OK). Enter manual menu via Scherm 6 + admin PIN. Issue commands. Press `*=back`. Wait for session timeout. STANDBY stays on (not auto-cleared because it was set before the menu).

---

## Phase 7 soak acceptance criteria (rc.1.5.2)

Same shape as prior releases. 14-day clean criterion applies independently; clock starts at OTA.

Inherited clean conditions from rc.1.3.3 → rc.1.5.1. New checks specific to rc.1.5.2:
- ⏳ gh#29 — `*=back` does NOT cause T6 to re-act within 30 s
- ⏳ gh#29 — session timeout fires the STANDBY auto-clear (`MODE,ADMIN,1,0,0,0` row)
- ⏳ gh#29 — explicit logout fires the STANDBY auto-clear

---

## Analyst follow-on (for the operator)

| Item | Status |
|---|---|
| TSDS update — Scherm 6 # flow rewritten for the respect-window model | ✅ done in this release |
| FRS update — FR-MM03 + FR-MM07 reworded | ✅ done in this release |
| Beheerder-manual §10.11 update (sessie-einde tabel + respect-window uitleg) | ✅ done in this release |
| Boer-manual update | Not required — gh#29 is admin-only; the boer manual already notes Scherm 6 # is admin-only and farmers can't use it |
| Quick-ref update | Not required — same reason |
| Changelog entry for rc.1.5.2 | ✅ done in this release |
| logparser.py / plot_daily.py change required? | ❌ no — log format and types unchanged |
| OTA-flash decision | ⏳ user choice |

---

## Files in this directory

| File | Purpose |
|---|---|
| `bootloader-2.0.0-rc.1.5.2.bin` | 22 528 B — bootloader (unchanged shape; rebuilt for consistency) |
| `greenhouse-controller-2.0.0-rc.1.5.2.bin` | 1 357 216 B — firmware image (rc.1.5.2 main artifact) |
| `web-assets-2.0.0-rc.1.5.2.zip` | 105 884 B — STORE-only ZIP of `/data` for `POST /api/ota/assets`. Content byte-identical to rc.1.5.1; only manifest version string changes |
| `partitions-2.0.0-rc.1.5.2.bin` | 3 072 B — partition table (unchanged shape; rebuilt) |
| `firmware-2.0.0-rc.1.5.2.elf` | ~13 MB — debug ELF for `addr2line` if a crash needs investigation |
| `release-notes.md` | this file |
