# Gotcha log

Append-only. Newest at top. Format per entry: **Problem → Root cause → Fix → Where it lives.**

When something weird happens, check here BEFORE debugging from scratch. Entries that recur or affect multiple subsystems graduate up to a topic file or to [CLAUDE.md](../CLAUDE.md) hard constraints.

---

## 2026-07-13 — Branch switch carried the whole staging area into the wrong commit

**Problem:** Creating the `rota` feature branch while ~23 unrelated files were staged, then committing "model changes" on `main`, swept the rota-only work (TDS + 2.2.0 version bump) into the model commit on `main` (`32ce6b5`). Cleanup needed a compensating commit on main (`5c3c6cc`, version back to 2.1.3) and re-pointing `rota` to the new tip.

**Root cause:** Git's index is shared across branch switches — `git checkout -b` carries all staged changes, and a broad `git commit` on the other branch commits everything staged, regardless of which "stream" of work it belongs to.

**Fix / pattern:** When work spans branches, commit (or stash) each stream *before* switching. When Claude stages changes that belong to different destinations, it must say so explicitly at hand-off and repeat the warning before any branch operation. Recovery recipe when it happens anyway: compensating commit on the polluted branch + re-point the feature branch (`git branch -D` + recreate at tip works when the feature branch has no own commits).

**Where it lives:** Process, not code. Commits `32ce6b5`/`5c3c6cc` are the example.

---

## 2026-07-13 — Wind measurements before 2026-06-19 12:00 are invalid

**Problem:** Wind speed/direction columns exist in SD logs and campaign data from Jun 4, but the wind vane was only commissioned **2026-06-19 12:00**. Earlier values (including the wind columns in `plot_summary.txt` for Jun 4–19) are garbage and must not be used in any analysis.

**Root cause:** Sensor commissioning happened mid-campaign; the log format carries the columns regardless.

**Fix:** Constraint recorded in `thermalProfileCampaign.md` §9.11 and enforced in `ns9_direction_stratified.py` (`WIND_VALID_FROM`). Any new wind-based analysis script must filter `timestamp >= 2026-06-19T12:00`.

**Where it lives:** `model/campaign-summer-2026/ns9_direction_stratified.py` (the reference filter); campaign doc §9.11.

---

## 2026-07-08 — Clock hours wrong while `ntp_synced=true` — DS1307 outranked SNTP [RESOLVED — 2.1.3, gh#37]

**Problem:** 2344's clock read 09:31 at real 14:06 (4 h 35 m behind) with `ntp_synced=true`. SD log showed an hourly ±16 500 s see-saw: SNTP set the correct time at :40 past, something dragged it back within a minute.

**Root cause:** Two-layer. (1) Firmware: T4 called `settimeofday()` from the DS1307 every ~60 s unconditionally — the RTC chip outranked SNTP by design. (2) Hardware: 2344's DS1307 was failing — lost ~40 s/h on Jul 7, froze overnight (rows stamped exactly `00:50:52` = halted oscillator), restarted 4 h 35 m behind; TN4's post-sync corrective writes did not hold.

**Fix:** 2.1.3 — DS1307 seeds the clock only while `nm_is_sntp_synced()` is false; once synced the system clock is authoritative and DS-vs-system divergence > 10 s emits `LOG_SYSTEM value_a=21` (~1/h). Hardware: battery replaced 2026-07-08; gh#37 closed 2026-07-09 (firmware verified: zero clock jumps in 24 h of logs). Follow-up enhancement (immediate DS self-heal instead of the 24 h TN4 cadence) tracked as gh#38.

**Diagnostic notes for next time:** `time_iso`/`ts_unix` in `/api/status` report the MX4 shadow, refreshed once per 60 s poll — they read 0–60 s stale by design; don't mistake that sawtooth for drift. A frozen repeated timestamp in SD rows = halted RTC oscillator. `ntp_synced` is a per-boot latch — it says SNTP succeeded once, not that the clock is currently right.

**Where it lives:** `firmware/src/data_manager/data_manager.cpp` — `read_rtc_and_seed_clock()`; `log/logparser.py` decodes value_a=21.

---

## 2026-07-05 — Model docs mis-identified M3 as a roof ridge panel for a week

**Problem:** `thermalProfileCampaign.md` §9.6–9.8 and `calibrate_plant_constrained.py` described M3 as a "171-step ridge ventilation panel" in the roof, and used the travel-time ratio (171 s/21 s = 8.1×) as an "area ratio". All window-strategy physics reasoning built on this. User caught it 2026-07-05.

**Root cause:** An earlier session inferred the window identity from motor travel times instead of checking the FRS. Authoritative facts (FRS + boerHandleiding): M1 = Dakbeluchting Zuid (south roof, ~8 m²), M2 = Dakbeluchting Noord (north roof, ~8 m²), **M3 = Zijwandbeluchting, north side WALL (~80 m²)**. Area ratio is 10× (80/8 m²); 8.1× is the travel ratio.

