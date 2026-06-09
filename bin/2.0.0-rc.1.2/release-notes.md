# 2.0.0-rc.1.2 — OTA reboot stack-overflow fix

Patch release on top of rc.1.1. **One-file C/C++ change** in `ota_manager.cpp` plus the version bump. Fixes a Tmr Svc task-stack overflow on every paired-OTA reboot — the cause of the residual coredump investigated in the rc.1.1 release notes (§ "Pre-soak post-deploy verification + coredump cleanup").

Supersedes rc.1.1 as the Phase 7 soak candidate; the 14-day clock restarts at day 0.

## The defect

The follow-up session spawned at the end of the rc.1.1 deploy decoded `coredump-rc.1.1-pre-soak-residual.bin` against a rebuilt rc.1 ELF (45 732 B raw dump, app-SHA prefix `438a2fdfa`). The panic was **not** in the OTA POST handler (where the original hypothesis pointed) but in the FreeRTOS timer-service task:

```
prvTimerTask  (FreeRTOS Tmr Svc, ~2 KB stack)
  prvProcessExpiredTimer
    reboot_timer_cb           (ota_manager.cpp)
      esp_restart             (esp_system.c:49)
        esp_wifi_stop
          ieee80211_ioctl
            wifi_stop_process
              queue_send_wrapper → STACK OVERFLOW
```

`esp_restart()` performs a graceful WiFi teardown (`esp_wifi_stop` → 802.11 ioctls → queue_send) that consumes several KB of stack — more than the FreeRTOS timer-service task's `configTIMER_TASK_STACK_DEPTH` (~2 KB) allotment. `reboot_timer_cb` was calling `esp_restart()` directly in the timer-service context, so every paired-OTA reboot ran the WiFi-teardown chain in that constrained stack.

The author had already half-acknowledged this gotcha at `ota_manager.cpp:212-213` ("`schedule_reboot` itself creates another one-shot timer that runs `esp_restart()` in xTimerService context") and had worked around the same constraint for SD-log writes by carving off `fw_done_commit_task` in alpha.6.34 — but `reboot_timer_cb` itself had not been moved off the timer task. The overflow was racy enough (depends on lwIP/WiFi cleanup work at reboot time, which scales with active connections) that most reboots survived; the rc.1.1 deploy happened to land in a state that didn't.

## Reproduction (during rc.1.2 verification)

The reproduction was **deterministic** under the rc.1.2 verification workflow:

1. Device running rc.1.1, coredump partition cleared.
2. Paired OTA: rc.1.1 → rc.1.2 (raw `.bin` POST, then STORE-only `.zip` POST, both via the standard endpoints).
3. T13 extraction completed; `schedule_reboot(1000)` armed; `reboot_timer_cb` fired in the timer-service task; `esp_restart()` invoked; **timer stack overflowed, panic logged to coredump partition, watchdog reset**.
4. New boot landed cleanly on rc.1.2 (the OTA *was* committed before the panic — `esp_ota_set_boot_partition` ran fine, only the graceful WiFi shutdown choked).
5. `GET /api/coredump/status` returned `present:true`, 45 316 B, embedded app-SHA prefix `ec8fcddde` (= rc.1.1).

So rc.1.1's bug fires on its **own** reboot to rc.1.2 — the final crash of the buggy version as it hands off to the patched one. Every rc.1.x upgrade up to rc.1.1 produced a fresh coredump on this exact path; we just hadn't been pulling them.

## The fix

```diff
-/* FreeRTOS timer callback: performs the deferred system restart. */
-static void reboot_timer_cb(TimerHandle_t xTimer)
-{
-    (void)xTimer;
-    ESP_LOGI(TAG, "[OTA] Rebooting now");
-    esp_restart();
-}
+/* Worker spawned by reboot_timer_cb. esp_restart() performs WiFi teardown
+ * (esp_wifi_stop → 802.11 ioctls → queue_send_wrapper) that consumes several
+ * KB of stack — more than the FreeRTOS timer service task's ~2 KB allotment
+ * (configTIMER_TASK_STACK_DEPTH). Same carve-off pattern as
+ * fw_done_commit_task. */
+static void reboot_worker_task(void *pv)
+{
+    (void)pv;
+    ESP_LOGI(TAG, "[OTA] Rebooting now");
+    esp_restart();
+    /* unreachable */
+}
+
+/* FreeRTOS timer callback: spawns reboot_worker_task with a 4 KB stack so
+ * esp_restart()'s WiFi teardown doesn't blow the timer-service stack. */
+static void reboot_timer_cb(TimerHandle_t xTimer)
+{
+    (void)xTimer;
+    BaseType_t rc = xTaskCreate(reboot_worker_task,
+                                "ota_reboot",
+                                4096,
+                                NULL,
+                                5,        /* priority — matches fw_done_commit_task */
+                                NULL);
+    if (rc != pdPASS) {
+        ESP_LOGE(TAG, "[OTA] reboot_worker_task spawn failed (%d) — "
+                      "falling back to in-timer esp_restart() (may overflow)",
+                 (int)rc);
+        esp_restart();
+    }
+}
```

