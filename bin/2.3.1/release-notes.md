# Release 2.3.1

**Date:** 2026-07-28
**Built on:** 2.3.0
**Closes:** gh#48 (anti-thrash dwell unguarded during travel)

## Why a patch bump

Bug fix only. No new config key, no new NVS namespace, no payload-shape change, no new task — behaviour once a window is settled is byte-for-byte the policy it was before. Per the CLAUDE.md SemVer test that is a patch.

## The defect

The anti-thrash dwell protects a window only after it has **finished travelling**. `dwell_deadline_ms` is armed on the `CH_MOVING_OPEN` → `CH_OPEN` transition, and `ch_start_close()` carried its dwell check only in the settled `CH_OPEN` branch. Its `CH_MOVING_OPEN` branch reversed **unconditionally** — no dwell check existed in that path at all. `ch_start_open()` had the mirror gap for `CH_MOVING_CLOSE`.

A window still in motion was therefore completely unprotected against a reversing climate command.

**M3 was the exposed case.** Its 171 s stroke leaves a ~3 minute window on every opening; M1/M2 (21 s) are past it almost immediately. Field evidence from 5C88:

```
2026-07-27T03:02:39  RELAY ch=3  M3 -> MOVING_OPEN
2026-07-27T03:04:42  RELAY ch=3  M3 -> MOVING_CLOSE     123 s in; travel needs 171 s
```

M3 was 72 % through its stroke when it was told to reverse. It never reached `CH_OPEN`, so its configured 25-minute `dwell_open_m3` never armed.

For contrast — and this is why the fix is narrow — when M3 *does* complete its stroke the dwell works exactly as designed. Physical `CH_OPEN` intervals over the same 9 days: **median 25 min, min 23 min** (n = 28) against a configured 25 min, while the climate demand underneath oscillates every 5–14 min. T2 correctly refuses to follow it. Only the in-travel window was unguarded.

## The fix

`firmware/src/relay_controller/relay_controller.cpp` — in `ch_start_close()`:

```c
case CH_MOVING_OPEN:
    if (source == SRC_T6) {
        ESP_LOGD(TAG, "CH%u: CLOSE deferred — stroke in progress (gh#48)", ch + 1u);
        return;
    }
    /* existing reversal path unchanged for SRC_T3 / SRC_OPERATOR_MANUAL */
```

and the mirror in `ch_start_open()` for `CH_MOVING_CLOSE`.

- **T6 (climate) defers** — the stroke completes, then the normal settled-state dwell governs. The command is **not lost**: T6's reconciliation is level-triggered and re-issues it every cycle until honoured (`climate_control.cpp` header, item 6).
- **T3 (wind safety) and SRC_OPERATOR_MANUAL are unchanged** and still reverse immediately. This is deliberate and important — a wind override must be able to shut a window mid-travel, and an admin command is an explicit choice. The fix extends the existing documented bypass policy rather than altering it.

No new state variable, no new config, no new timer.

## Trade-off (recorded in the TSDS)

A climate close issued during an opening stroke now waits up to travel + dwell — for M3, ~3 min + 25 min. That is the same behaviour the window already had once open, so it is consistent rather than newly introduced, but it is a real deferral and is documented in TSDS §T2.

## What did NOT change

- Settled-state dwell policy (`CH_OPEN` / `CH_CLOSED`): identical.
- Wind-safety and operator-manual reversal: identical.
- The 2 s inter-relay gap, travel timers, mutual exclusion, NVS state persistence: untouched.
- Web assets: content identical to 2.3.0 apart from the version stamp.
- No climate-control, T3, T9 or ROTA changes.

## Build artefacts

| Artefact | Size | SHA-256 (first 16) |
|---|---|---|
| `greenhouse-controller-2.3.1.bin` | 1,379,152 B (65.7 % of the 2 MB bank) | `c6a1fa72b640e94c` |
| `web-assets-2.3.1.zip` (STORE) | 116,477 B | `60065b7941327e55` |
| `bootloader-2.3.1.bin` | 22,528 B | — |
| `partitions-2.3.1.bin` | 3,072 B | — |

RAM 19.2 %. `manifest-2.3.1.json` (ROTA seq 44) is authored by `rota_release.py release` at publish time.

## Verification status

- Full release build SUCCESS, no new warnings.
- **Not yet exercised on hardware.** The defect fired once in 9 days on the production unit, so it will not reproduce on demand by waiting; the honest test is the bench harness — UT-CC-033/034/035 were added to the test plan for exactly this (climate defers, safety bypasses, deferred command is re-issued rather than lost).
- Recommended soak check: on 2344, confirm no `MOVING_OPEN` → `MOVING_CLOSE` pair appears in the RELAY rows with a gap shorter than the channel's travel time, and that M3 open intervals still cluster at the 25-minute dwell.

## Verifiable post-OTA

1. `GET /api/status` → `fw_ver` **and** `asset_version` both `2.3.1` (paired-commit invariant).
2. Windows still respond normally to climate demand — M3 opens on hot afternoons and holds ~25 min.
3. A wind override still closes windows immediately, including mid-travel (this is the guarded bypass; regression here would be the serious failure).
4. In the SD log, no `RELAY ch=3` `MOVING_OPEN` → `MOVING_CLOSE` transition shorter than 171 s.

## Rollout

**Not yet released to GitHub** — built and staged, awaiting operator commit + push. Then `python bin/rota_release.py release 2.3.1 --yes` (→ GitHub Release, tags `v2.3.1`, points **soak**, seq 44). 2344/FDA4 pull in their night windows. Promote to mainstream after soak.

## Context

Found while investigating M3 cycling for gh#47, now closed `not planned`. Replaying the campaign data showed the `vent_hyst` dead band proposed there would not have reduced the cycling at all — the temperature makes full ~5 °C excursions past the threshold rather than hovering at it — and that the actuator was not being thrashed in the first place, because dwell already holds M3 open for its full 25 minutes. This mid-stroke reversal was the one genuine defect the investigation turned up. Analysis tool: `model/vent_step_replay.py`.
