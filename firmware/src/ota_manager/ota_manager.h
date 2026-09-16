/**
 * @file ota_manager.h
 * @brief T13 — OTA Manager: dual-bank firmware and web-asset update with
 *        3-consecutive-fail rollback (Phase 10).
 *
 * Owns the inactive firmware OTA partition and the inactive LittleFS
 * partition during an update.  T13 itself is NOT a permanent task — T11
 * (web server) spawns it on demand from ota_assets_end() when the web-asset
 * ZIP has been fully buffered.  All other entry points (ota_firmware_*,
 * ota_assets_begin/accumulate, status accessors) run in the caller's task
 * context (typically T11 on Core 0).
 *
 * ## Firmware OTA flow
 *
 * T11 (web server) calls ota_firmware_begin() / ota_firmware_write() /
 * ota_firmware_end() from its HTTP body callback as each chunk arrives.
 * The inactive firmware bank is written in streaming fashion — no PSRAM
 * staging required.  ota_firmware_end() verifies the image and enters
 * FW_DONE; the boot partition is switched later, together with the assets
 * (or by the 120 s firmware-only fallback — see ota_firmware_end()).
 *
 * ## Session ownership and release
 *
 * The task whose ota_*_begin() returned true owns the session until the
 * session installs or is released. Every way a session can end short of
 * installing goes through ONE teardown, which aborts the esp_ota handle,
 * forgets the staged image, frees the ZIP buffer, cancels the fallback timer
 * and clears EG1_BIT_OTA_IN_PROGRESS — then leaves ERROR (or IDLE for a
 * deliberate back-out), from which the next upload is accepted at once. It
 * also writes a LOG_SYSTEM value_a=32 row. The manager releases the session
 * itself when one of its own calls fails; a caller releases it with
 * ota_firmware_abort() / ota_assets_abort() when the failure is its own (the
 * upload connection broke). Only the owner may call those: after one of the
 * manager's calls has returned false the session is already gone, and a
 * second release could hit a session another task has opened since.
 *
 * Before 2026-09-16 the web upload's receive-failure exit released nothing.
 * One upload cut off by a lossy WiFi link left the unit in FW_WRITING with
 * the EG1 bit set: every later upload was refused, ROTA skipped every check,
 * and only a reboot cleared it.
 *
 * ## Web-asset OTA flow
 *
 * T11 accumulates the ZIP archive entirely in PSRAM via
 * ota_assets_begin() / ota_assets_accumulate() / ota_assets_end().
 * ota_assets_end() spawns T13 (task_ota_manager) to:
 *   1. Mount the *inactive* LittleFS partition.
 *   2. Extract all STORE-only ZIP entries to that partition.
 *      (Deflate-compressed entries are rejected — use `zip -0`.)
 *   3. Write manifest.json as the final step.
 *   4. Unmount, switch the boot partition, and schedule a 1 s reboot.
 *
 * T11 continues to serve from the *active* LittleFS partition throughout;
 * MX5 is not acquired during the inactive-partition write (TSDS §4.3 T13).
 *
 * ## 3-fail rollback
 *
 * ota_check_rollback() is called at the start of setup() (after NVS init).
 * It increments NVS `system/ota_fail_cnt` on every boot.
 * ota_mark_healthy() resets the counter to 0 after OTA_HEALTHY_MS of
 * uptime; T1 (watchdog task) calls it once after 30 s of operation.
 * If the counter reaches 3 before a healthy boot, the current app is
 * invalidated and the previous OTA bank is restored.
 *
 * ## EG1 bit
 *
 * EG1_BIT_OTA_IN_PROGRESS is set by ota_firmware_begin() /
 * ota_assets_begin() and cleared on completion or error.  T1 (watchdog) and
 * T10 (network manager) read this bit to suppress non-essential activity
 * while the flash is being written.
 *
 * ## Reboot path (rc.1.2)
 *
 * All terminal paths (asset success, firmware-only fallback, scheduled
 * reboot) ultimately call schedule_reboot(), which posts a FreeRTOS
 * software timer.  The timer callback (reboot_timer_cb) does NOT call
 * esp_restart() directly — it spawns reboot_worker_task with a 4 KB stack,
 * because esp_restart()'s WiFi-teardown path (esp_wifi_stop) consumes
 * several KB of stack and would overflow the FreeRTOS timer-service task's
 * configTIMER_TASK_STACK_DEPTH allotment.  This carve-off was introduced
 * in 2.0.0-rc.1.2 to fix a stack-overflow panic during scheduled reboots.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Time after which a boot is considered healthy ──────────────────────── */

