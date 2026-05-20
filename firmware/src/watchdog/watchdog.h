/**
 * @file watchdog.h
 * @brief T1 — Watchdog / Heartbeat task (Phase 6.N.1 / 2.0.0-alpha.6.22).
 *
 * Minimal T1 — just the two responsibilities that gate the rest of the
 * system:
 *
 *   1. Subscribe to the IDF TWDT and kick it on every 500 ms tick. The
 *      other long-running tasks (T4, T6, T7, T9, …) each subscribe
 *      themselves; T1 exists so a panic-free boot has at least *one*
 *      subscriber registered before the TWDT begin call counts down.
 *
 *   2. Call `ota_mark_healthy()` exactly once, after OTA_HEALTHY_MS of
 *      stable uptime. This resets the boot-fail counter in NVS so a
 *      fresh OTA bank that survives ≥ 30 s is no longer eligible for
 *      rollback. Combined with `ota_check_rollback()` at the start of
 *      app_main, this implements the 3-fail rollback flow from
 *      `ota_manager.h`.
 *
 * Features deliberately deferred to a later phase (matches the
 * minimal-then-extend pattern of T10 / T14 / T11):
 *
 *   - NeoPixel day/night brightness control (1.20.3 used Adafruit_NeoPixel;
 *     2.0.0 will use the IDF RMT driver — separate change)
 *   - 60-second heap-free + largest-block + PSRAM-free LOG_SYSTEM rows
 *   - 30-second-offset heap-integrity check
 *   - 10-minute stack-HWM sweep across all task handles
 *
 * These will arrive together in a future alpha.6.22.X sub-phase once
 * the minimal task is verified stable on hardware. The instrumentation
 * is high-value but high-surface-area; getting the rollback wiring
 * exercised in isolation first keeps the bisect window narrow.
 *
 * Stack: 4 KB (4096 BYTES — IDF's xTaskCreatePinnedToCore takes bytes,
 * NOT FreeRTOS-vanilla words). 2 KB was tried first and caused an
 * immediate boot-loop ("stack overflow in task T1-WDT") because each
 * ESP_LOGI call consumes ~150 B of per-call buffer and nvs_cfg_set_i32
 * needs another 400 B. 4 KB matches T11 and gives headroom for the
 * deferred instrumentation (heap_caps_check_integrity_all walks the
 * entire heap).
 *
 * @author  Greenhouse Controller project
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task period in ms — drives the WDT kick + every other periodic check.
 *
 * 500 ms gives a 60 s heap-row cadence at tick % 120 == 0, a 30 s-offset
 * heap-integrity check at tick % 120 == 60, and a 10 min stack-HWM sweep
 * at tick % 1200 == 0. Changing this constant requires rederiving those
 * modulos and the OTA-healthy threshold (currently OTA_HEALTHY_MS / T1_TICK_MS).
 */
#define T1_TICK_MS  500u

/**
 * @brief T1 entry point — subscribes to TWDT, kicks every 500 ms,
 *        marks the boot healthy at OTA_HEALTHY_MS.
 *
 * Suggested xTaskCreatePinnedToCore parameters:
 *   - stack:    4096   (BYTES — ESP-IDF convention, NOT FreeRTOS words)
 *   - priority: 1      (low — the work is light)
 *   - core:     1      (APP_CPU — same as the other low-prio tasks)
 *
 * @param pvParameters  Unused; pass NULL.
 * @note This task subscribes to the IDF TWDT on entry — if it ever stops
 *       kicking, the panic handler dumps a coredump. Do not delete the
 *       task without first calling `esp_task_wdt_delete(NULL)`.
 * @warning A 2 KB stack causes an immediate boot-loop; do not reduce.
 */
void task_watchdog(void *pvParameters);

#ifdef __cplusplus
}
#endif
