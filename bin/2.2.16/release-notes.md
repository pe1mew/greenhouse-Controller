# Release 2.2.16

**Date:** 2026-07-17
**Built on:** 2.2.15
**Closes:** gh#43 (Log-tab layout consistency)

## Why a patch bump
Cosmetic web-GUI change only — no new API field, NVS key, config field, or
firmware-logic change. Patch bump (2.2.15 → 2.2.16).

## What changed — `firmware/data/index.html`, Log tab "Download log" section
- **Download CSV** moved onto the **same row** as the Log-source selector and
  **right-aligned** (`style="margin-left:auto"`), mirroring the Coredump line's
  right-aligned action buttons. The previous two-row layout (selector row + a
  separate Download row) is now a **single row**.
- `#log-src-select` shrunk from `flex:1;min-width:0` (full-width) to
  `min-width:12rem` — log filenames are short, so the full width was wasted.

**Verified** in the web-UI mock (admin, Log tab): Download renders on the same
line as the selector (ΔY = 0 px), pushed right (~291 px gap after the refresh
button, i.e. `margin-left:auto`), and the dropdown is ~22 % of the row width —
visually consistent with the Coredump row.

## What did NOT change
- `downloadLog()` / `loadLogFiles()` handlers in `app.js` — untouched.
- Firmware source (`firmware/src/`) — only the `-DFIRMWARE_VERSION` string bumped;
  the `.bin` is the same size as 2.2.15 (1,379,072 B).
- No SD / log-format, OTA, or control-logic change.

## Build artefacts

| Artefact | Size (bytes) | SHA-256 |
|---|---|---|
| `greenhouse-controller-2.2.16.bin` | 1379072 | `ad12a00942aebe748e207504bc5dd4d812513eae6d8c26181d43ba99e4a4b0b8` |
| `web-assets-2.2.16.zip` | 115712 | `ec98aad5136cef30b35147e4b561456029c358541792365a7308d732d7af8bfc` |

Flash: 1 378 669 B of 2 097 152 (65.7 %). In the asset ZIP only `index.html`
differs from 2.2.15 (plus the `asset_version` stamp).

## Verifiable post-OTA
- `GET /api/status` → `fw_ver` AND `asset_version` both `2.2.16` (paired commit).
- Web GUI → Log tab → **Download CSV** sits on the Log-source row, right-aligned;
  the file dropdown is compact.

## Open items after this release
- gh#41 footgun #2 (ROTA re-download per genuine deferral).
- Firmware signing (`key_id` / R-A10) still deferred before it gates production.

## Rollout
**Not yet released to GitHub** — prepared and staged, awaiting release. When
released: `python bin/rota_release.py release 2.2.16` (→ GitHub Release → soak,
seq auto-assigned 42), then `promote` after soak.