/**
 * @brief Boot stability threshold for OTA rollback bookkeeping.
 *
 * Milliseconds of uptime after which T1 (watchdog) calls ota_mark_healthy()
 * to reset the consecutive-fail counter to zero.  Set conservatively so a
 * crash inside a long startup task still counts as a failed boot.
 *
 * Units: milliseconds.  Default: 30 000 ms (30 s).
 */
#define OTA_HEALTHY_MS  30000U  /**< ms before ota_mark_healthy() resets the fail counter */

/* ── OTA state machine ──────────────────────────────────────────────────── */

/**
 * @brief OTA state machine — exposed to T11 via ota_get_state() and to the
 *        web UI via /api/ota/status.
 *
 * Linear progression for the firmware-then-assets path:
 *   IDLE → FW_WRITING → FW_VERIFYING → FW_DONE → ASSETS_BUFFERING →
 *   ASSETS_WRITING → REBOOTING.
 *
 * Asset-only OTA skips the FW_* states and starts in ASSETS_BUFFERING.
 * Any failure releases the session (see "Session ownership and release")
 * and transitions to ERROR; ota_get_error() then returns a human-readable
 * message. Nothing has been installed in that case.
 *
 * A failure after FW_DONE — the asset upload breaks, or its extraction
 * fails — discards the verified firmware too. ota_assets_begin() already
 * cancelled the firmware-only fallback, so the only way that image could
 * still be installed is together with the assets it was waiting for; with
 * those gone, nothing is installed and the operator uploads both again. The
 * alternative, re-arming the fallback, would install the firmware alone and
 * strand the asset partition, which is what the paired commit exists to
 * prevent.
 */
typedef enum {
    OTA_STATE_IDLE              = 0,  /**< No OTA in progress */
    OTA_STATE_FW_WRITING        = 1,  /**< Firmware chunks streaming in */
    OTA_STATE_FW_VERIFYING      = 2,  /**< esp_ota_end() in progress */
    OTA_STATE_ASSETS_BUFFERING  = 3,  /**< ZIP chunks accumulating in PSRAM */
    OTA_STATE_ASSETS_WRITING    = 4,  /**< T13 extracting ZIP to inactive LittleFS */
    OTA_STATE_REBOOTING         = 5,  /**< Reboot scheduled (1 s delay) */
    OTA_STATE_ERROR             = 6,  /**< Failed — see ota_get_error() */
    OTA_STATE_FW_DONE           = 7,  /**< Firmware verified; awaiting web-asset upload
                                       *   before the boot partition is switched.
                                       *   A 120 s fallback timer will commit the
                                       *   firmware-only update if no assets arrive. */
} ota_state_t;

/**
 * @brief Why an OTA session ended without installing anything.
 *
 * Recorded in the LOG_SYSTEM value_a=32 row (high byte of value_b; see
 * event_logger.h) and worded into the /api/ota/status error text. Each
 * failure reason points at a different fix: 1-2 the network, 3 the unit,
 * 4-5 the file, 6 the unit's flash.
 */
typedef enum {
    OTA_END_BACKED_OUT    = 0,  /**< Deliberate, not a failure: ROTA's final quiet
                                 *   gate. The only reason that leaves IDLE. */
    OTA_END_CONN_LOST     = 1,  /**< The uploader's connection closed or failed
                                 *   mid-body */
    OTA_END_STALLED       = 2,  /**< The uploader stopped sending (T11's stall bound) */
    OTA_END_SETUP_FAILED  = 3,  /**< No inactive partition, esp_ota_begin(), the
                                 *   PSRAM buffer, or the T13 spawn failed */
    OTA_END_WRITE_FAILED  = 4,  /**< esp_ota_write() refused a chunk (not an app
                                 *   image?), or the ZIP over- or under-ran its
                                 *   declared length */
    OTA_END_VERIFY_FAILED = 5,  /**< esp_ota_end(): the image failed verification */
    OTA_END_COMMIT_FAILED = 6,  /**< Extraction or the boot-partition switch failed */
} ota_end_reason_t;

