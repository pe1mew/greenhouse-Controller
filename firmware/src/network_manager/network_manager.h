/**
 * @file network_manager.h
 * @brief T10 — Network Manager task declaration + boot-time WiFi init helper.
 *
 * Public interface for the firmware's WiFi/network subsystem. Two entry
 * points are exposed:
 *
 *  1. `nm_wifi_init_blocking()` — called once from `app_main` BEFORE T10's
 *     task is spawned. Brings up the WiFi STA, registers the long-lived
 *     WIFI_EVENT/IP_EVENT handler, runs SNTP. Blocks until IP is acquired
 *     or a configurable timeout fires. Folded in from the retired
 *     `wifi_tickle.cpp` scaffold in 2.0.0-rc.1.3 — same logic, same boot
 *     ordering, the function just lives in T10's home now.
 *
 *  2. `task_network_manager()` — long-running T10 task spawned by main.cpp
 *     after `nm_wifi_init_blocking()` returns. Owns the Q5 producer role,
 *     sends `DM_NOTIFY_NTP_SYNCED` to T4 after the initial SNTP sync,
 *     handles AP-mode lifecycle (alpha.6.29), and runs the periodic 24 h
 *     NTP resync (a.6.33).
 *
 * ## Task graph
 *   - Spawned by `app_main` at boot (priority 3, stack 6 KB, tskNO_AFFINITY).
 *   - Q5 producer: posts `net_status_t` snapshot every NET_POLL_MS (5 s).
 *   - TN4 producer: notifies `task_t4` with `DM_NOTIFY_NTP_SYNCED` after
 *     the initial SNTP sync so T4 writes the post-SNTP system time back
 *     to the DS1307 RTC.
 *   - Reads `task_t4` handle from `system_globals.cpp` (must be spawned first).
 *
 * ## Thread safety
 *   - `nm_wifi_init_blocking()` is single-threaded boot code (called
 *     exactly once before any task starts).
 *   - `task_network_manager()` is the sole owner of the AP-mode state +
 *     reconnect timer state; no external callers should touch those
 *     directly.
 *
 * @see status_post.cpp — T14 consumes the WiFi state implicitly via
 *      `esp_netif_get_*`, not via this header.
 * @see types/app_types.h — defines Q5, task_t10, net_status_t.
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return status from `nm_wifi_init_blocking()`.
 *
 * Renamed from `wifi_tickle_status_t` in 2.0.0-rc.1.3 when the function was
 * folded into T10. Numeric values match the legacy enum 1:1 so any external
 * caller that compared against the integer values still works.
 */
typedef enum {
    NM_WIFI_OK              = 0, /**< Connected, got IP, SNTP synced — happy path. */
    NM_WIFI_OK_NO_NTP       = 1, /**< Connected, got IP, but SNTP timed out — STA is up, time-of-day will catch up via DS1307 or next sync attempt. */
    NM_WIFI_NO_SSID         = 2, /**< NVS `wifi/ssid` is empty — stack up, STA-connect skipped. AP-mode recovery flow still available. */
    NM_WIFI_INIT_FAILED     = 3, /**< esp_netif/esp_wifi init returned error — WiFi driver is broken. T11 (web) will not spawn. */
    NM_WIFI_CONNECT_TIMEOUT = 4, /**< STA_START fired but no STA_GOT_IP within @p connect_timeout_ms — AP out of range / wrong credentials. */
    NM_WIFI_DISCONNECTED    = 5, /**< STA_DISCONNECTED before STA_GOT_IP — auth failure or AP missing. */
} nm_wifi_status_t;

/**
 * @brief Blocking boot-time WiFi STA bring-up + SNTP sync.
 *
 * Folded in from the retired `wifi_tickle.cpp` (2.0.0-rc.1.3). Reads
 * SSID/PSK from NVS via LIB-7, brings up STA mode, registers the long-lived
 * WIFI_EVENT/IP_EVENT handler (the same handler stays alive after this
 * returns and drives subsequent reconnects via the exponential-backoff
 * timer added in alpha.6.31), waits for `IP_EVENT_STA_GOT_IP`, then runs
 * SNTP for up to ~10 s.
 *
 * Stays connected after returning. Called once from `app_main` BEFORE
 * `task_network_manager` is spawned — T10's monitoring loop assumes WiFi
 * init has already happened.
 *
 * @param connect_timeout_ms  How long to wait for IP_EVENT_STA_GOT_IP.
 *                            Recommend 10000 (10 s) — same value the old
 *                            `wifi_tickle_run()` used.
 * @return nm_wifi_status_t outcome.
 */
