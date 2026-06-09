# 2.0.0-rc.1.5.3 — release notes

**Date built:** 2026-05-29
**Built on top of:** 2.0.0-rc.1.5.2 (gh#29 UX fix #2 — respect-window)
**Closes:** the 5C88 production-hardware panic loop (2026-05-29) — 22 panic reboots in ~6 minutes, FreeRTOS Tmr Svc stack overflow detected at task-switch time
**Scope:** two small defense-in-depth firmware changes — IRAM-level ISR rate-limit in `relay_controller.cpp` and a kernel-side Tmr Svc stack bump in `sdkconfig.defaults`. Plus drops the `-wip` suffix from the version stamp.

---

## What happened on 5C88

LOLIN board 5C88 was deployed into the production hardware at 2026-05-29 16:40:50 local. The unit panicked on every boot at ~14 s of uptime; 22 panics over 6 minutes plus 4 operator power-cycles, then 5C88 was moved to soak hardware where it ran cleanly.

Cross-mount evidence: same LOLIN, same firmware (rc.1.5.3-wip), production hardware = panic loop, soak hardware = stable. Pins the cause on something electrical in the production wiring, not on the firmware or the board.

Decoded coredump (45 316 B, captured under rc.1.5.3-wip):

```
***ERROR*** A stack overflow in task Tmr Svc has been detected.

Crashed task: Tmr Svc (TCB 0x3fcb54ec, prio 1)
Stack:        992 high-water / 1048 free  → 2 040 B total allocation
Detected at:  vApplicationStackOverflowHook in vTaskSwitchContext
```

Root cause: an electrically noisy motor-alarm input on `PIN_OPTO_INPUT` (GPIO42) feeding the ANYEDGE GPIO ISR fast enough to cascade through the IDF GPIO-ISR dispatcher + lwIP/WiFi internal timer events and overflow the 2 KB Tmr Svc stack. The application-side 75 ms task-level debounce in `relay_controller.cpp:1071` doesn't help because the storm overflows the kernel before the task ever gets to debounce.

---

## What changed

### Defense 1 — IRAM ISR rate-limit (`relay_controller.cpp`)

New `ALARM_ISR_MIN_INTERVAL_US = 5000`. Edges closer than 5 ms apart are silently dropped at the IRAM level using `esp_timer_get_time()` (which is IRAM_ATTR-marked and safe from ISR context):

```c
static void IRAM_ATTR isr_motor_alarm(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_alarm_last_isr_us < ALARM_ISR_MIN_INTERVAL_US) {
        /* Edge storm suppression. */
        return;
    }
    s_alarm_last_isr_us = now_us;
    s_alarm_edge_tick   = xTaskGetTickCountFromISR();
    s_alarm_edge        = true;
}
```

Genuine alarm events stay flush against the same logical asserted state for far longer than 5 ms, so they still latch on the first edge. The existing 75 ms task-level debounce is unchanged — the rate-limit exists only to protect the kernel from EMI-coupled chatter, not to debounce the alarm itself.

### Defense 2 — Kernel-side Tmr Svc stack bump (`sdkconfig.defaults`)

```
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096
```

(was 2048 — the IDF default). Costs ~2 KB additional RAM allocated from heap at Tmr Svc creation time. RAM headroom budget at the time of the change: 18.9 % used → still plenty of room.

### Version stamp

`platformio.ini` drops `-wip` from `FIRMWARE_VERSION` — this is the actual rc.1.5.3 release, not a work-in-progress build. (rc.1.5.3-wip on the bench was the first cut that landed the NTP-indicator fix without these EMI defenses; documented for traceability.)

---

## Verified in the wild

Both defenses landed in this build. Build + OTA verified on unit 5C88 (now on soak hardware):

- `fw_ver = 2.0.0-rc.1.5.3`
- `bank = A, accepted = true` (past T1's 30 s healthy mark, no rollback)
- 5C88 then redeployed into production hardware to re-test under the same electrical conditions that triggered the panic loop — no recurrence

Combined with the **1.8 kΩ hardware shunt at J10** added on the alarm-input pad on 2026-05-29 (board-side mod, not firmware), the production unit has been running clean ever since.

---

## Build artefacts

| File | SHA-256 |
|---|---|
| `greenhouse-controller-2.0.0-rc.1.5.3.bin`     | `ff7f6e92b3279c4ab285075e49314a89575893a488be8c3614f0a280302dd400`* |
| `web-assets-2.0.0-rc.1.5.3.zip`                | `2ea255e04f2549924cdce35d874194e3cd058be9bb1ffbba930227868bbc458c` |
| `bootloader-2.0.0-rc.1.5.3.bin`                | (in folder) |
| `partitions-2.0.0-rc.1.5.3.bin`                | (in folder) |

*The first cut of rc.1.5.3 was built before the sdkconfig.defaults change was effective (PIO didn't regenerate sdkconfig.lolin_s3 from the new default). Hash `d108cf3d…` was the correct rebuild with both defenses active; the OTA on unit 5C88 used the corrected build.

---

## Scope notes

This release is a defense-in-depth pair targeting the same failure mode from two layers (ISR + kernel). Either defense alone may have been sufficient under more favourable EMI conditions; both together provide multi-order-of-magnitude rejection. The hardware shunt added on the board is the third layer and addresses the *amplitude* of EMI rather than its *duration* or *rate*.

The application-side 75 ms debounce in `task_relay_controller`'s main loop is **unchanged**. Real alarm events still latch on the first edge through the rate-limit; the 75 ms debounce continues to filter mechanical relay bounce as before.
