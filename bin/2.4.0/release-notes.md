# Release 2.4.0

**Date:** 2026-09-07
**Built on:** 2.3.1
**Closes:** gh#50 (unit ID erased from the web-GUI footer by the config refresh)

## Why a minor bump

The fix on its own would be a patch. The release also adds the unit ID to the
**browser tab title**, which is new operator-visible behaviour and is now
documented in `boerHandleiding.md` §3 as a fifth place to read the ID — so by
the CLAUDE.md heuristic ("did this add a user-visible feature?") it is a minor.

**No firmware source changed in this release.** The binary is a rebuild of
2.3.1's sources under a new version string; the entire behavioural delta is in
`firmware/data/app.js`. It ships as a normal paired firmware+assets release
because the OTA path has no assets-only mode, and because shipping changed
assets under an unchanged `asset_version` would break the version-to-content
mapping the paired-commit invariant exists to protect.

## The defect

Two writers targeted the same `#fw-ver` element with different formats:

| Source | Writes | Carries `unit_id`? |
|---|---|---|
| status push / cold-start fetch (`app.js:305`) | `v2.3.1 · FDA4` | **yes** |
| `GET /api/config` via `loadConfig()` (`app.js:519`) | `v2.3.1` | **no** |

`/api/config` has no `unit_id` field at all (`web_server.cpp:1146`), so the
config writer could not reproduce it — it simply overwrote whatever the status
writer had put there.

`loadConfig()` runs **on login** (`setRole()`) and then **every 60 s** while the
user is active. The status push restored the ID on its next frame ~2 s later, so
the ID disappeared for up to one push interval, on login and once a minute for
the rest of the session. The operator reported it as "gone after login", which
is where it is most reliably visible.

Reproduced against `webUiMock/mock_server.py`, sampling `#fw-ver` every 25 ms
across one `loadConfig()`:

```
  26 ms   v2.0.0-rc.1.3.2 · AABB     status push has set it
 326 ms   v2.0.0-rc.1.3.2            loadConfig() overwrites — ID gone
2100 ms   v2.0.0-rc.1.3.2 · AABB     next WS push restores it
```

## The fix

Neither writer touches the DOM. Both update last-known values and call a single
`renderIdentity()`, so a config refresh carrying no `unit_id` can no longer
erase one a status push already supplied.

```js
let g_fw_ver = null, g_unit_id = null;

function renderIdentity() {
  const parts = [];
  if (g_fw_ver)  parts.push('v' + g_fw_ver);
  if (g_unit_id) parts.push(g_unit_id);
  setText('fw-ver', parts.join(' · ') || '—');
  document.title = g_unit_id ? g_unit_id + ' · Greenhouse Controller'
                             : 'Greenhouse Controller';
}
```

## Unit ID in the browser tab title

The tab now reads `FDA4 · Greenhouse Controller`.

**ID first, deliberately.** Browser tabs truncate from the right, so with 5C88,
2344 and FDA4 open the ID is the part that survives; `Greenhouse Controller —
FDA4` would truncate to `Greenho…` and be useless for telling tabs apart. The
static `<title>` in `index.html` stays as the bare product name — it is the
pre-JS fallback, and the ID is not known until the first status arrives.

## What did NOT change

- No firmware source file. No task, NVS key, log format or payload shape.
- The status-push write path still sets footer content on every frame; only its
  destination changed from the DOM to `renderIdentity()`.
- `index.html`, `style.css` byte-identical to 2.3.1.

## Build artefacts

```
bin/2.4.0/greenhouse-controller-2.4.0.bin   1,379,312 B
bin/2.4.0/web-assets-2.4.0.zip                117,139 B  (STORE, method=0)
bin/2.4.0/bootloader-2.4.0.bin
bin/2.4.0/partitions-2.4.0.bin
bin/2.4.0/firmware-2.4.0.elf / .map
```

Asset zip verified before push: `manifest.json` = `{"asset_version":"2.4.0"}`,
`renderIdentity()` present, the old direct write absent, every entry method=0.

## Verification status

**Deployed and verified on FDA4 (192.168.20.169) 2026-09-07** via
`bin/ota_push.py`. Paired-commit invariant satisfied by an independent
post-reboot `/api/status` read:

```
unit_id = FDA4    fw_ver = 2.4.0    asset_version = 2.4.0
```

Beyond the version labels, the deployed content was verified: `app.js` fetched
back from the device is **byte-identical** to the release zip's copy
(sha256 `4a3f3ebe…`). In a browser against the live unit the tab title reads
`FDA4 · Greenhouse Controller` and the footer `v2.4.0 · FDA4`, and exercising
the config-refresh render path on the device leaves the unit ID intact.

The logged-in path was then verified **on the device itself**: a real farmer
login against 192.168.20.169, sampling the footer every 25 ms across the login
and a subsequent `loadConfig()`, recorded **zero changes** — against three on
the same instrumentation before the fix. Session logged out afterwards.

**Not soaked.** FDA4 is the dev/test bench. 2344 and 5C88 are untouched.

## Rollout

**Not released to GitHub** — built and staged, awaiting operator commit + push.
Then `python bin/rota_release.py release 2.4.0` (→ GitHub Release, tags
`v2.4.0`, points **soak**, next seq after 44). Promote to mainstream only after
soak.

## Context

Reported by the operator while the FDA4 board was on the bench for the 2.3.1
cable flash. Filed as gh#50 with the reproduction data.