/* ─────────────────────────────────────────────────────────────────────────
 * Rollback management
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Check for repeated boot failures and trigger OTA rollback if needed.
 *
 * Must be called in setup() after nvs_cfg_init() and before tasks are spawned.
 * Increments the NVS `system/ota_fail_cnt` counter on every boot.
 * If the counter reaches 3, calls esp_ota_mark_app_invalid_rollback_and_reboot()
 * to restore the previous firmware bank.  ota_mark_healthy() must be called
 * after OTA_HEALTHY_MS of uptime to reset the counter.
 *
 * @note T15 planned reboots (`system/t15_planreboot` NVS flag set with
 *       reset_reason == ESP_RST_SW) are exempted from the increment so that
 *       a deliberate supervisor restart cannot accumulate into a rollback.
 * @warning Calls esp_ota_mark_app_invalid_rollback_and_reboot() on the third
 *       consecutive failure — does not return on success.  Any work scheduled
 *       to run after this call in setup() will not execute when rollback fires.
 * @see   ota_mark_healthy(), OTA_HEALTHY_MS.
 */
void ota_check_rollback(void);

/**
 * @brief Mark the current boot as healthy and reset the fail counter to 0.
 *
 * Called by T1 (watchdog task) after OTA_HEALTHY_MS of stable operation.
 * Safe to call multiple times — a write to NVS with the same value is a
 * cheap no-op once the page is cached.
 *
 * @see ota_check_rollback(), ota_is_accepted().
 */