`firmware/src/ota_manager/ota_manager.cpp:132-162`. Stack 4096 + priority 5 match the existing `fw_done_commit_task` carve-off (alpha.6.34) so the two reboot paths run with identical resources.

The `xTaskCreate`-failure fallback still calls `esp_restart()` in-timer (best-effort — heap exhaustion at OTA-commit time is rare enough not to warrant a more elaborate retry; if the spawn fails the device may overflow exactly like rc.1.1 did, but at least it tries to reboot rather than silently sitting in `OTA_STATE_REBOOTING` forever).

## Verification

End-to-end on bench unit `192.168.20.160`:

| Step | Pre-OTA fw | OTA | Post-OTA fw | Coredump |
|------|------------|-----|-------------|----------|
| rc.1.1 → rc.1.2 | rc.1.1 | clean upload + extract + reboot | rc.1.2 (uptime ~125 s) | **present** (45 316 B, app-SHA `ec8fcddde` = rc.1.1) — final crash on the buggy outgoing reboot |
| Erase dump | — | — | — | absent |
| rc.1.2 → rc.1.2 (reflash) | rc.1.2 | clean upload + extract + reboot | rc.1.2 (uptime 101 s after reboot) | **absent** — fix's first self-exercise produced zero dumps |

The rc.1.2 → rc.1.2 reflash is the canonical verification: it forces the new firmware to exercise its own `schedule_reboot` / `reboot_timer_cb` / `reboot_worker_task` chain end-to-end. No coredump after that cycle confirms the carve-off works.

## What did NOT change

- Web GUI, web assets, all other firmware C/C++ (canonical JSON, LCD code, every task graph, every endpoint behaviour).
- Static RAM footprint (`reboot_worker_task` is only alive for the few milliseconds between spawn and `esp_restart()`, so the 4 KB stack is transient).
- Flash usage delta: +204 B (1 353 761 → 1 353 965 → 1 353 965 B vs rc.1 / rc.1.1 / rc.1.2 — the two functions plus a new `xTaskCreate` call).
- Acceptance criteria from rc.1 / rc.1.1 — all carry over verbatim, **plus** one new assertion: zero coredumps on the paired-OTA reboot path (already implied by "zero unplanned reboots / zero coredumps").

## Phase 7 soak — clock reset

A patch release on top of rc.1.1 — even a one-file one — counts as a new candidate, so **the 14-day clock restarts at day 0 against rc.1.2**, same convention as rc.1.1 vs rc.1. The bench unit at `192.168.20.160` carries the rc.1.2 paired binary + assets and runs against the production status server (`https://pe1mew.nl/hbwv/api.php`) at `status_interval_s = 120`.

## Bench checklist before leaving the unit alone

Same checklist as rc.1.1, with the OTA reboot bullet promoted from "happy path" to "explicit verification":

1. Boot the unit → confirm `fw_ver=2.0.0-rc.1.2`, `asset_version=2.0.0-rc.1.2`, `eg1=0`, `mode=AUTOMATIC`, `flags=[]`.
2. Confirm `GET /api/coredump/status` returns `present:false` and stays that way for the full 14 days.
3. **Trigger one paired-OTA reflash mid-soak (e.g. day 7) and re-verify coredump status remains `present:false` afterwards.** This catches a regression of the rc.1.x OTA-reboot panic if anything in the reboot path drifts.
4. Walk the full diagnostics chain once (deliberate panic on bench → coredump captured → blue badge → Download → `idf.py coredump-info` → Erase → badge gone).
5. Daily GUI smoke test (login, view status, change a setpoint, download a log, see audit rows fire) for the full 14 days.

## After the soak passes

- Tag `v2.0.0` on the merge commit
- Fast-forward merge `dev/2.0.0-esp-idf` → `main`
- Run `bin/build_release.ps1` from main → publishes `bin/2.0.0/`
- Operator deploys to Unit 2 first (7-day observation), then Unit 1

## Status

Day 0 of Phase 7 soak. Day 14 = `v2.0.0` if green across the board.
