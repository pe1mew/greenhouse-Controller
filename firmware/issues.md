# Open firmware items

This file is the canonical in-repo TODO list for the greenhouse-Controller
firmware. Add new items at the end of the numbered list. Items that need
outsider visibility (farmer-facing behaviour, public bug reports, RFC-style
design discussions) should go on **GitHub Issues** instead:
[pe1mew/greenhouse-Controller/issues](https://github.com/pe1mew/greenhouse-Controller/issues).
Cross-reference here with `→ gh#N` when an item has a GitHub thread.

The `changelog.md` file is for what's *done*; this file is for what's *open*.

## Status flags

- `(open)`           — to do, not started
- `(in-progress)`    — being worked on this session
- `(blocked: …)`     — needs external input (decision, data, third party)
- `(decision-needed)` — design choice has to be made before implementation
- `~~RESOLVED~~`     — done; include a closing reference (commit SHA, version, gh#N)

## Items

1. `(open)` → [gh#7](https://github.com/pe1mew/greenhouse-Controller/issues/7)
   **Serial-port use causes controller freeze.** When the serial port
   (USB-CDC) is in use the controller freezes; the activity LED stops blinking,
   the watchdog does not kick in, and the system crashes hard. Needs a
   repeatable reproducer first. Suspect: blocking `Serial.write()` when the USB
   host disconnects mid-frame (the `setTxTimeoutMs(0)` in `main.cpp::setup()`
   should prevent this, but evidently doesn't on every code path). With the
   new `esp_reset_reason()` boot log (1.17.27) the next occurrence will leave
   a verdict on the SD log — wait for that to confirm whether it's a panic
   or a task-WDT.

2. ~~`(decision: option b)` → [gh#8](https://github.com/pe1mew/greenhouse-Controller/issues/8)
   **Daily log upload only uploads closed files.**~~ — Resolved in 1.17.28.
   Picked option (b): T14 daily-fallback path now calls a new
   `event_logger_force_rotate()` API before `event_logger_newest_closed()`,
   so a fresh closed file is always available for the daily upload. The
   rotation marker (`SYSTEM,WEB,0,0,6,0`) is the last entry in each
   rotated-out file, documenting why it was closed. Side effect: ~10 days
   of SD-side history at the 10-file retention default — raise
   `SD_MAX_FILES` if you need more.

3. `(open)` → [gh#9](https://github.com/pe1mew/greenhouse-Controller/issues/9)
   **`firmware/data/index.html` placeholder is fragile.**
   The build script stamps `{{ASSET_VERSION}}` → literal version in-place;
   a subsequent commit can capture the literal and lose the placeholder,
   making the View-Source `<!-- web-assets X.Y.Z -->` stamp permanently
   stale until somebody notices. (Already happened once: literal `1.17.7`
   was committed and stayed for ~20 versions; restored to placeholder in
   the 1.17.26→1.17.27 cycle.) Mitigation options:
   - Add a `pre-commit` hook that refuses to commit `firmware/data/index.html`
     unless line 2 is exactly `<!-- web-assets {{ASSET_VERSION}} -->`.
   - Or move the stamp out of `index.html` entirely — read it from
     `/manifest.json` at runtime via fetch() and inject into the page.
     Removes the regression surface but adds a render-time fetch.
   The pre-commit hook is the lower-effort option.

4. ~~`(decision: option a)` → [gh#10](https://github.com/pe1mew/greenhouse-Controller/issues/10)
   **`Archive/images/IMG_*.jpg` blob bloat.**~~ — Resolved 2026-05-13
   with decision (a): leave the photos in history. The .gitignore rule
   added in 1.17.27 prevents further drift; the 25 MB cost is acceptable
   for a single-developer repo. Revisit if/when a second contributor joins
   or the repo gains public visibility.

5. `(open)` → [gh#11](https://github.com/pe1mew/greenhouse-Controller/issues/11)
   **Forward-port boot-reason field to web GUI.**
   Firmware 1.17.27 records `esp_reset_reason()` to the SD log on every
   boot. Make this visible in the local web GUI's Status tab → Clock card
   alongside `Uptime`, e.g. *"Last reboot: TASK_WDT (Tue 03:44)"*. Requires:
   - A new field in `cfg_shadow_t` (or `status_snapshot_t`) capturing the
     boot reason at startup.
   - `dm_status_snapshot()` populates it.
   - `build_canonical_status_json()` emits it as `system.last_reset_reason`
     (string) so both the local GUI and the external dashboard pick it up
     without further work.
   - `index.html` + `app.js` adds the line to the Clock card.

6. `(in-progress)` → [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12)
   **Unexpected reboot — investigation in progress.** The 2026-05-13
   ~03:44 reboot triggered the 1.17.27 diagnostic instrumentation
   (esp_reset_reason logging on SD + serial). The pre-crash SD log ends
   abruptly with a routine SENSOR event — no panic record, no graceful
   shutdown. Note: the SD file dates back to 2026-04-10 but that's T9's
   resume-across-reboots behaviour, **not 33 days of continuous uptime**.
   The integrated firmware has only existed since 2026-05-03 per `git log
   -- firmware/`, so the actual uptime of the build that died is unknown.
   **Currently capturing serial debug output to disk on the dev host**
   so a panic backtrace (if any) is preserved across the next reboot.
   Closes when the root cause is identified and a fix has shipped — no
   fixed-time observation window, since the firmware is too young to
   make "no reboot in N days" claims meaningful. Possibly related to
   [gh#7](https://github.com/pe1mew/greenhouse-Controller/issues/7)
   (serial-port-use freeze) — the current serial capture is itself an
   indirect test of that hypothesis.

7. `(open)` → [gh#15](https://github.com/pe1mew/greenhouse-Controller/issues/15)
   **User-configurable LCD contrast + backlight brightness.** Driver-side
   API `lcd_set_contrast(uint8_t)` shipped in 1.17.33 — pure addition, no
   call sites yet. This issue tracks the full plumbing to NVS-backed
   `cfg_shadow_t::lcd_contrast / lcd_day_brt / lcd_nite_brt`, T1 + T8
   consumption, System-tab sliders in the web GUI, mock-server update,
   manual changes. Estimated ~half-day; decomposes into four natural
   commits (NVS, task consumption, GUI, manuals). Out of scope:
   user-tunable alarm/fault/normal colour palette (safety — would let an
   operator hide the red), ambient-light auto-adjust (needs LDR hardware).

8. `(in-progress — waiting for reproduction)` → [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16)
   **Unit-2 reboots: PANIC + INT_WDT within 3 minutes on 1.17.32.**
   Field unit without an S200 wind sensor connected has rebooted twice
   in quick succession with *different* boot reasons (4 = PANIC, 5 =
   INT_WDT). **Wind protection is disabled on Unit 2** (`wind_prot_en
   = 0`), which rules out the T2-calib × T3 close-all race hypothesis
   — T3 doesn't fire WIND_OVERRIDE when wind protection is off. Top
   remaining suspect: the Modbus-RTU timeout-and-drain path
   (`drivers/modBus/src/modbus_rtu.cpp` lines 151-167), which runs
   four times per poll (2 reads × 2 retries × 200 ms timeout each =
   ~800 ms busy-poll per cycle, every 30 s, indefinitely). Unit 1
   (with serial logging) was reconfigured on 2026-05-13 ≈ 21:30 to
   match Unit 2's NVS and have its S200 made effectively absent via
   an emulator-address change; **manual reboot at 21:30 starts the
   reproduction-watch window — no unprovoked crash on Unit 1 yet**.
   Next action: watch Unit 1's serial capture; the first panic dump /
   INT_WDT trigger that arrives unprompted is the deliverable. Heap
   stable, no corruption, no Q3 overflow on Unit 2 throughout. Related
   to [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12).

9. `(open)` → [gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17)
   **Unique unit identifier derived from MAC.** Today the codebase has
   no concept of unit identity — two physically-different controllers
   running the same firmware produce SD logs that are byte-identical in
   provenance. The current gh#16 multi-unit debugging made the gap
   obvious (we had to fold "Unit 1" / "Unit 2" identity into the
   conversation manually). Proposed: derive a `GH-AABBCC` short ID and
   `aa:bb:cc:dd:ee:ff` full ID from `esp_read_mac()` and surface in:
   SD log preamble line, canonical JSON (`system.unit_id`), `Phase 0
   boot` serial line, LCD page 7, web GUI System tab, T14 status-POST
   body. Immutable (factory-burned MAC, no NVS, survives factory reset).
   Out of scope: user-typed friendly name, OTA signature pinning, fleet
   management. ~half-day effort.

10. ~~`(decision: shipped 1.18.0)` → [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)
    **Bulkhead policy: secondary network activity must not affect
    primary climate control.**~~ — All four phases shipped 2026-05-14.
    Phase 1 (1.17.34): HTTPS hardening + visibility scaffold (static
    `WiFiClientSecure`, connect/response timeouts split, keep-alive,
    breaker struct refactor, `net_backoff_active` JSON flag + web GUI
    badge). Phase 2 (1.17.35): persistent circuit breaker with 60 s →
    5 min → 30 min → 1 h backoff schedule and 3-fail / 5-ok hysteresis,
    NVS-persisted across reboot. Phase 3 (1.17.36): NVS-persisted
    window state (`CH_CLOSED` / `CH_OPEN`) with "write UNKNOWN before
    energising" invariant; boot calibration skipped when all three
    channels recovered CLOSED (saves up to 171 s climate-control
    outage). Phase 4 (1.18.0): T15 supervisor task with wedge / heap-
    leak / respawn-storm detection, force-respawn of T14 via
    `status_post_force_teardown()` + `vTaskDelete` + recreate,
    escalation to planned reboot when budget exhausted, LCD "BK"
    badge on page 3. Total bulkhead delta over 1.17.33 baseline:
    ~12 KB flash, +96 bytes RAM, 11 new NVS slots. Known limitation
    documented in 1.18.0 changelog: hard faults inside ESP-IDF /
    mbedTLS / lwIP on single-chip architecture cannot be intercepted —
    policy makes them bounded, not eliminated. Log-upload retention
    preserved as a future option. Related to
    [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16).

11. `(open)`
    **Document the WDT-subscriber design rule.** 1.18.0 shipped T15 with
    a `vTaskDelay(30 000)` between `esp_task_wdt_reset()` kicks. The
    default task-WDT timeout is 5 s, so T15 starved the WDT on every
    iteration and the chip TASK_WDT-reset every ~5–8 s of uptime until
    OTA rolled back to the previous bank. 1.18.1 fixed the bug with the
    chunked-wait pattern already used in `calib_close_all()` (since
    1.17.29). The lesson should be captured as a written invariant:
    *any task that subscribes to the task WDT and has a blocking call
    longer than the WDT timeout MUST break the wait into chunks of
    ≤ `WDT_timeout / 2` and kick the WDT each chunk.* Add to
    `design/tasks.md` (or a new `design/task_design_rules.md`) and
    cross-link from the 1.17.29 hardening section in `changelog.md`.
    Small-scope follow-up: ~30 minutes.

## Closed (most recent first)

- ~~Tier-1/2 hardening + 5 MB streaming refactor~~ — resolved in 1.17.29
  (four phases: compile-flag warning surface, `pio check`/cppcheck config,
  T1 heap-free + integrity + stack-HWM instrumentation + WDT subscription
  for 8 tasks, streaming `SDFileChunkedStream` adapter replacing the 5 MB
  PSRAM allocation in T14). Tracked as
  [gh#13](https://github.com/pe1mew/greenhouse-Controller/issues/13); closed
  2026-05-13.
- ~~LCD Wind status row 2 missing space after `Dir:`~~ — resolved in 1.17.26
  (one-character format-string fix). Tracked as
  [gh#6](https://github.com/pe1mew/greenhouse-Controller/issues/6); closed
  2026-05-13.
- ~~NTP resync overflow — "24 h" actually firing every 8 min~~ — resolved
  in 1.17.27 (`pdMS_TO_TICKS` overflow inside the macro; computing tick
  count directly avoids it). Documented in `changelog.md` [1.17.27].
- ~~No `esp_reset_reason()` boot log~~ — resolved in 1.17.27 (`main.cpp`
  setup posts a `LOG_SYSTEM, value_a=5` event with the reason code on
  every boot). Documented in `changelog.md` [1.17.27].
- ~~Daily-fallback silently no-ops when no closed file exists~~ — resolved
  in 1.17.27 (slot now emits `SYSTEM,WEB,0,0,0,2` or `…,0,3` so the SD log
  records the fact). Built the diagnostic floor; the design question above
  it (item 2 below at the time) was resolved in 1.17.28.
- ~~Daily log upload: should we force-rotate at the daily slot?~~ — resolved
  in 1.17.28 (option (b): force-rotate then upload). Tracked as
  [gh#8](https://github.com/pe1mew/greenhouse-Controller/issues/8); closed
  2026-05-13.
- ~~Archive/images/*.jpg blob bloat — keep or scrub?~~ — resolved
  2026-05-13 with decision (a): leave the photos in history; the .gitignore
  rule added in 1.17.27 prevents further drift. Tracked as
  [gh#10](https://github.com/pe1mew/greenhouse-Controller/issues/10); closed
  with no code change.
