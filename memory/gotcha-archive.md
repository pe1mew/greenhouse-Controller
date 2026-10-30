# Gotcha archive — cold entries

Entries retired from [gotcha-log.md](gotcha-log.md) because they are **fully resolved and can no longer recur**: the code was deleted, the design changed, or the fix landed on both sides. Kept for history, not for triage.

**Do not read this file when debugging.** Read `gotcha-log.md` — anything still able to bite you is in there. An entry only moves here when all three hold:

1. It is marked `[RESOLVED]`, **and**
2. no active constraint in `CLAUDE.md` or `architecture.md` depends on it, **and**
3. the failure path no longer exists (removed code / changed design / server-side fix), so no future session can hit it.

Retired 2026-07-20 (first pass): 3 of 32 entries. The other 10 resolved entries stayed in the main log because they remain load-bearing — `gh#36` is the parent of `gh#42`, the qio/dio and coredump-erase entries are live `CLAUDE.md` hard constraints, the DS1307 entry carries still-valid clock diagnostics, and the coredump-staleness entry documents a Part 2 that was deliberately never done.

---

## 2026-06-27 — Word COM automation hangs on large HTML files [RESOLVED — removed]

**Problem:** `make_rtf()` in `manual/build_pdf.py` calls `word.Documents.Open(html)` which hangs indefinitely on `beheerderHandleiding.html` (~180 KB, complex CSS). Multiple orphaned `WINWORD.EXE` processes accumulate; temp HTML files are left locked on disk.

**Root cause:** Word's non-interactive COM rendering path stalls on large, CSS-heavy HTML. The hang is silent — no exception, no timeout — so `make_rtf()` never returns and the script blocks.

**Fix:** Removed Word COM and all RTF generation from `manual/build_pdf.py` entirely. Script now generates PDF only via Edge headless. RTF files (if ever needed) must be created outside this script.

**Why it is cold:** the calling code no longer exists in the repo.

**Where it lived:** `manual/build_pdf.py` — `make_rtf()` and the `try/except` block in the main loop. Orphaned WINWORD processes can be killed via `Get-Process WINWORD | ForEach-Object { $_.Kill() }`.

---

## 2026-06-10 — `rfsee.net/hbwv/api.php` rejects POSTs > ~1 MB with HTTP 413 [RESOLVED — server limit raised 2026-06-28]

**Problem:** T14's daily log upload fails every day with `code=413` once an SD log file rotates to its 1 MB cap. The `log_last_up` field froze at a May 24 filename for 17 days while uploads silently failed.

**Root cause:** Server-side `post_max_size` (PHP) and/or `client_max_body_size` (nginx) was below 1 MB on rfsee.net.