**Fix:** Corrected in `thermalProfileCampaign.md` (erratum in §9.7, rewritten §9.9 finding 4), `calibrate_plant_constrained.py`, and user-global memory (window-identity table added). **Pattern: verify physical plant facts against the FRS before building analysis on them — never infer geometry from firmware constants.**

**Where it lives:** `design/functionalRequirementsSpecification.md` (window table); `manual/boerHandleiding.md` (Figuur 1 + raam table).

---

## 2026-07-05 — System Python 3.11 lost its site-packages mid-project

**Problem:** `plot_daily.py` failed with `ModuleNotFoundError: matplotlib`; system Python 3.11 (`AppData/Local/Programs/Python/Python311`) had an empty `pip list` even though the same interpreter ran matplotlib and scipy workloads earlier in the week.

**Root cause:** Unknown — likely a Python reinstall/update wiped site-packages. Not investigated further.

**Fix:** `python -m pip install mysql-connector-python numpy scipy matplotlib`. Note the PIO venv python (`~/.platformio/penv/Scripts/python.exe`) has matplotlib but **not** scipy or mysql-connector — it can run `plot_daily.py` but not the calibration or MySQL-fetch scripts.

**Where it lives:** Model pipeline needs, in system Python 3.11: numpy, scipy, matplotlib, mysql-connector-python.

---

## 2026-07-05 — A different matplotlib version re-renders ALL campaign PNGs with byte diffs

**Problem:** Running `plot_daily.py` under the PIO venv python (different matplotlib version) marked all 29 unchanged day-plots as modified in git — pure rendering churn, no data change.

**Fix:** `git checkout --` the plots for days whose data did not change; stage only days with new/extended data. Generally: regenerate plots with the same interpreter/matplotlib the previous renders used, or accept a one-time full re-render in a dedicated commit.

**Where it lives:** `model/campaign-summer-2026/plot_daily.py` output; any matplotlib-generated PNG under version control.

---

## 2026-07-04 — `storage_sd_list_csv()` truncates silently on a full buffer [RESOLVED — 2.1.2, gh#36]

**Problem:** Web GUI showed no SD log files newer than ~Jun 22 and T14 stopped uploading after Jun 24, on both units. No error anywhere.

**Root cause:** `storage_sd_list_csv()` returns `STORAGE_OK` even when entries didn't fit the caller's buffer — silent truncation by design (arduino-era behaviour). Three callers used 512-byte buffers that overflow at ~20 files (`SD_MAX_FILES=30` × ~25 B/name); the web handler additionally capped at `LOG_FILES_MAX=12`.

**Fix:** All SD capacity constants moved to `event_logger.h` as single source of truth with derived `SD_LIST_BUF_LEN` (871 B); every scan buffer and the web handler cap now derive from it. **Pattern: when a list/scan API cannot signal truncation, size its buffers from a shared derived constant — never a local literal.**

**Where it lives:** `firmware/src/event_logger/event_logger.h` (constants block); `drivers/sdCard/src/sd_storage.cpp:402` (the silently-truncating function, unchanged).

---

## 2026-06-27 — Word COM automation hangs on large HTML files [RESOLVED — removed]

**Problem:** `make_rtf()` in `manual/build_pdf.py` calls `word.Documents.Open(html)` which hangs indefinitely on `beheerderHandleiding.html` (~180 KB, complex CSS). Multiple orphaned `WINWORD.EXE` processes accumulate; temp HTML files are left locked on disk.

**Root cause:** Word's non-interactive COM rendering path stalls on large, CSS-heavy HTML. The hang is silent — no exception, no timeout — so `make_rtf()` never returns and the script blocks.

**Fix:** Removed Word COM and all RTF generation from `manual/build_pdf.py` entirely. Script now generates PDF only via Edge headless. RTF files (if ever needed) must be created outside this script.

**Where it lives:** `manual/build_pdf.py` — `make_rtf()` function and the `try/except` block in the main loop. Orphaned WINWORD processes can be killed via `Get-Process WINWORD | ForEach-Object { $_.Kill() }`.

---

## 2026-06-26 — `plot_daily.py` sorted-set dedup fails when two SD download chains overlap

**Problem:** Feeding log files from two independent SD card downloads of the same unit (different rotation boundaries, same time period) causes days in the overlap window to show ~2× the expected sample count (~5700 vs ~2860 per day). The `sorted(set(tuple(e)))` dedup at `load_logs()` line 182 does not remove the double-counted readings.

**Root cause:** The two chains have different SD file boundaries, so their `SENSOR_HR` event tuples differ in context (e.g., mode-change rows, BOOT rows) even though the sensor data timestamps are identical. The dedup is tuple-exact — any field difference between the two chains' representations of the same timestamp prevents deduplication.

