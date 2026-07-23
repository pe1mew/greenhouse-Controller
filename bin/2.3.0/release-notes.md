# Release 2.3.0

**Date:** 2026-07-23
**Built on:** 2.2.16
**Closes:** gh#46 (wind-speed hysteresis). **Advances:** gh#45 (Part 2 — log discriminator; issue stays open only if the operator wants the legacy-decode caveats tracked further, otherwise closable after soak).

## Why a minor bump

Two feature-bearing changes (CLAUDE.md SemVer test): a **new NVS key** (`wind/wind_hyst`, C45) with new config-API field + GUI control, and a **log-encoding extension** (ALARM `param` subtype band 240–243). Patch resets to 0.

## Origin

Both changes come from one afternoon of production data: 5C88's wind alarms of Sunday **2026-07-19** (gusts to 9.2 m/s, `v_max = 6.0`):

1. Three override episodes triggered at exactly `avg == v_max`; the parser rendered two of them as *"direction 60 deg in exclusion zone"* — a phantom bearing (the wind was NNW all afternoon). gh#45.
2. Episodes 2+3 were one gust front chopped in two: SET 6.0 → CLEAR 5.8 (58 s) → SET 6.2 → CLEAR 5.3 — a sub-minute motor reversal against M3's 171 s stroke. gh#46.

T3's protection was **correct** throughout — windows closed under gusts, resumed when safe. This release changes the audit encoding and the *release* threshold, never the engage threshold.

## What changed

### Part B — `wind_hyst` speed-hysteresis dead band (gh#46, FR-WS12)

- `firmware/src/safety_monitor/safety_monitor.cpp` — while the override is active the speed threshold drops to `v_max − wind_hyst`; clear requires the averaged speed below the band AND direction outside the exclusion zone. Runtime guard caps the effective hysteresis at `v_max − 1` (override always clearable). Implementation note 6 in the file header documents the deliberate no-per-cause-latching choice.
- Config plumbing (clones `avg_win_wind`): `cfg_defaults.h` (`DEF_WIND_HYST 1`), `cfg_limits.h` (0–5), `data_manager.{h,cpp}` (shadow field, NVS load, clamp, C45 audit id, apply), `app_types.h` (`LOG_PARAM_WIND_HYST = 45`).
- Web GUI: config JSON + limits entry (`web_server.cpp`); admin-only slider row in the Wind tab (`index.html`, `app.js`). **Not** added to `FARMER_WIND_KEYS[]` → farmer POST → 403.
- Default **1 m/s — active after OTA** (operator decision D1): fail-safe directional (only re-opening is delayed); default 0 would have left production chattering. `wind_hyst = 0` restores legacy behaviour exactly.
- The FR-WS08 hysteresis **timer** (minimum safe duration before reopen) stays deferred; revisit only if soak still shows chatter.

### Part A — self-identifying wind ALARM rows (gh#45 Part 2)

- `make_wind_log()` now stamps a subtype into `param` (band 240–254 reserved in `app_types.h`): **240** speed-SET, **241** dir-SET, **242** CLEAR (incl. disabled-while-active), **243** sensor-fault SET. All 5 call sites tagged; T2 motor-alarm rows deliberately keep `param = 0`, which makes the `(0,0)` clear-row alias unambiguous on 2.3.0+ data.
- `log/logparser.py` — param-first decode (exact), legacy heuristic retained for pre-2.3.0 rows (with the gh#45 Part-1 `>=` fix).
- `model/campaign-summer-2026/plot_daily.py` — `ALARM_W` events gain the `kind` element (exact onset/clear from param; heuristic fallback for legacy rows), which its interval inference now uses. Simultaneous speed+dir SETs can no longer double-toggle the inferred state.
- `log/logparser.md` — encoding documented (2.3.0+ table + legacy table + caveats).

### Mock

`webUiMock/mock_server.py` gains `wind_hyst` (config, POST map admin-only, limits) and the previously-missing `avg_win_wind` (gh#35 drift).

## What did NOT change

- Engage behaviour: SET is still `avg ≥ v_max` / direction-in-zone — same thresholds, same CLOSE_ALL path, same EG1 bit, same LCD/LED indications.
- Sensor-fault safe-fail (FR-W04) path unchanged (row now tagged 243).
- Log CSV **column layout** unchanged — `param` was already in every row (always 0 on ALARM); old parsers that ignore it keep working.
- T2 motor alarm, T5 sensor-fault rows: byte-identical to 2.2.16.
- No LCD menu changes (`wind_hyst` is web + admin only).

## Build artefacts

| Artefact | Size | SHA-256 (first 16) |
|---|---|---|
| `greenhouse-controller-2.3.0.bin` | 1,379,296 B (65.8 % of 2 MB bank) | `7af24f8bf4d07d82` |
| `web-assets-2.3.0.zip` (STORE) | 116,477 B | `50a60dbce4efaf2b` |
| `bootloader-2.3.0.bin` | 22,528 B | — |
| `partitions-2.3.0.bin` | 3,072 B | — |

RAM 19.2 %. `manifest-2.3.0.json` (ROTA seq 43) is authored by `rota_release.py release` at publish time.

## Verification done pre-release (no hardware run yet — bench soak pending)

- Full build SUCCESS, no new warnings (compile checkpoint mid-implementation + release build).
- 12-case decode matrix on `logparser._decode_alarm`: all new param codes exact; every legacy input byte-identical to the Part-1 parser.
- Campaign-archive replay (two full 1 MB legacy logs) through old vs new parser: **byte-identical** parsed output.
- `plot_daily.py` regression: regenerated the committed `plot_2026-07-19.png` **byte-for-byte** (3 wind events, 2864 samples).
- Web GUI in the mock: admin sees the slider (value 1, bounds 0–5 from the limits round-trip, correct row placement); farmer row hidden (height 0) and POST → **403**.
- **Honesty note:** TC-11…TC-15 are written but have NOT been executed against hardware; the hysteresis has not yet held a real gust. That is what the 2344 soak is for.

## Verifiable post-OTA

1. `GET /api/status` → `fw_ver` **and** `asset_version` both `2.3.0` (paired-commit invariant).
2. `GET /api/config` → `avg_win_wind` present **and** `wind_hyst: 1`; limits carry `wind_hyst: [0,5]`.
3. Admin Wind tab shows "Wind hysteresis (m/s)"; farmer login does not.
4. Farmer-session POST `{ns:"wind", key:"wind_hyst", value:3}` → 403.
5. First wind event on 2.3.0: SD row carries `param` 240/241/242/243; `logparser.py` renders it without the legacy heuristic (IT-EL-020).
6. Bench (emulator): run TC-11…TC-15 (`pytest test/test_04_wind_override.py -v`).

## Open items after this release

- Soak on 2344 (+FDA4), ideally spanning a windy day; then `promote` to mainstream.
- gh#45: firmware side done; consider closing after the first tagged wind event decodes correctly in a soak log.
- FR-WS08 timer variant: deferred contingency (gh#46 discussion).
- The gh#44 T15 build-guard shipped in this binary as well (committed earlier on main, comment/guard-only, no behaviour).

## Rollout

**Not yet released to GitHub** — prepared and staged, awaiting operator commit + push. Then: `python bin/rota_release.py release 2.3.0` (→ GitHub Release, tags `v2.3.0`, retriever points **soak**; seq 43). 2344/FDA4 pull in their night windows. After soak: `python bin/rota_release.py promote 2.3.0` → mainstream → 5C88.
