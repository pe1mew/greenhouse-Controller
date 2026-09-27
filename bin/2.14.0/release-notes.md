# Release 2.14.0

**Date:** 2026-09-25
**Built on:** 2.13.0 — **which was never published on its own, so everything in it ships here**: linear control that starts by itself (gh#86), the reason line on the Motors card (gh#85), `Lineair` as the factory default and `graded` as the chosen law. See `bin/2.13.0/release-notes.md`; its soak passed on 2026-09-26
**Fixes:** [gh#67](https://github.com/pe1mew/greenhouse-Controller/issues/67) — four config keys that could be written and never read; [gh#88](https://github.com/pe1mew/greenhouse-Controller/issues/88) — temperatures between −0.9 and −0.1 °C lost their minus sign

**Minor**, because four keys leave the configuration contract: they are no longer accepted by
`POST /api/config` or published by `GET /api/config/limits`. There is **no control-path change**, no
log-encoding change, and the status LED behaves exactly as before on any unit that kept the defaults.

> **Verified on hardware and soaked** on 2344 (2.14.0-bench, 2026-09-26): gh#88 and gh#67 on the rig,
> the regression suites, and a **12.25 h soak in mode 2 with every fault counter at 0**. The published
> image is the release build of the same commit (`fac549e`). See *Verification*.

## What changed

### A frost reading keeps its minus sign (gh#88)

Between −0.9 and −0.1 °C the controller reported temperatures without their sign: −0.5 °C
went out as `0.5`. That reached the public status site, the local GUI's temperature tiles, the
WebSocket push, and the sensor history. From −1.0 °C down the sign was always right, which is
why it went unseen — the error is confined to the band just below zero, which is the band a grower
most wants to read correctly on a frost night.

The cause was `v / 10` for the whole part and `|v| % 10` for the fraction. C division truncates
towards zero, so for −5 the whole part is 0 and the only carrier of the sign is gone. The history
endpoint's code was a copy of the status builder's — its own comment points there — so the defect
lived in two places. Both now use one helper, `firmware/src/types/fmt_tenths.h`, which prints the
sign as its own field and splits the magnitude in unsigned arithmetic, so no division ever sees a
negative number. Zero prints `0.0`, never `-0.0`.

Not affected, checked: the LCD prints whole degrees with `%3d` on a signed int, and
`log/logparser.py` uses Python's true division.

### The status LED's settings are constants

`led_day_brt`, `led_nite_brt`, `led_nite_from` and `led_nite_to` set the heartbeat NeoPixel's brightness
and its night window. Since the Phase 0 scaffold they had been config keys that could be **written**
through `POST /api/config` and were **advertised** by `/api/config/limits` — yet nothing could read them:
no GUI control, no LCD menu, no field in any response, no audit row. A setting you could change and never
see, verifiable only by looking at the LED.

gh#67 first proposed making them readable. The operator asked the better question — *they are only used
internally, so why are they on the external interface at all?* — and checking bore it out: no UI anywhere,
one consumer (T1, `watchdog.cpp`), never tuned. So they became what they always were in practice:

| Constant (`watchdog.cpp`) | Value |
|---|---|
| `LED_DAY_BRT` | 200 / 255 |
| `LED_NITE_BRT` | 20 / 255 |
| `LED_NITE_FROM` | 22 (local hour, inclusive) |
| `LED_NITE_TO` | 6 (local hour, exclusive) |

These are the previous defaults, so an unchanged unit sees no difference. T1 no longer takes a
configuration snapshot at all.

### FR-CF14 withdrawn

FR-CF14 said an administrator *should* be able to configure the night schedule and brightness. It was
never met — the TSDS claimed the System tab and the keypad offered it, and neither did — and it is
withdrawn by the same decision. FR-UI21 (dim the LED at night) is still met.

### The guards were tightened, not loosened

- **`bin/check_cfg_desc.py`** carried an exemption for exactly these four keys: "published but never
  returned". It is gone, so that rule is an error again for **every** key. Its comment had predicted
  this: *"when gh#67 is fixed this set empties and the rule tightens by itself."*
- **`bin/gen_cfg_desc.py`** re-derives the descriptor from the frozen pre-gh#64 tables and reported any
  missing migrated key as drift — correct for an accident, wrong for a decision. It now has a declared
  `RETIRED` list, and **refuses an untrue entry in either direction**: a "retired" key still in the
  table, or one the frozen tables never had.
- **`model/closedloop/settings.py`** raises at import if it names a key the descriptor no longer has, so
  its four `led_*` rows went in the same change — otherwise every simulator script on `main` would have
  failed on import.

## Verification

| Check | Result |
|---|---|
| `python bin/check_cfg_desc.py` | **PASS** — 51 keys, 40 published; bounds, shadow fields and mock agree, with **no exemptions** |
| `python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check` | **PASS** — reproduces all 47 migrated rows; 4 added since, 4 retired on purpose |
| Simulator settings import | **PASS** — `settings.py` imports with 51 keys |
| Fail-first: the derivation guard still catches a real removal | **CAUGHT** — deleting `K_POLL_INTERVAL` reports `MISSING ... DRIFT`, exit 1 |
| Fail-first: the config checker still catches a published-but-unreadable key | **CAUGHT** — publishing `log_upload_h` reports "GET /api/config never returns", exit 1 |
| Fail-first: a bogus `RETIRED` entry is refused | **CAUGHT** — exit **1** (checked without a pipe; a pipe had first reported `tail`'s exit 0) |
| Release and bench images build | **yes** — no errors, no unused warnings, no format warnings from the two gh#88 sites. RAM 19.9 % (−16 B: the four shadow fields), flash 67.3 % |
| gh#88 host test — the REAL `fmt_tenths.h` | **PASS** — compiled with MinGW g++ 14.2, `-Wall -Wextra -Wformat=2 -Werror`, through a printf-checked wrapper shaped like `status_json.cpp`'s `append()`. Every value from −60.0 to +60.0 °C plus the int16/int32 extremes (1 214) prints exactly, checked by parsing the text back to tenths, and none prints `-0.0` |
| gh#88 fail-first | **the old formula fails exactly where gh#88 says** — wrong on 9 values, all of them −0.9..−0.1, and on nothing else in range; −0.5 printed as `0.5` |
| **gh#88 on hardware** | **PASS** — 2344, 2.14.0-bench, 2026-09-26, with the rig's emulated T/RH sensor set to −0.5 °C (http://sensor-emulator.local/). Read as **raw text**, since a parsed number would hide the defect: `/api/status` gave `"temp_c":-0.5`, then `"temp_avg_c":-0.5` once the average came down into the band, and the newest of 60 `/api/history` rows gave `temp_c=-0.5`. The old formula sends `0.5` at all three |
| **gh#67 on hardware** | **PASS** — 2344, 2.14.0-bench: `/api/config/limits` publishes 40 keys and none of the four `led_*`; `POST /api/config` with `led_day_brt` or `led_nite_from` returns **400**; `GET /api/config` carries none. **AT-CFG64: 46 of 46 Q4-writable keys round-trip to their own field, and no key is unreadable** — before 2.14.0 the four `led_*` were the only fields the test could not reach |
| Regressions on 2.14.0-bench | **PASS** — gh#86 (`at_wp_rest_promote.py`): Lineair 10 s after the boot, M3 never moved; the 2.10.0 confirm suite 9/9 in mode 1, then Lineair restored in 33 s; 9/9 rig settings |
| **Soak on 2344, in mode 2** | **PASS** — 2026-09-26 10:55 to 23:10: **12.25 h, 28 judged strokes**, `stall_faults` / `early_stops` / `rejected_rate` / `err_comm` / `not_reached` all 0, `mode_changes` 0, `orphan_aborts` 0, gate settled at `position`, **no reboot** (13.46 h uptime spans the window). All 12 scripted stroke sessions ran, none skipped or failed: each opened M3 to 22–26 % under Lineair and closed it after mode 2's 600 s interval, the last two on the night setpoint. The 14 closes were confirmed; the 14 targeted openings are `not_judged` by design, since a drive to an aperture has no end to confirm. The rig's emulated T/RH and wind followed 5C88's own readings (emulator in REST, fed from the status site), so the soak ran on production's day and night climate. Heap: free 65 KB, largest block 20 KB at 13.5 h uptime (over the window: free 39–67 KB, largest 17–22 KB); the since-boot floor stepped 20 → 8 KB by 14:09 and did not move again |
| **ROTA pull on 2344** | **PASS** — published as seq 57 at 23:12; the channel offered 2.14.0 to a forced device check at 23:21. Plain 2.12.2 was pushed at 23:24, and 2344 then pulled 2.14.0 by itself: the **apply committed at 01:03:36** on 2026-09-27 (its apply window is 01:00–23:00), after which `fw_ver` **and** `asset_version` both read **2.14.0** and every hourly check reads up to date. **The first three downloads failed** (23:24:40 SHA-256/size mismatch; 23:24:41 and 23:36:24 TLS/transport). The unit's own check at 00:34 then downloaded and verified both artefacts in 13 s, deferred the apply to the window, and at 01:03 downloaded again (gh#71) and applied. All 9 rig settings survived |

## Upgrading

- **Existing units:** four NVS entries may remain from earlier firmware. Nothing reads them, and they are
  harmless. No migration.
- **A unit that stored non-default LED values reverts to the constants.** There is no way to tell whether
  any unit has them — they were unreadable and unaudited, which is the defect this release removes.
- **Clients:** a client writing any `led_*` key now receives 400. None is known: there was no GUI control.

## Known limitations

- The night window is fixed hours (22:00–06:00), not sunset-to-sunrise. Following the controller's own
  day/night would track the seasons but is a behaviour change nobody asked for.
