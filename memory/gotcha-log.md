# Gotcha log

Append-only. Newest at top. Format per entry: **Problem → Root cause → Fix → Where it lives.**

When something weird happens, check here BEFORE debugging from scratch. Entries that recur or affect multiple subsystems graduate up to a topic file or to [CLAUDE.md](../CLAUDE.md) hard constraints.

---

## 2026-06-26 — `plot_daily.py` sorted-set dedup fails when two SD download chains overlap

**Problem:** Feeding log files from two independent SD card downloads of the same unit (different rotation boundaries, same time period) causes days in the overlap window to show ~2× the expected sample count (~5700 vs ~2860 per day). The `sorted(set(tuple(e)))` dedup at `load_logs()` line 182 does not remove the double-counted readings.

**Root cause:** The two chains have different SD file boundaries, so their `SENSOR_HR` event tuples differ in context (e.g., mode-change rows, BOOT rows) even though the sensor data timestamps are identical. The dedup is tuple-exact — any field difference between the two chains' representations of the same timestamp prevents deduplication.

**Fix:** Keep only one chain per time window. Archive the overlapping files from the older chain (`archived_overlap/` subfolder). For the 5C88 campaign: Chain A (old downloads) covers Jun 4–15 uniquely; Chain B (new download) covers Jun 15–24. Archive the four Chain A files whose range is wholly covered by Chain B.

**Where it lives:** `temp/5c88_modelCampaign/plot_daily.py` line 182 (`load_logs`). Chain overlap detection: `temp/5c88_modelCampaign/check_dupes.py`.

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

## 2026-06-10 — `HEAD /api/log/download` reports constant 45 B regardless of file size

**Problem:** SD log file-size probe via HTTP HEAD returns `Content-Length: 45` even for 1 MB files. Misleads any tooling that uses HEAD to size files before downloading.

**Root cause:** Suspected — HEAD handler emits a hard-coded short response rather than running the GET path's size calculation.

**Fix:** Use GET with `Range: bytes=0-0` to read just one byte and inspect `Content-Range` for the total. Or just GET the whole file. Filing of a proper fix issue is pending.

## 2026-06-10 — `rfsee.net/hbwv/api.php` rejects POSTs > ~1 MB with HTTP 413

**Problem:** T14's daily log upload fails every day with `code=413` once an SD log file rotates to its 1 MB cap. The `log_last_up` field froze at a May 24 filename for 17 days while uploads silently failed.

**Root cause:** Server-side `post_max_size` (PHP) and/or `client_max_body_size` (nginx) is below 1 MB on rfsee.net. SD rotation is correctly capped at `LOG_ROT_BYTES = 1 MB`; every rotated file exceeds the server limit.

**Fix (preferred):** Raise the server-side upload limit to ≥ 2 MB. Alternative: drop device-side rotation size, but the server fix is cleaner. Forensic visibility tracked in [gh#34](https://github.com/pe1mew/greenhouse-Controller/issues/34) (record HTTP code in SD audit row).

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
