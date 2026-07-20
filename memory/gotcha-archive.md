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
