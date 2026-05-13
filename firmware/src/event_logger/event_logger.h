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
 * Log files are named `YYYYMMDDHHMMSS.csv` (local-time timestamp of creation,
 * e.g. `20250607163022.csv`).  Lexicographic sort = chronological order.
 * A new file is started when the current file reaches 512 KB.  At most 10
 * files are retained; the lexicographically oldest is deleted on each
 * rotation that would exceed this limit.  If free space drops below 2 MB,
 * the oldest file is proactively deleted; if already at the 3-file retention
 * floor, SD logging is suspended and NVS is used as fallback.
 *
 * Old sequential-index files (`ghc_NNNN.csv`) are ignored by the scan
 * filter and will not interfere with the new naming scheme.
 *
 * ## CSV line format
 *
 * ```
 * timestamp,type,initiator,ch,param,value_a,value_b
 * 2025-06-07T14:30:22,SENSOR,SYS,0,0,235,650
 * 2025-06-07T14:30:30,ALARM,SYS,0,0,80,70
 * ```
 *
 * Field      | Type    | Description
 * -----------|---------|---------------------------------------------------
 * timestamp  | string  | ISO 8601 UTC: YYYY-MM-DDTHH:MM:SS
 * type       | string  | SENSOR / RELAY / MODE / SETPT / SESSION / ALARM / SYSTEM
 * initiator  | string  | SYS / FARMER / ADMIN / MQTT / WEB
 * ch         | uint8   | Motor channel (1/2/3) or 0 for non-motor events
 * param      | uint8   | log_param_id_t; 0 for non-CONFIG events
 * value_a    | int16   | First payload (sensor value, reason code, etc.)
 * value_b    | int16   | Second payload (threshold, new setting, etc.)
 *
 * ## LOG_SYSTEM value_a encoding
 *
 * value_a categorises the SYSTEM-event subtype. The producers and their
 * conventions:
 *
 * value_a | meaning           | value_b                              | producer
 * --------|-------------------|--------------------------------------|--------------------------
 *   0     | T14 outcome / skip  | see "value_a=0 sub-codes" below      | T14 status_post.cpp
 *   1     | STA (WiFi client)   | 0 = disconnected, 1 = connected      | T10 network_manager.cpp
 *   2     | NTP                 | 0 = timeout,      1 = synced         | T10 network_manager.cpp
 *   3     | AP                  | 0 = stopped,      1 = started        | T10 network_manager.cpp
 *   4     | geolocation         | 1 = success                          | T10 network_manager.cpp
 *   5     | BOOT (since 1.17.27)| esp_reset_reason_t value (1–10)      | main.cpp setup()
 *   6     | force-rotate marker (since 1.17.28) | 0 = unused              | T14 → event_logger
 *  -1     | Q3 drop-overflow    | dropped count                        | T9 (synthetic)
 *
 * ### value_a=0 sub-codes (T14 outcome / diagnostic skip)
 *
 * The initiator is always LOG_BY_WEB. value_b distinguishes the outcome:
 *
 * value_b | meaning
 * --------|---------------------------------------------------------------
 *    0    | status POST attempt — failed (only on streak transitions)
 *    1    | log upload attempt — failed
 *    2    | daily-slot fired but no closed file on SD (since 1.17.27)
 *    3    | daily-slot fired but precondition blocked it (since 1.17.27).
 *           Blocking cause: status disabled / URL empty / WiFi down /
 *           pre-NTP / OTA in progress. Read the next/previous SYSTEM
 *           events to identify which one.
 *
 * For value_a=1 (success) the same value_b codes apply: 0 = status POST,
 * 1 = log upload. Codes 2 and 3 are skip-diagnostics only (value_a=0).
 *
 * The BOOT entry (value_a = 5) is posted once per boot, before any task is
 * scheduled, so every fresh SD log file starts with a verdict on why the
 * previous boot ended (POWERON / PANIC / TASK_WDT / BROWNOUT / …). The
 * esp_reset_reason_t codes are listed in the comment at the top of
 * main.cpp::setup().
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
 * Rotation-tracking helpers (T14 upload-on-rotation + daily fallback)
 * ----------------------------------------------------------------------- */

