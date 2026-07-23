# Gotcha log

Append-only. Newest at top. Format per entry: **Problem → Root cause → Fix → Where it lives.**

When something weird happens, check here BEFORE debugging from scratch. Entries that recur or affect multiple subsystems graduate up to a topic file or to [CLAUDE.md](../CLAUDE.md) hard constraints.

Entries that are resolved **and can no longer recur** (code deleted, design changed, fixed both sides) retire to [gotcha-archive.md](gotcha-archive.md) — history only, never needed for triage. Everything still able to bite you is in this file. Being `[RESOLVED]` is *not* sufficient to retire: most resolved entries here stay because an active constraint still depends on them.

## Promoted patterns

- **[PATTERN] One log chain per time window — never let two chains of the same unit's logs coexist in an analysis folder.** The dedup in `plot_daily.py` is tuple-exact and does **not** catch it; the symptom is a day showing ~2× the expected sample count (~5700 vs ~2860). Recurred twice: 2026-06-26 (two independent SD downloads with different rotation boundaries) and 2026-07-20 (files pulled off the card by hand vs. the unit's own later upload of the same files). Rule: **before plotting, run `check_dupes.py`; archive the superseded chain to `archived_overlap/`** rather than deleting it, and sanity-check sample counts in `plot_summary.txt` afterwards. Free verification: if `git status` shows the archived copy and the new file as a rename (`R old -> new`), they are byte-identical.

- **[PATTERN] Python CLI output on this machine must be ASCII-only.** The Windows console is cp1252; any `print()` (or the harness capturing stdout) crashes with `UnicodeEncodeError` on `→ … ✓ ✗ °` etc. Recurred 5+ times (calibration scripts, `check_dupes.py`, `rota_sim.py`, ad-hoc probes). Rule: write `->`, `...`, `deg`, `OK`/`FAIL` — never Unicode glyphs — in Python that prints. If Unicode is unavoidable, set `PYTHONIOENCODING=utf-8` on the invocation. Do NOT trust a crude keyword scan of captured serial/HTTP output either — loose substrings (`corrupted`, `format`) throw false positives; match on the specific message.

---

## 2026-07-23 — logparser mislabels a wind-override SET at `speed == v_max` as a "direction" event (gh#45)

**Problem:** 5C88's wind alarms on Sunday 2026-07-19 parsed as *"WIND OVERRIDE: SET - direction 60 deg in exclusion zone (low bound 60 deg)"* at 12:35:16 and 14:14:31. Taken at face value that says the wind blew from 60° (ENE) into a directional exclusion zone. **It didn't** — the instantaneous wind at both moments was from the NNW (~326–358°), never near 60°, and there is no evidence a directional exclusion was even configured. All three of that day's episodes were **speed** triggers (gusts to 7–9 m/s pushing the ~6-min averaged wind onto `v_max`).

**Root cause:** The raw rows are `value_a=60, value_b=60`. For a wind-override SET, `event_logger.h` encodes **speed** as `va=speed×10, vb=v_max×10` and **direction** as `va=direction°, vb=excl_low°` — two different meanings in one un-tagged `(va,vb)` space. `logparser.py`'s `_fmt_alarm()` disambiguation (`:462`) tests speed with **`va > vb`** (strict). But the firmware fires on `speed >= v_max` (confirmed by the same day's 14:16 row `62,60` → "6.2 >= 6.0"). At exactly `speed == v_max`, `va == vb` (60==60), the strict `>` fails, the CLEARED test (`vb > va`) fails too, and it falls through to the "otherwise → direction SET" branch, which reinterprets the *speed* `60` as a *bearing* of 60° and the `v_max` `60` as `excl_low`. Pure coincidence that 6.0 m/s ×10 = 60 is also a plausible angle.

Deeper point: `60,60` is **genuinely ambiguous from the row alone** — a real direction event with `direction == excl_low == 60°` produces the identical bytes. And the overlap isn't only at the boundary: any direction SET with `direction > excl_low` and `excl_low ≤ 200` already gets grabbed by the speed branch. No parser heuristic can fully separate the two; only a source-side discriminator can.

