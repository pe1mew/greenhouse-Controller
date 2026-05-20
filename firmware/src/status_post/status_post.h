/**
 * @file status_post.h
 * @brief T14 — Status website POST task (Phase 9).
 *
 * Pushes the controller's runtime status to a configurable REST endpoint on
 * a fixed cycle (60–300 s), and uploads the most recently closed CSV log
 * file to the same endpoint on rotation and as a daily fallback.
 *
 * Implements the controller side of design/technical-spec-statusWebsite.md:
 *  - POST /api.php                  → status payload (this task)
 *  - POST /api.php?action=log       → log file upload (Phase E)
 *
 * All configuration lives in NVS_NS_SYSTEM and is exposed via the
 * cfg_shadow_t (see data_manager.h §Status website / web-tab settings).
 *
 * The task is on Core 0 alongside T10/T11 with priority TASK_PRIO_LOW; it
 * sleeps 1 s between iterations and only opens an HTTP connection when a
 * cycle is due. HTTPS endpoints are supported via WiFiClientSecure with
 * certificate validation disabled (setInsecure) — see the impact analysis
 * for the trade-off.
 *
 * ## Subsystem ownership
 *  - **Reads**: `dm_cfg_snapshot()` once per cycle; `dm_status_snapshot()`
 *    when a status POST is due; SD CSV files via `storage_sd_read()` during
 *    log upload (streamed in 4 KB chunks).
 *  - **Writes**: HTTPS POSTs to `cfg.status_url` (status + log endpoints);
 *    Q3 via `log_post()` (LOG_SYSTEM rows for outcome accounting);
 *    `dm_set_log_last_up()` on successful log upload (gh#25 dedup latch);
 *    module-private `s_last_str` / `s_last_log_str` strings rendered by the
 *    web GUI and LCD.
 *  - **Task-notify consumer**: `T14_NOTIFY_LOG_ROTATED` (from T9 on CSV
 *    rotation), `T14_NOTIFY_CFG_CHANGED` (from `dm_reload_web_cfg()` after
 *    `/api/web` POST).
 *
 * @see status_json.h          (shared canonical JSON builder)
 * @see status_post_supervisor.h (T15 — wedge/leak/respawn-storm watchdog)
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task-notify bit set by T9 when it rotates the active SD CSV file.
 *
 * T14 sets a `xTaskNotifyWait`-style mask for this bit during its main-loop
 * cycle wait. When T9's `rotate_sd_file()` closes the active CSV and opens a
 * new one, it calls `xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits)`.
 * T14 then reads the just-closed filename via `event_logger_last_rotated()`
 * and (subject to `cfg.log_upload_rot`) uploads it.
 *
 * Since 2.0.0-a.6.35.
 */
#define T14_NOTIFY_LOG_ROTATED  (1u << 0)

/**
 * @brief Task-notify bit fired by `dm_reload_web_cfg()` after any /api/web POST.
 *
 * Wakes T14 immediately from its idle wait so an operator-driven enable / URL /
 * interval change takes effect within ~1 s of clicking Apply rather than after
 * the full 60 s disabled-branch idle. The disabled and active branches both
 * accept this bit; on receipt T14 simply re-reads `cfg_shadow_t` and proceeds
 * with whatever state the new cfg dictates. The disabled→enabled transition
 * additionally clears `s_last_str` so the GUI shows `—` (pending) until the
 * next status POST completes — avoids the confusing window where the operator
 * sees `enable=1` in the form but `last_post=DISABLED` in the indicator.
 *
 * Since 2.0.0-a.6.35.1 (UX follow-up to a.6.35).
 */
#define T14_NOTIFY_CFG_CHANGED  (1u << 1)

