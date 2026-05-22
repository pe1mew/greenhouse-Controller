# Post-mortem — rc.1.3.2 Phase 7 soak panic 2026-05-22 13:23

| Field | Value |
|---|---|
| Firmware | 2.0.0-rc.1.3.2 (ELF SHA matches coredump) |
| Unit | 0x2344 (test bench, 192.168.20.160) |
| Boot time | 2026-05-19 11:18:29 |
| Panic time | 2026-05-22 13:23:09–13:23:21 (12 s panic→reset→RTC window) |
| Uptime at panic | ~74 h 4 min 40 s |
| Reset reason | `ESP_RST_PANIC` (4) — confirmed via SD log boot-marker row |
| Coredump | 45 540 B / 45 KB, captured to flash, retrieved via `/api/coredump/download` |
| Coredump status | Decoded cleanly with `esp-coredump info_corefile` |

## Conclusion

**Root cause is a missing `esp_sntp_stop()` in the 24-hour NTP-resync path (T10).** It is **not** the heap-fragmentation pattern (gh#23) we initially suspected.

The `run_ntp_resync()` function in `firmware/src/network_manager/network_manager.cpp:1187` calls `esp_sntp_setoperatingmode()` then `esp_sntp_init()` to kick a fresh sync, but never calls `esp_sntp_stop()` afterwards. The first resync (~24 h after boot) succeeds and leaves the SNTP client running. The next time the 24 h trigger fires, the call to `esp_sntp_setoperatingmode(POLL)` hits lwIP's assertion in `lwip/src/apps/sntp/sntp.c:748` — *"Operating mode must not be set while SNTP client is running"* — because the underlying `sntp_pcb` is still allocated from the previous `esp_sntp_init()`.

The assertion fires inside lwIP's tcpip thread (`tiT`), triggering `panic_abort` → `esp_system_abort` → coredump capture → `ESP_RST_PANIC`.

## Evidence

### From the coredump

```
Crashed task handle: 0x3fccbf2c, name: 'tiT'
Panic reason: assert failed: sntp_setoperatingmode
              /IDF/components/lwip/lwip/src/apps/sntp/sntp.c:748
              (Operating mode must not be set while SNTP client is running)

Stack:
#0 panic_abort                         panic.c:481
#1 esp_system_abort                    esp_system_chip.c:87
#2 __assert_func                       assert.c:80
#3 sntp_setoperatingmode               sntp.c:748              ← lwIP assert
#4 do_setoperatingmode                 sntp.c:163
#5 tcpip_thread_handle_msg             tcpip.c:201
#6 tcpip_thread                        tcpip.c:148
#7 vPortTaskWrapper                    port.c:139
```

All other tasks were idle / waiting at panic time. No deadlock, no stack overflow, no out-of-memory. Free heap immediately before panic: 102 KB; largest contiguous block: 31 KB (these are the steady-state values seen throughout the 74-hour run — the heap-fragmentation theory does not apply here).

### From the SD log (pre-panic file `20260520224920.csv`)

```
2026-05-22T13:23:09,SYSTEM,SYS,0,0,7,102     <- T1 heap probe: free 102 KB
2026-05-22T13:23:09,SYSTEM,SYS,0,0,8,8153    <- PSRAM free 8153 KB
2026-05-22T13:23:09,SYSTEM,SYS,0,0,12,31     <- largest contig 31 KB
                  ▼  12-second gap = panic + reset + RTC restore  ▼
2026-05-22T13:23:21,SYSTEM,SYS,0,0,5,4       <- reset_reason = 4 (ESP_RST_PANIC)
2026-05-22T13:23:21,SYSTEM,SYS,0,0,11,9028   <- file-rotation header (unit 0x2344)
2026-05-22T13:23:21,SYSTEM,SYS,0,0,18,45     <- coredump_present, size 45 KB
2026-05-22T13:23:21,RELAY,SYS,1,0,4,0        <- M1 reset state
2026-05-22T13:23:21,RELAY,SYS,2,0,4,0        <- M2 reset state
2026-05-22T13:23:21,RELAY,SYS,3,0,4,0        <- M3 reset state
2026-05-22T13:23:22,SYSTEM,SYS,0,0,1,1       <- boot snapshot: STA up
2026-05-22T13:23:22,SYSTEM,SYS,0,0,2,1       <- boot snapshot: NTP synced
```

The SD log boot-marker (`value_a=5, value_b=4`) confirms `ESP_RST_PANIC` independently of the coredump.

### From the source — `network_manager.cpp:1187` (run_ntp_resync)

```c
static void run_ntp_resync(void)
{
    ESP_LOGI(TAG, "[T10] Starting periodic NTP resync (24 h cadence)");

    sntp_sync_status_t st = esp_sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
        ESP_LOGI(TAG, "[T10] NTP resync: another sync already in progress — skipping");
        return;
    }

    /* Setup. esp_sntp_init() is repeatable; calling it again kicks a new
     * query against the configured server. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);   // ← lwIP asserts here on 2nd call
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();                                // ← allocates sntp_pcb

    /* wait, success, update s_last_ntp_sync_us, return — */
    /* but NEVER calls esp_sntp_stop() / esp_netif_sntp_deinit().      */
    ...
}
```

The neighbouring boot-time function `nm_sntp_quick_sync()` does the cleanup correctly — it uses `esp_netif_sntp_init()` / `esp_netif_sntp_deinit()` (the high-level helper that pairs init with deinit). The resync path uses the low-level API but skips the matching stop.

The author's comment block at line 1148 incorrectly claims *"esp_sntp_setoperatingmode / setservername are idempotent — they only..."* — that assumption is what produced the bug. `esp_sntp_setoperatingmode()` is **not** idempotent if SNTP is already running; lwIP asserts.

## Sequence of events

| Wall-clock | Action | SNTP state after |
|---|---|---|
| 2026-05-19 11:18:29 | Boot — `nm_sntp_quick_sync()` runs `esp_netif_sntp_init` → `esp_netif_sntp_deinit` (matched pair) | Stopped (`sntp_pcb == NULL`) |
| 2026-05-19 11:18:40 | T10's post-boot init seeds `s_last_ntp_sync_us = esp_timer_get_time()` | Stopped |
| **~2026-05-20 11:18** | **1st `run_ntp_resync()` fires.** `esp_sntp_setoperatingmode(POLL)` works because `sntp_pcb == NULL`. `esp_sntp_init()` allocates `sntp_pcb`. Sync completes. `s_last_ntp_sync_us` updated. | **Running (`sntp_pcb != NULL`)** |
| (Timing uncertainty here — see "Timing detective note" below) | | |
| **2026-05-22 13:23:09** | **2nd `run_ntp_resync()` fires.** `esp_sntp_setoperatingmode(POLL)` is called on still-running SNTP. lwIP assertion at sntp.c:748 fires from inside tcpip thread. | **Panic** |
| 2026-05-22 13:23:21 | Reset complete. `nm_wifi_init_blocking()` runs, NTP re-syncs cleanly (boot-time path uses the matched pair). |
| 2026-05-22 13:46 | Coredump retrieved via `/api/coredump/download` after operator intervention. |

### Timing detective note

The expected second-resync fire time was ~2026-05-21 11:18 (24 h after the first resync). The actual fire time was ~26 h later than expected. Likely explanations (cannot disambiguate from the data we have):

- The first resync's `s_last_ntp_sync_us` update happened later in the day than 11:18 (e.g. WiFi was busy and the loop iteration deferred the resync trigger by several iterations);
- Or a transient `prev.client_connected = false` window suppressed the trigger for an iteration;
- Or NET_POLL_MS coarse-grains the trigger.

The exact timing is academic for the fix — the **structural bug** is the missing stop, and it would have manifested on the second resync regardless of when that second resync fired.

## Fix proposal

**Single-line defensive addition** — call `esp_sntp_stop()` at the top of `run_ntp_resync()`. Calling stop when SNTP is already stopped is a no-op in lwIP (it just notices `sntp_pcb == NULL` and returns), so this is safe on first invocation. On subsequent invocations it cleans up the prior session before `esp_sntp_setoperatingmode()` is called.

```c
static void run_ntp_resync(void)
{
    ESP_LOGI(TAG, "[T10] Starting periodic NTP resync (24 h cadence)");

    /* rc.1.3.3 — defensive stop. Prior invocations left sntp_pcb allocated
     * (via esp_sntp_init below) without a matching stop, which made the
     * SECOND invocation hit lwIP's assert in sntp_setoperatingmode (sntp.c:748,
     * "Operating mode must not be set while SNTP client is running").
     * esp_sntp_stop() on an already-stopped client is a no-op, so this is
     * safe on every entry. */
    esp_sntp_stop();

    sntp_sync_status_t st = esp_sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
        ESP_LOGI(TAG, "[T10] NTP resync: another sync already in progress — skipping");
        return;
    }
    ...
}
```

### Alternative — symmetry with the boot-time path

A more uniform fix is to make `run_ntp_resync()` use the high-level `esp_netif_sntp_init / esp_netif_sntp_deinit` pair just like `nm_sntp_quick_sync()` does. That removes the dual-API maintenance burden and makes the init/deinit symmetry obvious. Code-volume change is comparable.

The minimal fix above is recommended for rc.1.3.3 because:
- It's a one-line patch that doesn't restructure working code.
- The boot-time path and the resync path remain visibly different, which the existing doxygen calls out.
- A larger refactor can land separately if/when the dual-API surface becomes a maintenance problem.

### Also update the misleading comment

Strike the comment in lines 1147–1150 that claims `esp_sntp_setoperatingmode` is idempotent. Replace with a note that lwIP asserts when called on a running client, hence the defensive stop above.

## Release impact

- **rc.1.3.2 is invalidated** as a Phase 7 soak candidate. The bug is reproducible at ~24-hour intervals after boot; any soak unit will eventually hit it. There is no field workaround other than rebooting at < 48 h intervals.
- **No production units were affected** (rc.1.3.2 only ran on the test bench).
- **Next release**: `2.0.0-rc.1.3.3` carrying the single-line fix in `run_ntp_resync()`. Phase 7 soak day-counter restarts at day 0 on rc.1.3.3.
- **GitHub issue**: open `gh#28 — T10 NTP resync missing sntp_stop() causes lwIP assert at 2nd cycle` as a tracking issue; close once rc.1.3.3 passes its 14-day soak.

## What the gh#23-style heap data tells us (separate observation)

The pre-panic heap samples consistently show:
- Free internal heap: 102–104 KB
- Largest contiguous block: 31 KB

The 71 KB gap between these is **not** the gh#23 fragmentation pattern — it is the normal ESP-IDF runtime layout for this firmware (web-server task buffers, mbedTLS context, lwIP buffers, all live in non-contiguous chunks). The values were stable throughout the 74-hour run with no drift. This is reassuring data for the ESP-IDF-migration gh#23 closure claim and a counter-data-point against the heap-fragmentation hypothesis I initially formed.

## Files in this directory

| File | Purpose |
|---|---|
| `coredump-rc.1.3.2-panic-20260522T1323.bin` | Raw coredump from `/api/coredump/download` (45 KB) |
| `coredump.core.elf` | Decoded core ELF, produced by `esp-coredump --save-core` |
| `coredump_status.json` | `/api/coredump/status` snapshot at retrieval time |
| `sd-log-20260520224920.csv` | Pre-panic SD log (containing the panic moment + 12s gap + boot marker) |
| `sd-log-20260522132717.csv` | Post-restart SD log (first 20 min of new boot) |
| `status.json` | `/api/status` snapshot at retrieval time (uptime=1238 s) |
| `login.json` | `/api/login` response (admin role granted) |
| `log_files.json` | `/api/log/files` listing |
| `.cookies` | Curl cookie jar holding the admin session — should be deleted after the post-mortem closes |
| `POST_MORTEM.md` | This document |

## Reproducible decode command

```bash
export PYTHONUTF8=1
"$HOME/.platformio/penv/Scripts/esp-coredump.exe" \
    --chip esp32s3 info_corefile \
    --gdb "$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb.exe" \
    --core "bin/2.0.0-rc.1.3.2/post-mortem/coredump-rc.1.3.2-panic-20260522T1323.bin" \
    --core-format raw \
    --save-core "bin/2.0.0-rc.1.3.2/post-mortem/coredump.core.elf" \
    "bin/2.0.0-rc.1.3.2/firmware-2.0.0-rc.1.3.2.elf"
```

For interactive gdb after `--save-core` produces the `.elf`:

```bash
"$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb.exe" \
    -ex "set pagination off" \
    -ex "set print pretty on" \
    -ex "file bin/2.0.0-rc.1.3.2/firmware-2.0.0-rc.1.3.2.elf" \
    -ex "core-file bin/2.0.0-rc.1.3.2/post-mortem/coredump.core.elf" \
    -ex "info threads" \
    -ex "thread apply all bt" \
    -ex "quit"
```
