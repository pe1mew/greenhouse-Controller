# Release 2.11.0

**Date:** 2026-09-20
**Built on:** 2.10.1
**Implements:** [gh#77](https://github.com/pe1mew/greenhouse-Controller/issues/77) — teach the M3 position sensor on release firmware

**Minor.** A surface that existed only in bench builds is now in every build. No config key, no NVS change, no log-encoding change, and nothing in the ventilation path.

> **2.10.1 was committed but never published**, so its three fixes ship here: the LCD self-heal (gh#80), the greyed-block reason (gh#74) and the mock's `deadzone_m3` (gh#76). See `bin/2.10.1/release-notes.md`.

## What changed

### Commissioning works on a release build (gh#77)

Teaching the sensor used to need a bench image on the unit. `GET`/`POST /api/diag/commission` — window size, teach, abort, calibration verdict — were compiled only with `MODBUS_BENCH`, so on release firmware the GUI's *Commissioning* card was greyed with "teaching the sensor needs a bench build". **When 5C88's encoder is fitted, it would have had to be taught that way, on site**, with a build that also opens the arbitrary Modbus write route.

Now:

- **The two routes are in every build, and stay admin-only.** They touch only the sensor's own registers, through the windowPos driver.
- **`commission.cpp` is compiled into every build**, rather than being an empty translation unit in release.
- **T17's commissioning hooks moved with them**, which is the part the issue did not name and the reason the routes alone would not have worked: `commission_tick()` drives the teach from T17's readings, `commission_refresh()` judges the calibration when the sensor gate opens, `commission_wants_prompt_read()` raises the idle sampling rate while a teach runs, and `commission_owns_teach()` keeps T17 from aborting a teach the commissioning path owns. All were bench-gated. Without them a release build would have armed a teach that never progressed, and reported *verdict UNKNOWN, window size 0* beside a live "Teach (moves M3)" button.
- **What stays bench-only:** `/api/diag/modbus` (arbitrary Modbus writes), `/api/diag/windowpos` (the direct sensor read and the soak counters) and `/api/diag/lcd` (gh#80's injector).
- **Unchanged:** the gh#73 refusal when no sensor is fitted, the teach's STANDBY hold until the admin session ends (gh#65), and the abort path.
- **The GUI's 404 reason is rewritten.** A 404 is now a fault on any firmware, and the text says so while naming the version, because firmware older than 2.11.0 answers 404 on a release build legitimately.

### Consequences worth knowing

- **A release build now costs two extra Modbus transactions per sensor-gate opening**, the `commission_refresh()` read. `bin/at_wpos_fitted.py`'s `SWITCH_ON_TXN_MAX` of 6 already allowed for them on a bench build; the comment there now records that both builds cost 6 and the limit no longer tells them apart.
- **The calibration verdict is populated on a release build from the first gate opening**, where it used to stay UNKNOWN for ever.

## Size

|  | 2.10.1 | 2.11.0 | delta |
|---|---|---|---|
| app image | 1 395 728 | **1 402 256** | **+6 528 B** |
| `.flash.text` | 940 302 | 944 954 | +4 652 B |
| `.flash.rodata` | 310 716 | 312 572 | +1 856 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 216 | 42 312 | +96 B |
| web assets | 147 547 | 147 458 | −89 B |

The bench image is 1 411 504 B, unchanged: it always carried this code.

## Verification

**On 2344, running the 2.11.0 RELEASE image** (no `-bench` suffix), which is the build that could not do any of this before:

| Check | Result |
|---|---|
| `GET /api/diag/commission` | **200**, with the real calibration: `verdict valid`, `window_mm 1500`, taught 0…852, `span_pct 83` |
| `POST /api/diag/commission` with an unknown action | `ok:false, unknown_action` — the route is live and validating |
| `GET /api/diag/windowpos` | **404**, as a bench-only route should be |
| `POST /api/diag/modbus` | **404**, likewise |
| The GUI's *Commissioning* card | **Not greyed**: `#cm-body` opacity 1, no reason text, verdict **VALID**, window size 1500 mm, with *Apply*, *Teach (moves M3)* and *Abort* enabled |

- **The first attempt failed in a way worth recording.** With only the routes moved, the release build answered 200 but reported `verdict unknown, window_mm 0, span 0` and never changed, because `commission_refresh()` was still bench-gated. That is what sent me looking for the rest of the hooks.
- **A full teach ran on the release build** (2344, 11:28:37, operator's go). From CLOSED: two legs, `done` after 33 s, **verdict valid**, and the fresh span landed at 863 against the previous 852 — 1.3 % apart, which is the self-consistency the design claims for a teach against a known distance. `teach_armed` cleared by itself.
  - **The STANDBY hold behaved** (gh#65): `standby` was set while the teach ran and released when the admin session ended, after which T2's recalibration swept the windows closed and the unit returned to automatic with no flags.
  - The teach runner itself is the code the bench build has run repeatedly (`bin/at_wp_teach.py`, `at_wp_teach_standby.py`); what changed is which builds compile it.

## Upgrading

- **No partition change, no NVS migration, no configuration change.**
- **The web assets change** (the 404 reason text), so firmware and assets go together as always.
- **What an operator gains:** a sensor can be taught on the firmware the unit already runs. No bench image, and therefore no window during which the arbitrary Modbus write route is open on a production unit.
- **On the ROTA soak channel since 2026-09-20, seq 54** ([v2.11.0](https://github.com/pe1mew/greenhouse-Controller/releases/tag/v2.11.0)). **2344 pulled it.**
  - 2.10.0 was pushed back to 2344 first, because it was already running the 2.11.0 release image from a direct push.
  - Published 11:38:57. The first forced check, at 11:39:31, was **skipped**: the clock had not re-synced after the push, and T16 will not sign a request on an untrusted clock (gotcha log, 2026-07-13).
  - **The first download failed `dl` 2, a SHA/size mismatch, and the next one succeeded.** The published artefacts are not at fault: the manifest's hashes and sizes match the local files exactly. The check at 11:41:32 came about 2.5 min after publishing, where earlier releases took ~11 min to be offered, so the server had pointed the channel while still fetching the artefacts. A retry a minute later verified cleanly (`dl` 0). **Worth knowing before forcing checks immediately after a release.**
  - The unit rebooted into it at about 11:42:40. **Verified after the reboot:** `fw_ver` and `asset_version` both 2.11.0, `travel_m3` 13, `wpos_fitted_m3` 1, `deadzone_m3_mm` 20, no flags — and `GET /api/diag/commission` on the **pulled** release build answers 200 with the calibration this release's teach wrote (valid, 1500 mm, span 863).
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting, and record the heap figures the way TC-09 now asks (plan row 5.8).

## Known limitations

- **One teach has run on release firmware** (see Verification), on the rig's 13 s window. Production's traverse is 176 s, so a teach there takes minutes per leg and should be watched the first time.
- **Commissioning is still admin-only and still refuses when no sensor is fitted** — that is deliberate, not a limitation, but it means a unit whose `wpos_fitted_m3` is 0 shows the card greyed with the gh#73 reason.
- **The LCD offers no commissioning surface.** Teaching is a web-GUI operation.
- **This release carries 2.10.1's fixes**, whose own known limitations still apply, including that the cause of the original LCD corruption is unknown.