**Fix:** Server-side upload limit raised to ≥ 2 MB (2026-06-28). Firmware side complete in 2.1.1 (gh#34 commit): T14 records the HTTP status code in the SD audit row. Both sides resolved.

**Why it is cold:** fixed on both sides, and a recurrence would now be *visible* rather than silent — T14 writes the HTTP code to the audit row. **Note for "uploads stopped" debugging:** start from the `gh#42` entry in the main log, which is the canonical entry point for that symptom; this one is only the earlier, server-side chapter.

---

## 2026-XX-XX — Reboot from FreeRTOS timer service task overflows its stack [RESOLVED — 2.0.0-rc.1.2]

**Problem:** Scheduled reboot via a software-timer callback calling `esp_restart()` directly produces a stack-overflow panic during reboot.

**Root cause:** `esp_wifi_stop()`'s teardown consumes several KB of stack. The timer service task's `configTIMER_TASK_STACK_DEPTH` doesn't have enough headroom.

**Fix:** Timer callback spawns a dedicated `reboot_worker_task` with a 4 KB stack; that task calls `esp_restart()`. `firmware/src/ota_manager/ota_manager.cpp` — `reboot_timer_cb` and `reboot_worker_task`. Landed in 2.0.0-rc.1.2.

**Why it is cold:** the design changed — nothing calls `esp_restart()` from a timer callback any more. The general lesson (a task doing TLS or WiFi teardown needs ≥ 12–16 KB of stack) survives in the main log's T16 stack-overflow entry.

## 2026-10-10 — a bring-up task still took key events off Q2 every 5 s

**Problem:** gh#93 stops M3 when the operator lets go of an LCD key, so T7's new release event has to reach T8. While designing it, the keypad queue Q2 turned out to have a second consumer: `heartbeat_task` in `main.cpp` drained it every 5 s and logged each event to serial.

**Root cause:** the drain was the keypad's acceptance test in alpha.6.4, before T8 existed. T8 became Q2's consumer in Phase 7, and the drain stayed. With two consumers on one queue, whichever receives first gets the event. T8 almost always waits on Q2, so a key press lost this way was rare and never explained. A lost release, though, would let a hold run on until T8's 400 ms fail-safe.

**Fix (2.17.0):** the drain is removed, and T8 is Q2's only consumer (the TSDS Q2 row says so). **When the real consumer of a queue arrives, remove the bring-up reader.**

**Where it lives:** `firmware/src/main.cpp` (`heartbeat_task`), `firmware/src/ui_display/ui_display.cpp`.

**Why it is cold:** the code is gone. 2.17.0 removed the drain, and T8 is Q2's only consumer (`ui_display.cpp`). Retired 2026-10-30.

## 2026-09-12 — LCD-set STANDBY survives a reboot but its auto-clear flag does not, pausing ventilation indefinitely (gh#65) [RESOLVED 2026-09-16, 2.8.0]

**Problem**: FDA4 sat in STANDBY for over two hours. Three later LCD admin
sessions opened and closed normally without clearing it, and logging out of the
web GUI did nothing. The `MODE` log shows `STANDBY entered` via LCD at 18:11:16
and **no matching exit**.

**Root cause**: the state is persisted, the authorisation to clear it is not.
`ui_display.cpp:301` holds `static bool s_manual_set_standby_on_entry` — RAM
only, false on every boot — and `session_close()` clears STANDBY **only** if
that flag is set. Meanwhile `data_manager.cpp:2177` writes the STANDBY to NVS
and `:1435` restores it at boot. Any reboot between "menu sets STANDBY" and
"session closes" keeps the state and loses the permission to release it, after
which no future session can ever clear it. Surfaced because the unit had been
OTA-flashed six times in sixteen minutes.

**Fix**: filed as **gh#65**, not yet fixed. Only an *explicit* exit works, since
that path calls `dm_set_standby()` unconditionally: the LCD Scherm-3 toggle, or
`POST /api/mode {"mode":"automatic"}` (which posts `CMD_RECALIBRATE` and drives
the channels). **On production this is worse than on the rig**: a power blip or
a ROTA night-window update during an open admin session leaves 5C88 with
ventilation paused indefinitely, reporting `mode: STANDBY` truthfully with no
fault anywhere. Rule to keep: **a state that survives a reboot must not depend
on RAM to be released.**

**Resolved 2026-09-16 (2.8.0).** The menu now takes a STANDBY *hold* (`dm_standby_hold(DM_STANDBY_HOLD_LCD, …)`), which is never written to NVS, so the state and its release both live in RAM and a reboot ends them together. The mechanism was built the same day for the web teach, which needed the same pause. Session end still drops the dwell debt and recalibrates. A web teach and the LCD menu can hold the pause together, and it ends only when both sessions have.

**Verified on hardware the same day with `bin/at_lcd_standby.py`**, which needs a person at the keypad. It has four cases: L (logout: STANDBY off, windows recalibrate), E (an operator's own STANDBY outlives a menu session), C (a web teach and the menu hold the pause together) and R (a reboot ends the pause). All four pass on the fix. **Fail-first:** on the build before it (088e463), C and R FAIL. R is this entry's trap exactly: STANDBY came back after the boot. The SD log shows the difference directly. The old build's menu row is a plain "STANDBY entered via LCD" (`value_b` 0, persisted); the fixed build's says "held for manual window control" (`value_b` 1).

**Where it lives**: gh#65; `firmware/src/ui_display/ui_display.cpp` (`session_close()`, the two menu-entry sites); `firmware/src/data_manager/data_manager.cpp` (`dm_standby_hold()` / `dm_standby_release()`).

**Why it is cold:** the design changed in 2.8.0: the menu takes a RAM-only hold, so state and release end together. One caveat: a 2.3.1 image still holds the defect, should a unit ever run one again. Retired 2026-10-30.
