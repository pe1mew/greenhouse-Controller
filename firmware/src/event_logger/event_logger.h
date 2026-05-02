/**
 * @file event_logger.h
 * @brief Q3 log-posting helper and T9 task declaration.
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
 * without losing any current event to report it.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include "../types/app_types.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Public API
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
 * @brief T9 — Event Logger task.
 *
 * Sole consumer of Q3.  On each wake:
 *   1. Calls `xQueueReceive(Q3, &evt, portMAX_DELAY)` to block until an
 *      event is available.
 *   2. Appends the event to the NVS ring buffer via `nvs_log_append()`.
 *   3. If an SD card is mounted, appends a CSV line to the current log
 *      file via `storage_sd_write_append()`; rotates the file when it
 *      reaches 512 KB; deletes the oldest file when count > 10.
 *   4. After each drain pass, calls `log_take_dropped_count()`; if > 0,
 *      constructs and posts a synthetic `LOG_SYSTEM` event (via
 *      `xQueueSend(Q3, ..., 0)` directly — **not** via `log_post()` —
 *      to avoid re-entrant eviction) with `value_a` = drop count.
 *
 * Falls back gracefully to NVS-only logging if SD card is absent or
 * returns an error (FR-LG07, FR-LG08).
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_event_logger(void *pvParameters);