nm_wifi_status_t nm_wifi_init_blocking(uint32_t connect_timeout_ms);

/**
 * @brief T10 — Network Manager task entry point.
 *
 * Long-running monitoring + AP-management loop. Spawn ONCE from `app_main`
 * via `xTaskCreatePinnedToCore` AFTER `nm_wifi_init_blocking()` has
 * returned (the task assumes esp_wifi is already up and the event handler
 * is already registered).
 *
 * Per-iteration responsibilities (every NET_POLL_MS = 5 s):
 *  - Sample current netif/wifi state (`snapshot_state()`).
 *  - Overwrite Q5 with the latest `net_status_t` (queue depth 1,
 *    `xQueueOverwrite` semantics — T8 reads the latest, never blocks).
 *  - Poll `wifi/ap_enable` from NVS; toggle AP mode on/off as it changes.
 *  - On AP active: enforce the `cfg.ap_timeout_min` auto-shutdown.
 *  - Once-per-boot: notify T4 with `DM_NOTIFY_NTP_SYNCED` when the SNTP
 *    sync first lands. Subsequent re-syncs (24 h cadence) do not re-notify.
 *  - Periodic NTP resync at 24 h cadence (a.6.33).
 *
 * Never returns under normal operation.
 *
 * @param pvParameters  Unused; pass `NULL`.
 * @note   Stack should be ≥ 6 KB — handles geolocation HTTP fetch which
 *         needs a transient esp_http_client + JSON buffer.
 * @note   Priority 3 — same as T14. Network state polling is
 *         latency-tolerant; T2/T3/T4/T5/T6 (priorities 4-6) preempt cleanly.
 * @warning Must be spawned AFTER `task_t4` so the `DM_NOTIFY_NTP_SYNCED`
 *          notification has a valid handle to send to.
 * @see    nm_wifi_init_blocking() — must be called before this task is spawned.
 * @see    types/app_types.h — Q5, net_status_t, task_t4, task_t10 declarations.
 */
void task_network_manager(void *pvParameters);

/**
 * @brief Has SNTP actually completed since boot? (rc.1.5.4)
 *
 * Returns the `s_sntp_synced` module-scope latch from network_manager.cpp.
 * The latch is set only when `esp_sntp_get_sync_status()` reports
 * `SNTP_SYNC_STATUS_COMPLETED` — once from `nm_sntp_quick_sync()` at boot,
 * and again from each successful `run_ntp_resync()` on the 24 h cadence.
 *
 * Use this accessor instead of testing `time(NULL) > NTP_MIN_EPOCH`:
 * the time-based heuristic is fooled by T4's DS1307 RTC pre-seed at
 * boot+500ms, which primes the system clock with a plausible 2026 epoch
 * before SNTP has ever run. The pre-seed makes `time(NULL)` plausible
 * regardless of internet presence, so any code path that drives a
 * user-visible "NTP synced" indicator off the time comparison shows a
 * false positive on units with battery-backed RTCs.
 *
 * rc.1.5.3 fixed `snapshot_state()` (which feeds Q5 → T8's LCD); rc.1.5.4
 * adds this accessor so `dm_status_snapshot()` (which feeds the web
 * GUI's /api/status JSON) can use the same trustworthy source.
 *
 * @return `true` if SNTP completed at least once since boot, else `false`.
 * @note Monotonic-rising per boot. Cleared only by reboot.
 * @note Safe to call from any task — read of a single `bool` is atomic
 *       on Xtensa and the variable has no other writers besides the two
 *       network_manager.cpp call sites.
 */
bool nm_is_sntp_synced(void);

#ifdef __cplusplus
}
#endif
