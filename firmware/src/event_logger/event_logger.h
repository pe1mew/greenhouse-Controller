/**
 * @file event_logger.h
 * @brief Q3 log-posting helper and T9 Event Logger task (Phase 5).
 *
 * ## Responsibilities
 *
 * - `log_post()` — single entry point for all Q3 producers; enforces
 *   drop-oldest overflow policy (Gap H).
 * - `log_take_dropped_count()` — atomically read and reset the Q3 drop
 *   counter; called by T9 after each drain pass.
 * - `task_event_logger()` — T9 task; drains Q3, persists every event to
 *   the NVS ring buffer and (if an SD card is present) appends a CSV line
 *   to the current SD log file.
 *
 * ## Drop-oldest overflow policy (Gap H)
 *
 * FreeRTOS queues provide no native drop-oldest semantics for multi-element
 * queues.  `xQueueOverwrite()` exists but is valid only for depth-1 queues.
 * All producers that post to Q3 **must** call `log_post()` — never
 * `xQueueSend(Q3, ...)` directly — so that the drop-oldest policy is
 * enforced at a single location.
 *
 * ### Two-step evict-and-retry pattern
 *
 * ```
 *  1. xQueueSend(Q3, evt, 0)         -- common path; returns immediately
 *     → pdPASS  → done
 *     → pdFAIL  → queue is full; continue to step 2
 *
 *  2. xQueueReceive(Q3, &discard, 0) -- evict the oldest entry
 *     g_q3_dropped++                 -- count the eviction
 *
 *  3. xQueueSend(Q3, evt, 0)         -- retry
 *     → pdPASS  → done
 *     → pdFAIL  → rare concurrent-sender race; new event is lost
 *                 g_q3_dropped++
 * ```
 *
 * ### Thread-safety of the drop counter
 *
 * `g_q3_dropped` is a `volatile uint32_t` protected by a FreeRTOS spinlock
 * (`portMUX_TYPE`).  `portENTER_CRITICAL()` / `portEXIT_CRITICAL()` are used
 * for the increment in `log_post()` and the read-and-reset in
 * `log_take_dropped_count()`.  Critical sections on the ESP32-S3 are
 * spinlock-based and extremely short (< 10 cycles), so the latency impact on
 * calling tasks is negligible.
 *
 * ### Concurrent-sender race condition
 *
 * `xQueueReceive` and the retry `xQueueSend` are not executed atomically.
 * If two tasks both arrive at step 2 simultaneously, one may take the slot
 * freed by the other's eviction, causing the second sender's retry to also
 * fail.  The net effect is that both the evicted oldest entry *and* the
 * second sender's new event are lost.  This is an acknowledged trade-off:
 *
 * - Q3 has depth 32; T9 drains it continuously.
 * - A full-queue burst generates at most ~10 events (e.g. a wind event).
 * - The race requires two senders to interleave within a sub-microsecond
 *   window; in practice this is extremely rare.
 * - A mutex around the entire evict-and-retry sequence would eliminate the
 *   race but adds latency in time-critical callers (T3, T6).  Deferred until
 *   profiling shows it is needed.
 *
 * ### Drop counter surfacing
 *
 * T9 calls `log_take_dropped_count()` after each drain pass.  If the
 * returned count is non-zero, T9 emits one synthetic `LOG_SYSTEM` event with
 * `value_a = (int16_t)count` so that queue pressure is visible in the log
 * without losing any current event to report it.  The synthetic event is
 * posted via `xQueueSend(Q3, ..., 0)` directly — **not** via `log_post()` —
 * to avoid re-entrant eviction.
 *
 * ## T9 SD log rotation
 *
 * Log files are named `/ghc_NNNN.csv` (4-digit zero-padded sequential index
 * stored in NVS `log/file_idx`).  A new file is started when the current
 * file reaches 512 KB.  At most 10 files are retained; the oldest is deleted
 * on each rotation that would exceed this limit.
 *
 * ## CSV line format
 *
 * ```
 * timestamp,type,initiator,ch,param,value_a,value_b
 * 1776014381,SENSOR,SYS,0,0,11,81
 * 1776014390,ALARM,SYS,0,0,80,70
 * ```
 *
 * Field      | Type    | Description
 * -----------|---------|---------------------------------------------------
 * timestamp  | uint32  | Unix epoch seconds
 * type       | string  | SENSOR / RELAY / MODE / SETPT / SESSION / ALARM / SYSTEM
 * initiator  | string  | SYS / FARMER / ADMIN / MQTT / WEB
 * ch         | uint8   | Motor channel (1/2/3) or 0 for non-motor events
 * param      | uint8   | log_param_id_t; 0 for non-CONFIG events
 * value_a    | int16   | First payload (sensor value, reason code, etc.)
 * value_b    | int16   | Second payload (threshold, new setting, etc.)
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include "../types/app_types.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Public API — log producer helper
 * ----------------------------------------------------------------------- */

/**
 * @brief Post a log event to Q3 with drop-oldest overflow protection.
 *
 * Non-blocking.  On overflow, evicts the oldest entry in Q3 and retries
 * once.  Increments an internal drop counter for each event that cannot be
 * delivered (either the evicted oldest entry, or the rare race-condition
 * miss on the retry).
 *
 * All firmware tasks that produce log events **must** call this function
 * instead of calling `xQueueSend(Q3, ...)` directly.
 *
 * @param evt  Pointer to the log event to post.  The struct is copied
 *             into the queue by value; the caller may reuse or free the
 *             pointed-to memory immediately after this call returns.
 */
void log_post(const log_event_t *evt);

/**
 * @brief Return and atomically reset the Q3 drop counter.
 *
 * Intended to be called by T9 only, after each drain pass.  If the
 * returned value is non-zero, T9 should emit a `LOG_SYSTEM` event
 * recording the count before resetting it.
 *
 * @return  Number of log events dropped since the last call to this
 *          function (or since boot).
 */
uint32_t log_take_dropped_count(void);

/* -----------------------------------------------------------------------
 * T9 task entry point
 * ----------------------------------------------------------------------- */

/**
 * @brief T9 — Event Logger task (Phase 5 full implementation).
 *
 * Sole consumer of Q3.  On each drain pass:
 *
 *  1. Calls `xQueueReceive(Q3, &evt, portMAX_DELAY)` — blocks until the
 *     first event of the pass is available.
 *  2. Calls `xQueueReceive(Q3, &evt, 0)` in a loop to drain all remaining
 *     immediately-available events without blocking.
 *  3. For each event:
 *     - Appends a 12-byte binary record to the NVS ring buffer via
 *       `nvs_log_append()` (always; fallback store when SD is absent).
 *     - If an SD card is mounted, appends a CSV line to the current file
 *       via `storage_sd_write_append()`; rotates to a new file when the
 *       current file reaches 512 KB; deletes the oldest file when more
 *       than 10 files exist.
 *  4. After the drain pass, calls `log_take_dropped_count()`; if > 0,
 *     posts a synthetic `LOG_SYSTEM` event directly to Q3 (not via
 *     `log_post()`) with `value_a` = drop count.
 *
 * Falls back gracefully to NVS-only logging if the SD card is absent or
 * returns an error (FR-LG07, FR-LG08).  A LOG_SYSTEM event with
 * `value_a = −1` is emitted into the NVS log when SD becomes unavailable
 * mid-session.
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_event_logger(void *pvParameters);