void ota_mark_healthy(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Firmware OTA — called from T11 HTTP body callback
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Prepare the inactive OTA partition for a firmware write.
 *
 * Resolves the inactive partition via esp_ota_get_next_update_partition()
 * and opens it with esp_ota_begin().  Sets EG1_BIT_OTA_IN_PROGRESS so that
 * other tasks can suppress non-essential activity during the flash write.
 * Transitions state IDLE/ERROR → OTA_STATE_FW_WRITING.
 *
 * The state is claimed BEFORE esp_ota_begin(), whose erase takes seconds, so a
 * second begin in that window is refused rather than opening a second handle
 * on the same partition.
 *
 * @param  total_bytes  Expected firmware image size in bytes.  May be 0 if the
 *                      caller does not know the size up front (e.g. chunked
 *                      transfer-encoding); esp_ota_begin() then uses
 *                      OTA_SIZE_UNKNOWN.
 * @return true on success — the caller now owns the session; false if already
 *         busy (state ≠ IDLE/ERROR; the session is someone else's, so do NOT
 *         abort it) or if the setup fails (session released, state ERROR).
 * @note   Idempotent against a previous failed attempt — a state of ERROR is
 *         treated as "ready to retry".
 * @see    ota_firmware_write(), ota_firmware_end(), ota_firmware_abort().
 */
bool ota_firmware_begin(size_t total_bytes);

/**
 * @brief Write a chunk of the firmware image to the inactive partition.
 *
 * Must only be called after a successful ota_firmware_begin().  Updates the
 * progress percentage if total_bytes was known.  On any esp_ota_write()
 * error the session is released (OTA_END_WRITE_FAILED) and the state moves
 * to OTA_STATE_ERROR.
 *
 * @param  chunk  Pointer to chunk data; must not be NULL.
 * @param  len    Chunk length in bytes.
 * @return true on success; false on wrong state or on write error (state
 *         transitions to ERROR).
 * @see    ota_firmware_begin(), ota_firmware_end().
 */
bool ota_firmware_write(const uint8_t *chunk, size_t len);

/**
 * @brief Finalise and verify the firmware write; wait for web-asset upload.
 *
 * Calls esp_ota_end() to verify the image SHA-256.  The boot partition is
 * NOT switched here — that happens atomically together with the LittleFS
 * partition swap at the end of the web-asset upload (task_ota_manager).
 *
 * Enters OTA_STATE_FW_DONE and starts a 120 s fallback timer.  If
 * ota_assets_begin() is called before the timer expires the timer is
 * cancelled; otherwise the timer commits the firmware-only update
 * (switches boot partition and reboots without touching LittleFS).
 *
 * @return true if verification succeeded; false on wrong state or on
 *         esp_ota_end() failure (session released, state ERROR).
 * @note   The fallback timer fires after FW_DONE_FALLBACK_MS (120 s) of
 *         inactivity in OTA_STATE_FW_DONE; the operator may pair this call
 *         with an immediate ota_assets_begin() to commit firmware+assets
 *         together, or rely on the timer to commit firmware alone.
 * @see    ota_assets_begin().
 */
bool ota_firmware_end(void);

/**
 * @brief Release a firmware session that has NOT been finalised.
 *
 * The caller's own failure exit: the upload connection broke or went silent
 * (T11), or ROTA's R-P03 quiet gate failed after the image was staged but
 * before ota_firmware_end() (T16) — backing out there instead of entering
 * FW_DONE, whose fallback timer would otherwise install the firmware alone.
 * Releases everything the session holds (see "Session ownership and release")
 * without touching the boot partition, and leaves ERROR — or IDLE for
 * OTA_END_BACKED_OUT, which is not a failure.
 *
 * Owner only. No-op unless the state is FW_WRITING.
 *
 * @param  why  What ended it; recorded in the value_a=32 row and the error text.
 * @return true if a session was released; false if none was open.
 */
bool ota_firmware_abort(ota_end_reason_t why);

/* ─────────────────────────────────────────────────────────────────────────
 * Web-asset OTA — called from T11 HTTP body callback
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Allocate a PSRAM buffer to receive the web-asset ZIP archive.
 *
 * Cancels any pending firmware-only fallback timer armed by a preceding
 * ota_firmware_end() — the asset upload supersedes the fallback path and
 * commit is deferred until task_ota_manager finishes extraction.  Sets
 * EG1_BIT_OTA_IN_PROGRESS and transitions to OTA_STATE_ASSETS_BUFFERING.
 *
 * Accepted prior states: IDLE, ERROR, FW_DONE. From FW_DONE the state is
 * claimed before anything else, so the fallback's worker — which commits only
 * while it still sees FW_DONE — cannot install the firmware alone once an
 * asset upload has started. From then on the verified image is installed
 * with these assets or not at all (see ota_state_t).
 *
 * @param  total_bytes  Exact size of the ZIP file in bytes.  Must equal the
 *                      sum of subsequent ota_assets_accumulate() chunks.
 * @return true on success — the caller now owns the session; false if another
 *         OTA is in progress (state ≠ IDLE/ERROR/FW_DONE; do NOT abort it) or
 *         if heap_caps_malloc(SPIRAM) fails (session released — including a
 *         verified firmware from FW_DONE — state ERROR).
 * @warning Allocates total_bytes of PSRAM up front — caller must verify
 *          heap availability for the largest expected ZIP (~512 KB today).
 * @see    ota_assets_accumulate(), ota_assets_end().
 */
bool ota_assets_begin(size_t total_bytes);

/**
 * @brief Copy a received ZIP chunk into the PSRAM accumulation buffer.
 *
 * Bounds-checks `offset + len` against the total size declared at
 * ota_assets_begin().  Chunks may arrive out of order — `offset` is the
 * authoritative position within the buffer.
 *
 * @param  chunk   Pointer to chunk data; must not be NULL.
 * @param  len     Chunk length in bytes.
 * @param  offset  Byte offset of this chunk in the total ZIP (matches the
 *                 HTTP body cursor T11 maintains).
 * @return true on success; false on wrong state or on buffer overrun
 *         (session released, state ERROR).
 */
bool ota_assets_accumulate(const uint8_t *chunk, size_t len, size_t offset);

/**
 * @brief Release an asset session that has not been handed to T13.
 *
 * The asset counterpart of ota_firmware_abort(), for the caller's own failure
 * exit (T11: the upload connection broke or went silent). Frees the PSRAM
 * buffer, and if this session continued a verified firmware (FW_DONE) that
 * image is discarded with it — nothing is installed, and the error text asks
 * for firmware and assets again. Leaves ERROR.
 *
 * Owner only. No-op unless the state is ASSETS_BUFFERING.
 *
 * @param  why  What ended it; recorded in the value_a=32 row and the error text.
 * @return true if a session was released; false if none was open.
 */
bool ota_assets_abort(ota_end_reason_t why);

/**
 * @brief Verify the complete ZIP is buffered and spawn T13 to process it.
 *
 * Confirms s_zip_rcvd == s_zip_total, transitions to
 * OTA_STATE_ASSETS_WRITING, and creates task_ota_manager pinned to Core 0
 * with a 16 KB stack.  Ownership of the PSRAM buffer is transferred to T13,
 * which frees it on completion or error.
 *
 * @return true if T13 was spawned; false on wrong state, incomplete buffer,
 *         or xTaskCreate failure (session released, state ERROR).
 * @see    task_ota_manager().
 */
bool ota_assets_end(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Status accessors — called from T11 GET /api/ota/status
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Return the current OTA state machine state.
 *
 * Thread-safe — acquires the internal module mutex if it has been created.
 * Safe to call before any OTA session has started; returns OTA_STATE_IDLE.
 *
 * @return Current ota_state_t value.
 */
ota_state_t ota_get_state(void);

/**
 * @brief Return OTA progress as a percentage (0–100).
 *
 * Meaning depends on the current state:
 *  - OTA_STATE_FW_WRITING       — fraction of expected firmware bytes received.
 *  - OTA_STATE_ASSETS_BUFFERING — fraction of ZIP bytes received.
 *  - OTA_STATE_ASSETS_WRITING   — coarse milestone (extraction → manifest →
 *                                 boot-partition switch).
 *  - All other states           — last-set value (0 at session boundaries).
 *
 * @return Progress percentage clamped to 0–100.
 */
uint8_t     ota_get_progress_pct(void);

/**
 * @brief Return a pointer to the last error message string.
 *
 * Valid only when ota_get_state() == OTA_STATE_ERROR.
 * The string is module-static and remains valid until the next OTA begins.
 *
 * @return NUL-terminated error string; empty string if no error.
 */
const char *ota_get_error(void);

/**
 * @brief Return the active OTA bank label.
 *
 * Reads the running partition via esp_ota_get_running_partition().
 *
 * @return 'A' if booted from app0 (Bank A), 'B' if booted from app1 (Bank B),
 *         '?' if the running partition cannot be determined.
 */
char ota_get_active_bank(void);

/**
 * @brief Return true if the current boot has been marked healthy.
 *
 * Reads the NVS boot-fail counter.  Returns true when the counter is 0,
 * meaning ota_mark_healthy() has been called (after OTA_HEALTHY_MS of uptime)
 * and no rollback is pending.  Returns false during the first 30 s after boot
 * or after a failed boot that has not yet recovered.
 *
 * @return true if counter == 0; false otherwise (including NVS read error,
 *         which defaults the counter to 0 → returns true: conservatively safe
 *         since rollback only fires at counter ≥ 3).
 * @see    ota_check_rollback(), ota_mark_healthy().
 */
bool ota_is_accepted(void);

/* ─────────────────────────────────────────────────────────────────────────
 * T13 task entry point
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief T13 — OTA Manager task entry point (spawned on demand by T11).
 *
 * Processes the PSRAM-buffered ZIP archive: mounts the inactive LittleFS
 * partition (formatting it on first-time mount failure, alpha.6.24),
 * extracts STORE-method entries, preserves the manifest.json shipped inside
 * the ZIP, unmounts, switches the active OTA bank if firmware was also
 * uploaded in this session, and schedules a 1 s reboot via
 * schedule_reboot() → reboot_worker_task.  Deletes itself on completion or
 * error.
 *
 * Asset-only sessions (no paired firmware upload) take a special fix-up
 * path: assets are mirrored to the *active* LittleFS partition and the
 * boot partition is NOT switched.  This prevents a stranded-asset / unbootable
 * bank rollback after a clean `pio run -t upload` flash (1.17.3 fix).
 *
 * Only STORE (uncompressed) ZIP entries are supported.  Deflate-compressed
 * entries cause the task to abort with an error.  Build the ZIP with:
 *   @code
 *   zip -0 assets.zip index.html style.css app.js
 *   @endcode
 *
 * @param  pvParameters  Unused; pass NULL.
 * @warning This task takes exclusive ownership of the PSRAM ZIP buffer
 *          allocated in ota_assets_begin() and frees it on every exit path.
 *          Callers must not access s_zip_buf after ota_assets_end() returns
 *          true.
 * @see     ota_assets_end(), extract_zip_store().
 */
void task_ota_manager(void *pvParameters);