/**
 * @brief Return the filename of the most recently *rotated-away* CSV file.
 *
 * Set by T9 immediately before each rotation overwrites s_cur_filename.
 * Cheap: simply reads an in-memory string under a short critical section.
 * Returns the bare filename (no leading '/'), e.g. "20260507143022.csv".
 *
 * @param out  Destination buffer; always NUL-terminated on return.
 * @param cap  Capacity of @p out. 24 bytes is sufficient.
 * @return true if at least one rotation has occurred this boot, false otherwise.
 */
bool event_logger_last_rotated(char *out, size_t cap);

/**
 * @brief Force T9 to rotate the active SD log file.
 *
 * Sets an internal request flag that T9 polls after each drain pass.
 * T9 closes the current file (which becomes "closed" and detectable via
 * event_logger_newest_closed() and event_logger_last_rotated()) and
 * opens a new file with the current timestamp.
 *
 * Used by T14's daily-upload path to force a fresh nightly snapshot when
 * the active file has not yet reached the 512 KB rotation threshold.
 * Without this, controllers that emit events slowly (one SENSOR every
 * 30 s = ~1.5 KB/h ≈ 36 KB/day) would never accumulate enough to trigger
 * a natural rotation, and the daily upload slot would have nothing to send.
 *
 * Posts a synthetic LOG_SYSTEM event (value_a=6, "force-rotate marker")
 * to Q3 to wake T9 from its receive-block. The marker is written to the
 * outgoing CSV file as its last entry, documenting why the file was
 * closed.
 *
 * Blocks the caller for up to @p timeout_ms waiting for the rotation to
 * complete. Returns false on timeout or when SD logging is currently
 * inactive (no card mounted).
 *
 * Safe to call from any task context.
 *
 * @param timeout_ms Maximum wait, milliseconds (5000 is reasonable).
 * @return true if rotation completed; false on timeout / SD inactive.
 */
bool event_logger_force_rotate(uint32_t timeout_ms);

/**
 * @brief Return the lexicographically newest closed CSV file on SD.
 *
 * Scans the SD card for *.csv files and returns the newest name that is not
 * the currently active (open) file. Suitable for T14's daily-fallback path
 * when no rotation has happened since boot. Falls back to the most-recently
 * rotated file in memory when the SD scan finds no candidate.
 *
 * @param out  Destination buffer; always NUL-terminated on return.
 * @param cap  Capacity of @p out. 24 bytes is sufficient.
 * @return true if a closed file was found, false if none exists or SD is
 *         unavailable.
 */
bool event_logger_newest_closed(char *out, size_t cap);

/* -----------------------------------------------------------------------
 * SD card mount / unmount helpers (called by T11 web-server endpoints)
 * ----------------------------------------------------------------------- */

/**
 * @brief Attempt to mount the SD card and re-enable SD logging in T9.
 *
 * Calls `storage_init()` and, on success, writes a CSV header to the current
 * log file if it is empty, then sets T9's internal `s_sd_ok` flag so that
 * subsequent events are written to SD.
 *
 * Safe to call from any task context: when `s_sd_ok` is false (the only case
 * where this is useful) T9 never touches the SD bus, so there is no
 * contention on the SPI peripheral.
 *
 * @return true if the card is now mounted and logging is active.
 * @return false if `storage_init()` failed or the header write failed.
 */
bool event_logger_sd_remount(void);

/**
 * @brief Stop SD logging in T9 and unmount the SD card.
 *
 * Clears T9's internal `s_sd_ok` flag first, then calls
 * `storage_sd_unmount()`.  Clearing the flag before unmounting prevents T9
 * from attempting a write to a card that is being torn down.
 */
void event_logger_sd_unmount(void);

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