**Fix:** Keep only one chain per time window. Archive the overlapping files from the older chain (`archived_overlap/` subfolder). For the 5C88 campaign: Chain A (old downloads) covers Jun 4–15 uniquely; Chain B (new download) covers Jun 15–24. Archive the four Chain A files whose range is wholly covered by Chain B.

**Where it lives:** `model/campaign-summer-2026/plot_daily.py` (`load_logs`). Chain overlap detection: `model/campaign-summer-2026/check_dupes.py`. *(Paths updated 2026-07-05 — scripts moved out of `temp/`.)*

---

## 2026-06-20 — `ota_push.py` exits 1 at step [8] verify even when OTA succeeded

**Problem:** Running `python bin/ota_push.py ... 2>&1` in PowerShell exits with code 1 and prints `post-OTA /api/status failed` even though the unit rebooted to the correct `fw_ver` and `asset_version`. Manual `/api/status` query immediately after confirmed both versions correct.

**Root cause:** The `2>&1` redirect in PowerShell 5.1 causes Python's stderr output (the "post-OTA /api/status failed" diagnostic line) to be captured and wrapped as a `NativeCommandError`, setting exit code 1. The verify step in the script polls `/api/status` while the unit is still finishing boot — the request times out, the script prints to stderr, and PowerShell promotes that to a fatal error.

**Fix:** Drop `2>&1` when running `ota_push.py` from PowerShell; let Python manage its own streams. If the script reports exit 1 at step [8] only, wait 10–15 s and manually verify: `Invoke-WebRequest http://<ip>/api/status`. Check both `fw_ver` AND `asset_version` match the release. The OTA itself was almost certainly successful.

**Where it lives:** `bin/ota_push.py` step 8. PowerShell `2>&1` pattern re-documented in gotcha-log — mirrors the rc.1.3 EAP=Stop issue.

---

## 2026-06-10 — OTA firmware-only push silently strands a unit's assets

**Problem:** Unit 5C88 ended up running `fw_ver=2.0.3` with `asset_version=2.0.0-rc.1.5.6` after an interrupted OTA. Web UI footer kept showing the old version even though the running firmware was new.

**Root cause:** `ota_firmware_end()` enters `OTA_STATE_FW_DONE` and starts a 120 s timer. If `ota_assets_begin()` doesn't arrive in time, the firmware is committed (boot-slot swap) without an asset partition swap. The paired-commit invariant is enforced inside `ota_assets_end()`, not in the firmware path.

**Fix:** Either complete the asset push within 120 s of the firmware POST (the canonical `ota_push.py` flow does this), or do an assets-only push afterwards. Assets-only writes to the *active* LittleFS partition (1.17.3 fix path) so the boot slot isn't re-flipped.

**Where it lives:** `firmware/src/ota_manager/ota_manager.cpp` — `do_fw_done_timer_cb`. Described in [design/OTAimplementation.md §4.1](../design/OTAimplementation.md).

## 2026-06-10 — `HEAD /api/log/download` reports constant 45 B regardless of file size [RESOLVED — issue filed 2026-06-28]

**Problem:** SD log file-size probe via HTTP HEAD returns `Content-Length: 45` even for 1 MB files. Misleads any tooling that uses HEAD to size files before downloading.

**Root cause:** Suspected — HEAD handler emits a hard-coded short response rather than running the GET path's size calculation.

**Fix:** Use GET with `Range: bytes=0-0` to read just one byte and inspect `Content-Range` for the total. Or just GET the whole file. GitHub issue filed 2026-06-28 (see repository issue tracker).

## 2026-06-10 — `rfsee.net/hbwv/api.php` rejects POSTs > ~1 MB with HTTP 413 [RESOLVED — server limit raised 2026-06-28]

**Problem:** T14's daily log upload fails every day with `code=413` once an SD log file rotates to its 1 MB cap. The `log_last_up` field froze at a May 24 filename for 17 days while uploads silently failed.

**Root cause:** Server-side `post_max_size` (PHP) and/or `client_max_body_size` (nginx) was below 1 MB on rfsee.net.

