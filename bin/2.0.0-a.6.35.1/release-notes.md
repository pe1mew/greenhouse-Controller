# 2.0.0-a.6.35.1 — Web tab UX: clear "DISABLED" when operator enables status

UX follow-up to a.6.35. Single bug fixed, two-line root cause, ~10 lines of new code.

## The bug the operator saw

1. On the Web tab, with status disabled, the read-only "Last POST" indicator shows `DISABLED`.
2. Operator ticks `Enable`, fills in URL + interval, clicks `Apply`.
3. Toast says `Saved`.
4. **But "Last POST" still says `DISABLED`** — for up to 60 s.
5. Manually refreshing the browser doesn't help either: the field still reads `DISABLED` until the controller's next T14 cycle actually fires (and even then only updates at the GUI's 5 s auto-refresh).

End result: operator thinks the Apply didn't work, re-toggles, gets frustrated.

## Root cause

Two compounding issues in the a.6.35 implementation of T14:

1. **T14 sleeps 60 s when disabled.** The main-loop disabled-branch idle wait was `xTaskNotifyWait(0, ULONG_MAX, &drain, pdMS_TO_TICKS(STATUS_IDLE_RECHECK_MS))` with `STATUS_IDLE_RECHECK_MS = 60000`. T11's `/api/web` POST handler updates the cfg shadow synchronously, but T14 doesn't re-check until either the timeout expires or a notify bit arrives. Nothing was firing a notify on cfg changes, so the worst-case latency for "T14 notices the operator just enabled status" was 60 s.

2. **`s_last_str` re-stamped to `"DISABLED"` every disabled-branch iteration.** Even after T14 eventually woke up and noticed `enable=1`, the next disabled-branch iteration (if cfg was briefly stale) would over-write `s_last_str` back to `"DISABLED"`. And on the very first re-enable, there was no transition handling at all — the operator saw the stale `"DISABLED"` value persist into the active branch until the first POST completed, which could be another `status_interval_s` away (60–300 s).

## Fix

Two changes that target the two issues:

### 1. Wake T14 immediately on cfg change

New task-notify bit:

```c
#define T14_NOTIFY_CFG_CHANGED  (1u << 1)
```

Fired from `dm_reload_web_cfg()` (which is called by every successful `/api/web` POST):

```c
if (task_t14 != NULL) {
    xTaskNotify(task_t14, T14_NOTIFY_CFG_CHANGED, eSetBits);
}
```

T14's `xTaskNotifyWait` in the disabled branch already accepts any bit (mask = `ULONG_MAX`), so the existing wait wakes within ~1 ms of the notify. No code change needed on the wait side beyond accepting the bit semantically.

### 2. Track disabled→enabled transition; clear `s_last_str`; force immediate POST

New local `s_was_disabled = true` tracker in `task_status_post`:

- Disabled branch only stamps `"DISABLED"` once per enabled→disabled transition (when `!s_was_disabled`).
- Just past the disabled check, on the disabled→enabled edge: clear `s_last_str` (so GUI shows `—` pending), reset `last_post_ms = 0` (so the first POST fires on the very next iteration without waiting up to a full `status_interval_s` for the cadence check), and flip `s_was_disabled = false`.

```c
if (s_was_disabled) {
    s_last_str[0] = '\0';
    last_post_ms = 0;
    s_was_disabled = false;
    ESP_LOGI(TAG, "[T14] re-enabled (url=%s interval=%lds expose=0x%02lX) — POST imminent", ...);
}
```

## What changed

- **`firmware/src/status_post/status_post.h`** — new `T14_NOTIFY_CFG_CHANGED (1u << 1)` macro with rationale block.
- **`firmware/src/status_post/status_post.cpp`** — new `s_was_disabled` tracker; transition handling at top of disabled branch + top of active branch (just past the disabled gate).
- **`firmware/src/data_manager/data_manager.cpp`** — new `#include "../status_post/status_post.h"`; `dm_reload_web_cfg()` fires `xTaskNotify(task_t14, T14_NOTIFY_CFG_CHANGED, eSetBits)` at the end, NULL-guarded.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35.1`.

## Acceptance — hardware verified on 192.168.20.160 (a.6.35.1 paired flash, fresh boot)

Reproduced the original bug scenario step by step:

```
[Setup]   enable=0, url=""    → /api/web reports: enable=0 last_post='DISABLED'
[t=0]     POST {enable:1, interval_s:60, url:"https://192.168.99.99/api.php"}
[t+0.5s]  /api/web → enable=1 last_post=''     ← cleared! GUI renders '—'
[t+2s]    /api/web → enable=1 last_post=''     ← still pending, no cycle complete yet
[t+10s]   /api/web → enable=1 last_post='FAIL 2026-05-19 11:31:22 code=0'  ← first cycle landed
```

The bug window between "Apply clicked" and "first POST result visible" went from `up to 60 s of stale DISABLED text` to `~0.5 s of pending dash, then the real result whenever the cycle completes`. The GUI's existing 5 s auto-refresh then maintains the displayed value normally.

## Build delta vs a.6.35

| Metric | a.6.35 | a.6.35.1 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 348 457 B | **1 348 669 B** | +212 B |
| RAM static | 60 552 B | 60 552 B | 0 |

Trivial cost: one new notify bit, one xTaskNotify call site, one bool tracker, ~10 lines of transition logic.

## Next

Phase 7 — 14-day soak. The a.6.35 → a.6.35.1 step shows the value of running end-to-end on the bench unit before declaring an alpha "complete" — even with both acceptance tests in a.6.35 passing (item D + item G), the field-test exposed a real UX gap the unit test couldn't have caught. Keep this lesson when prepping rc.1.
