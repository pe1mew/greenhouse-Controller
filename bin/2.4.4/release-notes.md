# Release 2.4.4

**Date:** 2026-09-10
**Built on:** 2.4.3
**Closes:** [gh#51](https://github.com/pe1mew/greenhouse-Controller/issues/51) — Group D, the last of four

## What this fixes

Two unit errors that had been telling the operator the wrong thing, both found while tracing consumers for the earlier groups.

### The dwell fields said minutes and carried seconds

`cfg_shadow_t.dwell_open_min[]` / `dwell_close_min[]` were documented as *"(minutes, C18/C19)"*. They hold **seconds**. Confirmed against live data before anything was changed: FDA4 reports `[300, 300, 1500]`, and `CFG_MAX_DWELL_OPEN_S` is 1500.

Renamed to `dwell_open_s` / `dwell_close_s`, unit corrected in the doc comments.

**Why the old name was convincing rather than obviously wrong.** `_min` is a real convention in this struct — `session_timeout_min` and `ap_timeout_min` sit a few lines below and genuinely *are* minutes (defaults 5 and 30). The dwell pair followed the convention while meaning something else, which is exactly why it survived. The name was also ambiguous in a second way: the doc comment opens *"**Min** hold at OPEN…"*, so `_min` could plausibly have been read as "minimum". The `_s` suffix removes both readings and matches `DEF_DWELL_OPEN_M1_S` and `CFG_MAX_DWELL_OPEN_S`.

### Six wrong bounds in the Motors-tab tooltips

Three dwell-open tooltips claimed *"Max 600 s"*; three dwell-close claimed *"Max 300 s"*. The real limit is **1500 s** for both, and always has been.

The dwell-close case was the worse of the two: at a stated max of 300 s, an operator would have believed M3's own factory default of 600 s was out of range — and for dwell-open, that M3's 1500 s default was more than double the maximum. Both now read *"Max 1500 s (25 min)"*.

## `/api/config` — additive, nothing breaks

- Now emits `dwell_open_s` and `dwell_close_s`.
- **`dwell_open_min` / `dwell_close_min` are still emitted, carrying identical values.** They are deprecated and will be dropped in the next minor. External consumers should move to the `_s` names.
- The bundled dashboard reads `_s` with a `_min` fallback, so it renders correctly against firmware older than 2.4.4 and through the paired-commit window, when assets and firmware can briefly disagree.

Response size after the addition: ~680 bytes against a 1536-byte buffer (44%), so the two extra arrays have ample headroom.

## Paired commit required

This release changes **assets** (`app.js`, `index.html`) as well as firmware. Push both within 120 s of each other, and verify `fw_ver` **and** `asset_version` both read 2.4.4 afterwards. A firmware-only push leaves the old tooltips and the old `_min` reader in place.

## Verification

Firmware builds on `lolin_s3` and `lolin_s3_bench`. `app.js` could only be checked structurally before flashing (`node` is not on this machine); it was parsed for real afterwards by loading the served page — see below.

**Verified on FDA4 2026-09-10** (2.4.4, `fw_ver` and `asset_version` both confirmed post-reboot). `/api/config` dual-emits `_s` and `_min` with identical values (`[300,300,1500]` / `[0,0,600]`, 676 bytes against a 1536 buffer); `app.js` parses and its fallback returns `[1500, 600]` for both firmware shapes when evaluated in the live page; six tooltips read `Max 1500 s (25 min)` with none stale; the Motors tab populates all six dwell fields; and a dwell round-trip emitted `dwell_open (M1): 300 s -> 420 s` and back. The checks were:

1. `GET /api/config` contains **both** `dwell_open_s` and `dwell_open_min` with identical values, and likewise for close.
2. The Motors tab still populates all six dwell fields — proving the dashboard reads the new keys.
3. Hovering a dwell tooltip reads *"Max 1500 s (25 min)"*.
4. Setting a dwell value still round-trips and produces a `dwell_open (M1)` SETPT row in the SD log.

Check 2 is the one that matters: a broken `app.js` would leave the fields blank while everything else looked fine.

## gh#51 closed

All four groups shipped and hardware-verified on FDA4:

| Group | Release | Verified |
|---|---|---|
| A — travel/dwell apply without a reboot | 2.4.2 | Fail-first, three measured runs on FDA4 |
| B — audit trail for travel time | 2.4.3 | Real SD rows |
| C — factory-reset screen tells the truth | 2.4.3 | Physical IO0 press |
| D — dwell unit naming and tooltip bounds | 2.4.4 | Five checks, incl. the live page |

Left open deliberately, tracked separately: [gh#52](https://github.com/pe1mew/greenhouse-Controller/issues/52) (`nvs_load_mode()` asymmetry) and [gh#53](https://github.com/pe1mew/greenhouse-Controller/issues/53) (`/api/config` accepts unknown keys).

## Upgrade notes

No configuration migration, no new NVS keys. The rename is internal to the firmware and additive in the API.

Soak on FDA4 before promoting to `mainstream` (5C88).

**ROTA is provisioned and confirmed working** — the secret was restored by the operator after the Group C verification, and a forced check returned `http: 200` from `ota.rfsee.net` for id `30eda0a0fda4`, which is the proof the HMAC matches (a wrong secret gives 401/403, not 200).

**But the soak channel still offers 2.4.1** while FDA4 runs 2.4.4: 2.4.2, 2.4.3 and 2.4.4 were all push-OTA'd and never published to ROTA. The check reports `up_to_date` because anti-downgrade correctly refuses to go backwards — but a reflashed or replaced FDA4 would be pulled back to 2.4.1. Publish the chain to `soak` before relying on the pull path. 5C88 on `mainstream` remains on 2.3.1.