/**
 * @brief T14 task entry. Spawned by main.cpp on Core 0 at TASK_PRIO_LOW.
 *
 * Loops forever; sleeps via `xTaskNotifyWait` with a 1 s cycle timeout
 * (`CYCLE_WAIT_MS`) or the 60 s idle re-check when disabled. The notify
 * mask consumes both `T14_NOTIFY_LOG_ROTATED` and `T14_NOTIFY_CFG_CHANGED`
 * atomically.
 *
 * @param pvParameters Unused; pass NULL.
 * @see   T14_NOTIFY_LOG_ROTATED, T14_NOTIFY_CFG_CHANGED
 */
void task_status_post(void *pvParameters);

/**
 * @brief Format the last-attempt outcome for display on the Web tab.
 *
 * Writes a short human-readable string into @p buf, e.g.
 *   "OK 2026-05-10 14:30:22"   (last cycle succeeded)
 *   "FAIL 2026-05-10 14:30:22" (last cycle failed)
 *   ""                          (no attempt yet this boot)
 *
 * @param buf  Destination buffer.
 * @param cap  Capacity of @p buf in bytes (NUL is always written if cap > 0).
 * @note  Safe to call from any task — copies the module's snapshot string
 *        without locking; the writer is single-threaded (T14 itself) and
 *        a torn read produces at worst a partial timestamp.
 */
void status_post_last_str(char *buf, size_t cap);

/**
 * @brief Format the last log-upload outcome for display on the Web tab.
 *
 * Same shape as status_post_last_str(), but for the periodic log upload
 * (Phase E). Returns an empty string until Phase E is wired up.
 *
 * @param buf  Destination buffer.
 * @param cap  Capacity of @p buf in bytes (NUL is always written if cap > 0).
 * @see   status_post_last_str()
 */
void status_post_last_log_str(char *buf, size_t cap);

/**
 * @brief Return true if T14's circuit breaker is currently open (in backoff).
 *
 * Surfaced as the `net_backoff_active` flag in the canonical status JSON
 * (rendered as a "Net backoff" badge on the Alarms card) and on the LCD.
 * Always returns false in Phase 1 (v1.17.34); wired to real breaker state
 * in Phase 2 (v1.17.35). Lock-free read of a primitive type — caller need
 * not hold any mutex.
 *
 * @return true iff the status-post or log-upload breaker is open.
 */
bool status_post_backoff_active(void);

/* ============================================================
 * Supervisor integration (gh#18 Phase 4, since 1.18.0)
 *
 * These three entry points let the T15 supervisor monitor T14 health and
 * recover from a wedged or leaking T14 incarnation without interrupting
 * primary climate control. They are private to the bulkhead-policy build
 * — no other consumer should call them.
 * ============================================================ */

/**
 * @brief Heartbeat counter incremented by T14 at the top of every loop tick.
 *
 * The supervisor (T15) snapshots this value and watches for staleness over
 * a 60-second window — if it does not advance for that long, T14 is
 * considered stuck and is force-respawned. Reader is racy with writer but
 * the value is a single 32-bit aligned store / load → coherent.
 *
 * Exposed via a getter rather than a raw `extern volatile uint32_t` so the
 * variable can stay file-local in status_post.cpp.
 */
uint32_t status_post_heartbeat(void);

/**
 * @brief Cumulative heap drop attributed to T14 since the last counter reset.
 *
 * T14 samples free internal heap immediately before and after each
 * HTTPS call (status POST + log upload). The signed delta is accumulated
 * here; negative deltas (heap freed back) are clamped to zero so transient
 * recovery doesn't mask a real leak. Supervisor compares against the
 * 64 KB planned-reboot threshold.
 */
uint32_t status_post_heap_drop_bytes(void);

/**
 * @brief Force a clean teardown of the persistent TLS session.
 *
 * Called by the supervisor immediately before `vTaskDelete(task_t14)`. The
 * static `WiFiClientSecure` (`s_secure`) survives task deletion — without
 * this call, the next T14 incarnation would inherit a half-closed socket
 * pointing at lwIP state that the killed task never had a chance to release.
 * Idempotent: safe to call multiple times.
 */
void status_post_force_teardown(void);

#ifdef __cplusplus
}
#endif
