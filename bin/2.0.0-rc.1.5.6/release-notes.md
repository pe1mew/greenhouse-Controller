# 2.0.0-rc.1.5.6 — release notes

**Date built:** 2026-05-30
**Built on top of:** 2.0.0-rc.1.5.5 (IO MUX pin glitch filter on GPIO42)
**Closes:** the four-issue NTP-indicator regression (rc.1.5.3-wip → rc.1.5.4 → rc.1.5.6) and the production-hardware motor-alarm EMI cluster (rc.1.5.3 → rc.1.5.5 + 1.8 kΩ shunt mod)
**Scope:** small targeted patch in `network_manager.cpp` adding an SNTP retry cadence so a single failed boot SNTP no longer strands the controller on RTC for the rest of the boot.
**Soak status:** **PASSED.** Closed by operator on 2026-06-08 after 9 d 5 h 25 min of continuous, clean operation on unit 2344 (bench, Node-RED-mirrored from production unit 5C88). 0 panics, 0 coredumps, 0 SD errors, 0 sensor faults, 6 934 / 6 943 status POSTs (99.87 %), 11 successful end-to-end T9 → T14 log uploads.

---

## Why a patch bump (1.5.5 → 1.5.6)

Single, targeted firmware fix in T10's main poll loop. No new task, no new public API, no SD-log format change, no web-asset content change. Patch bump is the right granularity.

---

## What changed

### The latent bug rc.1.5.4 exposed

rc.1.5.3-wip + rc.1.5.4 fixed two copies of the `time(NULL) > NTP_MIN_EPOCH` heuristic that drove the user-visible "Src:NTP / Src:RTC" indicator: one in `snapshot_state()` for the LCD path, one in `dm_status_snapshot()` for the web GUI path. Both now read the real `s_sntp_synced` latch instead.

The latch is set only when `esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`, either at boot via `nm_sntp_quick_sync()` or every 24 h via `run_ntp_resync()`. The honest indicator immediately exposed a latent bug elsewhere in T10 that the time-based heuristic had been hiding:

```c
// network_manager.cpp main loop, before rc.1.5.6:
if (s_last_ntp_sync_us != 0 && prev.client_connected) {
    int64_t elapsed_us = esp_timer_get_time() - s_last_ntp_sync_us;
    if ((uint64_t)elapsed_us >= NTP_RESYNC_INTERVAL_US) {
        run_ntp_resync();
    }
}
```

`s_last_ntp_sync_us == 0` is the "boot SNTP timed out" state. The 24 h resync was gated on `s_last_ntp_sync_us != 0`, so a unit whose boot-time `nm_sntp_quick_sync` (10 s budget) timed out was **stranded on RTC for the rest of the boot** — the periodic resync timer never started counting, and the wall clock would only update via the next reboot.

Observed on the soak unit (192.168.20.160) at uptime 125 s with `wifi_rssi = −41 dBm`: `ntp_synced = false` despite healthy upstream — a real-world condition reproducing about once every dozen boots when DNS/UDP to pool.ntp.org takes longer than 10 s.

### Fix

Dual-cadence retry path in the same main-loop check. Two intervals depending on whether the controller has *ever* successfully synced this boot:

- **Synced (`s_last_ntp_sync_us != 0`)** — retry on the canonical `NTP_RESYNC_INTERVAL_US = 24 h` cadence, unchanged.
- **Never synced (`s_last_ntp_sync_us == 0`)** — retry every `NTP_RETRY_INTERVAL_US = 5 min` so the unit catches up quickly once the network recovers. Short enough to recover within an operator coffee break, long enough not to hammer pool.ntp.org under a persistent DNS/UDP outage.

```c
// network_manager.cpp main loop, rc.1.5.6:
if (prev.client_connected) {
    int64_t   now_us       = esp_timer_get_time();
    bool      synced       = (s_last_ntp_sync_us != 0);
    uint64_t  interval_us  = synced ? NTP_RESYNC_INTERVAL_US
                                    : NTP_RETRY_INTERVAL_US;
    int64_t   reference_us = synced ? s_last_ntp_sync_us
                                    : s_last_ntp_attempt_us;
    if ((uint64_t)(now_us - reference_us) >= interval_us) {
        s_last_ntp_attempt_us = now_us;
        run_ntp_resync();
        // run_ntp_resync updates s_last_ntp_sync_us on success → flips
        // us back to the 24 h branch on the next iteration.
    }
}
```

`s_last_ntp_attempt_us` is seeded at T10 task start so the first retry waits a full `NTP_RETRY_INTERVAL_S` from there, not from the epoch — no immediate back-to-back retry with the boot quick_sync.

`NTP_RETRY_INTERVAL_S` overrideable at compile time via `-D` so verification builds can shorten it.

### Verified in the wild

Within the rc.1.5.6 OTA-and-watch on unit 5C88 (192.168.20.150), the exact failure case the fix is designed to recover was observed:

