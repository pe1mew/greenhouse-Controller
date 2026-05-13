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
