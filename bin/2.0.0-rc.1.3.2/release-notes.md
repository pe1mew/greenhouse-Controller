# 2.0.0-rc.1.3.2 — initial /api/status fetch on page load + login

Patch release on top of rc.1.3.1. **JS-only change** (single file: `firmware/data/app.js`) plus the version bump. Fixes the operator-reported "shields stay blank for ~2 s after page load" bug.

Firmware bin is byte-identical to rc.1.3.1 — only the asset bundle and version string change. Supersedes rc.1.3.1 as the Phase 7 soak candidate; the 14-day clock restarts at day 0.

## The operator's report

> *"when browsing to the webgui the status shields do not get populated directly at load. the sensor status is loaded directly, refresh and login does not help."*

## Root cause

The dashboard had **no synchronous fetch of `/api/status`** at page-load. `handleStatus()` was only invoked via the WebSocket `onmessage` callback. The page-load sequence was:

```
loadLimits()  → wsConnect()  → loadHistory()  → loadSdStatus()  → fetch('/api/whoami')
```

`wsConnect()` opens the WebSocket immediately, but the first push from `task_ws_push` arrives **0–2 s later** (the task uses `vTaskDelayUntil(WS_PUSH_MS = 2000)` between pushes). Between page-load and the first WS frame, the status tiles + alarms shield + mode badge were entirely unpopulated.

Why the user perceived sensors as "loading directly" while shields didn't:
- Sensor tiles had `—` placeholder text → transitioned from `—` to `24.6` (looks "loading")
- Shield element had **no placeholder** → transitioned from blank to `OK` badge (visible "pop-in")

`doLogin()` had the same hole — after a successful PIN check, the role flipped but no `/api/status` re-fetch fired. The user had to wait for the next WS push to see role-gated rows re-render.

## The fix

`firmware/data/app.js` — added `fetchStatusNow()` helper + two call sites:

```js
// New helper (one fetch, no retry):
function fetchStatusNow() {
  fetch('/api/status', { credentials: 'same-origin' })
    .then(r => r.ok ? r.json() : null)
    .then(s => { if (s && s.type === 'status') handleStatus(s); })
    .catch(function () { /* WS push will catch up within 2 s */ });
}

// Call site 1 — page load (runs in parallel with wsConnect):
loadLimits();
wsConnect();
fetchStatusNow();   // ← new
loadHistory();
loadSdStatus();
fetch('/api/whoami')...

// Call site 2 — post-successful-login (inside doLogin().then):
if (r && r.ok) {
    hideLoginModal();
    setRole(r.role);
    document.getElementById('login-err').textContent = '';
    document.getElementById('login-pin').value = '';
    fetchStatusNow();   // ← new — role-gated rows re-render immediately
}
```

The WS push remains the steady-state update channel (every 2 s); this fetch only covers the **cold-start + role-change** cases. The payload is byte-identical to the WS frame (same `build_canonical_status_json()` builder, same `STATUS_EXPOSE_ALL` + `include_disabled_setpoints=true`) so `handleStatus()` handles both uniformly without any branching.

## Failure-tolerant by design

If `/api/status` is unreachable at the moment of fetch (T11 not up yet, WiFi hiccup, captive portal interstitial), the `.catch(function () {})` silently swallows the error and the dashboard falls back to the WS-push timing. No retry logic needed — `wsConnect()`'s 3 s reconnect timer takes over.

## What did NOT change

- Firmware C/C++ — zero changes.
- `firmware.bin` is **byte-identical to rc.1.3.1** apart from the FIRMWARE_VERSION string (which sits in a static .rodata section, hashes differently but doesn't change codegen elsewhere).
- WS push task, status JSON builder, `task_ws_push`, all other endpoints — unchanged.
- All prior fixes (rc.1.1 wind direction, rc.1.2 OTA reboot, rc.1.2.1 log-upload buffer, rc.1.3 housekeeping, rc.1.3.1 temperature precision) — preserved verbatim.

## Build delta vs rc.1.3.1

| Metric | rc.1.3.1 | rc.1.3.2 | Delta |
|---|---:|---:|---:|
| Firmware bin | 1 352 432 B | (build-pending) | ≈0 (only FIRMWARE_VERSION string changes 1 char) |
| RAM static | 60 568 B | 60 568 B | 0 |
| Web assets ZIP | 99 191 B | (build-pending) | +~600 B (the new helper + comments) |

## Verification on bench

After deploy, load the dashboard. The Alarms card should display "OK" (or whatever active flags are present) **synchronously with the page render** — no ~2 s blank window.

Test sequence:
1. Hard refresh the dashboard tab (Ctrl-Shift-R).
2. Observe: status tiles + alarms shield + mode badge appear together with the page, not after a delay.
3. Click Login → enter PIN → submit.
4. Observe: the admin-only rows render their gated state immediately, not after the next WS tick.

If `/api/status` fails at load time (e.g. by simulating a network drop), the dashboard should still populate within 2 s via the WS push — fail-safe.

## Phase 7 soak

Day-counter resets to day 0 against rc.1.3.2. JS-only patch but the firmware version increments because the dashboard's MISMATCH badge logic compares `sys.fw_ver !== sys.asset_version` — keeping them in lockstep avoids spurious MISMATCH warnings.
