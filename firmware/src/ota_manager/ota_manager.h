/**
 * @file ota_manager.h
 * @brief T13 — OTA Manager: dual-bank firmware and web-asset update with
 *        3-consecutive-fail rollback (Phase 10).
 *
 * ## Firmware OTA flow
 *
 * T11 (web server) calls ota_firmware_begin() / ota_firmware_write() /
 * ota_firmware_end() from its HTTP body callback as each chunk arrives.
 * The inactive firmware bank is written in streaming fashion — no PSRAM
 * staging required.  ota_firmware_end() sets the new boot partition and
 * schedules a 1 s reboot via a FreeRTOS software timer.
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
 * ota_assets_begin() and cleared on completion or error.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Time after which a boot is considered healthy ──────────────────────── */
#define OTA_HEALTHY_MS  30000U  /**< ms before ota_mark_healthy() resets the fail counter */

/* ── OTA state machine ──────────────────────────────────────────────────── */
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
 */
void ota_check_rollback(void);

/**
 * @brief Mark the current boot as healthy and reset the fail counter to 0.
 *
 * Called by T1 (watchdog task) after OTA_HEALTHY_MS of stable operation.
 */
void ota_mark_healthy(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Firmware OTA — called from T11 HTTP body callback
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Prepare the inactive OTA partition for a firmware write.
 *
 * Sets EG1_BIT_OTA_IN_PROGRESS.
 *
 * @param total_bytes  Expected firmware image size in bytes (may be 0 if unknown).
 * @return true on success; false if already busy or if esp_ota_begin fails.
 */
bool ota_firmware_begin(size_t total_bytes);

/**
 * @brief Write a chunk of the firmware image to the inactive partition.
 *
 * Must only be called after a successful ota_firmware_begin().
 *
 * @param chunk  Pointer to chunk data.
 * @param len    Chunk length in bytes.
 * @return true on success; false on write error (state transitions to ERROR).
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
 * @return true if verification succeeded; false on error.
 */
bool ota_firmware_end(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Web-asset OTA — called from T11 HTTP body callback
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief Allocate a PSRAM buffer to receive the web-asset ZIP archive.
 *
 * Sets EG1_BIT_OTA_IN_PROGRESS.
 *
 * @param total_bytes  Exact size of the ZIP file in bytes.
 * @return true on success; false if already busy or if PSRAM alloc fails.
 */
bool ota_assets_begin(size_t total_bytes);

/**
 * @brief Copy a received ZIP chunk into the PSRAM accumulation buffer.
 *
 * @param chunk   Pointer to chunk data.
 * @param len     Chunk length in bytes.
 * @param offset  Byte offset of this chunk in the total ZIP (= HTTP index).
 * @return true on success; false on buffer overrun or wrong state.
 */
bool ota_assets_accumulate(const uint8_t *chunk, size_t len, size_t offset);

/**
 * @brief Verify the complete ZIP is buffered and spawn T13 to process it.
 *
 * Transitions state to OTA_STATE_ASSETS_WRITING and creates task_ota_manager.
 * Ownership of the PSRAM buffer is transferred to T13, which frees it on
 * completion or error.
 *
 * @return true if T13 was spawned; false on error.
 */
bool ota_assets_end(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Status accessors — called from T11 GET /api/ota/status
 * ───────────────────────────────────────────────────────────────────────── */

/** @brief Return the current OTA state machine state. */
ota_state_t ota_get_state(void);

/** @brief Return OTA progress as a percentage (0–100). */
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
 */
bool ota_is_accepted(void);

/* ─────────────────────────────────────────────────────────────────────────
 * T13 task entry point
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief T13 — OTA Manager task entry point (spawned on demand by T11).
 *
 * Processes the PSRAM-buffered ZIP archive: mounts the inactive LittleFS
 * partition, extracts STORE-method entries, writes manifest.json, unmounts,
 * switches the active OTA bank, and schedules a reboot.  Deletes itself on
 * completion or error.
 *
 * Only STORE (uncompressed) ZIP entries are supported.  Deflate-compressed
 * entries cause the task to abort with an error.  Build the ZIP with:
 *   @code
 *   zip -0 assets.zip index.html style.css app.js
 *   @endcode
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_ota_manager(void *pvParameters);