**Fix:**
- **Immediate (parser, low-risk):** change the speed test at `logparser.py:462` from `va > vb` to `va >= vb`. This is strictly an improvement — it captures the `va == vb` boundary as a speed SET (the common, observed case) and changes nothing else (the CLEARED branch needs `vb > va`, mutually exclusive). It does **not** resolve the residual speed-vs-direction overlap; add a caveat comment saying so.
- **Proper (firmware, the real fix — tracked in gh#45):** disambiguate at the source the way T5 sensor faults already did (see the `_fmt_alarm` docstring: "the new ch-based encoding lets T2/T3 keep ch=0 and T5 own ch≥4"). Give T3 wind-override rows a `channel` (or `param`) discriminator — e.g. speed-SET / direction-SET / CLEAR each get their own code — so the parser reads the type instead of guessing. **Per CLAUDE.md this is a log-format change: `log/logparser.py` must learn the new encoding in the same changeset**, and it needs a version bump.

**How to recognise it:** a wind "direction" override whose reported bearing does **not** match the `SENSOR_HR` wind-direction samples around the same timestamp, especially when the bearing numerically equals `v_max×10` (60 = 6.0 m/s, 80 = 8.0, …). Cross-check every "direction" wind event against the instantaneous wind before believing it. This sits alongside the ASCII-only promoted pattern's warning: don't trust a decode you can cross-check but didn't.

**Where it lives:** `log/logparser.py` — `_fmt_alarm()` disambiguation block (`:445`–`:490`); source encoding in `firmware/src/safety_monitor/safety_monitor.cpp` (T3) and the `value_a/value_b` catalogue in `firmware/src/event_logger/event_logger.h`.

---

## 2026-07-20 — you cannot tell which firmware wrote an SD log; post-OTA proof must come from `/api/status`

**Problem:** Trying to confirm that 5C88 had actually pulled 2.2.15 (to explain why its uploads resumed), the obvious move was to read the version out of the freshly-uploaded logs. There is nothing to read. Two separate reasons, both non-obvious.

**Root cause:** (1) The SD CSV format carries **no firmware-version field** — `SYSTEM` rows log unit ID, heap, reset info and OTA *progress* ("firmware verified OK", "ROTA apply: committed"), but never the version string itself. (2) Worse, even the OTA-apply *event* is unreachable at the moment you want it: a unit applies an update and keeps writing to its **currently-active** log file, which by definition has not rotated and therefore has not uploaded. The apply record for version N sits on the card until the *next* rotation. So the uploaded corpus is always one file behind the event you are trying to confirm.

**Fix:** Treat uploaded logs as evidence of **behaviour**, never of **version**. Version confirmation has exactly one source: `GET /api/status` → both `fw_ver` **and** `asset_version` (the paired-commit invariant in CLAUDE.md). Logs can strongly *corroborate* — 5C88's six-day `0,2` run ending in a backlog drain is about as good as behavioural evidence gets — but "the behaviour changed" is not "the version is X", and the two must not be conflated in a report.

**Tool note:** `log/logparser.py <file>` writes `parsed_<name>.txt` **next to the input** and prints only a one-line summary to stdout — piping it to `grep` looks like the parser found nothing. Grep the output file. It also decodes the window bitmask to `M1=CLOS M2=CLOS M3=OPEN (0x…)`, which makes per-motor state greppable without touching the raw encoding.

**Where it lives:** `log/logparser.py`; `firmware/src/event_logger/event_logger.h` (value_a/value_b catalogue — note the absence of a version field); CLAUDE.md "Releases & OTA" hard constraints.

---

## 2026-07-17 — 5C88 log uploads stopped: T14's upload enumerator truncates its SD-scan buffer once >~21 files exist (gh#42, gh#36 redux — NOT a card/SD fault) [RESOLVED 2.2.15 — verified on hardware 2026-07-17]

**Problem:** 5C88 (production, **2.1.3**) stopped uploading SD logs to `rfsee.net/hbwv` after Jul 13 19:29, with **zero** upload-failure rows — it just went silent. Status POSTs kept succeeding the whole time.

**FALSE START (recorded so the next person doesn't repeat it):** first hypothesis was an SD **write-fault** — "mounted but unwritable, `s_sd_ok` oscillating." **Disproven** the moment the operator pulled the local files: three cleanly-rotating CSVs (`5C88_20260713192815` → `…0715142154` → `…0717090944`, ~1.8-day cadence). The card writes and T9 rotates *perfectly*. Lesson: **before theorising a storage failure, get the actual files off the card** — filename timestamps alone prove whether rotation is happening. Don't infer "no new files" from the *upload* side going quiet.

**Root cause (confirmed in code + log data):** both T14 upload triggers — daily (`status_post.cpp` ~L862) and on-rotation (~L904) — call `upload_pending()` → **`event_logger_next_pending()`**, which scans the SD file list into a **`char list[512]`** (`event_logger.cpp:1020`; `event_logger_newest_closed()` at `:953` has the same bug). The correct size is **`SD_LIST_BUF_LEN` = SD_MAX_FILES(30) × (SD_NAME_ONLY_LEN(28)+1) + 1 = 871 B**, used correctly by the other three scan callers (`:409/:617/:732`). 512 B holds only **~21** names (`5C88_YYYYMMDDHHMMSS.csv,` = 24 B each). Once the SD carries **>~21 CSVs**, `sd_scan()` **silently truncates** (the same `storage_sd_list_csv()` "returns OK on overflow" behavior as the 2026-07-04 gh#36 gotcha) and the **newest** closed files drop out of the list. `next_pending(after=log_last_up)` then finds nothing newer than the last-uploaded latch → returns false → `upload_pending` returns 0 → daily logs `value_a=0,value_b=2` "no closed file", on-rotation uploads nothing. **Uploads stop permanently** while rotation / status POST / TLS / heap stay perfectly healthy. This is an **incomplete gh#36 fix**: the a.6.35.2 multi-file drainer (added before the 2.1.2 gh#36 sweep) kept/reintroduced the `512` literal in these two functions and the sweep missed them.

**Affected every build through 2.2.14** (the a.6.35.2 drainer kept the `512` literal at `:953`/`:1020`), not just 2.1.3 — any such unit silently stops uploading ~at its 22nd log file. **Fixed in 2.2.15** (gh#42): both functions now size from `SD_LIST_BUF_LEN`. **Verified on hardware 2026-07-17** (2344 on 2.2.15): a **26-file** card rotated to 27 and T14 uploaded the newest-closed ~1 MB file (`2344_20260717111737.csv` → server `2026-07-17_154407.log`, 1,024.0 KB) plus drained the fillers to `pe1mew.nl/hbwv` — the exact truncation failure, exercised and **passed**; gh#42 closed. This is what stalled 5C88; plausibly 2344's pre-swap Jul-14 stop too (board swap confounds it).

**Confirmed in production 2026-07-20 — a six-day A/B on one unit.** After 2.2.15 reached `mainstream`, 5C88's own T14 rows tell the whole story: last upload success **Jul 13 19:29**, then the `0,2` "no closed file" diagnostic on **six consecutive daily slots (Jul 14, 15, 16, 17, 18, 19)** while closed files demonstrably accumulated, then a clean **three-file backlog drain at Jul 20 03:30** (uploads 26 s apart). Same card, same 03:30 slot, same site — only the firmware changed. Recovered from the drained files themselves: `2026-07-20_033026/_033052/_033112.log`.

**The `0,2` diagnostic is the tell:** in the SD audit rows, `initiator=WEB, value_a=0, value_b=2` = "daily slot fired but found no closed file." If you see it while closed files demonstrably exist on the card, the enumerator is truncating — not the card, not the network. (Encoding catalogue: `event_logger.h` value_a/value_b table; WEB `1,1`=upload OK, `1,0`=status POST OK, `0,1`=upload fail/HTTP-code, `0,2`=nothing fresh.)

**Fix (applied 2.2.15, commit `5e1bdc1`):** `char list[512]` → `char list[SD_LIST_BUF_LEN]` at `event_logger.cpp:953` and `:1020`. `:437`/`:668` in `check_free_space`/`write_to_sd` **kept** `list[512]` with a clarifying comment (count-only vs SD_MIN_FILES=5 — truncation is harmless there). Bug-fix → **patch** bump; soak on 2344/FDA4 **needs >21 files on the card** (or temporarily lower SD_MAX_FILES) to exercise the truncation. **Interim recovery for 5C88 with no firmware update:** have farm-hands delete ~10 of the **oldest already-uploaded** CSVs via the web GUI Log tab (keep the ones newer than the Jul-13 latch) → count drops under ~21 → the 512-B scan fits again → `upload_pending` drains the pending files → verify a new file lands in `…/hbwv/log/logs/`.

**Diagnostic that reframed it (keep for next "unit stopped uploading"):** the status site's live cached `…/hbwv/data/status.json` proved the unit was alive (fresh `time_iso`, `uptime_s` = 6 d ⇒ no reboot, climate running, heap flat) — ruling out offline/crash/leak remotely. Then the **local CSVs** proved rotation works and carried the `0,2` markers. (Note `eg1` in that JSON is the climate EventGroup — WIND_OVERRIDE/MOTOR_ALARM/SENSOR_FAULT/STANDBY — not a T9/SD flag; `eg1:0` = no alarms.)

**Where it lives:** `firmware/src/event_logger/event_logger.cpp` — `event_logger_next_pending` (`:1020`), `event_logger_newest_closed` (`:953`); `firmware/src/status_post/status_post.cpp` — `upload_pending` (`:705`), daily trigger (`:862`), on-rotation trigger (`:904`).

---

## 2026-07-14 — `-Werror=format-truncation`: enlarging a char buffer broke a DOWNSTREAM snprintf

**Problem:** The 2.2.14 build failed with `error: '%s' directive output may be truncated writing up to 79 bytes into a region of size 74 [-Werror=format-truncation=]` at `web_server.cpp` — pointing at the `Content-Disposition` snprintf, a line I had NOT changed.

**Root cause:** For the gh#39 filename change I enlarged `fname[64]` → `fname[80]`. GCC's `-Werror=format-truncation` computes a worst case for every `%s`: the downstream `snprintf(disp[96], "attachment; filename=\"%s\"", fname)` now assumed `%s` could write up to `sizeof(fname)-1 = 79` chars, and 23 (literal) + 79 + NUL > 96 → hard error. The *enlarged buffer*, not the changed line, tripped it.

**Fix:** Size buffers to their real max, not "generously" — reverted to `fname[64]` (actual max ~47). **Lesson:** on a `-Werror=format-truncation` build, enlarging a `char[]` can break a *different* `snprintf` that formats it into a fixed buffer; the error points at the downstream line, not the one you changed.

**Where it lives:** `firmware/src/web_server/web_server.cpp` (coredump download filename + `disp`).

---

## 2026-07-14 — testing the ROTA quiet gate / session exemption needs the night window OPEN

**Problem:** The gh#41 hardware test (a session-triggered update must apply without deferring) looked like it FAILED — FDA4 held `apply=1` (deferred) for 3 min with the session active, as if the fix were broken.

**Root cause:** `rota_apply()` gates on `if (!in_night_window(lo,hi) || !quiet_gate())` — the night window is tested FIRST and short-circuits `||`, so `quiet_gate()` (and the session exemption) never runs when the window is shut. FDA4's window was `22–04` and it was 16:44 local → deferred on the *window*, not the session. `eg1=0` (no gate bits) confirmed the gate itself was clear.

**Fix:** To exercise the quiet gate / session logic, open the window first — `POST /api/ota/config {"win_lo":0,"win_hi":0}` (equal = disabled = apply any hour), test, then restore. With the window open the fix applied immediately while the session stayed active (confirmed). **Lesson:** before diagnosing a ROTA "downloads but won't install," check `GET /api/ota/config` win_lo/win_hi against the local hour — the window blocks *before* the quiet gate.

**Where it lives:** `firmware/src/ota_client/ota_client.cpp` (`rota_apply`, `in_night_window`, `quiet_gate`).

---

## 2026-07-14 — logrotate: validate as root, and group-writable /var/log needs `su`

**Problem:** A new `/etc/logrotate.d/rota` config passed `logrotate --debug` when validated as `remko`, but failed under `sudo` (the real cron context) with `error: skipping "/var/log/rota-pull.log" because parent directory has insecure permissions ... Set "su" directive`.

**Root cause:** Two things. (1) logrotate only enforces its parent-directory security check when running **as root** — a non-root `--debug` run silently skips it, so a normal-user dry-run is NOT a valid test. (2) `/var/log` is `root:syslog 0775` (the standard rsyslog layout — group-writable by `syslog`); logrotate 3.14+ refuses to rotate a log whose parent dir is writable by a non-root group unless the config names a rotation user via `su`.

**Fix:** Add `su root root` inside the config block (root renames/creates in `/var/log`; the `create 0664 www-data www-data` line still hands the fresh log back to www-data). Always validate logrotate configs with `sudo logrotate --debug <file>`, never as a normal user, or the check won't fire.

**Where it lives:** ⧉ **separate repo** `greenhouse-Controller-FOTA-server` (these paths do **not** resolve in this repo) — `tools/rota-logrotate` (carries the `su root root` + a comment explaining why), `tools/bootstrap.md` §7.

---

## 2026-07-13 — ROTA server (rfsee.net VPS): registry/permission setup traps

**Problem:** After deploying the FOTA server, authenticated requests kept returning `204` (looked like auth failure) then `404 manifest_missing` — cost several round trips to diagnose.

**Root causes (both operator-side config, not code):** (1) `devices.json` was pasted as a bare `"id": {…}` row **without the wrapping `{ }`** → invalid JSON → `json_decode` null → empty registry → every device 204 (the server fails closed, so it *looks* like auth failure). (2) A hand-place command computed `sha256sum`/`stat` as the login user against **root-owned** files → "Permission denied" → empty vars → a manifest written with `"fw_size":,` (invalid JSON) → 404.

**Fix / rules for VPS work:** validate every JSON file after editing (`php -r 'echo json_decode(file_get_contents($f))===null?"INVALID":"valid";'`). Run file-creating command blocks entirely as root (`sudo bash -c '…'`) so the reads inside succeed. To check a file php-fpm will read, test **as php-fpm's user**: `sudo -u www-data cat <file>` — a `remko`-run `cat` on a `www-data`-owned file gives a misleading "Permission denied". After creating store files, `chown -R www-data:www-data` so php-fpm can read/write (registry `.lock`/`.tmp`, `checkins.csv`, `nonce-cache/`).

**Where it lives:** ⧉ **separate repo** `greenhouse-Controller-FOTA-server` (these paths do **not** resolve in this repo) — `tools/init-store.sh`, `examples/devices.example.json` (the correct full-object shape), `public/lib/rota_lib.php` (fails closed on unparseable registry — safe, but hard to diagnose). The files named in the body above (`devices.json`, `checkins.csv`, `nonce-cache/`, `.lock`/`.tmp`) are **deployed server state on the VPS**, in neither repo.

---

## 2026-07-13 — Greenfield cable-flash: web assets need `mklittlefs`, NOT `pio buildfs`

**Problem:** After a from-scratch esptool flash of a new S3 (bootloader + partitions + otadata + app all fine, board boots clean), the web GUI served "Web assets not yet uploaded / requested path /index.html". The firmware ran but the LittleFS assets partition (lfs0) had no readable `index.html`.

**Root cause:** Two-part. (1) `platformio.ini` has **no `board_build.filesystem` setting**, so `pio run -t buildfs` produces a **SPIFFS** image (`spiffs.bin`) — but the firmware mounts **LittleFS** on lfs0/lfs1. Flashing that SPIFFS image → mount finds nothing. (2) `firmware/data/manifest.json` in source form carries the `{{ASSET_VERSION}}` placeholder (restored by build_release Step 3.5, gh#9), so a naive image also has a placeholder asset_version.

**Fix:** Build the LittleFS image directly with the bundled tool and flash it to the active bank:
```
# stamp version, build littlefs (esp params b=4096 p=256, size = lfs0 partition 0x100000)
printf '{"asset_version":"2.1.3","checksum":""}' > firmware/data/manifest.json
~/.platformio/packages/tool-mklittlefs/mklittlefs.exe -c firmware/data -b 4096 -p 256 -s 0x100000 lfs_assets.bin
esptool ... write_flash 0x420000 lfs_assets.bin          # lfs0 = app0's /lfsa
printf '{"asset_version":"{{ASSET_VERSION}}","checksum":""}' > firmware/data/manifest.json   # restore placeholder (gh#9)
```
Verify: serial shows `littlefs_mount(A (lfs0)) returned 0 (OK)` + `/index.html exists`. The normal (non-greenfield) asset path is OTA zip extraction via `POST /api/ota/assets` — greenfield-by-cable is the exception that needs the raw LittleFS image.

**Full greenfield recipe:** `esptool erase_flash` (clears coredump @0x620000) → `write_flash --flash_mode dio` bootloader@0x0 / partitions@0x8000 / otadata@0xe000 / app@0x20000 → `mklittlefs` image @0x420000. Offsets from `firmware/.pio/build/lolin_s3/flash_args` + `partitions.csv`.

**Where it lives:** `firmware/platformio.ini` (no filesystem setting); `bin/build_release.ps1` Step 2 (calls it "LFS image" though buildfs defaults to SPIFFS here — works in the release ZIP path because assets ship as an OTA STORE zip, not a flashed image).

---

## 2026-07-13 — Branch switch carried the whole staging area into the wrong commit

**Problem:** Creating the `rota` feature branch while ~23 unrelated files were staged, then committing "model changes" on `main`, swept the rota-only work (TDS + 2.2.0 version bump) into the model commit on `main` (`32ce6b5`). Cleanup needed a compensating commit on main (`5c3c6cc`, version back to 2.1.3) and re-pointing `rota` to the new tip.

**Root cause:** Git's index is shared across branch switches — `git checkout -b` carries all staged changes, and a broad `git commit` on the other branch commits everything staged, regardless of which "stream" of work it belongs to.

**Fix / pattern:** When work spans branches, commit (or stash) each stream *before* switching. When Claude stages changes that belong to different destinations, it must say so explicitly at hand-off and repeat the warning before any branch operation. Recovery recipe when it happens anyway: compensating commit on the polluted branch + re-point the feature branch (`git branch -D` + recreate at tip works when the feature branch has no own commits).

**RECURRED 2026-07-20 (2nd time) — no branch switch involved; `commit -a` was enough.** Three unrelated streams were pending (a one-file repair of `memory/gotcha-archive.md`, ~115 lines of T15 firmware hardening, and a new `log/heap_soak.py`). Only the repair was staged, and it was announced as such. The commit still swept the firmware in, because **`git commit -a` / `git add -A` picks up every modified tracked file regardless of what was deliberately staged**. Result: `cb6a178` carries the T15 build guard, stub markers and corrected header docs under the message "docs(memory): add the gotcha-archive file 46b709f left untracked". Already pushed, and branch protection forbids the rewrite, so it stands.

**Lesson that the 2026-07-13 version missed:** staging one stream does **not** protect the others. Selective staging only works if the commit is also selective (`git commit` with no `-a`). Two consequences:

- If the workflow is `commit -a`, then **everything pending will land in one commit** — so the honest move is to stage *all* pending work together and write a message that covers all of it, not to stage one stream and hope.
- A brand-new file the commit *depends on* is the sharpest edge: `46b709f` committed `gotcha-log.md` + the memory index both linking to `gotcha-archive.md` while the archive itself sat untracked, so HEAD briefly had two dangling links and had lost the three retired entries outright. **Untracked-and-load-bearing must be called out explicitly at hand-off, not listed among `??` entries.**

**Where it lives:** Process, not code. Commits `32ce6b5`/`5c3c6cc` (branch-switch variant) and `46b709f`/`cb6a178` (`commit -a` variant) are the examples.

---

## 2026-07-13 — Wind measurements before 2026-06-19 12:00 are invalid

**Problem:** Wind speed/direction columns exist in SD logs and campaign data from Jun 4, but the wind vane was only commissioned **2026-06-19 12:00**. Earlier values (including the wind columns in `plot_summary.txt` for Jun 4–19) are garbage and must not be used in any analysis.

**Root cause:** Sensor commissioning happened mid-campaign; the log format carries the columns regardless.

**Fix:** Constraint recorded in `thermalProfileCampaign.md` §9.11 and enforced in `ns9_direction_stratified.py` (`WIND_VALID_FROM`). Any new wind-based analysis script must filter `timestamp >= 2026-06-19T12:00`.

**Where it lives:** `model/campaign-summer-2026/ns9_direction_stratified.py` (the reference filter); campaign doc §9.11; `model/campaign-summer-2026/plot_daily.py` — the day-plot wind-direction axis is gated on the same date, so pre-Jun-19 plots render **without** a direction scatter by design (added 2026-07-20; if you regenerate all plots, `git checkout --` the pre-Jun-19 PNGs).

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

## 2026-06-26 — `plot_daily.py` sorted-set dedup fails when two SD download chains overlap

**Problem:** Feeding log files from two independent SD card downloads of the same unit (different rotation boundaries, same time period) causes days in the overlap window to show ~2× the expected sample count (~5700 vs ~2860 per day). The `sorted(set(tuple(e)))` dedup at `load_logs()` line 182 does not remove the double-counted readings.

**Root cause:** The two chains have different SD file boundaries, so their `SENSOR_HR` event tuples differ in context (e.g., mode-change rows, BOOT rows) even though the sensor data timestamps are identical. The dedup is tuple-exact — any field difference between the two chains' representations of the same timestamp prevents deduplication.

**Fix:** Keep only one chain per time window. Archive the overlapping files from the older chain (`archived_overlap/` subfolder). For the 5C88 campaign: Chain A (old downloads) covers Jun 4–15 uniquely; Chain B (new download) covers Jun 15–24. Archive the four Chain A files whose range is wholly covered by Chain B.

**RECURRED 2026-07-20 (2nd time) — new shape, same trap.** This time the two chains were (a) log files pulled off 5C88's SD **by hand** during the gh#42 investigation and (b) the unit's **own later upload of the same files** once the fix landed. Two of the three were byte-identical, the third a strict prefix of a longer file. Same remedy (archive the superseded chain to `archived_overlap/`), and a free verification trick: **if `git status` reports the archived copy and the new file as a rename (`R old -> new`), they are byte-identical** — git's rename detection doubles as dedup proof. Promoted to a pattern, see top of file.

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

**Fix:** Pass `--flash_mode dio` to esptool for the full-chip flash; runtime `board_build.flash_mode = qio` in `firmware/platformio.ini` stays.

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

## 2026-XX-XX — Dual LittleFS partitions need separate VFS basePaths [RESOLVED]

**Problem:** Mounting `lfs0` and `lfs1` to the same VFS basePath (`/lfs`) causes the second mount to overlay the first; assets are read from the wrong partition.

**Root cause:** ESP-IDF's VFS treats basePath as the mount point; identical basePaths collide.

**Fix:** Each partition gets its own basePath: `/lfsa` for `lfs0`, `/lfsb` for `lfs1`. The active-partition resolver picks via the running app bank. **Invariant + full mechanism now recorded in [architecture.md](architecture.md) "Partition table"** (moved there 2026-07-20 from user-global memory, so it is tracked in git and reachable by anyone).

## 2026-05-XX — PowerShell 5.1 with `$ErrorActionPreference='Stop'` treats `pio` stderr warnings as fatal [RESOLVED — rc.1.3]

**Problem:** `bin/build_release.ps1` exits with a terminating error even when `pio` itself returned exit code 0 — because PIO emits `-Wmissing-field-initializers` warnings to stderr and PS treats those as terminating errors under `EAP=Stop`.

**Root cause:** PowerShell 5.1's interaction between strict mode and native-tool stderr.

**Fix:** Locally toggle `$ErrorActionPreference='Continue'` around the `& $PIO run` call; gate failure on `$LASTEXITCODE` alone. Landed in rc.1.3. See header comment block in [bin/build_release.ps1](../bin/build_release.ps1) Step 1.

## 2026-07-13 — Rapid OTA reboots rate-limit SNTP → T16 (ROTA) skips checks

**Problem:** During ROTA client testing on a dev unit (FDA4), T16's manifest check kept returning `result:"skipped", code:3` for many minutes, even though `/api/status` reported a correct wall-clock time. `system.ntp_synced` stayed `false`.

**Root cause:** OTA-pushing/reflashing the *same* unit many times in an hour makes it re-run `nm_sntp_quick_sync()` on every boot; pool.ntp.org rate-limits (KoD) the source IP after the burst, so the fresh per-boot sync never completes and the `nm_is_sntp_synced()` latch never sets. The internal ESP32 RTC retains valid time across warm reboots (so the *clock* is right), but T16 gates on the strict SNTP latch by design (see [rotaImplementationPlan.md](../design/rotaImplementationPlan.md) risk #3), so it skips rather than sign a request on an untrusted clock.

**Fix:** Not a firmware bug — a test-environment artifact. Wait it out: T16 recovers on the rc.1.5.6 SNTP retry cadence (`NTP_RETRY_INTERVAL_S=300`, 5 min). To avoid it, batch firmware changes before pushing rather than reflashing the same unit in a tight loop when you need a synced clock. Never arises in production (units don't reboot 8×/hour). Check state via `/api/ota/check` (`result`) + `/api/status` (`system.ntp_synced`).

## 2026-07-13 — T16 (ROTA) 8 KB stack overflows on the download/apply path [RESOLVED — 2.2.1]

**Problem:** First live pull-install put FDA4 into a **crash loop** (reboots at ~35 s uptime, stayed on the old version, GUI slow). Coredump: `A stack overflow in task T16-rota has been detected` (`esp-coredump ... info_corefile --core-format raw`).

**Root cause:** T16 was created with an 8 KB stack — enough for the manifest *check* (one mbedTLS handshake + a `cert[2048]` on the stack), but the *download* path nests a **second** mbedTLS handshake (`rota_download_verify → rota_https_get`) inside `ota_check_once`'s still-live frame, with a `cert[2048]` buffer live in **both** `ota_check_once` and `rota_handle_update`. Two TLS contexts + two 2 KB PEM buffers blew past 8 KB. It only surfaced on a real artefact download (the earlier 204/200 check tests never entered this path).

**Fix (2.2.1):** T16 stack 8 KB → **16 KB** (matches T13's OTA-work sizing, `firmware/src/main.cpp`), and both `cert[ROTA_CERT_MAX]` PEM buffers moved from stack to `malloc`/`free` (`ota_client.cpp`, −4 KB peak). **Lesson:** any task that runs `esp_http_client` over TLS needs ≥ ~12–16 KB; never stack-allocate the pinned-cert PEM. A stack that survives a small GET can still overflow on a large download that nests a second handshake.

## [RESOLVED 2.2.14] 2026-07-13 — `/api/coredump/status` labels a stale dump with the RUNNING version, not the crashed one

**Problem:** After FDA4 was updated to 2.2.2 (successfully, no crash), `/api/coredump/status` reported `{"present":true,"fw_ver":"2.2.2"}` — reading as "2.2.2 crashed." It hadn't. The dump was byte-identical (`cmp`) to the earlier 2.2.0 stack-overflow dump.

**Root cause:** two independent things compound. (1) `coredump_status_handler` hardcodes `"fw_ver":"` `FIRMWARE_VERSION` `"` (web_server.cpp:1953) — the *compile-time running* version — and the download filename is `coredump-` `FIRMWARE_VERSION` `-<ts>.bin` (web_server.cpp:2029); neither reflects the version that actually produced the dump. (2) The coredump partition is **only erased on greenfield flash** (the first-flash rule), **not** on OTA push/pull — so a pre-update crash dump lingers and then gets stamped with each subsequent running version.

**Fix / workaround:** Before believing a coredump matches the running version, verify: `cmp` against known prior dumps, or decode it (`esp-coredump ... --core-format raw <that-version's elf>`) and check the backtrace/task set. Erase a known-stale dump with `POST /api/coredump/erase`. **Improvements to consider:** erase the coredump partition as part of the OTA apply (T13 + T16/rota_apply) so a dump always matches the running image; and/or have the status endpoint report the version parsed from the dump's `esp_app_desc` instead of `FIRMWARE_VERSION`.

**Resolved (2.2.14, gh#39):** Part 1 done — at boot the dump's ELF-SHA (`esp_core_dump_get_summary`) is compared to the running image's (`esp_app_get_elf_sha256`) and cached as `stale`. `/api/coredump/status` now reports `running_fw_ver` (not the misleading `fw_ver`) + `"stale":bool`; the download filename gains a `-stale` marker; the Log-tab GUI shows "from an EARLIER firmware (running X)." Part 2 (erase the coredump on OTA apply) was **deliberately NOT done** — the operator chose to preserve dumps across updates and rely on the `stale` flag, so no crash data is ever lost to an update.
