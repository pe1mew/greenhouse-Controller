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
 * @brief T14 task entry. Spawned by main.cpp on Core 0 at TASK_PRIO_LOW.
 * @param pvParameters Unused; pass NULL.
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
 * @param cap  Capacity of @p buf in bytes.
 */
void status_post_last_str(char *buf, size_t cap);

/**
 * @brief Format the last log-upload outcome for display on the Web tab.
 *
 * Same shape as status_post_last_str(), but for the periodic log upload
 * (Phase E). Returns an empty string until Phase E is wired up.
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
