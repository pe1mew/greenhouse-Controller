# Release 2.4.3

**Date:** 2026-09-10
**Built on:** 2.4.2
**Advances:** [gh#51](https://github.com/pe1mew/greenhouse-Controller/issues/51) (Groups B and C)
**Opens:** [gh#52](https://github.com/pe1mew/greenhouse-Controller/issues/52) (`nvs_load_mode()` asymmetry)

## Why this release exists

2.4.2 made motor travel and dwell times take effect without a reboot. This release makes that change **visible in the audit log**, and fixes a second instance of the same root cause on the LCD.

Both are prerequisites for the M3 window-control rework (operator, 2026-09-10): that work makes travel timing a commissioning activity, so a travel change has to leave a record, and a factory reset has to actually reset. Patch rather than minor for the same reason — a mandatory correction on the path to that work, not a feature line of its own.

## Added — motor travel time is now audited

`travel_mX` mapped to `LOG_PARAM_NONE`, on the grounds that it was set once at commissioning. The SD log therefore held **no record of travel times at all** — not when changed, not when they took effect. Defensible while the value only applied at reboot; 2.4.2 removed that.

`LOG_PARAM_TRAVEL = 46` carries the motor in `channel` and old → new in seconds, matching `dwell_open` / `dwell_close`. `log/logparser.py` and `log/logparser.md` were updated in the same change, per the standing rule that the parser learns a new encoding alongside the firmware.

## Fixed — the LCD factory-reset screen reported an action that had not happened

IO0 menu case 2 (*"Reset all NVS namespaces + PINs; no reboot"*) erased seven namespaces and displayed **"Settings Reset! / Defaults loaded"**, while nothing reloaded T4's config shadow or T2's cached motor timings. Three sources disagreed afterwards: NVS (erased), the shadow (pre-reset), T2 (pre-reset).

Same root cause as gh#51, broader — climate and wind were stale too — and unlike the travel-time case this one asserts a **completed action**.

New `dm_reload_all_cfg()` re-reads every config namespace under MX4 and notifies T2 and T14. T6 and T3 need nothing; they snapshot once per loop iteration. Because the loaders go through `nvs_cfg_get_*_or_default()`, reading an erased namespace also **writes the factory default back**, which is what makes the claim true rather than merely displayed.

Checked before writing, since it would have deadlocked T4 outright: none of the six `nvs_load_*` helpers or `update_sun_times()` takes MX4 internally — zero references in all seven. MX4 is a plain, non-recursive mutex.

## Fixed — `logparser.py` never learned param 45

`LOG_PARAM_WIND_HYST` shipped in **2.3.0 (gh#46)**; the parser's table stopped at 44. Every `wind_hyst` change logged since has rendered as the bare `param#45`, no name and no unit. Unknown ids degrade rather than crash, which is why it survived three minor releases unnoticed. Found while tracing consumers for the travel-time change.

## Known limitation (gh#52)

`dm_reload_all_cfg()` deliberately does **not** reset the operating mode.

`nvs_load_mode()` only ever *sets* `EG1_BIT_STANDBY` and never clears it — it is written for boot, where the bit starts clear, so after an erase it is a silent no-op. Clearing STANDBY properly means `dm_set_standby(false, ...)`, which posts `CMD_RECALIBRATE` and drives a full CLOSE_ALL sweep: a real three-minute actuation, started from a menu whose entire distinguishing feature is *"no reboot"*, without the operator asking.

So a unit in STANDBY when case 2 is used **stays in STANDBY** while NVS says AUTOMATIC, until the next boot. One bit, documented rather than buried, and tracked in gh#52 with three candidate fixes.

WiFi and MQTT connectivity likewise survive the erase until reboot — those credentials belong to other tasks and are not part of the config shadow.

## Verification

**Group B — verified on FDA4, real SD rows.** Travel and `wind_hyst` were changed over the API in both directions, the day's CSV pulled from the card, and run through the updated parser:

```
2026-09-10T13:40:02,SETPT,WEB,1,46,21,25   ->  travel (M1): 21 s -> 25 s
2026-09-10T13:40:04,SETPT,WEB,1,46,25,21   ->  travel (M1): 25 s -> 21 s
2026-09-10T13:40:07,SETPT,WEB,0,45,1,2     ->  wind_hyst: 1 m/s -> 2 m/s
2026-09-10T13:40:09,SETPT,WEB,0,45,2,1     ->  wind_hyst: 2 m/s -> 1 m/s
```

Both directions, the channel carried, and param 45 confirmed against real firmware output rather than the synthetic rows used during development.

**Group C — verified on FDA4 2026-09-10**, by physical IO0 press released at the `Reset settings?` stage. Eight settings were moved off-default first, because the unit was already at defaults for nearly everything and the test would otherwise have proved nothing. **11/11 reverted at `uptime_s = 12456` (no reboot)**, and T2's cached timings followed: M1's pulse went 36.5 s → **24.4 s** (travel 33 → 21) and M3's went 16.2 s → **174.8 s** (travel 13 → 171). The M3 result is the decisive one — a 158-second change on a channel never touched during setup. Full detail in `design/fixMotorTimingRefresh.md` §4.3.

The reset destroyed three secrets that cannot be read back from the device (WiFi PSK, ROTA HMAC secret, status-post secret). **Capture `/api/config`, `/api/ota/config` and `/api/web` before running this test.** Original steps, for reference:

1. Change a setting off-default and confirm via `GET /api/config`.
2. Hold IO0 → reset menu → stage 2.
3. Without rebooting, `GET /api/config` → every value at its factory default.
4. Serial shows `cfg shadow reloaded from NVS (all namespaces); T2 + T14 notified`, then T2's three `CH%u: travel=… dwell_open=…` lines.
5. Trigger a recalibration (mode standby → automatic) and confirm M1's pulse is back to 26 s.

Step 5 is the one that matters — steps 3 and 4 only prove T4 reloaded, not that T2's cache followed.

**OTA verified** per the standing rule: `fw_ver` **and** `asset_version` both 2.4.3, read post-reboot on FDA4.

## Still open on gh#51

Group D: `cfg_shadow_t.dwell_open_min[]` / `dwell_close_min[]` carry **seconds**, are documented as minutes, and are exposed under those names in `/api/config` (confirmed on live data: FDA4 reports `[300, 300, 1500]`, and 1500 is `CFG_MAX_DWELL_OPEN_S`). `design/logAnalysis.md` states the same values in minutes. Plus six wrong dwell tooltip bounds in `index.html`.

## Upgrade notes

No configuration migration, no new NVS keys, no web-asset changes.

A parser older than 2.4.3 renders the new travel rows as `param#46` — update `log/logparser.py` alongside the firmware.

Soak on FDA4 before promoting to `mainstream` (5C88).