```
uptime=245s   ntp_synced=False    (4.1 min — boot SNTP timed out)
uptime=261s   ntp_synced=False    (4.3 min)
uptime=276s   ntp_synced=False    (4.6 min)
uptime=291s   ntp_synced=False    (4.8 min)
uptime=306s   ntp_synced=False    (5.1 min — retry threshold just crossed)
uptime=322s   ntp_synced=True     ← retry fired, SNTP succeeded, latched
```

Without rc.1.5.6 this unit would have shown "NTP pending" until the next reboot. With rc.1.5.6 it self-healed in 5 minutes 22 seconds, unattended.

---

## What the full rc.1.5.x cycle accomplished

| Release | Issue addressed | Surface |
|---|---|---|
| **rc.1.5.0** | gh#28 MODE_STANDBY, gh#29 admin manual motor control | LCD Scherm 6 `#` flow, web `/api/mode`, `LOG_RELAY` audit |
| **rc.1.5.1** | gh#29 UX fix #1 — auto-STANDBY at menu entry | T6 inhibit on STANDBY, no idle-dismiss |
| **rc.1.5.2** | gh#29 UX fix #2 — respect-window = admin PIN-session timeout | STANDBY clear moved from `go_status()` to `session_close()` |
| **rc.1.5.3** | Tmr Svc stack overflow defense | 5 ms ISR rate-limit on GPIO42 + `FREERTOS_TIMER_TASK_STACK_DEPTH 2048→4096` |
| **rc.1.5.4** | Web-GUI ↔ LCD agreement on NTP indicator | New `nm_is_sntp_synced()` accessor used by `dm_status_snapshot()` |
| **rc.1.5.5** | IO MUX pin glitch filter on GPIO42 (motor-alarm EMI defense) | `gpio_new_pin_glitch_filter()` in T2 init |
| **rc.1.5.6** | NTP retry cadence so failed boot SNTP doesn't strand the unit | Dual-cadence main-loop check; 5 min retry while not synced |

Combined with the hardware change (**1.8 kΩ shunt at J10** on the alarm-input GPIO), the full defense stack against the 2026-05-29 production-hardware EMI panic loop on unit 5C88 is:

| Layer | Source | Function |
|---|---|---|
| Hardware divider (1.8 kΩ) | board mod 2026-05-29 | amplitude floor for motor-alarm EMI; clamps GPIO to ≤ 3.21 V |
| IO MUX pin glitch filter (~25 ns) | rc.1.5.5 | silicon-level edge floor |
| ISR rate-limit (5 ms) | rc.1.5.3 | IRAM edge throttle |
| Task debounce (75 ms) | rc.1.4 baseline | application-level pin re-read |
| Tmr Svc stack 4 KB (was 2 KB) | rc.1.5.3 sdkconfig | kernel-side headroom |
| NTP retry (5 min while not synced) | rc.1.5.6 | recovers from failed boot SNTP |

Zero of these were observably stressed during the 9 d 5 h 25 min soak window on 2344.

---

## Build artefacts

| File | Size | SHA-256 |
|---|---:|---|
| `greenhouse-controller-2.0.0-rc.1.5.6.bin` | 1 358 800 B | `c784604ef2de1f808e16be640cf65960ab16fbbfc158dc37f765f6bf318863a9` |
| `web-assets-2.0.0-rc.1.5.6.zip`            |   105 884 B | `c4f2dcb0c5e274702ea845417d53459942245331abe068024e721d23710f2ff5` |
| `bootloader-2.0.0-rc.1.5.6.bin`            |    22 528 B | `04a07a8d26fd0d2975499f7943713eea6bbde086cf4bb63b6e5c73950565fe1b` |
| `partitions-2.0.0-rc.1.5.6.bin`            |     3 072 B | `18fbe59ac37567be8897bc7f5266aec2ba2df85934a3b8fb9b229d8e59e7e74d` |
| `firmware-2.0.0-rc.1.5.6.elf` | — | (for coredump decoding) |
| `firmware-2.0.0-rc.1.5.6.map` | — | (for coredump decoding) |

RAM: 18.9 % (62 040 / 327 680). Flash: 64.8 % (1 358 397 / 2 097 152). Web-assets ZIP method=STORE verified.

---

## Deployed on

| Unit | IP | OTA timestamp | Hardware mods |
|---|---|---|---|
| 2344 | 192.168.20.160 | 2026-05-30 13:38:40 | none (bench LOLIN, Node-RED bridge for sensor data) |
| 5C88 | 192.168.20.150 | 2026-05-30 13:39:05 | 1.8 kΩ shunt at J10 (alarm-input EMI hardening) |

---

## Open issues unblocked by this release

The soak passing without observed stress on any defense layer frees the next firmware update window from soak-stability constraints. The following can now be sequenced normally:

- **gh#30** — prefix SD-log filenames with unit ID
- **gh#31** — SD mount-state in `/api/status` JSON
- **gh#32** — SD handling on LCD / keypad GUI
- **gh#33** — T14 / T10 L3 self-recovery ladder (DHCP renew + STA reassociate on consecutive POST failures)
- **gh#27** — T15 heap-drop sampling timing (pre-existing)
- **gh#7** — serial-port WDT freeze (pre-existing bug)
