# Gotcha log

Append-only. Newest at top. Format per entry: **Problem → Root cause → Fix → Where it lives.**

When something weird happens, check here BEFORE debugging from scratch. **Start at the [index](#index--by-where-it-bites-you)** — it groups every entry by subsystem with symptom-first hooks, which is faster than scrolling 48 entries. **Adding an entry means adding its index line too**; the pair is checked by counting `^## 20` headings against `^- \*\*20` index lines. Entries that recur or affect multiple subsystems graduate up to a topic file or to [CLAUDE.md](../CLAUDE.md) hard constraints.

Entries that are resolved **and can no longer recur** (code deleted, design changed, fixed both sides) retire to [gotcha-archive.md](gotcha-archive.md) — history only, never needed for triage. Everything still able to bite you is in this file. Being `[RESOLVED]` is *not* sufficient to retire: most resolved entries here stay because an active constraint still depends on them.

> **Archive pass 2026-09-07 — all 48 entries reviewed, none retired.** The bar is *resolved **and** can no longer recur*, and nothing clears it. Every `[RESOLVED]` entry is held by something live: an active CLAUDE.md hard constraint (qio/dio, coredump erase, paired-commit, the broken HEAD endpoint), a diagnostic fact about *current* firmware found nowhere else (the DS1307 entry's "`time_iso` is the MX4 shadow, 0–60 s stale by design"), the ability to read **historical** logs (gh#45's pre-2.3.0 misparse, wind invalid before 2026-06-19), a documented **recurrence** (the branch-switch entry recurred 2026-07-20; SD-buffer truncation recurred as gh#42), or an environmental trap that was never code-fixed (PowerShell `2>&1`, `pio` not on PATH). The log is large because it is load-bearing, not because it hoards. **Re-run the pass only after a release that deletes code**, not on size alone. The pass did find a live defect: `rota_tds.md` R-C01 still specified the 8 KB T16 stack that crash-looped FDA4 — corrected the same day.

## Promoted patterns

- **[PATTERN] A grep is a claim about spelling; only running the code is evidence about behaviour.** Four instances: the real gh#51 Group B parser gap was found **by running the parser** after inspection had missed it (2026-09-10); then three false findings in a single audit (2026-09-12) — a mistyped symbol (`persist_state` for `persist_ch_state`) "proved" a function had no callers, a guessed variable name (`s_win_ws_last` for `s_win_w_last`) "proved" a config change was ignored, and a `va == N` regex "proved" a decoder missed six subtypes it handles. Rules: (a) to test a decoder, **feed it rows and read the output** — never enumerate its branches; (b) to test a call graph, grep the *exact* symbol, print the hit list, and sanity-check the count; (c) **a negative grep is the weakest evidence in the toolbox** — before reporting "X never happens", find the positive case you expect to exist and confirm the same grep finds *that* first (the fail-first rule, applied to searching); (d) sibling of the cross-reference pattern below — a link check and a grep both test the address, not the content.

- **[PATTERN] A failure that only reaches the serial console has not been logged. Anything that changes control behaviour or hides a state must produce an SD row.** Four instances: (1) T2's dwell deferral of T6 at `ESP_LOGD` — the controller sat inert for 25 min with no trace (2.4.5); (2) a refused manual LCD command leaves nothing (2026-09-10); (3) the IO0 stage-2 factory reset leaves nothing — seven namespaces erased, no row (2026-09-11); (4) a failed DS1307 read at `ESP_LOGW` — the only evidence was a boot row stamped 1970 (gh#55, 2026-09-11). Rules: (a) if a code path can change what T6/T2/T3 do, or can make the unit refuse an operator, it emits a `LOG_SYSTEM` row with a value_a code and the parser learns it in the same change; (b) "it's in the serial log" is not observability on a unit in a greenhouse; (c) when a symptom has "no trace", search the log for rows stamped 1970 and for rows the *shadow* stamps differently from `time(NULL)` — the absence has a shape.

- **[PATTERN] Show the check can fail before trusting a pass.** Three instances in two days: (1) a before/after motor-timing measurement landed on the same 24 s for opposite reasons (2026-09-10) — a third run at a different setting was the evidence; (2) AT-WP05's headline counter `err_busy` **cannot fail with two callers** (500 ms lock timeout vs ~215 ms hold) — 7117 clean reads proved nothing about contention (2026-09-11); (3) the gh#52 hardware test run from AUTOMATIC passed on code where the fix is inert (2026-09-11). Rules: (a) before reading a pass, name the input that would make the check fail and confirm the check sees it — the Modbus fail-first rule generalised; (b) a criterion that no plausible failure can trip is a *description*, not a test — say so in the results; (c) a fix to a transition is tested from the state the transition leaves.

- **[PATTERN] When a change gates a shared queue, table or key set, enumerate every producer and consumer with `grep` and put the list in the release notes — never reason about "the other callers".** 2026-09-11, 2.4.6: the gh#53 fix rejected any `/api/config` key without a `cfg_shadow_t` field. The verification enumerated the web GUI's 33 keys from `app.js`, then *reasoned* about the LCD and T10 (the code comment said "defence in depth for the LCD"). `grep -rn 'xQueueSend(Q4\|post_q4('` lists five producers; two of them post `wifi/ap_enable`, which has no shadow field on purpose because T10 polls NVS for it. The AP toggle — the recovery path for a unit that has lost its WiFi — was dead within minutes of the OTA, and the operator found it, not the tests. Rules: (a) the enumeration is a `grep` output pasted into the notes, not a sentence; (b) a key set defined as "the set of things X handles" is wrong whenever some consumer reads the store directly — ask *who reads NVS / the queue without going through X*; (c) the full 2.3.1→2.4.6 surface comparison (`design/releaseComparison_2.3.1_vs_2.4.6.md`) is the template — regenerate its §2 mechanically for the next production candidate.

- **[PATTERN] A rule written to *hit* a requirement exactly will fail it in practice. Check whether the derivation leaves margin, and test it on the fastest hardware you have.** Two instances in the window-sensor work, both found only because the rig runs ~13x faster than production and both affecting production identically once seen. (1) `poll = travel/100` was chosen so overshoot would meet FR-WP04's 1 % of stroke — but overshoot = poll x speed = (t/100) x (100/t) = **exactly 1.00 %, by construction, at every travel time**, leaving nothing for poll jitter, task scheduling or a Modbus retry. (2) `reject rate above 2x nominal` was measured at a **9 % margin** (peak 2100 vs threshold 2306) because nominal derives from `travel_m3`, which covers switch-to-limit while the position span is switch-to-switch — so 2x nominal was really ~1.5x real. Rules: (a) when a derived constant is defined so that it *equals* the limit, treat that as a defect, not a tight fit — budget it at 1/2 to 2/3; (b) **a fast rig is not a nuisance, it is the only place these show up** — a production-speed bench would have shipped both; (c) a plausibility check that false-trips **discards good data silently**, which is worse than not having the check.

- **[PATTERN] This codebase has a recurring class of defect: an affirmative success signal for something that did not happen. Never accept a UI tick, an HTTP 200 or an LCD confirmation as evidence of effect.** Four instances, three of them found in a single session (2026-09-10): (1) **gh#51** -- the Motors tab showed a new travel time with a green tick while T2 kept running the old one until reboot, because `/api/config` reads T4's shadow and T2 caches its own copy; (2) **gh#51 Group C** -- the LCD showed "Settings Reset! / Defaults loaded" after an IO0 stage-2 erase while T4's shadow and T2's cache both still held pre-reset values; (3) **gh#53** -- `POST /api/config` with an unrecognised key returns `{"ok":true}`, writes junk to NVS and applies nothing; (4) the standing **paired-commit rule** exists for the same reason -- a firmware-only OTA reports success while stranding the asset partition, which is why both `fw_ver` AND `asset_version` must be read post-reboot; (5) **2.4.6's own release notes** asserted the LCD path was covered by "defence in depth" with no enumeration behind the sentence — the AP toggle was dead (2026-09-11); (6) the first **gh#52 hardware test** passed from AUTOMATIC, where old and new code are equally inert (2026-09-11). Rules: (a) a 200 from an async endpoint means *queued*, not *applied* -- find the endpoint that reports the outcome (`GET /api/ota/check`, not `POST`); (b) when a value is cached by a task, verify the **behaviour** it controls, not the field that reports it (measure the relay pulse, do not read `/api/config`); (c) when adding any new confirmation to a UI, ask what would have to be true for it to lie, and make the check assert that instead.

- **[PATTERN] The git index is a single shared, easily-misread resource — verify it, never narrate it.** Three incidents (2026-07-13 branch switch, 2026-07-20 `commit -a` sweep, 2026-07-23 false "staged" report): each time the index's real state diverged from what was said or assumed about it. Rules: (1) after staging, show `git status --short` / `git diff --cached --stat` and report THAT, never a claim from memory; (2) staging one stream protects nothing if the commit is `-a` — if anything tracked-modified is pending, either stage it all with a covering message or say explicitly what must not be committed; (3) an untracked file that a staged change links to must be called out by name, not left among the `??` noise; (4) before any branch switch, empty the index.

- **[PATTERN] One log chain per time window — never let two chains of the same unit's logs coexist in an analysis folder.** The dedup in `plot_daily.py` is tuple-exact and does **not** catch it; the symptom is a day showing ~2× the expected sample count (~5700 vs ~2860). Recurred twice: 2026-06-26 (two independent SD downloads with different rotation boundaries) and 2026-07-20 (files pulled off the card by hand vs. the unit's own later upload of the same files). Rule: **before plotting, run `check_dupes.py`; archive the superseded chain to `archived_overlap/`** rather than deleting it, and sanity-check sample counts in `plot_summary.txt` afterwards. Free verification: if `git status` shows the archived copy and the new file as a rename (`R old -> new`), they are byte-identical.

- **[PATTERN] Python CLI output on this machine must be ASCII-only.** The Windows console is cp1252; any `print()` (or the harness capturing stdout) crashes with `UnicodeEncodeError` on `→ … ✓ ✗ °` etc. Recurred 5+ times (calibration scripts, `check_dupes.py`, `rota_sim.py`, ad-hoc probes). Rule: write `->`, `...`, `deg`, `OK`/`FAIL` — never Unicode glyphs — in Python that prints. If Unicode is unavoidable, set `PYTHONIOENCODING=utf-8` on the invocation. Do NOT trust a crude keyword scan of captured serial/HTTP output either — loose substrings (`corrupted`, `format`) throw false positives; match on the specific message.

- **[PATTERN] A cross-reference is a claim about a file, not evidence — open the target and confirm it says what the citation says.** A link check proves only that a path resolves; nearly all the damage lives in the half it cannot test, whether the target actually contains the claim. **2026-09-07:** this log's own ff-only-merge entry said *"Recovery sequence documented in `BRANCH_NOTES.md`"* — that file never contained one; the steps were sitting in the entry itself. The false pointer survived because nobody opened the target, and it surfaced only because the file was read before being deleted. **Earlier, 2026-07:** an `audit-context` pass flagged bare filename citations that resolved ambiguously; fixed inline and — the second half of the lesson — **never logged**, which is why this pattern rests on one recorded incident plus one recalled. Rules: (1) before repeating or acting on a cross-reference, open it and confirm the specific claim is present; (2) cite a path **plus** a section or line anchor, never a bare filename — `foo.cpp:352` can be checked, `foo.cpp` cannot; (3) when deleting a file, grep for inbound references **and read them** — one may be wrong in a way that changes whether anything is actually lost; (4) when a citation turns out to be false, fix the *citation*, do not re-point it at another unread file.

---

## Index — by where it bites you

53 entries is too many to scan. Find your subsystem, then **Ctrl+F the date** to jump.
Hooks are the *symptom*, not the title — you rarely know the cause when you arrive here.
Entries stay in reverse-chronological order below; this index is the only grouped view.

### Windows, climate & manual control (T2, T6, T8)
- **2026-09-10** — windows sit where the admin left them, mode says AUTOMATIC, T6 does nothing for
  up to 25 min (dwell debt from a manual move; T6 is fine, T2 is refusing it) **[RECURRENCE of a
  May-2026 issue whose fix was recorded only in a code comment]**
- **2026-07-31** — anti-thrash dwell was unguarded during travel (gh#48)

### Modbus bus, sensors & clock (T5, drivers)
- **2026-09-07** — a task polling flat out panics the board after 5 s (driver never yields; TWDT idle check)
- **2026-09-07** — every Modbus read times out early in boot, but T5 works fine later (bus dies during RTC/LittleFS/SD init)
- **2026-09-07** — `pio run` on a driver env fails with `UART_SCLK_DEFAULT was not declared` [RESOLVED]
- **2026-09-05** — the header promises a UART mutex the source never creates (gh#49)
- **2026-08-26** — a ~59 s T/RH sensor fault that clears itself, roughly monthly; plus one 100-min wind fault that is *not* a defect
- **2026-07-28** — a hardware test "passes" but the emulator was still fed live data
- **2026-07-13** — wind readings before 2026-06-19 12:00 are meaningless (vane not commissioned)
- **2026-07-08** — clock hours wrong while `ntp_synced=true`; also: `time_iso` is a 0–60 s stale shadow **by design** [RESOLVED]

### Climate control & relays (T2, T6)
- **2026-07-31** — a window reverses mid-stroke; and how to tell a defect from a legitimate wind override (gh#48) [RESOLVED]
- **2026-07-28** — replaying T6 from SD logs gives plausible-but-wrong numbers (five traps; MODE is logged only on *change*)
- **2026-07-05** — M3 is the north **side wall**, not a roof panel; 8.1× is travel time, 10× is area

### OTA & ROTA releases
- **2026-09-07** — `rota_release --dry-run` writes the seq-ledger manifest despite claiming no changes
- **2026-09-07** — the ROTA night window and check interval are on `/api/ota/config`, not `/api/config`; a wide window inverts gh#41 so a stray browser tab blocks updates
- **2026-09-07** — a few short USB bench sessions silently arm an OTA rollback (4 boots under 30 s)
- **2026-07-23** — `rota_release release` looks like it hung; it aborted on an interactive prompt under null stdin
- **2026-07-20** — an SD log cannot tell you which firmware wrote it; post-OTA proof needs `/api/status`
- **2026-07-14** — the quiet-gate/session-exemption test does nothing unless the night window is open
- **2026-07-13** — ROTA server (VPS) registry and permission traps
- **2026-07-13** — T16 crash-loops on the first live pull-install (8 KB stack, nested TLS) [RESOLVED]
- **2026-07-13** — rapid OTA reboots rate-limit SNTP, so T16 skips its checks
- **2026-06-20** — `ota_push.py` exits 1 at step [8] even though the OTA worked (PowerShell `2>&1`)
- **2026-06-10** — a firmware-only push silently strands the asset partition (paired-commit invariant)
- **2026-04-XX** — `{{ASSET_VERSION}}` shipped to a unit as a literal string (gh#9) [RESOLVED]

### Flash, partitions & boot
- **2026-09-07** — `mklittlefs` builds a valid image of an EMPTY directory and every downstream check passes
- **2026-07-13** — greenfield cable-flash web assets need `mklittlefs`; `pio buildfs` emits SPIFFS
- **2026-05-14** — `ets_loader.c` crash loop after a full flash (qio vs dio header byte) [RESOLVED]
- **2026-05-XX** — panic on every boot of a brand-new unit (coredump partition garbage)
- **2026-XX-XX** — OTA flips the firmware version but assets stay old (shared LittleFS basePath) [RESOLVED]

### SD logging & the log parser
- **2026-08-25** — an SD log's FILENAME is its upload time, not its coverage window (silently parses the wrong period)
- **2026-07-23** — a wind override at `speed == v_max` is mislabelled a "direction" event (gh#45) [RESOLVED, but pre-2.3.0 logs still misparse]
- **2026-07-17** — log uploads stop dead once the card holds >~21 files (gh#42) [RESOLVED]
- **2026-07-04** — `storage_sd_list_csv()` truncates silently on a full buffer (gh#36) [RESOLVED]
- **2026-06-10** — `HEAD /api/log/download` always reports 45 B — **never** use it to check for gaps

### Model, campaign & plotting
- **2026-08-16** — `plot_daily.py` exits 143 under a 2-minute timeout but has already succeeded
- **2026-07-05** — a matplotlib upgrade re-renders every campaign PNG with byte diffs
- **2026-06-26** — a day shows ~2× the expected samples (two overlapping SD download chains)

### Build, toolchain & shell
- **2026-09-07** — you fix a file, rebuild, and get the identical error (`lib_deps = file://../x` compiles a stale copy)
- **2026-07-14** — enlarging one buffer breaks a `-Werror=format-truncation` in a *different* function
- **2026-07-05** — system Python 3.11 loses its site-packages mid-project
- **2026-05-XX** — `pio: command not found` in Git Bash
- **2026-05-XX** — PowerShell treats `pio` stderr warnings as fatal (`$ErrorActionPreference='Stop'`) [RESOLVED]

### Git, GitHub & scripted editing
- **2026-09-07** — you swap boards and the COM port is identical, so you flash the wrong one (CH340 has no serial)
- **2026-09-07** — a string-replace anchored on the first occurrence lands in a comment and breaks the build
- **2026-09-05** — `gh_issue.py` 401s on every call: the fine-grained PAT expired (not a script bug)
- **2026-09-05** — a large inline heredoc dies at parse time before running
- **2026-07-23** — `test/` is gitignored yet 15 files inside it are tracked (deliberate)
- **2026-07-13** — a branch switch carried the whole staging area into the wrong commit *(recurred 2026-07-20)*
- **2026-06-09** — branch protection on `main` rejects merge commits

### Server side (VPS)
- **2026-07-14** — logrotate: validate as root; group-writable `/var/log` needs `su`


## 2026-09-12 — a regex over source is not evidence of coverage: three false findings in one audit

**Problem**: During the duplicated-state audit I reported three defects that do not exist.
(1) *"`persist_ch_state()` has no callers"* — it has **seven** (`relay_controller.cpp` 443/512/544/556/635/678/717); I had grepped `persist_state`.
(2) *"a change to `avg_win_wind` never resets the wind averages"* — it does, at `sensor_poll.cpp:436`, resetting both the speed and direction contexts; I had grepped for `s_win_ws_last` / `s_win_wd_last` when the variable is `s_win_w_last`.
(3) *"`logparser.py` does not decode `LOG_SYSTEM` subtypes 16–21"* — it decodes all six correctly, including 21 = RTC divergence; my `va == N` regex missed the branch form those use.
Two of the three were one sentence away from being filed as issues.

**Root cause**: In each case I inferred behaviour from a pattern match over source text and stopped. **A regex tests my guess at the spelling, not the property I care about** — and a *negative* result is indistinguishable from a typo in the pattern.

**Fix**: Execute the thing. Thirteen synthetic CSV rows through `logparser.py` settled all four multi-emitter event types in one call — which is the same method that found the real gh#51 Group B gap on 2026-09-10, after inspection had missed it. For a call graph, grep the exact symbol, print the hit list, and check the count against expectation.

## 2026-09-11 — a fix to a mode transition tested from the wrong starting state passes vacuously (gh#52)

**Problem**: The first hardware test of gh#52 option (c) — "`dm_reload_all_cfg()` restores the operating mode" — was run from AUTOMATIC. It passed. It would have passed on the unfixed code too.
**Root cause**: `dm_set_standby_ex()` early-returns when the bit already matches. From AUTOMATIC, both the old omission and the new restore do exactly nothing, so the test could not distinguish them. gh#52 itself had warned: *"a test would have had to put the unit in STANDBY first, and the obvious test does not."* I wrote the release-notes step ("confirm the calibration runs once") without noticing that from AUTOMATIC there is no calibration to count.
**Fix**: Re-ran from STANDBY (LCD manual mode, then IO0 stage 2): exactly one MODE-leave row (initiator Admin/1 = `session_close()`), one CLOSE_ALL sweep, no second row from the reload — the idempotence guard proven. **Rule: a fix to a transition is tested from the state the transition leaves.** Promoted into the "show the check can fail" pattern.

## 2026-09-11 — the release paperwork rode inside the next feature commit, so no commit on `main` *was* 2.4.5

**Problem**: To split the window-sensor work off `main`, the plan was "rewind `main` to the 2.4.5 release commit". There was no such commit: `0caff3f` (the 2.4.5 *code* fix) still declared `2.4.4`, and the version bump, the `## [2.4.5]` changelog section and `bin/2.4.5/release-notes.md` had all been staged with the first sensor commit (`9260613`). `git reset --hard 0caff3f` also deleted the tracked release notes from disk.
**Root cause**: The release-cycle steps were done, but staged together with unrelated work instead of as their own commit. A "release: X.Y.Z" commit must contain exactly the bump, the changelog section and the notes — otherwise the release is not addressable.
**Fix**: Recovered the paperwork with `git show 9260613:<path>` and carried it across by hand; the rewind needed a ruleset bypass (`--force-with-lease`). **Two traps recorded in `CLAUDE.md`:** a *revert* or a hand-written deletion commit on `main` would later make `merge`/`rebase` strip the sensor files out of `ropeSensor` (git reads a deletion on one side of the merge base as intentional) — rewinding was the only clean split; and integration must be `rebase ropeSensor onto main`, never `merge main into ropeSensor`.

## 2026-09-11 — a 1970 BOOT row beside 2026 rows means "DS1307 seed failed, system clock survived the soft reset" (gh#55)

**Problem**: After the 2.4.7 OTA the web clock showed `—`, sun times 00:00, T6 on night thresholds, for 11+ minutes; the SD log and the sensor history carried correct timestamps throughout. Looked like a 2.4.7 regression (2.4.7 had touched nothing in the clock path).
**Root cause**: Boot and unit-ID rows are stamped from the *shadow* `current_unix_ts` (`data_manager.cpp:1382`); every other row from `time(NULL)`. `read_rtc_and_seed_clock()` returns on an RTC read failure *before* the shadow update, and the failure is `ESP_LOGW` only. ESP-IDF keeps `time(NULL)` across a **soft** reset (`CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y`), so the mismatch — one 1970 BOOT row, 2026 rows around it — is the whole diagnosis. The DS1307 read failed at the second (assets) OTA reboot for the first time in 27 boots and kept failing through a power cycle; the driver returns `RTC_ERR_INVALID` on garbage fields, which persists on a battery-backed chip until a full write. The LCD's `--/--/----` keys on the shadow timestamp, not on the chip.
**Fix**: Operator set the time from the LCD (`dm_set_manual_time()` writes the DS1307, clears CH); the next 60-s poll read it fine. Hardware vs reboot-time register corruption undetermined — gh#55 (also: manual set does not recompute sun times; no MX1 guard around `esp_restart()`).

## 2026-09-11 — read your own interference out of a soak before reading the soak

**Problem**: Three false signals in one AT-WP05 evaluation: a "28× rise in wind faults" (10 of the 13 fault rows in 99 h fell inside my own 1.67 h of bench-route polling at 0.4–1.2 s — a third bus caller); a "−1092 KB PSRAM leak" (the `/api/log/download` handler PSRAM-allocs per file — my three pulls); and a pre-T17 baseline of 1 event that the soak-start capture had recorded as 5 (log rotation made the "before" window empty and the denominator wrong).
**Root cause**: Measuring while polling the thing being measured, and computing rates across a file boundary without checking the window was populated.
**Fix**: Segment by *my* activity windows first; check `n` in every window before dividing; re-read memory after the downloads have been freed. And the headline counter, `err_busy`, **structurally cannot fail with two callers** (500 ms lock timeout vs ~215 ms worst-case hold) — recorded as a qualified pass, not a pass. The test with real power was T5's own cadence (30.32 s at rest vs 30.35 s in-stroke, max 32 s).

## 2026-09-11 — `vTaskDelay(period)` after a blocking transaction gives period + transaction, not period

**Problem**: T17's derived poll was 100 ms; the SD log showed 162–173 ms across all 31 stroke windows (`ropeSensor` branch).
**Root cause**: The loop ends in `vTaskDelay(poll_ms)` — a *relative* delay after a ~68 ms Modbus read — so the period is the sum. FR-WP04's 1 % overshoot budget, which the `/150` divisor was chosen to protect, is exceeded (1.68 %) on the rig; production (171 s travel) stays inside it at 0.71 %.
**Fix**: `vTaskDelayUntil()` for a fixed period (Phase 4). General: any "poll every N ms" loop that does I/O before the delay needs `vTaskDelayUntil`, or the measured period is wrong by the I/O time.

## 2026-09-11 — three tooling traps that each cost a turn

**Problem**: (1) `strings <bin> | grep 2.4.6` returned nothing on this MINGW — the binary *did* contain the version ×5; (2) a bash heredoc holding C string literals with `\"` escapes failed to parse (`unexpected EOF while looking for matching '`); (3) a urllib verification harness reported five FAILs for correct 400 responses because `HTTPError` bodies were never JSON-parsed, so `ok` read as `None`.
**Root cause**: (1) `strings` is not functional here; (2) shell quoting of escaped quotes inside a quoted heredoc; (3) error-path body handling omitted.
**Fix**: (1) `python -c "open(f,'rb').read().count(b'2.4.6')"`; (2) write the script with the Write tool, run it with Bash; (3) parse `e.read()` in the `HTTPError` branch — then the 400s showed `{"ok":false,"err":"unknown ns/key"}` as designed. Rule from the 2026-09-10 entry still stands: syntax-check the generator before running it.

## 2026-09-10 — a refused manual LCD command leaves NO trace, so "it got rejected" is unreconstructable

**Problem:** Operator drove M3 from the LCD, saw a refusal, and later could not say which one. There
was no way to find out from the logs.

**Root cause:** `ui_display.cpp:2109` has three gates, each with its own 1500 ms `show_msg()` and
then a bare `return`:

| gate | message | blocks |
|---|---|---|
| `EG1_BIT_MOTOR_ALARM` | `MOTOR ALARM / cmd refused` | everything |
| `EG1_BIT_CALIBRATING` | `Calibrating / wait + retry` | everything |
| `EG1_BIT_WIND_OVERRIDE` | `WIND OVERRIDE / OPEN refused` | **OPEN only**, CLOSE always allowed |

**None of them writes a log row.** The command never reaches Q1, so T2 logs nothing either. The only
record is 1.5 s of LCD text. Same shape as the dwell deferral fixed in 2.4.5 — a refusal that is
correct behaviour but invisible afterwards.

**Fix (partial, 2026-09-10):** none applied — operator deferred it. The **workaround** is the
window-state bitmask in `SENSOR_HR ch=2`, which carries bit 12 WIND_OVERRIDE, bit 13 MOTOR_ALARM and
bit 14 CALIBRATING at the 30 s sample cadence. That reconstructs *which gate was active* to within
30 s, which was enough to narrow this case to two candidates but not to one.

**Rules:**
- **When diagnosing "the controller refused my command", go to `SENSOR_HR ch=2` first** and decode
  bits 12/13/14 around the timestamp. Do not assume dwell: `SRC_OPERATOR_MANUAL` bypasses the dwell
  timer and the gh#48 in-travel guard defers `SRC_T6` only, so a manual refusal is **always** one of
  the three LCD gates, never T2.
- **Wind override blocks OPEN and permits CLOSE.** "It refused my open but the close worked" is that
  gate, not a fault.
- If a gate refuses an operator's deliberate action, it should leave an audit row. Three do not.

## 2026-09-10 — after LCD manual window control, T6 resumes in AUTOMATIC and is silently refused for up to 25 minutes (RECURRENCE) [RESOLVED 2.4.5 — hardware-verified 2026-09-10 and again 2026-09-11]

**Problem:** Admin drives windows by hand from the LCD, logs out. Mode reads AUTOMATIC, `/api/status` shows the
real window states, T6 is running — and nothing happens. The controller looks correct and does nothing, with no
trace in the SD log, on the web GUI, or in the serial log at default level.

**Root cause — two halves, and only the second is the defect.**

1. T6 is **not** confused. `reconcile_to_step()` is level-triggered: it runs **every cycle unconditionally**
   (`climate_control.cpp:589`), reads **actual** window states via `t2_get_window_states()`, and posts
   per-channel `CMD_OPEN`/`CMD_CLOSE` for whatever disagrees. It does not care how the windows got there.
   **Do not go looking for a stale step variable — there isn't one.**
2. T2 **refuses** those commands. `ch_start_open`/`ch_start_close` (`relay_controller.cpp:483`) defer
   `SRC_T6` while a dwell deadline is pending. The asymmetry is the bug: `SRC_OPERATOR_MANUAL` **bypasses**
   the dwell going in, but completing any move **sets** the dwell deadline in `ch_update()`, which has **no
   source parameter** — so a hand-set position leaves T6 holding an anti-thrash debt it never incurred.

On FDA4 `dwell_open_m3` is **1500 s**: manually open M3, log out, and T6 retries every 30 s and is refused
for **25 minutes**. `dwell_close_m3` is 600 s, so the reverse costs 10.

**Why it was invisible:** the deferral logged at `ESP_LOGD` — below the default level, serial-only, never in
the SD log. Four deferral sites had this (`:395`, `:421`, `:467`, `:486`).

**Why it RECURRED — this is the part worth remembering.** The operator hit this class of problem in May 2026
(*"during manual operation climate control kicked in and took over"*, 2026-05-26) and rc.1.5.2 fixed it two
ways at once: it moved the STANDBY clear to session-end (the respect window — which is what actually fixed
the complaint) **and** suppressed the CLOSE_ALL via `dm_set_standby_ex(..., recalibrate_on_clear=false)`.
The second half went further than the complaint required. Its rationale was written up carefully — in a
**code comment inside `session_close()`** — but **nothing went into this log**, so nobody searching for
"T6 out of sync after manual control" could find it. The comment recorded *why the suppression was chosen*.
It never recorded *what would now not happen*. Four months later the consequence was rediscovered from
scratch.

**Fix (2026-09-10):**
- `session_close()` now posts **`T2_NOTIFY_CLEAR_DWELL`** (new bit, `relay_controller.h`) so T2 drops the
  dwell debt in its own context — T2 stays the only writer of `s_ch[]`, no cross-task race.
- `recalibrate_on_clear` flipped back to **true**: session-end returns the windows to a known CLOSED baseline
  and T6 resumes from that. The respect window still holds positions for the whole session, so the 2026-05-26
  complaint stays fixed — what changed is that positions no longer survive *past* the session.
- The two dwell deferrals are now **`ESP_LOGI`, latched to one line per episode** via `dwell_defer_logged`
  (reset wherever a fresh dwell is set), so it is visible without being spam.

**Rules:**
- **If a command source bypasses a timer, decide explicitly whether it should also clear the debt that timer
  leaves behind.** Bypassing on the way in and not on the way out is the trap; it looks correct at both sites.
- **If a fix SUPPRESSES a mechanism, log what will now not happen, not just why you suppressed it.** A
  rationale in a code comment is findable only by someone already reading that function — which is nobody,
  because the symptom appears in a different task.
- **Never diagnose "T6 is out of sync" as a T6 state-model problem.** It is level-triggered and state-based.
  Look at what is refusing its commands: dwell (`:483`), the gh#48 in-travel guard (`:395`/`:467`), the
  MOTOR_ALARM discard (`:821`), or an EG1 inhibit bit.

## 2026-09-10 — Two concurrent `pio run` invocations on the same env report a spurious FAILED

**Problem:** Verifying the gh#51 Group A edits, `pio run -e lolin_s3 -e lolin_s3_mbprobe -e lolin_s3_bench` reported `lolin_s3_mbprobe  FAILED` while the other two succeeded. Re-running that env alone: `SUCCESS` in 3.7 s. Nothing in the source had changed between the two runs.

**Root cause:** self-inflicted. A background `pio run -e lolin_s3_mbprobe -e lolin_s3_bench` was still running when the foreground build of the overlapping envs started. Both processes write `.pio/build/<env>/`, so they clobber each other's objects and link artefacts. The reported failure belongs to the collision, not the code.

**Fix:** never run two `pio run` invocations over overlapping envs at once — background one only if the foreground work touches different envs, or just wait. **Diagnostic rule:** a build failure with no matching compiler diagnostic in the output is a build-system artefact, not a code defect — re-run the single env in isolation before touching source. This is the second artefact of this shape this session (the first was a stale `lolin_s3_mbprobe` binary that rebuilt SUCCESS), so the reflex is worth having: **confirm a failure reproduces in isolation before believing it.**

## 2026-09-10 — a before/after measurement that lands on the SAME number for opposite reasons is not evidence

**Problem:** Verifying gh#51 Group A on FDA4. Fail-first run (buggy 2.4.1, travel_m1 set to 40, T2 cached 21) measured **24.0 s**. Post-fix run (2.4.2, travel_m1 set to 21, T2 booted with 40) measured **23.6 s**. Both "passed" their intended reading, and the two numbers are indistinguishable. The only thing separating a reproduced defect from a confirmed fix was a chain of reasoning about what NVS happened to hold at boot -- unreconstructable from the logs a week later.

**Root cause:** The two runs were designed as "old value vs new value", but the old value in run 1 and the new value in run 2 were **the same number (21)**. The measurement could not discriminate; only the surrounding argument could.

**Fix:** Added a third run at a value matching neither side -- travel_m1 = 60, measured **63.3 s**, which is neither the cached 40 (45 s) nor the old 21 (26 s). It also carried its own control: M2 (unchanged, 21) held 23.9 s and M3 (unchanged, 13) held 15.7 s in the same sweep, so only the written channel moved. **Rule: before running a before/after test, check that the two expected results differ. If either side's expected value appears anywhere in the other side's setup, pick a third value that appears in neither.** Prefer a delta large enough to be unmistakable -- the Group C confirmation used travel_m3 13 -> 171, a 158-second change in pulse length, which needs no argument at all.

## 2026-09-10 — web assets are NEVER parsed by the firmware build: a broken `app.js` ships silently

**Problem:** gh#51 Group D edited `firmware/data/app.js`. `pio run` reported SUCCESS for every env, and `build_release.ps1` packaged the file happily. Neither one parses JavaScript. A syntax error would have shipped, and the failure mode is quiet: the Motors tab renders with six blank fields while every other tab looks normal.

**Root cause:** `firmware/data/` is packed into a LittleFS image byte-for-byte. There is no lint, no parse, no test in the release path. The C toolchain's warnings-as-errors discipline creates a false sense that "it built" means "it is valid".

**Fix:** `node` is not installed on this machine, so pre-flash checking is structural only (brace/paren/bracket balance, and reading the edited block). **The real check is post-flash: load the served page and confirm the functions exist** -- `javascript_tool` with `['setVal','postCfg','renderIdentity'].map(n => typeof window[n])` returned three `function`s, and the console showed only the expected 401 from the admin-gated config fetch. Do that for any asset change; "the firmware built" proves nothing about `app.js`, `index.html` or `style.css`.

## 2026-09-10 — `/api/config` field names are NOT the NVS keys, and an unknown key is written to NVS with `ok:true` [RESOLVED 2.4.6 (gh#53), corrected 2.4.7 — the fix over-rejected NVS-only `wifi/ap_enable`; see the producer-enumeration pattern]

**Problem:** Restoring FDA4 after a factory reset, `POST /api/config {"ns":"system","key":"poll_interval_s","value":45}` returned `200 {"ok":true}` and changed nothing. The NVS key is `poll_interval`; `poll_interval_s` is the field name `/api/config` **returns**. So reading the API and writing its own output back is exactly the mistake that triggers it.

**Root cause:** Two things compound. The 200 only means "queued to Q4" -- the handler returns before T4 applies anything. And `apply_config_update()` calls `nvs_cfg_set_i32()` **before** it knows whether the key is recognised, so an unknown key is persisted as a junk NVS entry, skips `cfg_clamp()`, updates no shadow field, emits no audit row, and logs nothing (the INFO line sits inside `if (updated)`).

**Fix:** Filed as gh#53 with a suggested `cfg_key_is_known()` predicate + 400 response. Until then: **never assume a JSON field name is the NVS key.** The authoritative list is the `K_*[]` string constants at the top of `data_manager.cpp`; the GUI's `postCfg('ns','key',...)` calls in `index.html` are a second reliable source. Known mismatch today: `poll_interval` (NVS) vs `poll_interval_s` (JSON).

## 2026-09-10 — `POST /api/ota/check` only QUEUES; the result comes from `GET` on the same path

**Problem:** Verifying the restored ROTA secret. `POST /api/ota/check` returned `{"ok":true,"queued":true}` and `/api/ota/status` then read `{"state":"idle","error":""}`. That looked like a clean pass but proves nothing -- an idle state with no error is also what you see if the check never ran.

**Root cause:** Two handlers share the URI: `rota_check_post_handler` queues a check and returns immediately (`web_server.cpp:2645`), `rota_check_get_handler` returns the last result. `/api/ota/status` reports the *download/apply* state machine, not the manifest check.

**Fix:** `GET /api/ota/check` gives the real answer: `{"id":"30eda0a0fda4","result":"up_to_date","result_code":0,"http":200,"checks":3,"offered":"2.4.1","running":"2.4.4"}`. **`http: 200` is what proves the per-unit HMAC matches** -- a wrong secret returns 401/403, so `secret_set: true` on `/api/ota/config` only means "a secret is stored", not "the right one". The same call also exposes channel lag: `offered` vs `running`.

## 2026-09-10 — the LCD factory reset (IO0 stage 2) destroys three secrets that cannot be read back

> **Recurred 2026-09-11, twice** (gh#52 verification). It also wipes `travel_m3` back to the production 171 s — on the 13 s rig the next M3 stroke would drive 158 s into the end stop — and zeroes the lat/lon fractions (sunrise/sunset drift). Re-set `travel_m3 = 13` before any M3 movement; capture `/api/config`, `/api/ota/config`, `/api/web` first. The WiFi *station* reconnects after a reboot regardless: ESP-IDF keeps the STA config in its own NVS entries that the app-level `wifi` erase does not touch.

**Problem:** Verifying gh#51 Group C required a real IO0 stage-2 reset on FDA4. It erases seven namespaces. Three of the erased values are **not readable from the device at any endpoint**: the WiFi PSK, the ROTA per-unit HMAC secret (`/api/ota/config` shows only `secret_set: bool`), and the status-post shared secret (no field at all in `/api/web`).

**Root cause:** Write-only-by-design, correctly -- the SD card and the API must not become a credential exfil surface. The consequence is that a reset is only reversible if you hold those values elsewhere.

**Fix:** **Capture `/api/config`, `/api/ota/config` and `/api/web` before the press** -- everything else (about 30 settings incl. ROTA url/window/enable and the status schedule) restores from that in seconds by script. The three secrets come from the operator's secret store and must be re-entered by hand. Also note the exposure window: WiFi credentials are gone but the *connection* survives until reboot, so a power blip between the reset and the re-provision strands the unit off the LAN with ROTA unable to recover it. Restore WiFi first. FDA4 also reverts `travel_m3` 13 -> 171, which is not a fault -- 13 is FDA4's mock value, 171 is the production default.

## 2026-09-10 — when a Python script generates C/JS source, syntax-check the GENERATOR before running it

**Problem:** Two separate bugs in edit-generator scripts this session. In `groupA.py`, a log line was written inside a single-quoted Python string, so `" + DASH + "` would have been emitted **literally** into the C source. In `groupC.py`, `" + DASH " an actuation"` was missing a `+` and died at import with a SyntaxError.

**Root cause:** Building source text by concatenating string literals with a unicode `DASH` variable puts two languages' quoting rules in the same line. The second failed loudly; the first would not have -- it would have produced a file that compiles nowhere near the mistake, or worse, silently in a comment.

**Fix:** `python -c "import ast,io; ast.parse(io.open(f,encoding='utf-8').read())"` on the generator before running it catches the loud class. For the quiet class, **grep the generated region afterwards** -- and prefer building long comment blocks as a list of lines joined with `"\n".join([...])` over `+`-concatenation, which is where both bugs lived.

## 2026-09-07 — swapping boards keeps the SAME COM port *and* the same DeviceID, so the port cannot identify a unit

**Problem:** with FDA4 unplugged and a different board attached to the same USB socket, Windows reported an identical port — `USB-SERIAL CH340 (COM10)`, DeviceID `USB\VID_1A86&PID_7523\5&2C6AC496&0&1`. Byte-for-byte what FDA4 had shown minutes earlier. Trusting it would have flashed hardware-test firmware onto the development controller.

**Root cause:** the CH340 has **no USB serial number**, so Windows cannot distinguish two of them. It enumerates by *socket*: the same physical port yields the same COM number and the same instance path regardless of which board is in it. The DeviceID looks unique and specific — it identifies the socket, not the device.

**Fix — identify by MAC, always, before any write:**

```bash
python ~/.platformio/packages/tool-esptoolpy/esptool.py --port COM10 --no-stub read_mac
```

The ESP32's MAC is burned into eFuse and is the only reliable identity. The unit ID is its last two bytes, so the mapping is direct: `30:ed:a0:a0:fd:a4` = FDA4, `64:e8:33:7c:12:f0` = 12F0, `64:e8:33:7c:23:44` = 2344. Full table in user-global `project_unit_inventory.md`.

**Also:** when only one board needs to be written, unplug the others. `pio test` and `pio run -t upload` auto-detect ports, and a `--upload-port` flag is a defence you have to remember every time; an empty socket is one you cannot forget.

**Where it lives:** any bench session with more than one board — the dev controller and the loopback board are both LOLIN S3 with CH340.

---

## 2026-09-07 — `lib_deps = file://../x` compiles a stale COPY; editing the source changes nothing

**Problem:** after fixing `modbus_rtu.cpp`, three driver envs built cleanly and `FG6485A -e lolin_s3` still failed — with the *original* error, on a file I had just corrected.

**Root cause:** the path in the error was the giveaway: `.pio/libdeps/lolin_s3/modbus_rtu/src/modbus_rtu.cpp`. PlatformIO resolves `lib_deps = file://../modBus` by **copying** the library into the consuming project's `.pio/libdeps/`, and does not reliably re-copy when the source changes. The build was compiling a snapshot taken before the edit — confirmed by grepping the cached copy for the new code and finding none.

**Fix:** `rm -rf <project>/.pio/libdeps` and rebuild. **Read the path in the error message**: a `.pio/libdeps/` prefix means you are not looking at the file you edited.

**Where it hides:** the driver projects that consume `modBus` this way — `drivers/FG6485A`, `drivers/s200`. The firmware does *not* have this problem: it uses component proxies (`firmware/components/*/CMakeLists.txt`) that reference `drivers/*/src/*.cpp` by path, so it always compiles the real file.

---

## 2026-09-07 — the Modbus driver never yields, so any high-rate poller trips the task watchdog

**Problem:** two tasks issuing back-to-back Modbus transactions on core 1 would have panicked the board within 5 s instead of producing a result. Caught by reading `sdkconfig.lolin_s3` **before** flashing, not by a crash.

**Root cause — two facts that only bite together:**

1. `modbus_rtu.cpp`'s receive loop spins on `uart1_available()` with **no `vTaskDelay`**, for up to `MODBUS_TIMEOUT_MS` (200 ms) per transaction. The DE guard and settle delays are `esp_rom_delay_us()` busy-waits too. A transaction is ~25-30 ms of pure spin even when it succeeds.
2. `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y` **and** `..._CPU1=y`, with `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`.

So a task above idle priority that runs transactions continuously starves that core's idle task, and the TWDT panics in 5 s. **T5 is safe only because it polls every 30 s and sleeps in between** — the driver has never been asked to run flat out.

**Fix:** any task issuing back-to-back transactions must `vTaskDelay(pdMS_TO_TICKS(1))` between them. One tick per ~25 ms transaction is ample for the idle task and costs no throughput. See `firmware/src/diag/modbus_probe.cpp`.

**Why this matters beyond the probe:** [`design/refactorSensorConfiguration.md`](../design/refactorSensorConfiguration.md) §2.2 proposes **1 Hz position polling** during a window stroke. At 1 Hz with a 30 ms transaction the duty is ~3 % and the idle task is fine — but a **tight retry loop, a burst read, or a `series()` request with a short interval** would hit this. Anyone building the JIT bus task should either add the yield in the scheduler or fix the driver's receive loop to block on `uart_read_bytes` with a timeout instead of spinning. The second is the better fix and has never been attempted.

**Where it lives:** `drivers/modBus/src/modbus_rtu.cpp` (receive loop ~`:347`, busy-wait delays); `firmware/sdkconfig.lolin_s3` (TWDT settings); `firmware/src/diag/modbus_probe.cpp` (the yield, with rationale).

---

## 2026-09-07 — a string-replace anchored on the first occurrence lands in a comment

**Problem:** inserting a source file into `firmware/src/CMakeLists.txt`'s `SRCS` list broke the build with `Parse error. Expected a command name, got quoted argument`. The new entry had been written into a **comment block** 30 lines above the real list.

**Root cause:** the edit script anchored on `s.index('"status_post/status_post.cpp"')`. That filename appears **first** in a phase-progression comment (`# Phase 4 (alpha.4): "status_post/status_post.cpp"`) and only later in the actual `idf_component_register(SRCS ...)` call. `index()` returns the first match, which was prose.

**Fix:** anchor after a **structural** marker, not on a bare identifier:

```python
reg = s.index("idf_component_register(")   # structural anchor first
i   = s.index('"status_post/status_post.cpp"', reg)   # then search from there
```

**Where this bites in this repo:** files that document their own history inline — `firmware/src/CMakeLists.txt` (phase-progression comments listing files not yet added), `firmware/platformio.ini` (a commented-out `[env:test_t2_relay]`), `changelog.md` (every filename ever). In all of them the *first* occurrence of a name is usually narrative, not the live entry. **Verify after every scripted edit**: build it, or at minimum print the surrounding lines — this one was only caught because CMake refused to parse.

---

## 2026-09-07 — the Modbus bus does not survive boot: `main.cpp`'s `modbus_init()` is dead by the time T5 starts

**Problem:** a diagnostic task placed early in `app_main` (right after T5 is spawned, t ≈ 1.8 s) got **1000/1000 Modbus timeouts** — not one response from either sensor. The same firmware, same boot, moments later: T5 polled both sensors perfectly (`T=29 °C RH=60 % ws=2.3 m/s`). The sensors were fine; the bus was not.

**Root cause:** `main.cpp:714` calls `modbus_init()` at t ≈ 1.4 s. Boot then brings up the **RTC (I²C)**, **LittleFS**, and the **SD card over SPI** — including a `storage_sd_unmount()` — and the Modbus UART/DE-RE state does not survive that sequence. T5 works only because it calls `modbus_init()` **again** at its own task entry (`sensor_poll.cpp:384`, t ≈ 9.5 s).

This is what the comment at `main.cpp:1025` means by *"T5's own modbus_init **reconfirms** the driver state at task entry"* — the word is doing real work. Somebody hit this before and wrote a comment instead of an entry.

**Fix:** anything that touches the Modbus bus outside T5 must call `modbus_init()` itself first, not rely on the boot-time init. It is idempotent (it deletes and reinstalls the UART driver), so this is safe **provided nothing else is mid-transaction** — see the use-after-delete note in `modbus_rtu.h`.

**Do not "fix" this by moving `main.cpp`'s init later** without establishing which of RTC / LittleFS / SD actually breaks it. The exact culprit is **not yet identified** — only the window is. A blind reorder would move the dead zone rather than remove it.

**Where it lives:** `firmware/src/main.cpp:714` (early init) and `:1025` (the comment that hints at it); `firmware/src/sensor_poll/sensor_poll.cpp:384` (T5's re-init); `firmware/src/diag/modbus_probe.cpp` (calls `modbus_init()` for exactly this reason). Full write-up: `design/addModbusMutex.md` §4.3c.

---

## 2026-09-07 — the modBus hardware test suite has been unbuildable since the ESP-IDF migration [RESOLVED same day]

**Problem:** `pio test -e lolin_s3_loopback -d drivers/modBus` — the driver's whole hardware suite, HW-MB-001…011 — fails to compile:

```
src/modbus_rtu.cpp:257:22: error: 'UART_SCLK_DEFAULT' was not declared in this scope
                                   suggested alternative: 'UART_SCLK_XTAL'
```

**Root cause:** `[env:lolin_s3_loopback]` is `platform = espressif32` (**unpinned**) with `framework = arduino`; the firmware is `espressif32@6.12.0` with `framework = espidf`. `modbus_rtu.cpp` moved to pure ESP-IDF in the v2.0.0 migration and uses `UART_SCLK_DEFAULT`, which the Arduino platform's older bundled IDF does not define. The test env was left on the old framework and nobody noticed, because a hardware suite needing three jumper wires is rarely run.

**Confirm it is not your change:** build the env against `HEAD`'s source. Copy the file aside, `git show HEAD:<file> > <file>`, build, restore — pure file operations, no stash or index games:

```bash
cp drivers/modBus/src/modbus_rtu.cpp /tmp/mine.cpp
git show HEAD:drivers/modBus/src/modbus_rtu.cpp > drivers/modBus/src/modbus_rtu.cpp
pio run -e lolin_s3_loopback -d drivers/modBus     # same error => pre-existing
cp /tmp/mine.cpp drivers/modBus/src/modbus_rtu.cpp
```

**The wider trap:** `drivers/*/` each carry their own `platformio.ini` with their own platform/framework, independent of `firmware/platformio.ini`. A driver source file shared between them can compile in one and not the other, and the divergence is silent until someone runs the neglected env. **Do not read a green `pio test -e native` as evidence the driver builds everywhere** — the native env also `#define`s `NATIVE_TEST`, which stubs out real behaviour (for gh#49 it shims the bus lock to a no-op, so the 12 host tests say nothing about the mutex).

**Fix — a 4-line version guard, and my first cost estimate was badly wrong.** I judged this "a half-day of migration work" and recommended re-platforming the env, on the assumption the Arduino/ESP-IDF divergence would cascade. **It does not: `UART_SCLK_DEFAULT` is the ONLY error.** arduino-esp32 *is* ESP-IDF underneath, so every other IDF call in the driver (`uart_driver_install`, `uart_read_bytes`, `esp_timer_get_time`, `esp_rom_delay_us`…) resolves fine. Only that one symbol is IDF 5.0+; the Arduino platform (arduino-esp32 2.0.9 -> IDF 4.4) has `UART_SCLK_APB`, which is what `DEFAULT` resolves to on the S3 anyway:

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.source_clk = UART_SCLK_DEFAULT;
#else
    cfg.source_clk = UART_SCLK_APB;
#endif
```

**Lesson: count the errors before estimating the fix.** One `pio run` would have replaced the whole options table I wrote.

**Blast radius was wider than the loopback suite** — every Arduino-framework env compiling this file was dead: `modBus -e lolin_s3_loopback` (HW-MB-001…011), `modBus -e lolin_s3`, `FG6485A -e lolin_s3`, `s200 -e lolin_s3`. All four build after the guard; firmware flash size is byte-identical (1 378 949) and the 12 host tests still pass.

**Still cannot RUN**, only build: HW-MB-001…012 need three jumper wires (GPIO 17->38, 21->18, 8->16) on a board that is not the assembled dev unit.

**Where it lives:** `drivers/modBus/platformio.ini` `[env:lolin_s3_loopback]`; `drivers/modBus/src/modbus_rtu.cpp:257`.

---

## 2026-09-07 — `rota_release.py --dry-run` is not side-effect free: it writes the release manifest

**Problem:** `--dry-run` is documented as *"print planned actions; make no changes"* and prints `[dry-run] would create GitHub release`. It nevertheless **writes `bin/<version>/manifest-<version>.json` to disk** — proven by running it twice on 2.4.0 and watching the file's `released_at` change (07:59:30Z, then 08:00:46Z).

**Why it matters:** that file is the **seq ledger's master copy** — `cmd_release` derives the next seq solely from local `bin/*/manifest-*.json` (R-S08). A dry-run therefore mutates the input to the very calculation it claims to only preview. In this instance the seq stayed correct at 45 (the tool excludes the version being released from its own max), so no harm — but the mechanism is there.

**Unresolved, and worth knowing:** a published GitHub Release `v2.4.0` already existed when the real `release --yes` ran, created at 08:00:34Z — bracketing the second dry-run (08:00:46Z) between its create and publish timestamps. The operator may have run the release themselves; that is the likely explanation. **It is not proven either way**, and proving it would mean deliberately dry-running another version to see whether a release appears. If a future `--dry-run` is followed by an unexpected "release already exists", this is the first thing to check.

**Fix / how to work:** treat `--dry-run` as read-mostly, not read-only. Check `git status bin/<version>/` after one. Before concluding a release is missing or duplicated, query the API directly:
`curl -s https://api.github.com/repos/pe1mew/greenhouse-Controller/releases/tags/v<ver>` — it shows `draft`, `assets`, and the commit the tag resolves to. **Never reach for `--force` on the strength of "already exists"**: verify the existing release first. Here it was complete and correct (tag on the right commit, all three assets, sha256s matching the local build), so forcing would have replaced good artefacts for nothing.

**Where it lives:** `bin/rota_release.py` (`cmd_release`, `local_bin_seqs`); ledger files `bin/*/manifest-*.json` (tracked in git since 2.2.15).

---

## 2026-09-07 — ROTA runtime settings are on `/api/ota/config`, NOT `/api/config` — and a wide window inverts gh#41

**Problem:** looking for the ROTA night window, `GET /api/config` returned nothing matching (`ota_win_lo` / `ota_win_hi` are absent from that payload even though they are real NVS keys in `data_manager.cpp:138-139`).

**Root cause:** ROTA has its own admin endpoints. `GET/POST /api/ota/config` carries `enable`, `url`, `check_h`, `win_lo`, `win_hi`, `secret_set`, `cert_custom`. `GET/POST /api/ota/check` reports and forces the manifest check (`result`, `offered`, `running`, `last_check`, `http`). Both are admin-only.

**The operationally important part — FDA4 is not on a night window.** Read 2026-09-07: `win_lo = 1`, `win_hi = 23`, i.e. the apply window is **01:00-23:00, open 22 h a day**; `check_h = 1`.

That **inverts the gh#41 failure mode.** `rota_apply()` gates on `if (!in_night_window(lo,hi) || !quiet_gate())`, and `||` short-circuits — so the quiet gate is only *skipped* when the window is shut. On a 22-04 unit you defer on the clock and never reach the session check. With 1-23 the window is nearly always open, so `quiet_gate()` **always** runs, and it returns false on *any other active web session* (`ota_client.cpp:536`), plus a window MOVING, `WIND_OVERRIDE` / `MOTOR_ALARM` / `CALIBRATING`, or an LCD PIN session. Deferral retries every 300 s.

**So on FDA4 the thing that silently blocks an update is a stray logged-in browser tab, not the clock** — with no visible symptom beyond the update never applying. `POST /api/ota/check` exempts *its own* session (`s_exempt_token`), so trigger from the GUI you are sitting in, and close the others.

**Where it lives:** `firmware/src/web_server/web_server.cpp:3023-3030` (routes); `firmware/src/ota_client/ota_client.cpp:511` `in_night_window`, `:536` `quiet_gate`, `:564` the gate expression.

---

## 2026-09-07 — every bench reset counts as a boot failure: four short USB sessions trigger an OTA rollback

**Problem:** connecting FDA4 over USB and resetting it a few times for short serial captures silently walked the OTA fail counter up. It was already at **2** on arrival, and two 12-14 s captures took it to **3** — one boot away from the rollback branch, which would have reverted the unit to whatever bank B held, mid-debug, for no reason connected to firmware health.

**Root cause — read from `ota_manager.cpp:352-410`, not inferred:**

- The counter lives in **NVS** (`NVS_NS_SYSTEM` / `OTA_FAIL_KEY`), so it **survives reflashing the app**. Only an NVS erase or a healthy boot clears it.
- `ota_check_rollback()` runs early in `setup()`. It reads the counter, and **if the value read is >= 3 it calls `esp_ota_mark_app_invalid_rollback_and_reboot()`**; otherwise it increments and continues. So the sequence is 0 -> 1 -> 2 -> 3 -> **rollback on the fourth consecutive boot**.
- The **only** thing that resets it is `ota_mark_healthy()`, called by T1 after `OTA_HEALTHY_MS` = **30 000 ms** of uptime (`watchdog.cpp:324`). Nothing else does.
- The one exemption — `esp_reset_reason() == ESP_RST_SW` plus the `t15_planreboot` NVS flag — **cannot fire on current firmware**: T15 is dormant and excluded from the build. A DTR/RTS bench reset reports `rst:0x1 (POWERON)` anyway, so it would miss the gate regardless.

**The design is correct** — it is what makes a genuinely bad OTA revert itself. The trap is that **a deliberate short bench session is indistinguishable from a crash loop**: unplug the board, or capture for 15 s and stop, and the firmware records exactly what a boot failure looks like.

**Fix — on the bench, let it run past 30 s:**

- Size every serial capture **> 35 s** so the log shows `[OTA] Boot marked healthy - fail counter reset to 0`. That line, not the absence of a panic, is the proof the board is in a clean state.
- **Read `[OTA] Boot fail counter = N` at the start of every session.** A non-zero value means the previous session left it dirty; N = 3 means the next boot rolls back.
- Before unplugging, leave the board powered 30 s. Note `esptool` operations each end with a hard reset, so a flash or a `read_flash` also starts a fresh sub-30 s boot if you stop there.
- After a cable flash, do one settle run and confirm the healthy line before calling the unit done.

**Not fully observed:** the boot(s) between `write_flash` finishing (esptool hard-resets) and the verification capture were not recorded, so the counter's exact path across the flash is unreconstructed. Verified values only: **2 -> 3** before the flash, and **1 -> 2 -> cleared to 0 at 31 s** after it. The mechanism above is from the source, not from that gap.

**Where it lives:** `firmware/src/ota_manager/ota_manager.cpp:352` (`ota_check_rollback`), `:412` (`ota_mark_healthy`); `firmware/src/ota_manager/ota_manager.h:82` (`OTA_HEALTHY_MS`); `firmware/src/watchdog/watchdog.cpp:324` (T1 calls it).

---

## 2026-09-07 — `mklittlefs` builds a perfectly valid image of an EMPTY directory, and every downstream check passes

**Problem:** while cable-flashing FDA4 to 2.3.1, the step that extracts `web-assets-<ver>.zip` into a staging directory failed — but `mklittlefs -c <empty dir>` then produced a **1 048 576-byte image containing zero files, exit code 0, no warning**. Had it been flashed, the result would have been a web GUI serving "Web assets not yet uploaded".

**Root cause — two independent traps, and the second is the dangerous one:**

1. *The extraction failed silently in the same command block.* Under Git Bash the shell path `/c/Users/...` is **not** a valid path for the native Windows Python, and an inline `python -c` that tried to convert it (`.replace('/','\\\\')`) died with a `SyntaxError`. The `mkdir -p` had already run, so a valid **empty** directory was waiting.
2. *`mklittlefs` treats an empty source directory as a legitimate request.* It is not an error to build an empty filesystem, so it does not warn.

**Every check downstream also passes**, which is what makes this worth an entry:

| Stage | What it reports on an empty image |
|---|---|
| `mklittlefs -c` | exit 0, correct file size |
| `esptool write_flash` | **"Hash of data verified"** — it faithfully wrote the empty image |
| firmware boot | `littlefs_mount(A (lfs0)) returned 0 (OK)` — an empty LittleFS mounts fine |
| firmware self-test | `LFS write/read verify: PASS` — it writes its own probe file, which succeeds |

Only `/index.html` being absent reveals it, and that surfaces in the browser, not on the console.

**Fix — list the image before flashing it:**

```bash
mklittlefs -l <image>.bin -b 4096 -p 256 -s 0x100000
```

It must show `index.html`, `app.js`, `style.css` and `manifest.json` with plausible sizes. Also verify `manifest.json` carries the intended `asset_version` *before* building, and after flashing read the partition back (`esptool read_flash 0x420000 0x40000`) and grep for `{"asset_version":"X.Y.Z"` — that is a check against the **device**, not against intent, and it is the only `asset_version` confirmation available when the unit has no working network (the canonical `/api/status` pair-read needs WiFi).

**Also:** pass Windows paths to native tools via `cygpath -w`, and hand them to Python as **argv**, never embedded in `-c` source — `python -c "import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$(cygpath -w a.zip)" "$(cygpath -w outdir)"`.

**Related:** the 2026-07-13 greenfield entry below — a *different* cause (`pio buildfs` emits SPIFFS, not LittleFS) producing the **identical** symptom. Two ways to get an unreadable assets partition; both look healthy from the serial log. Extract the release zip to a scratch directory rather than stamping `firmware/data/manifest.json`, and the gh#9 placeholder dance is avoided entirely — the released zip already carries the correct `asset_version`.

**Where it lives:** `~/.platformio/packages/tool-mklittlefs/mklittlefs.exe`; the recipe in the 2026-07-13 entry; `bin/build_release.ps1` Step 2.

---

## 2026-09-05 — `gh_issue.py` returns `401 Bad credentials` on every call: the fine-grained PAT expired

**Problem:** `create`, then `list`, then everything else answered `HTTP 401 Bad credentials`. It looked like a broken script or a revoked repository permission; the same script had commented on gh#45 nine days earlier.

**Root cause:** the token is a GitHub **fine-grained PAT** created 2026-06-06 with the 90-day default lifetime, so it expired on 2026-09-04. Nothing warns; the file just stops working. The pointer in `.github/gh_issue.local` resolved, no env override was shadowing it, the token had no stray whitespace — the credential itself was simply dead.

**Fix:** regenerate it in GitHub (Settings → Developer settings → Fine-grained tokens; Issues: Read and write on the repo) and overwrite the file named on the `GH_ISSUE_TOKEN_FILE=` line of `.github/gh_issue.local` — that file lives in the operator's secret store (never name it here). **Diagnose in one step:** `python bin/gh_issue.py list` — if even that 401s, stop debugging the call. Renewed 2026-09-05; with the default lifetime it lapses again around **2026-12-04**.

**Where it lives:** `bin/gh_issue.py` (resolution order: env `GITHUB_TOKEN`/`GH_TOKEN` → `GH_ISSUE_TOKEN_FILE` → legacy `.github/token.local`); `.github/gh_issue.local`.

---

## 2026-09-05 — a ~14 KB `python - <<'PY'` heredoc dies at parse time with "unexpected EOF while looking for matching `''"

**Problem:** a long inline Python edit script (the §12 write-up for the position-sensor study) failed before a single line ran — no file changed, the bash parser reported an unterminated quote. Heredocs of a few KB in the same session had worked repeatedly.

**Root cause:** the command was cut short before the heredoc terminator, so bash reached EOF inside the body and then tripped over an apostrophe in the prose. A length limit on inline commands, not a quoting mistake in the script.

**Fix:** write long scripts to the scratchpad with the Write tool and run `python <path>`; keep inline heredocs to a few KB. Side benefit: nothing to escape. (Distinct from the ASCII-only pattern above, which is about *printing*, not parsing.)

**Where it lives:** agent tooling, not the repo.

---

## 2026-09-05 — `modbus_rtu.h` promises a UART mutex that does not exist (gh#49)

**Problem:** the Modbus driver header says, twice (lines 35 and 100), that the driver "serialises wire access internally with a UART mutex" created in `modbus_init()`. Read that and you would happily let a second task — T2 stopping a window on a position reading, say — call `modbus_read_input_registers()` alongside T5.

**Root cause:** `drivers/modBus/src/modbus_rtu.cpp` contains **no semaphore, mutex or critical section of any kind** (grep for `Semaphore|mutex|CRITICAL` finds nothing outside the loopback test). The comment describes an intention, not the code. It is harmless today only because T5 is the sole caller — which is exactly the "single-owner by convention" rule in `architecture.md`, and it is load-bearing, not decorative.

**Fix:** treat the header as wrong. Any second caller needs the mutex to be *added* (one `xSemaphoreCreateMutex()` in `modbus_init()`, take/give around each transaction — but a 200 ms timeout inside a High-priority WDT-subscribed task is still a bad idea, so prefer keeping all bus I/O in T5 or a dedicated bus task, per `design/refactorSensorConfiguration.md` §2.2). Found while evaluating the M3 position sensor (`design/windowPositionSensorRequirements.MD` §12), whose 1 Hz polling is the first thing that would tempt a second caller.

**Where it lives:** `drivers/modBus/src/modbus_rtu.h:35`, `:100`; `drivers/modBus/src/modbus_rtu.cpp` (no lock); `memory/architecture.md` T5 row. **Filed as gh#49** (options: fix the doc / add the mutex / both — both recommended).

---

## 2026-08-26 — sensor-fault transients on 5C88: a recurring ~59 s T/RH blip, and one long wind fault that is fully explained

**Every sensor-fault pair in the campaign to date** (`ALARM` rows on ch 4 = T/RH, ch 5 = wind; `value_a` 1 = triggered, 0 = cleared):

| when | sensor | duration | reading |
|---|---|---|---|
| 2026-06-10 16:04:14 | T/RH | same second | early-campaign blip |
| 2026-06-19 09:39:46 -> 11:19:21 | wind | **~100 min** | **pre-commissioning — not a field failure**, see below |
| 2026-07-22 04:02:38 -> 04:03:37 | T/RH | 59 s | |
| 2026-07-29 17:11:54 -> 17:12:53 | T/RH | 59 s | |
| 2026-08-18 17:07:56 | wind | same second | T3 raised and released the safe-fail override correctly (`param=243`) |
| 2026-08-23 03:19:19 -> 03:20:18 | T/RH | 59 s | |
| 2026-09-04 19:32:18 -> 19:33:16 | T/RH | 58 s | added 2026-09-05 |

**The ~59 s T/RH blip is the recurring one** — five T/RH events total, four of them 58-59 s (Jul 22, Jul 29, Aug 23, Sep 4; intervals 7 / 25 / 12 days). All self-cleared; climate control rode through on the last good average and none is visible as an excursion in the day-plots. Consistent with an occasional Modbus read collision or a transient on the RS485 pair, not a failing sensor. **Note the spacing is irregular** — Jul 22 and Jul 29 are only a week apart — so "roughly monthly" would be wrong.

**The 100-minute wind fault is explained and is not a defect:** the wind vane was commissioned on **2026-06-19 at 12:00** (see the 2026-07-13 wind-validity entry). The fault ran 09:39 -> 11:19 that same morning, i.e. entirely *before* the sensor was in service. It is an installation artefact. Anything wind-related before 2026-06-19 12:00 should be read the same way.

**Why this is logged:** so the next occurrence is recognised rather than investigated from scratch, and so nobody re-derives the Jun 19 alarm as a mystery.

**When the T/RH blip would become worth chasing:** duration exceeding a couple of minutes, a fault that does *not* self-clear, a rising rate, or clustering by time of day or weather — the last would point at motor electrical noise on the RS485 pair rather than the sensor.

**Motor-noise hypothesis tested 2026-09-05 and ruled out.** None of the five T/RH faults falls within +/-120 s of any `RELAY` transition on any channel (0 of 5). Whatever causes the dropout, it is not window-motor switching.

**The constant duration is itself a clue.** T5 raises the fault on the *second* consecutive failed read and clears it on the *first* success (`sensor_poll.cpp` header, lines 14-18). At the 30 s poll, a 58-59 s fault therefore means: failure #1, failure #2 (trigger logged), failure #3, success at read #4 (clear logged) -- **every one of the four events is exactly three failed reads.** Random bus collisions would mostly give two-failure (~30 s) events with a spread of lengths; four identical three-failure events point at a deterministic ~60-90 s outage in the sensor itself -- an internal reset or watchdog is the obvious candidate -- rather than at the RS485 bus. *Inference from duration statistics only, not verified; if it ever matters, the FG6485A's diagnostic registers (its driver has an info helper) would show a reset counter.*

**Count them with:**

```bash
grep -h ",ALARM," *.log | awk -F, '$4==4 || $4==5'
```

**A caution learned writing this entry:** an earlier draft claimed "2 occurrences, roughly monthly" from memory. Running the command above immediately showed six pairs across two sensors, including the 100-minute one. **Run the count before characterising a pattern** — a fault log is exactly the place where recollection is unreliable.

**Where it lives:** `firmware/src/sensor_poll/sensor_poll.cpp` (two-consecutive-failure fault policy); `EG1_BIT_SENSOR_FAULT_T` / `_W`; ch 4/5 encoding per `logparser.py`.

---

## 2026-08-25 — an SD log's FILENAME is its upload time, not its coverage window

**Problem:** looking for a wind event timestamped `2026-08-22T15:35:56`, I opened `2026-08-22_001307.log` — the obvious choice by name — and the parser produced no ALARM rows at all. It looked briefly like the parser was dropping them, i.e. a firmware/parser bug.

**Root cause:** the filename is the moment T14 **uploaded** the file, which is always *after* the period it covers, and by a variable margin. `2026-08-22_001307.log` actually spans **Aug 20 07:13 -> Aug 22 02:12**; the 15:35 event lives in `2026-08-23_190517.log` (Aug 22 02:12 -> Aug 23 21:04). Off-by-one-file, every time, and it fails *silently* — you get a valid parse of the wrong period, not an error.

**Fix:** never select a file by name. Resolve coverage first:

```bash
for f in *.log; do echo "$f  $(head -2 "$f" | tail -1 | cut -d, -f1)  ->  $(tail -1 "$f" | cut -d, -f1)"; done
```

or simply `grep` the timestamp across all of them and let the match name the file. The contiguity table produced at the start of every log-processing run already contains the answer — read it rather than the filenames.

**Where it lives:** SD filenames are set at upload by T14 (`status_post.cpp`); the campaign chain in `model/campaign-summer-2026/`.

---

## 2026-08-16 — a full `plot_daily.py` run now exceeds a 2-minute command timeout (it still succeeds)

**Problem:** running `python plot_daily.py` with no arguments returned exit 143 (SIGTERM) under a 2-minute tool timeout, which reads as a failure. It was not: the run had already printed `Generated 71 PNG(s)` and every plot was written correctly.

**Root cause:** the campaign has grown to **39 log files and 71 day-plots** (Jun 4 – Aug 14). The full run re-renders *every* day, and matplotlib's per-figure cost now pushes the total past two minutes. Nothing is wrong; the job is simply bigger than it was when the 30-plot habit formed.

**Fix:** give the full run a longer timeout (5 min is comfortable), or avoid it entirely — `python plot_daily.py YYYY-MM-DD` renders a single day and returns in seconds. **Check `Generated N PNG(s)` before treating a timeout as a failure**, and confirm with `ls plot_*.png | wc -l`; a killed run leaves the already-written PNGs intact, so a re-run is safe and idempotent.

**Related:** the full run also re-renders unchanged days, which is why the pre-Jun-19 plots must be `git checkout --`'d afterwards (matplotlib version churn — 2026-07-05 entry). Both problems disappear with single-day mode.

**Where it lives:** `model/campaign-summer-2026/plot_daily.py` (single-day CLI mode added 2026-07-19).

---

## 2026-07-31 — anti-thrash dwell was unguarded during travel (gh#48) [RESOLVED 2.3.1 — confirmed in production]

**Problem:** M3 could be commanded to reverse **mid-stroke** by climate control, with no dwell protection. Observed on 5C88, 2026-07-27 03:02:39 `MOVING_OPEN` → 03:04:42 `MOVING_CLOSE` — 123 s into a 176 s travel, 72 % open.

**Root cause:** `dwell_deadline_ms` arms only when a stroke **completes** (`CH_MOVING_OPEN` → `CH_OPEN`, `relay_controller.cpp:492`), and `ch_start_close()` carried its dwell check **only in the settled `CH_OPEN` branch** — its `CH_MOVING_OPEN` branch reversed unconditionally. So a window in motion had no protection at all. M3 was uniquely exposed: 171 s travel leaves a ~3 min window on every opening, versus ~21 s for M1/M2.

**Fix (2.3.1):** a reversing command from **T6 (climate) is deferred** while a stroke is in progress — the travel completes and the settled-state dwell then governs. Nothing is lost: T6 reconciles level-triggered and re-issues every cycle. **T3 (wind safety) and SRC_OPERATOR_MANUAL still reverse immediately** — verified on hardware 2026-07-28 (M3 reversed 4 s after the override, 32 s into a 176 s stroke).

**Confirmed in production**, before vs after the 2.3.1 apply on 5C88, over the full dataset to 2026-08-11:

| | span | M3 strokes | mid-stroke reversals | median OPEN dwell |
|---|---|---|---|---|
| before 2.3.1 | 28 days | 152 | **7** | 25 min |
| after 2.3.1 | 12.7 days | 66 | **0** | **25 min** |

If the pre-fix rate (7/152 ≈ 4.6 % of strokes) were unchanged, `P(0 in 66) = (1 − 7/152)^66 ≈ 0.044` → **p ≈ 4 %**. Statistically meaningful, and resting entirely on the reversal count.

**CORRECTION (2026-08-12) — an earlier version of this entry, and the gh#48 closing comment, claimed the median OPEN dwell rose 25 → 40 min and called that "the stronger signal, the deferral leaving a positive fingerprint". Both the number and the reasoning were wrong.** The 40 min came from a 2.5-day window with only 15 strokes — small-sample noise; over the full data the median is unchanged at 25 min. And it *should* be unchanged: the guard only alters strokes that would otherwise have been reversed (7 of 152, under 5 %), so it cannot move a median dominated by the 95 % of strokes that never meet it. **Lesson: before calling a shifted summary statistic "evidence", check whether the mechanism could plausibly move it, and whether the sample is large enough to have detected it.** The conclusion survived; the argument for it did not.

**Detection heuristic — and the way I got it wrong.** The 2.3.1 release notes first proposed scanning logs for "any `MOVING_OPEN` → `MOVING_CLOSE` pair shorter than the travel time". **That check is wrong and will cry wolf**: a legitimate wind override produces exactly that pattern by design, and the hardware test proved it. **A mid-stroke reversal is only a defect if it is NOT immediately preceded by a wind `ALARM` row (param 240/241/243).** Correlate the two, or every correct safety reversal reads as a regression.

**Where it lives:** `firmware/src/relay_controller/relay_controller.cpp` — `ch_start_close()` / `ch_start_open()` in-travel guards, dwell arming at `:492`; TSDS §T2 FSM table + dwell section; test plan UT-CC-033/034/035; gh#48.

---

## 2026-07-28 — replaying T6 control logic from SD logs: five traps, each of which silently produces plausible-but-wrong numbers

**Problem:** building `model/vent_step_replay.py` (replay the ventilation-step decision under candidate `hyst_t`) took **three failed model iterations** before it validated. Every failure produced authoritative-looking output that was simply wrong; only an explicit validation gate (reproduce the logged T-demand, ≥ 90 %) caught them.

**Root causes — all five will bite any future log analysis:**

1. **`model/campaign-summer-2026/config.json` is 2344's config, and stale** (`ap_ssid: Greenhouse-2344`, `fw_ver: 2.0.0-rc.1.5.6`). It is the source of the setpoint lines on the 5C88 day-plots. Using it as "the unit's config" for a 5C88 replay gives wrong `avg_win_t`. **Read the live config from the unit** (`GET /api/config`, admin session) or ask the operator. *(5C88 and 2344 do differ: `avg_win_t` is 3 on 5C88, 6 on 2344.)*
2. **A `MODE` row is logged only when the resolved step CHANGES**, but T6 evaluates on **every** sensor cycle and carries `current_step` statefully. Stepping a simulation only at MODE rows instead of at every sample diverged the hysteresis state and fitted only ~50 %.
3. **`meas.t_avg_c` is the ROUNDED integer °C, not truncated.** With `hyst_t = 5` the step width is `5/3 = 1 °C`, so a 1-degree rounding error is a whole ventilation step — truncating collapsed the fit from ~97 % to ~59 %.
4. **Window bitmask (`SENSOR_HR` ch=2): OPEN = 2, not 1.** Two bits per channel, `0 = CLOSED, 2 = OPEN, 1/3 = moving`. Decoding OPEN as 1 makes every window read as "moving" forever.
5. **RELAY `value_a` is a state enum where 2 = MOVING_OPEN and 3 = OPEN** (`0 UNKNOWN, 1 CLOSED, 2 MOVING_OPEN, 3 OPEN, 4 MOVING_CLOSE, 5/6 gap`). Treating 2 as OPEN measures *travel duration* (~3 min for M3) and reports it as "open time" — which looks plausible and is off by an order of magnitude. The authoritative table is `RELAY_STATE_NAME` in `plot_daily.py`.

**Fix:** `model/vent_step_replay.py` ports `step_from_deviation()` / `vent_resolve_conflict()` faithfully, documents traps 2 and 3 in its module docstring, and **refuses to print projections unless the replay first reproduces ≥ 90 % of the logged T-demands** at the unit's configured settings (currently 97.8 %). Reuse that gate for any future replay — it is the only thing that distinguishes a model from a plausible guess.

**Where it lives:** `model/vent_step_replay.py`; encoding tables in `log/logparser.py` and `model/campaign-summer-2026/plot_daily.py` (`RELAY_STATE_NAME`); `firmware/src/climate_control/climate_control.cpp` (`step_from_deviation`, call site at `:556` confirming `t_avg_c` units).

---

## 2026-07-28 — an emulator-driven hardware test is void if the sensor emulator is still being fed live data

**Problem:** the first gh#48 wind-bypass regression run on 2344 pushed 12 m/s and the unit's average stalled at 2.7 m/s. The test reported INCONCLUSIVE after 3 minutes.

**Root cause:** the sensor emulator was still receiving live sensor data, which overwrote every injected value. Setting the sensors to REST mode (`POST /config/sensor {"sensor":..,"mode":3}`) is **not** sufficient on its own if something upstream keeps writing.

**Fix:** before any emulator-driven test, **inject a distinctive value and confirm the unit actually sees it** — e.g. push `Speed=9.9, Direction=312` and check `/api/status` reports raw 9.9 / 312 one poll cycle later. Costs ~45 s and converts a silent false result into a known-good starting state. Build the same guard into test scripts: assert the precondition rather than assuming it, so a spoiled run reports INCONCLUSIVE instead of a wrong PASS/FAIL.

**Where it lives:** sensor emulator at `192.168.20.226` (`/config/sensor`, `/api/data`); pattern used in the gh#48 test.

---

## 2026-07-23 — `test/` is gitignored but 15 files inside it remain tracked (deliberate mixed state)

**Problem (future trap, recorded pre-emptively):** `.gitignore` carries `/test/` (operator decision: the pytest HIL bench harness — `conftest.py`, `lib/`, `test_01..09` — stays local-only). But 15 files under `test/` were tracked *before* the rule and deliberately stay tracked: `softwareTestPlan.md`, `testPlan.md`, `softwareTestResult.md`, `firmwareIntegrationTestPlan.md`, `manualREST.md`, `test_10_rota.md`, and the `3_3_*`/`3_4_*`/`5_3_2_*` manual-test records. Consequences a future session will hit: (1) **a NEW file created under `test/` never appears in `git status`** — it silently stays local (`git add` needs `-f`); (2) edits to the 15 tracked files still show and commit normally; (3) `softwareTestPlan.md` cross-references `[→ TC-xx]` implementations that exist only on the bench machine — that is by design, not an omission.

**Root cause:** gitignore only affects untracked files; the operator chose to keep the previously-tracked spec/result docs while excluding the executable harness.

**Fix / rule:** to add a new *document* under `test/` to the repo, use `git add -f test/<file>` knowingly — or better, ask whether it belongs elsewhere. Never "fix" the situation by bulk `git rm --cached` (the tracked 15 are referenced by CLAUDE.md workflows and the 2.3.0 release notes) or by deleting the `/test/` rule.

**Where it lives:** `.gitignore` (rule + inline note); operator decision 2026-07-23.

---

## 2026-07-23 — `rota_release.py release` aborts silently-looking under a null-stdin shell: interactive [y/N] prompt

**Problem:** First `python bin/rota_release.py release 2.3.0` printed the full plan then ended with `Create GitHub release v2.3.0 and upload 3 assets? [y/N] aborted.` — the harness shell has stdin on the null device, the prompt read EOF, and the tool aborted (correctly, but after a full round trip).

**Fix:** pass **`--yes`** when running any `rota_release.py` mutating subcommand from Claude's shell (`release`, `promote`, `publish` all share the confirmation flag via the common parser). Print the plan first without `--yes` only if the operator should review it before execution — the abort-on-EOF behaviour makes that a safe two-step. Also note: the tool warns "working tree has uncommitted changes" whenever ANY untracked file exists; if the release-relevant files are committed and the artefact hashes match the release notes, that warning is noise.

**Where it lives:** `bin/rota_release.py` (`--yes` on the common argument parser, `:720`).

---

## 2026-07-23 — logparser mislabels a wind-override SET at `speed == v_max` as a "direction" event (gh#45) [RESOLVED 2.3.0 — both parts shipped same day]

**Problem:** 5C88's wind alarms on Sunday 2026-07-19 parsed as *"WIND OVERRIDE: SET - direction 60 deg in exclusion zone (low bound 60 deg)"* at 12:35:16 and 14:14:31. Taken at face value that says the wind blew from 60° (ENE) into a directional exclusion zone. **It didn't** — the instantaneous wind at both moments was from the NNW (~326–358°), never near 60°, and there is no evidence a directional exclusion was even configured. All three of that day's episodes were **speed** triggers (gusts to 7–9 m/s pushing the ~6-min averaged wind onto `v_max`).

**Root cause:** The raw rows are `value_a=60, value_b=60`. For a wind-override SET, `event_logger.h` encodes **speed** as `va=speed×10, vb=v_max×10` and **direction** as `va=direction°, vb=excl_low°` — two different meanings in one un-tagged `(va,vb)` space. `logparser.py`'s `_fmt_alarm()` disambiguation (`:462`) tests speed with **`va > vb`** (strict). But the firmware fires on `speed >= v_max` (confirmed by the same day's 14:16 row `62,60` → "6.2 >= 6.0"). At exactly `speed == v_max`, `va == vb` (60==60), the strict `>` fails, the CLEARED test (`vb > va`) fails too, and it falls through to the "otherwise → direction SET" branch, which reinterprets the *speed* `60` as a *bearing* of 60° and the `v_max` `60` as `excl_low`. Pure coincidence that 6.0 m/s ×10 = 60 is also a plausible angle.

Deeper point: `60,60` is **genuinely ambiguous from the row alone** — a real direction event with `direction == excl_low == 60°` produces the identical bytes. And the overlap isn't only at the boundary: any direction SET with `direction > excl_low` and `excl_low ≤ 200` already gets grabbed by the speed branch. No parser heuristic can fully separate the two; only a source-side discriminator can.

**Fix:**
- **Immediate (parser, low-risk):** change the speed test at `logparser.py:462` from `va > vb` to `va >= vb`. This is strictly an improvement — it captures the `va == vb` boundary as a speed SET (the common, observed case) and changes nothing else (the CLEARED branch needs `vb > va`, mutually exclusive). It does **not** resolve the residual speed-vs-direction overlap; add a caveat comment saying so.
- **Proper (firmware, the real fix — tracked in gh#45):** disambiguate at the source the way T5 sensor faults already did (see the `_fmt_alarm` docstring: "the new ch-based encoding lets T2/T3 keep ch=0 and T5 own ch≥4"). Give T3 wind-override rows a `channel` (or `param`) discriminator — e.g. speed-SET / direction-SET / CLEAR each get their own code — so the parser reads the type instead of guessing. **Per CLAUDE.md this is a log-format change: `log/logparser.py` must learn the new encoding in the same changeset**, and it needs a version bump.

**RESOLUTION (2026-07-23, 2.3.0):** both fixes landed the same day. Part 1 (`>=` in the parser) in `e15a17b`; Part 2 (the source-side discriminator) shipped in **2.3.0** — `make_wind_log()` stamps `param` 240–243, parser+plotter decode param-first, legacy rows keep the heuristic (campaign replay byte-identical). The recognition heuristic below still applies to **pre-2.3.0 logs**, which is every campaign file before Jul 2026.

**How to recognise it (legacy `param=0` rows only):** a wind "direction" override whose reported bearing does **not** match the `SENSOR_HR` wind-direction samples around the same timestamp, especially when the bearing numerically equals `v_max×10` (60 = 6.0 m/s, 80 = 8.0, …). Cross-check every "direction" wind event against the instantaneous wind before believing it. This sits alongside the ASCII-only promoted pattern's warning: don't trust a decode you can cross-check but didn't.

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

**RECURRED 2026-07-23 (3rd time, new variant): Claude REPORTED ".gitignore staged" without having run `git add` at all.** The edit existed only in the working tree; the "staged" claim was narrative, not verified state. Caught one turn later when staging the manifest showed a one-file index. No damage (nothing lost, the files were untracked-safe), but the operator's mental model of the index was wrong for a full turn. Lesson: **a staging claim must be backed by `git status --short` output in the same turn — state what git says, not what you meant to do.** Promoted with the other two variants, see top of file.

**Where it lives:** Process, not code. Commits `32ce6b5`/`5c3c6cc` (branch-switch variant), `46b709f`/`cb6a178` (`commit -a` variant), and the 2026-07-23 false-staged report (no commit — caught in-session) are the examples.

---

## 2026-07-13 — Wind measurements before 2026-06-19 12:00 are invalid

**Problem:** Wind speed/direction columns exist in SD logs and campaign data from Jun 4, but the wind vane was only commissioned **2026-06-19 12:00**. Earlier values (including the wind columns in `plot_summary.txt` for Jun 4–19) are garbage and must not be used in any analysis.

**Root cause:** Sensor commissioning happened mid-campaign; the log format carries the columns regardless.

**Fix:** Constraint recorded in `thermalProfileCampaign.md` §9.11 and enforced in `ns9_direction_stratified.py` (`WIND_VALID_FROM`). Any new wind-based analysis script must filter `timestamp >= 2026-06-19T12:00`.

**Where it lives:** `model/campaign-summer-2026/ns9_direction_stratified.py` (the reference filter); campaign doc §9.11; `model/campaign-summer-2026/plot_daily.py` — the day-plot wind-direction axis is gated on the same date, so pre-Jun-19 plots render **without** a direction scatter by design (added 2026-07-20; if you regenerate all plots, `git checkout --` the pre-Jun-19 PNGs).

---

## 2026-07-13 — Rapid OTA reboots rate-limit SNTP → T16 (ROTA) skips checks

**Problem:** During ROTA client testing on a dev unit (FDA4), T16's manifest check kept returning `result:"skipped", code:3` for many minutes, even though `/api/status` reported a correct wall-clock time. `system.ntp_synced` stayed `false`.

**Root cause:** OTA-pushing/reflashing the *same* unit many times in an hour makes it re-run `nm_sntp_quick_sync()` on every boot; pool.ntp.org rate-limits (KoD) the source IP after the burst, so the fresh per-boot sync never completes and the `nm_is_sntp_synced()` latch never sets. The internal ESP32 RTC retains valid time across warm reboots (so the *clock* is right), but T16 gates on the strict SNTP latch by design (see [rotaImplementationPlan.md](../design/rotaImplementationPlan.md) risk #3), so it skips rather than sign a request on an untrusted clock.

**Fix:** Not a firmware bug — a test-environment artifact. Wait it out: T16 recovers on the rc.1.5.6 SNTP retry cadence (`NTP_RETRY_INTERVAL_S=300`, 5 min). To avoid it, batch firmware changes before pushing rather than reflashing the same unit in a tight loop when you need a synced clock. Never arises in production (units don't reboot 8×/hour). Check state via `/api/ota/check` (`result`) + `/api/status` (`system.ntp_synced`).

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

**Fix:** Rebase the feature branch onto `origin/main`, then `git checkout main && git merge --ff-only dev/X && git push origin main`. Never `--force` to `main`. The recovery sequence is the one given above — it was previously said to live in `BRANCH_NOTES.md`, which never actually contained it; that file was deleted 2026-09-07.

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

## 2026-07-13 — T16 (ROTA) 8 KB stack overflows on the download/apply path [RESOLVED — 2.2.1]

**Problem:** First live pull-install put FDA4 into a **crash loop** (reboots at ~35 s uptime, stayed on the old version, GUI slow). Coredump: `A stack overflow in task T16-rota has been detected` (`esp-coredump ... info_corefile --core-format raw`).

**Root cause:** T16 was created with an 8 KB stack — enough for the manifest *check* (one mbedTLS handshake + a `cert[2048]` on the stack), but the *download* path nests a **second** mbedTLS handshake (`rota_download_verify → rota_https_get`) inside `ota_check_once`'s still-live frame, with a `cert[2048]` buffer live in **both** `ota_check_once` and `rota_handle_update`. Two TLS contexts + two 2 KB PEM buffers blew past 8 KB. It only surfaced on a real artefact download (the earlier 204/200 check tests never entered this path).

**Fix (2.2.1):** T16 stack 8 KB → **16 KB** (matches T13's OTA-work sizing, `firmware/src/main.cpp`), and both `cert[ROTA_CERT_MAX]` PEM buffers moved from stack to `malloc`/`free` (`ota_client.cpp`, −4 KB peak). **Lesson:** any task that runs `esp_http_client` over TLS needs ≥ ~12–16 KB; never stack-allocate the pinned-cert PEM. A stack that survives a small GET can still overflow on a large download that nests a second handshake.

## [RESOLVED 2.2.14] 2026-07-13 — `/api/coredump/status` labels a stale dump with the RUNNING version, not the crashed one

**Problem:** After FDA4 was updated to 2.2.2 (successfully, no crash), `/api/coredump/status` reported `{"present":true,"fw_ver":"2.2.2"}` — reading as "2.2.2 crashed." It hadn't. The dump was byte-identical (`cmp`) to the earlier 2.2.0 stack-overflow dump.

**Root cause:** two independent things compound. (1) `coredump_status_handler` hardcodes `"fw_ver":"` `FIRMWARE_VERSION` `"` (web_server.cpp:1953) — the *compile-time running* version — and the download filename is `coredump-` `FIRMWARE_VERSION` `-<ts>.bin` (web_server.cpp:2029); neither reflects the version that actually produced the dump. (2) The coredump partition is **only erased on greenfield flash** (the first-flash rule), **not** on OTA push/pull — so a pre-update crash dump lingers and then gets stamped with each subsequent running version.

**Fix / workaround:** Before believing a coredump matches the running version, verify: `cmp` against known prior dumps, or decode it (`esp-coredump ... --core-format raw <that-version's elf>`) and check the backtrace/task set. Erase a known-stale dump with `POST /api/coredump/erase`. **Improvements to consider:** erase the coredump partition as part of the OTA apply (T13 + T16/rota_apply) so a dump always matches the running image; and/or have the status endpoint report the version parsed from the dump's `esp_app_desc` instead of `FIRMWARE_VERSION`.

**Resolved (2.2.14, gh#39):** Part 1 done — at boot the dump's ELF-SHA (`esp_core_dump_get_summary`) is compared to the running image's (`esp_app_get_elf_sha256`) and cached as `stale`. `/api/coredump/status` now reports `running_fw_ver` (not the misleading `fw_ver`) + `"stale":bool`; the download filename gains a `-stale` marker; the Log-tab GUI shows "from an EARLIER firmware (running X)." Part 2 (erase the coredump on OTA apply) was **deliberately NOT done** — the operator chose to preserve dumps across updates and rely on the `stale` flag, so no crash data is ever lost to an update.

## 2026-09-12 — `POST /api/config` is asynchronous, so a read-back right after the write returns the PREVIOUS value

**Problem:** The 2.5.0 clamp verification harness POSTed an out-of-range value, immediately `GET /api/config`, and reported **13 FAILs out of 43** — every one of them showing the value from the *previous* write in the loop. The clamp was working correctly the whole time. Worse, the run left two keys (`t_max_day`, `cr_priority`) mid-flight because the "restore" step was itself read back too early and looked like it had not applied.

**Root cause:** `/api/config` does not write NVS on the request thread. It validates, then enqueues onto **Q4**; T4 drains Q4 on a later loop pass and only then clamps, writes NVS and updates the shadow. The HTTP 200 means *accepted*, not *applied*. The lag is one T4 loop period, which is long enough to lose a race against a script but short enough to look like a flaky clamp.

**Fix:** Never assert on a value read straight after a POST. Either settle (a few seconds) or poll until the stored value stops changing. And make the transition **observable**: if the original value already equals the bound you expect, move the key to a different in-range base first — otherwise a broken clamp and a working one produce identical read-backs.

**Rule (promoted):** *when a write goes through a queue, the HTTP status tells you it was accepted, not that it took effect — prove the effect separately, and make sure the expected effect is distinguishable from no-op.*

## 2026-09-12 — a verification step that does not check its own HTTP status can report a false FAILURE

**Problem:** The gh#58 harness verified "a successful login adds no PIN_AUTH row" by downloading the SD log twice and comparing raw line counts. It reported `0 new rows` after five minutes — impossible, since sensor rows land every 30 s — and therefore a FAIL. The firmware was fine.

**Root cause:** the second `GET /api/log/download` result was never status-checked. Any non-200 (or any body that was not the CSV) silently became "the file did not grow". This is the mirror image of the 2026-09-11 harness bug that reported five false PASSes by not JSON-parsing `HTTPError` bodies — same class, opposite sign.

**Fix:** assert the shape of every response the assertion depends on, including the *second* fetch of the same resource. Then count the thing you actually care about (PIN_AUTH rows: 7 before, 7 after) rather than a proxy (total line count) that a broken fetch can fake. Re-run gave 7/7.

**Rule (promoted):** *every fetch an assertion rests on needs its own status and shape check — a silently empty response is indistinguishable from "nothing happened".*

## 2026-09-12 — `log_type_t` is in `types/app_types.h`, not `event_logger.h`

**Problem:** Went looking for the log event enum in `firmware/src/event_logger/event_logger.h` — where a prior session's notes said it lived — and `grep -n "log_type"` returned **nothing**, twice, on a 426-line file that plainly exists. Briefly suspected a broken grep or an encoding problem.

**Root cause:** the enum is declared in `firmware/src/types/app_types.h` (section 3, with the other queue/message types); `event_logger.h` only documents the `LOG_SYSTEM` `value_a` subtypes. The note was wrong about the file.

**Fix:** `grep -rn "LOG_MODE_CHANGE" --include=*.h firmware/` finds it in one step. This is the same lesson as the three false findings from bad greps on 2026-09-11: *an empty grep is evidence about the pattern, not about the codebase* — when a grep for something you are sure exists comes back empty, the search is wrong before the tree is.

## 2026-09-12 — a semicolon inside a C comment truncates any "split on `;`" tool

**Problem:** The 2.5.0 pre-flight check that expands `LIMITS_JSON` and `json.loads` it failed with `Expecting property name ... char 624` — the JSON stopped dead after `"ap_timeout"`, exactly where a newly added comment block sat. Looked like the eleven new entries had not been added.

**Root cause:** the checker did `split("static const char LIMITS_JSON[] =")[1].split(";", 1)[0]` to grab the initialiser, and only *then* stripped comments. The new comment contained the prose "...nothing for app.js; it is the documented contract" — so the split cut the body at that semicolon. The firmware was correct; the C compiler strips comments first.

**Fix:** strip comments **before** splitting on any C token. The prose semicolon was also changed to a full stop, so the next naive tool does not trip on it either.

**Rule:** *when parsing C from a script, remove comments as step one — anything you split on can legally appear inside one.*

## 2026-09-12 — a file named "single source of truth" was the newest and the wrongest statement of a bound

**Problem:** gh#57 recorded `poll_interval` as a stand-off: FR-S03 and FR-CF07 (both "Must") say 15–120 s, `cfg_limits.h` says 30–300, so "either amend the FRS or change the code". Filed as a decision for the operator. It was not a stand-off at all.

**Root cause:** the search stopped at the config layer. `sensor_poll.cpp` — the task that actually sets the cadence — defines `SP_POLL_MIN_S = 15` / `SP_POLL_MAX_S = 120` and clamps to them on every loop pass, four lines above the `vTaskDelay`. The TSDS states 15–120 in five places citing FR-CF07. The manual's own advice lines say 15–30 short / 60–120 long. `git log -S` on the constants shows `cfg_limits.h` was created whole in v1.16.25 (2026-05-07) while `SP_POLL_MIN_S = 15` already existed in that commit's parent. The header of `cfg_limits.h` claims to be the "single source of truth for all integer config parameter bounds", and that claim is exactly what made it look authoritative.

**Consequence of believing it:** a stored 300 was accepted, reported back by `/api/config`, written to the audit log — and polled at 120. Worse, T5 sizes the averaging window from the **raw** shadow value (`poll_s_cfg`), not the clamped one, so a 6-minute window at a stored 300 became `(6*60)/300 = 1` sample: averaging silently gone, which is the noise rejection FR-S06 asks for.

**Fix:** `cfg_limits.h` narrowed to 15/120 in 2.5.1, which also makes the clamped and raw variables identical over the whole legal range. Verified on FDA4: 15 s stored and the SD `SENSOR_HR` cadence measured at eight consecutive 15 s gaps, with no reboot.

**Rule (promoted):** *a bound in a config/limits table is a claim, not the truth. Before trusting it, check the consuming task for its own clamp and the FRS/TSDS for the requirement — and when they disagree, `git log -S` the constants to see which one drifted. A config bound WIDER than the consumer's clamp is a silent lie, not a harmless slack.*

## 2026-09-12 — a poll-cadence change cannot be measured until the in-flight sleep drains

**Problem:** After setting `poll_interval = 15`, a check waited 100 s and asserted the last four `SENSOR_HR` gaps were ~15 s. It failed: the gaps read `[..., 31, 76, 31, 15]`.

**Root cause:** T5 reads the interval at the **top** of its loop and only then sleeps (`poll_s = dm_get_poll_interval_s()` at `:404`, `vTaskDelay` at `:408`). A change therefore takes effect after the currently in-flight sleep completes, so the worst-case latency is one whole **old** interval. The clamp tests immediately before had set 120 s twice, so a 120 s sleep was in flight and the measurement window straddled the transition. The firmware was correct; the 76 s and 120 s gaps are the old cadence draining.

**Fix:** wait out a worst-case old interval *plus* several new cycles before measuring, then assert on the tail only. Re-run gave `[15, 15, 16, 15, 15, 16, 15, 15]`.

**Also learned the same run:** a verification that sleeps more than `session_timeout` (default **5 min**) loses its cookie and starts getting `401 no_session` mid-script. Long-running harnesses need a 401 retry that re-logs in, not just an `HTTPError` body parse.

## 2026-09-12 — the documented subtype table was three entries behind, and an issue was filed on its authority

**Problem:** gh#59 asked for six new `LOG_SYSTEM` subtypes and stated *"Next free `LOG_SYSTEM` `value_a` subtypes are 22 and upward (the documented table in `event_logger.h` runs −1 and 0–21)"*. Implementing that verbatim would have given the six new events subtypes 22–27, silently colliding the first three with ROTA.

**Root cause:** 22 = ROTA check, 23 = ROTA download/verify, 24 = ROTA apply have existed since firmware 2.2.0. They were never added to the table in `event_logger.h`, and they are invisible to the obvious grep because they go through a helper — `audit_row(22, sub)` inside `ota_client.cpp`, not `ev.value_a = 22`. `logparser.py` decoded them the whole time, so the parser was ahead of the firmware's own documentation.

**Fix:** the occupied set was re-derived from the emitters plus the parser before anything was assigned; the new subtypes start at 25. `event_logger.h` now documents 22–30 and carries a note that the emitters are authoritative and this comment is not.

**Rule (promoted):** *a "documented encoding table" in a header is a claim about the past. Before consuming a free slot in ANY enumerated space — log subtypes, param ids, NVS keys, bit positions — derive the occupied set from the emitters AND from every consumer, and remember that values passed through a helper function will not match a grep for the literal assignment.* Same shape as the 2026-09-11 lesson that an empty grep is evidence about the pattern, not the codebase.

## 2026-09-12 — three verification failures in one day, all of them the harness not waiting

**Problem:** Across 2.5.1 and 2.6.0, five assertions failed against correct firmware:
- a config read-back straight after `POST /api/config` returned the *previous* value (13 false FAILs), because the write goes through Q4 and T4 applies it a loop later;
- a poll-cadence measurement taken 100 s after setting 15 s read `[..., 31, 76, 31, 15]`, because T5 reads the interval at the **top** of its loop and only then sleeps, so a change lands after the in-flight sleep drains — worst case one whole **old** interval;
- `mode == AUTOMATIC` and `eg1 == 0` checked 12 s after leaving STANDBY, while the `CMD_RECALIBRATE` sweep that leaving STANDBY triggers was still running with `EG1_BIT_CALIBRATING` set.

**Root cause:** every one is a queue or a state machine between the request and the observable, and in each case the HTTP 200 means *accepted*, not *in effect*.

**Fix / rule:** *before asserting on an effect, name the mechanism that produces it and wait for that mechanism, not for a round number of seconds.* Q4 writes need a settle or a poll-until-stable; a cadence change needs one whole old period; leaving STANDBY needs the recalibration sweep to clear `eg1`. And make the expected transition observable — if the value you expect already equals the current value, move it to a different base first, or a broken implementation and a working one read identically.

**Related:** the mirror-image failure, a harness reporting false PASSes because it did not JSON-parse `HTTPError` bodies (2026-09-11), and one reporting a false FAIL because it never status-checked its second fetch of the same resource (2026-09-12).

## 2026-09-12 — `rota_release.py release` warns "working tree has uncommitted changes" on UNTRACKED files

**Problem:** The 2.6.0 publish printed

```
warning   : working tree has uncommitted changes; the release tags HEAD (a8226e7e),
            which may not match these artefacts.
```

on a tree where `git status --short` showed **zero** modified tracked files. Taken at face value the warning says the published binary may not correspond to the tag, which for a release that field units pull is the one thing that must not be true.

**Root cause:** the check is coarse — it looks at `git status` broadly, so untracked files trip it. Three were present: `bin/2.6.0/manifest-2.6.0.json`, which the release run itself had just authored, and `firmware/sdkconfig.lolin_s3_bench` / `firmware/sdkconfig.lolin_s3_mbprobe`, which belong to the bench and mbprobe environments and are not inputs to the `lolin_s3` release build. So the warning was a false positive, and it will fire on essentially every release, because the tool always writes a new untracked manifest before checking.

**How it was settled, rather than assumed:** `git status --short | grep -v '^??'` showed no tracked modification, so every tracked input to the build matched the tagged commit. Then the release was verified independently against the public API: the tag dereferences to the committed HEAD, the release is neither draft nor prerelease, and the firmware and asset zip were **re-downloaded and hashed** equal to the local files (12/12).

**Rules:** *don't dismiss this warning and don't trust it either — resolve it.* `git status --short | grep -v '^??'` answers the question the warning was trying to ask. And *after any outward-facing publish, verify the artefact from the outside*: a hash of the re-downloaded file proves both the upload and the tag, which no amount of local checking can.

**Worth fixing in the tool:** the dirty check should consider tracked modifications only, and ideally run before it writes the manifest. Until then the warning carries no signal.