**Fix:** Server-side upload limit raised to ≥ 2 MB (2026-06-28). Firmware side complete in 2.1.1 (gh#34 commit): T14 records HTTP status code in the SD audit row. Both sides now resolved; tracked in GH issues.

## 2026-06-09 — Branch protection on `main` rejects merge commits

**Problem:** `git merge dev/X --no-ff && git push origin main` rejected with "This branch must not contain merge commits".

**Root cause:** Repo settings enforce linear history on `main`.

**Fix:** Rebase the feature branch onto `origin/main`, then `git checkout main && git merge --ff-only dev/X && git push origin main`. Never `--force` to `main`. Recovery sequence documented in `BRANCH_NOTES.md`.

## 2026-05-XX — `pio: command not found` in Git Bash / MINGW64

**Problem:** Running `pio run` in Git Bash returns "command not found" even though PlatformIO is installed.

**Root cause:** PlatformIO's venv binary lives at `~/.platformio/penv/Scripts/platformio.exe`, not on PATH in Git Bash.

**Fix:** Use the full path: `"$HOME/.platformio/penv/Scripts/platformio.exe" run -e lolin_s3`. Same trick for `python.exe` from that venv when scripts need the PIO-bundled Python.

## 2026-05-14 — `ets_loader.c` crash loop after greenfield flash (qio vs dio) [RESOLVED]

**Problem:** Full-chip flash succeeds; device boots into an infinite `ets_loader.c` error loop and won't run user code.

**Root cause:** Bootloader header byte must be `dio` for the ESP32-S3 ROM. esptool's default header doesn't match the runtime `flash_mode = qio` setting.

**Fix:** Pass `--flash_mode dio` to esptool for the full-chip flash; runtime `board_build.flash_mode = qio` in `platformio.ini` stays.

**Where it lives:** Canonical statement in [`../CLAUDE.md`](../CLAUDE.md) "Releases & OTA" hard constraints. User-global personal copy at `~/.claude/projects/.../memory/feedback_full_flash_mode.md`.

## 2026-05-XX — Coredump partition garbage panic on first boot

**Problem:** Newly-flashed unit logs `esp_core_dump_flash: Core dump flash config is corrupted! CRC=…` on every boot. (Unit 12F0 forensic capture 2026-05-14.)

**Root cause:** IDF unconditionally reads the coredump partition at boot. Random-content flash → CRC fails → panic message fires.

**Fix:** First flash on each unit MUST be followed by:
```
esptool.py --chip esp32s3 --port COMx erase_region 0x620000 0x10000
```
Encoded in [firmware/partitions.csv](../firmware/partitions.csv) header comment. gh#21 / 1.19.0.

## 2026-04-XX — `manifest.json` placeholder accidentally shipped as literal version (gh#9) [RESOLVED]

**Problem:** OTA goes out with a stale `asset_version` (last release's version, not the current one).

**Root cause:** `firmware/data/manifest.json` source carries a `{{ASSET_VERSION}}` placeholder. `bin/build_release.ps1` Step 0 stamps the literal version for the LittleFS build. If a human commits mid-build, the literal form lands in git and ships with the next release.

**Fix:** Build script Step 3.5 restores the placeholder after Step 3. `.githooks/pre-commit` refuses commits where `manifest.json` is in literal form. Both are necessary; either alone is bypassable.

## 2026-XX-XX — Reboot from FreeRTOS timer service task overflows its stack [RESOLVED — 2.0.0-rc.1.2]

**Problem:** Scheduled reboot via a software-timer callback calling `esp_restart()` directly produces a stack-overflow panic during reboot.

**Root cause:** `esp_wifi_stop()`'s teardown consumes several KB of stack. The timer service task's `configTIMER_TASK_STACK_DEPTH` doesn't have enough headroom.

**Fix:** Timer callback spawns a dedicated `reboot_worker_task` with a 4 KB stack; that task calls `esp_restart()`. `firmware/src/ota_manager/ota_manager.cpp` — `reboot_timer_cb` and `reboot_worker_task`. Landed in 2.0.0-rc.1.2.

## 2026-XX-XX — Dual LittleFS partitions need separate VFS basePaths [RESOLVED]

**Problem:** Mounting `lfs0` and `lfs1` to the same VFS basePath (`/lfs`) causes the second mount to overlay the first; assets are read from the wrong partition.

**Root cause:** ESP-IDF's VFS treats basePath as the mount point; identical basePaths collide.

**Fix:** Each partition gets its own basePath: `/lfsa` for `lfs0`, `/lfsb` for `lfs1`. The active-partition resolver picks via the running app bank. Encoded in `~/.claude/projects/.../memory/project_littlefs_basepath.md`.

## 2026-05-XX — PowerShell 5.1 with `$ErrorActionPreference='Stop'` treats `pio` stderr warnings as fatal [RESOLVED — rc.1.3]

**Problem:** `bin/build_release.ps1` exits with a terminating error even when `pio` itself returned exit code 0 — because PIO emits `-Wmissing-field-initializers` warnings to stderr and PS treats those as terminating errors under `EAP=Stop`.

**Root cause:** PowerShell 5.1's interaction between strict mode and native-tool stderr.

**Fix:** Locally toggle `$ErrorActionPreference='Continue'` around the `& $PIO run` call; gate failure on `$LASTEXITCODE` alone. Landed in rc.1.3. See header comment block in [bin/build_release.ps1](../bin/build_release.ps1) Step 1.
