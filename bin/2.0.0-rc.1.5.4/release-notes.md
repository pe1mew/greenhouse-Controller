# 2.0.0-rc.1.5.4 — release notes

**Date built:** 2026-05-29
**Built on top of:** 2.0.0-rc.1.5.3 (Tmr Svc stack overflow defenses + drop `-wip`)
**Closes:** the LCD-vs-web-GUI NTP-indicator disagreement reported on unit 5C88 (`Src:RTC` on the LCD while `NTP synced` on the web GUI on the same boot)
**Scope:** add a single public accessor in T10 (`nm_is_sntp_synced()`) so the web-GUI status-snapshot path reads from the same canonical SNTP-completed latch the LCD path already uses (rc.1.5.3-wip baseline). Three small files touched, no behaviour change in the happy path.

---

## Why a patch bump (1.5.3 → 1.5.4)

Single-site bug fix in `data_manager.cpp` plus the new accessor in `network_manager.cpp/h`. No new task, no new HTTP route, no SD-log format change, no web-asset content change. Patch bump is the right granularity.

---

## What changed

### The bug rc.1.5.3-wip half-fixed

rc.1.5.3-wip replaced the `time(NULL) > NTP_MIN_EPOCH` heuristic in `network_manager.cpp:snapshot_state()` with a proper `s_sntp_synced` latch, set only when `esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`. That fixed the LCD path (Q5 → T8). But the **web-GUI path** in `data_manager.cpp:dm_status_snapshot()` had its own copy of the same heuristic:

```c
// data_manager.cpp:1303 — before rc.1.5.4
out->ntp_synced = (cfg.current_unix_ts > 1700000000UL);
```

`cfg.current_unix_ts` is populated from T4's RTC pre-seed (DS1307 → `settimeofday()` around boot+500 ms), so it's always plausible at boot regardless of SNTP success. On any unit with a battery-backed RTC and no internet, this produced a *false positive* on the web GUI while the LCD path (now using the latch) correctly reported "RTC pending."

Observed on unit 5C88 immediately after OTA:
- LCD: `Src:RTC` ✓
- Web GUI: `NTP synced` ✗

### Fix

Add a public accessor in `network_manager.h`/`.cpp`:

```c
/* extern "C" for data_manager.cpp consumer */
extern "C" bool nm_is_sntp_synced(void)
{
    return s_sntp_synced;
}
```

Update `data_manager.cpp:1303` to use it:

```c
// data_manager.cpp:1303 — rc.1.5.4
out->ntp_synced = nm_is_sntp_synced();
```

LCD and web GUI now derive `ntp_synced` from the same canonical source — they agree by construction. The flag is also monotonic-rising per boot (set only by `esp_sntp_get_sync_status() == COMPLETED`, never cleared until reboot), so they stay agreed even across STA disconnect/reconnect.

---

## What this didn't fix (deferred to rc.1.5.6)

The rc.1.5.4 fix made `ntp_synced` *honest* on the web GUI. Honesty exposed a separate latent bug in the *periodic NTP resync gate*: when boot SNTP timed out, the resync timer never started, so the unit stayed on RTC for the full boot session. That's the rc.1.5.6 release. The rc.1.5.4 fix is necessary scaffolding for rc.1.5.6 to be diagnosable — without rc.1.5.4 you'd never have seen the stuck state.

---

## Verified

Build + OTA verified on unit 5C88 (192.168.20.150):
- `fw_ver = 2.0.0-rc.1.5.4`, `asset_version = 2.0.0-rc.1.5.4`
- `bank = B, accepted = true` (past T1's 30 s healthy mark)
- LCD and web GUI's NTP indicators now agree under both `s_sntp_synced=true` and `s_sntp_synced=false` (the latter observable on this unit because its boot SNTP intermittently times out at low-signal conditions — the symptom that motivates rc.1.5.6)

---

## Scope notes

The accessor is intentionally narrow — only the SNTP-completed flag is exposed. The retry counter, last attempt timestamp, and last sync timestamp remain module-private in `network_manager.cpp`. Consumers that need them can grow more accessors later if a use case emerges; for now `dm_status_snapshot()` is the only caller.
