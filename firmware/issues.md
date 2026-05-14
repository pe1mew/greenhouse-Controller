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

7. `(open — investigation 2026-05-14: I2C contrast register confirmed
   updating but not driving visible V0 on this hardware; UI feature
   reverted)` → [gh#15](https://github.com/pe1mew/greenhouse-Controller/issues/15)
   **User-configurable LCD contrast + backlight brightness.** Driver-side
   API `lcd_set_contrast(uint8_t)` shipped in 1.17.33 — pure addition, no
   call sites yet. This issue tracks the full plumbing to NVS-backed
   `cfg_shadow_t::lcd_contrast / lcd_day_brt / lcd_nite_brt`, T1 + T8
   consumption, System-tab sliders in the web GUI, mock-server update,
   manual changes. Estimated ~half-day; decomposes into four natural
   commits (NVS, task consumption, GUI, manuals). Out of scope:
   user-tunable alarm/fault/normal colour palette (safety — would let an
   operator hide the red), ambient-light auto-adjust (needs LDR hardware).

   **Investigation 2026-05-14 (not committed):** an LCD-only
   implementation (1.18.4 + 1.18.5 driver hotfix) was built, deployed,
   and tested on Unit 1 (`debug/unit1/1.18.5/20260514_055501.log`). The
   firmware path is provably correct — `[T8_UI] contrast → N` log lines
   appear for every A/B press across the full range 12–52, every
   `lcd_set_contrast()` returns LCD_OK, no MX1 timeouts — but the
   visible LCD contrast does not change. Hardware-side bottleneck: the
   chip on the unit's LCD module either (a) has V0 overridden by an
   external trimmer pot, or (b) is a different controller that ACKs but
   doesn't implement the AiP31068L IS=1 extended-instruction-set
   contrast register. Physical inspection of the LCD module's back side
   is the next step. Per the operator's decision, the 1.18.4 / 1.18.5
   work has been reverted; gh#15 returns to its original open state
   pending hardware identification.

8. ~~`(RESOLVED — gh#16 closed 2026-05-14; no proven root-cause, but
   structurally bounded by gh#18)` → [gh#16](https://github.com/pe1mew/greenhouse-Controller/issues/16)
   **Unit-2 reboots: PANIC + INT_WDT within 3 minutes on 1.17.32.**~~
   The original Modbus-RTU busy-poll hypothesis never accumulated
   evidence; multiple observations counted against it (heap stable, no
   corruption, Q3 fine, wind protection already disabled). The crash
   interval (~2 min) instead correlated strongly with the
   `status_interval_s` default of 120 s — the secondary HTTPS /
   mbedTLS / WiFiClientSecure / lwIP chain triggered by every status
   POST. That failure surface is exactly what gh#18 (bulkhead policy,
   closed) addresses: TLS reuse + 3 s connect timeout (1.17.34),
   persistent circuit breaker with 60 s → 1 h escalation (1.17.35),
   ~2 s recovery from planned reboot via NVS-persisted window state
   (1.17.36), T15 supervisor (1.18.0). Heap-fragmentation diagnostic
   blind spot closed by `LOG_SYSTEM value_a=12` row in 1.18.2 (gh#20).
   Forensic question (which exact code path triggered the original
   reboots) unresolved and probably unresolvable from the evidence
   we have; operational question (what should the firmware do when
   this class of fault occurs) answered structurally. Remaining
   verification tracked on gh#20: deploy 1.18.2 to Unit 1 (24 h
   soak) then Unit 2. Related to
   [gh#12](https://github.com/pe1mew/greenhouse-Controller/issues/12).

9. ~~`(RESOLVED — gh#17 closed 2026-05-14, shipped 1.18.3)` →
   [gh#17](https://github.com/pe1mew/greenhouse-Controller/issues/17)
   **Unique unit identifier derived from MAC.**~~ Shipped in 1.18.3 with
   the **2-byte short form** (last 2 MAC bytes, 4 hex chars) matching the
   existing `Greenhouse-XXXX` AP-SSID convention. Surfaced on four
   channels per the gh#17 evaluation: (a) Phase 0 boot serial line
   `id=AABB`, (b) `LOG_SYSTEM value_a=11` emitted once at boot by T4 +
   once at every SD-rotation by T9, (c) `system.unit_id` in canonical
   status JSON, (d) AP SSID (pre-existing). 16-bit collision rate <1%
   for ≤30-unit fleet; deterministic 0% within a single procurement
   batch (sequential MAC allocation). Upgrade path to 24-bit ID
   documented inline in `firmware/src/system_id/system_id.h` if fleet
   ever exceeds ~50 units. **Out of scope** (per the original gh#17
   "out of scope" list, kept out of scope here): LCD page 7 surfacing,
   web GUI System tab tile, full MAC string, user-typed friendly name,
   OTA signature pinning, fleet management.

10. ~~`(RESOLVED: gh#18 closed 2026-05-14)` → [gh#18](https://github.com/pe1mew/greenhouse-Controller/issues/18)
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

11. ~~`(RESOLVED — gh#19 closed 2026-05-14)` →
    [gh#19](https://github.com/pe1mew/greenhouse-Controller/issues/19)
    **Document the WDT-subscriber design rule.**~~ Rule written into
    `design/tasks.md` §6 *Watchdog-subscriber discipline* (new design
    note alongside "T4 as single source of truth", "T2 as sole relay
    owner", etc.). Contains the invariant, a table of reference
    implementations (T2 `calib_close_all`, T2 `handle_alarm_clearance`,
    T15 supervisor poll), the 1.18.0 cautionary example with the
    forensic boot-row trace, and the rationale for why a written rule
    beats code review for this class of bug. `changelog.md` [1.17.29]
    "WDT subscription" bullet cross-linked to the new rule + to gh#19.
    Optional `pio check` lint deferred to future work (documented in the
    new design note as "tracked on gh#19" — but closing gh#19 anyway
    since the must-have deliverable is the rule itself, not the lint).

12. `(open — shipped 1.18.2, awaiting Unit-1 24 h soak)` → [gh#20](https://github.com/pe1mew/greenhouse-Controller/issues/20)
    **1.18.2 defensive pass: platform pin + heap-fragmentation probe +
    TLS audit.** Three small additions triggered by the mbedTLS research
    thread on gh#18: PlatformIO `platform = espressif32@6.12.0` (was
    unpinned — drift risk eliminated); new `LOG_SYSTEM value_a=12, value_b=KB`
    row every 60 s recording `heap_caps_get_largest_free_block` (closes
    Phase 4's fragmentation blind spot — see arduino-esp32 #7884 / #4523);
    `design/tls_leak_audit.md` static-source audit of `WiFiClientSecure::
    stop()` against #3808 and of TLS 1.3 status against esp-idf #8515
    (verdict: Phase 1's static-client pattern correctly dodges both).
    Supervisor integration of the largest-block trigger is a deliberate
    follow-up — needs one field-capture session to set the threshold
    empirically. Close when Unit 1 has run 1.18.2 for 24 h and the first
    fragmentation baseline is observable in the log. See gh#16 verification
    plan for the deployment sequence.

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
