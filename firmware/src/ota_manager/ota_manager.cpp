/**
 * @file ota_manager.cpp
 * @brief T13 — OTA Manager (Phase 10).
 *
 * Implements dual-bank firmware OTA, web-asset ZIP OTA, and 3-consecutive-fail
 * rollback.  See ota_manager.h for the full design description.
 *
 * ZIP extraction notes
 * --------------------
 * Only STORE (method 0, uncompressed) ZIP entries are supported.  The parser
 * walks Local File Headers (PK\x03\x04) sequentially until a Central Directory
 * header (PK\x01\x02) or End-of-Central-Directory record (PK\x05\x06) is
 * encountered.  Each file is written to the inactive LittleFS partition under
 * its basename (any directory prefix in the ZIP is stripped).
 *
 * If deflate-compressed entries (method 8) are found, the task aborts with an
 * error message instructing the user to re-pack with `zip -0`.
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
/* alpha.6.13 — dropped <Arduino.h>. T13 has zero Arduino-specific calls;
 * everything (esp_ota_*, esp_partition_*, FreeRTOS timers, mbedtls) was
 * already IDF-native in the 1.20.3 source. Single-line patch. */
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_system.h>     /* esp_reset_reason() — 1.19.2 planned-reboot exemption */
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "ota_manager.h"
#include "../types/app_types.h"
#include "../event_logger/event_logger.h"
#include "nvs_config.h"
#include "littlefs_storage.h"

static const char *TAG = "T13_OTA";

/* NVS key for consecutive-fail counter (≤15 chars). */
#define OTA_FAIL_KEY  "ota_fail_cnt"

/* ============================================================
 * Module-level state
 * ============================================================ */

static SemaphoreHandle_t s_mx = NULL;

static volatile ota_state_t s_state    = OTA_STATE_IDLE;
static volatile uint8_t     s_progress = 0;
static          char        s_error[80] = {};

/* Firmware OTA */
static esp_ota_handle_t       s_ota_handle = 0;
static const esp_partition_t *s_ota_part   = NULL;
static size_t                 s_fw_total   = 0;
static size_t                 s_fw_written = 0;

/* a.6.34 — firmware-only fallback timer.
 *
 * After ota_firmware_end() verifies the image, the device sits in
 * OTA_STATE_FW_DONE waiting for a paired ota_assets_begin() to follow.
 * If no asset upload arrives within FW_DONE_FALLBACK_MS, this one-shot
 * timer fires and commits the firmware-only update — esp_ota_set_boot_partition
 * on the inactive bank, then schedule_reboot(1000). Cancelled in
 * ota_assets_begin() if the asset upload starts before the timer fires.
 *
 * 120 s matches the 1.20.3 design header. Long enough for an operator who
 * is uploading both .bin and .zip via the GUI to push the second file
 * after the first commit; short enough that a "firmware-only on purpose"
 * upload commits within a couple of minutes without needing a manual
 * trigger.
 *
 * NULL when no firmware-only fallback is pending. Created on first use
 * via xTimerCreate, then reused (xTimerStart re-arms it). */
#define FW_DONE_FALLBACK_MS  120000u
static TimerHandle_t s_fw_done_timer = NULL;

/* Asset ZIP buffer (owned by ota_manager until passed to T13) */
static uint8_t *s_zip_buf   = NULL;
static size_t   s_zip_total = 0;
static size_t   s_zip_rcvd  = 0;

/* ============================================================
 * Internal helpers
 * ============================================================ */

static void ota_mx_init(void)
{
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
    }
}

static void set_state_locked(ota_state_t st)
{
    if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY);
    s_state = st;
    if (s_mx) xSemaphoreGive(s_mx);
}

static void set_error_locked(const char *msg)
{
    if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY);
    s_state = OTA_STATE_ERROR;
    snprintf(s_error, sizeof(s_error), "%s", msg);
    if (s_mx) xSemaphoreGive(s_mx);
    ESP_LOGE(TAG, "[OTA] error: %s", msg);
}

static void update_progress(size_t done, size_t total)
{
    s_progress = (total > 0) ? (uint8_t)(done * 100U / total) : 0;
}

/* Post a minimal LOG_SYSTEM entry to Q3. */
static void post_log(int16_t val_a)
{
    log_entry_t evt = {};
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_SYSTEM;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.value_a    = val_a;
    log_post(&evt);
}

/* FreeRTOS timer callback: performs the deferred system restart. */
static void reboot_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "[OTA] Rebooting now");
    esp_restart();
}

/* Schedule a system restart after delay_ms milliseconds. */
static void schedule_reboot(uint32_t delay_ms)
{
    set_state_locked(OTA_STATE_REBOOTING);
    TimerHandle_t tmr = xTimerCreate("ota_reboot",
                                     pdMS_TO_TICKS(delay_ms),
                                     pdFALSE, NULL, reboot_timer_cb);
    if (tmr) {
        xTimerStart(tmr, 0);
    } else {
        ESP_LOGE(TAG, "[OTA] Reboot timer create failed — rebooting immediately");
        esp_restart();
    }
}

/* a.6.34 — firmware-only fallback commit worker.
 *
 * Spawned as a one-shot task from fw_done_fallback_cb() (the xTimerService
 * callback). The actual commit work — state check, audit log, partition
 * swap, deferred reboot — runs here with a proper 4 KB task stack instead
 * of xTimerService's much smaller configTIMER_TASK_STACK_DEPTH. Empirically
 * required: doing the commit directly in the timer callback produced clean
 * reboots at the correct time but the value_a=13 audit row never reached
 * SD, indicating an SD-write failure in the constrained timer-callback
 * stack/context. */
static void fw_done_commit_task(void *pv)
{
    (void)pv;

    /* State check under s_mx. If anything else (asset upload, error, retry)
     * has moved the state away from FW_DONE, this fallback is no longer the
     * right action — silently bail. */
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool should_commit = (s_state == OTA_STATE_FW_DONE);
    xSemaphoreGive(s_mx);
    if (!should_commit) {
        ESP_LOGI(TAG, "[OTA] firmware-only fallback worker started but state moved "
                      "off FW_DONE — bailing (likely an asset upload started)");
        vTaskDelete(NULL);
        return;
    }

    if (s_ota_part == NULL) {
        /* Shouldn't happen — s_ota_part is set in ota_firmware_begin() and not
         * cleared until the next OTA. Belt-and-braces. */
        ESP_LOGE(TAG, "[OTA] firmware-only fallback: s_ota_part is NULL — refusing to commit");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "[OTA] No asset upload arrived in %u ms — committing firmware-only update",
             (unsigned)FW_DONE_FALLBACK_MS);

    /* Audit-log the commit — value_a=13 (documented in event_logger.h).
     * Posting from this task context (4 KB stack, normal task priority) means
     * T9 reliably picks up the entry and flushes to SD before the 3 s reboot
     * timer fires. The cancel-path's post_log(2) follows the same pattern from
     * T13's task context and is empirically reliable. */
    post_log(13);

    esp_err_t err = esp_ota_set_boot_partition(s_ota_part);
    if (err != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "fw-only fallback: esp_ota_set_boot_partition: %s",
                 esp_err_to_name(err));
        set_error_locked(msg);
        vTaskDelete(NULL);
        return;   /* leave state = ERROR; device stays on current bank */
    }

    /* 3 s deferred-reboot — T9 has plenty of runway to flush the value_a=13
     * audit row to SD before esp_restart(). schedule_reboot itself creates
     * another one-shot timer that runs esp_restart() in xTimerService context. */
    schedule_reboot(3000);

    vTaskDelete(NULL);
}

/* a.6.34 — firmware-only fallback timer callback (lightweight dispatcher).
 *
 * Fires FW_DONE_FALLBACK_MS after ota_firmware_end() set state to FW_DONE
 * if no asset upload arrived in the window. Spawns fw_done_commit_task to
 * do the actual commit work — the timer-service task has a small stack
 * (configTIMER_TASK_STACK_DEPTH) that is not enough for SD writes + the
 * esp_ota_set_boot_partition flash write.
 *
 * Cancellation: ota_assets_begin() stops the timer; if a cancel races with
 * the fire, the spawned task's first action is a state re-check under s_mx
 * and it bails silently if state moved off FW_DONE. */
static void fw_done_fallback_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    BaseType_t rc = xTaskCreate(fw_done_commit_task,
                                "ota_fb_commit",
                                4096,
                                NULL,
                                5,        /* priority — above T9's 4 */
                                NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "[OTA] fw_done_commit_task spawn failed (%d) — firmware-only "
                      "commit will not happen this cycle", (int)rc);
        /* Non-fatal: device stays on current bank. Operator can re-trigger
         * by uploading firmware again. */
    }
}

/* ============================================================
 * Rollback management
 * ============================================================ */

void ota_check_rollback(void)
{
    ota_mx_init();

    int32_t fail_cnt = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, OTA_FAIL_KEY, 0, &fail_cnt);
    ESP_LOGI(TAG, "[OTA] Boot fail counter = %d", (int)fail_cnt);

    /* 1.19.2 — Don't count T15 PLANNED REBOOTs as boot failures.
     * The supervisor (status_post_supervisor.cpp:101) sets the NVS key
     * `t15_planreboot=1` immediately before calling esp_restart() to escalate
     * a heap-drop or respawn-storm condition. When we see that flag here with
     * reset_reason == ESP_RST_SW, this boot is the intentional resume from
     * that planned reboot — NOT a crash. The flag gets cleared by T15 a few
     * seconds later (line 262) once T14 is healthy, so this is a one-shot
     * exemption per planned reboot.
     *
     * Without this guard a unit hitting gh#20 (TLS-handshake heap fragmentation)
     * at the wrong cadence could accumulate counter=3 within hours and trip
     * the rollback, reverting from 1.19.x to 1.18.3 — exactly the firmware
     * the 1.19.0 release was issued to fix.
     *
     * The ESP_RST_SW gate matters: if the unit *panicked* while the flag was
     * still set (e.g. the original 2026-05-14 gh#21 cascade — flag set,
     * esp_restart called, next boot asserted on tcpip_api_call before the
     * flag could be cleared), reset_reason will be ESP_RST_PANIC and we still
     * want to count that as a real failure. */
    if (esp_reset_reason() == ESP_RST_SW) {
        int32_t plan_flag = 0;
        nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, "t15_planreboot", 0, &plan_flag);
        if (plan_flag != 0) {
            ESP_LOGI(TAG, "[OTA] T15 PLANNED REBOOT detected — fail counter NOT incremented (stays at %d)",
                     (int)fail_cnt);
            return;
        }
    }

    if (fail_cnt >= 3) {
        ESP_LOGE(TAG,
            "[OTA] 3 consecutive boot failures detected — initiating OTA rollback");
        /* Reset counter before rollback so the restored bank doesn't inherit
         * the bad count (rollback restores flash; NVS may or may not be reset
         * depending on partition layout — explicit write here is a safety net). */
        nvs_cfg_set_i32(NVS_NS_SYSTEM, OTA_FAIL_KEY, 0);
        /* This call marks the running app as invalid and reboots to the
         * previous valid OTA bank.  It does not return on success. */
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Reaches here only if there is no valid rollback target
         * (e.g. initial factory flash with single bank). */
        ESP_LOGW(TAG,
            "[OTA] Rollback unavailable (no previous bank) — counter cleared, continuing");
        return;
    }

    /* Increment: ota_mark_healthy() resets this to 0 after OTA_HEALTHY_MS. */
    nvs_cfg_set_i32(NVS_NS_SYSTEM, OTA_FAIL_KEY, fail_cnt + 1);
    ESP_LOGI(TAG, "[OTA] Fail counter incremented to %d (resets after %u ms healthy uptime)",
             (int)(fail_cnt + 1), OTA_HEALTHY_MS);
}

void ota_mark_healthy(void)
{
    nvs_cfg_set_i32(NVS_NS_SYSTEM, OTA_FAIL_KEY, 0);
    ESP_LOGI(TAG, "[OTA] Boot marked healthy — fail counter reset to 0");
}

/* ============================================================
 * Firmware OTA
 * ============================================================ */

bool ota_firmware_begin(size_t total_bytes)
{
    ota_mx_init();

    /* Reject if another OTA is already running. */
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool busy = (s_state != OTA_STATE_IDLE && s_state != OTA_STATE_ERROR);
    xSemaphoreGive(s_mx);
    if (busy) {
        ESP_LOGW(TAG, "[OTA] ota_firmware_begin: OTA already in progress");
        return false;
    }

    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) {
        set_error_locked("no inactive OTA partition available");
        return false;
    }

    esp_err_t err = esp_ota_begin(s_ota_part,
                                  (total_bytes > 0) ? total_bytes : OTA_SIZE_UNKNOWN,
                                  &s_ota_handle);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "esp_ota_begin: %s", esp_err_to_name(err));
        set_error_locked(msg);
        return false;
    }

    xEventGroupSetBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
    s_fw_total   = total_bytes;
    s_fw_written = 0;
    s_progress   = 0;
    s_error[0]   = '\0';
    set_state_locked(OTA_STATE_FW_WRITING);

    ESP_LOGI(TAG, "[OTA] Firmware OTA begin — target partition: %s, size: %u B",
             s_ota_part->label, (unsigned)total_bytes);
    /* a.6.35.3 — OTA stage codes moved to value_a=14..17 to avoid colliding
     * with T14 status outcomes (0/1), T10 STA (1), T10 NTP (2), and T9 Q3
     * drop overflow (-1). Old codes (post_log(0/1/2/-1)) made the parser
     * misrender ota_firmware_begin as "Legacy boot marker", ota_firmware_end
     * as "STA WiFi disconnected", and ota_assets_end as "NTP timeout". */
    post_log(14);   /* 14 = OTA firmware-begin */
    return true;
}

bool ota_firmware_write(const uint8_t *chunk, size_t len)
{
    if (s_state != OTA_STATE_FW_WRITING) return false;

    esp_err_t err = esp_ota_write(s_ota_handle, chunk, len);
    if (err != ESP_OK) {
        esp_ota_abort(s_ota_handle);
        char msg[64];
        snprintf(msg, sizeof(msg), "esp_ota_write: %s", esp_err_to_name(err));
        set_error_locked(msg);
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
        return false;
    }

    s_fw_written += len;
    update_progress(s_fw_written, s_fw_total);
    return true;
}

bool ota_firmware_end(void)
{
    if (s_state != OTA_STATE_FW_WRITING) return false;
    set_state_locked(OTA_STATE_FW_VERIFYING);

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "esp_ota_end: %s", esp_err_to_name(err));
        set_error_locked(msg);
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
        return false;
    }

    /* Boot partition switch is deferred: it happens atomically with the
     * LittleFS switch at the end of task_ota_manager (after asset upload).
     * The verified firmware sits uncommitted in the inactive partition until
     * the web-asset ZIP is also uploaded — OR the a.6.34 firmware-only
     * fallback timer commits it after FW_DONE_FALLBACK_MS of inactivity
     * (matches the 1.20.3 design). */
    ESP_LOGI(TAG,
        "[OTA] Firmware verified OK — waiting for web-asset upload "
        "(fallback commit in %u ms if none arrives)",
        (unsigned)FW_DONE_FALLBACK_MS);
    post_log(15);   /* 15 = OTA firmware-end / verified (a.6.35.3 re-numbering) */
    s_progress = 0;   /* Reset: assets phase has not started yet. */
    set_state_locked(OTA_STATE_FW_DONE);

    /* a.6.34 — arm the firmware-only fallback. Create the timer lazily on
     * first use; subsequent ota_firmware_end() calls reuse it via xTimerStart
     * (which is documented as safe on a stopped one-shot timer). */
    if (s_fw_done_timer == NULL) {
        s_fw_done_timer = xTimerCreate("ota_fw_fallback",
                                       pdMS_TO_TICKS(FW_DONE_FALLBACK_MS),
                                       pdFALSE,   /* one-shot */
                                       NULL,
                                       fw_done_fallback_cb);
        if (s_fw_done_timer == NULL) {
            ESP_LOGW(TAG, "[OTA] xTimerCreate(fw_fallback) failed — fallback disabled");
            /* Non-fatal: the operator can still upload assets to commit. */
            return true;
        }
    } else {
        /* Reuse: change period (also re-arms) so a previous expired/stopped
         * timer gets a fresh FW_DONE_FALLBACK_MS countdown. */
        xTimerChangePeriod(s_fw_done_timer,
                           pdMS_TO_TICKS(FW_DONE_FALLBACK_MS), 0);
    }
    if (xTimerStart(s_fw_done_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "[OTA] xTimerStart(fw_fallback) failed — fallback disabled");
    }
    return true;
}

/* ============================================================
 * Web-asset OTA
 * ============================================================ */

bool ota_assets_begin(size_t total_bytes)
{
    ota_mx_init();

    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool busy = (s_state != OTA_STATE_IDLE &&
                 s_state != OTA_STATE_ERROR &&
                 s_state != OTA_STATE_FW_DONE);
    xSemaphoreGive(s_mx);
    if (busy) {
        ESP_LOGW(TAG, "[OTA] ota_assets_begin: OTA already in progress");
        return false;
    }

    /* a.6.34 — cancel the firmware-only fallback timer if it was armed by a
     * preceding ota_firmware_end(). The asset upload supersedes the
     * fallback path; commit happens via the normal task_ota_manager flow
     * at the end of asset extraction. xTimerStop on a not-running timer is
     * a no-op — safe to call unconditionally. */
    if (s_fw_done_timer != NULL) {
        xTimerStop(s_fw_done_timer, 0);
    }

    /* Free any stale buffer from a previous failed attempt. */
    if (s_zip_buf) {
        heap_caps_free(s_zip_buf);
        s_zip_buf = NULL;
    }

    s_zip_buf = (uint8_t *)heap_caps_malloc(total_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_zip_buf) {
        set_error_locked("PSRAM alloc failed for ZIP buffer");
        return false;
    }

    s_zip_total = total_bytes;
    s_zip_rcvd  = 0;
    s_progress  = 0;
    s_error[0]  = '\0';
    xEventGroupSetBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
    set_state_locked(OTA_STATE_ASSETS_BUFFERING);
    ESP_LOGI(TAG, "[OTA] Assets OTA begin — buffering %u B in PSRAM", (unsigned)total_bytes);
    return true;
}

bool ota_assets_accumulate(const uint8_t *chunk, size_t len, size_t offset)
{
    if (s_state != OTA_STATE_ASSETS_BUFFERING) return false;
    if (!s_zip_buf || offset + len > s_zip_total) {
        set_error_locked("ZIP accumulate: buffer overrun");
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
        return false;
    }
    memcpy(s_zip_buf + offset, chunk, len);
    s_zip_rcvd = offset + len;
    update_progress(s_zip_rcvd, s_zip_total);
    return true;
}

bool ota_assets_end(void)
{
    if (s_state != OTA_STATE_ASSETS_BUFFERING) return false;

    if (s_zip_rcvd < s_zip_total) {
        set_error_locked("incomplete ZIP received");
        heap_caps_free(s_zip_buf);
        s_zip_buf = NULL;
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
        return false;
    }

    set_state_locked(OTA_STATE_ASSETS_WRITING);
    s_progress = 0;

    /* Spawn T13; it takes ownership of s_zip_buf + s_zip_total. */
    BaseType_t rc = xTaskCreatePinnedToCore(
        task_ota_manager, "T13_OTA",
        16384,   /* 16 KB: ZIP parser + LittleFS ops + manifest formatting */
        NULL,
        3,       /* Low priority — same as other network tasks */
        NULL,    /* No permanent handle */
        0        /* Core 0 (protocol core) */
    );
    if (rc != pdPASS) {
        set_error_locked("T13 task spawn failed");
        heap_caps_free(s_zip_buf);
        s_zip_buf = NULL;
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
        return false;
    }

    ESP_LOGI(TAG, "[OTA] T13 spawned for asset extraction");
    return true;
}

/* ============================================================
 * Status accessors
 * ============================================================ */

ota_state_t ota_get_state(void)
{
    /* NULL-safe symmetric with set_state_locked. s_mx is lazily created on
     * first ota_*_begin(); callers that only ever read (e.g. T11
     * /api/ota/status before any OTA upload) would otherwise panic via
     * xSemaphoreTake(NULL). The read of s_state is a single byte and
     * happens often enough that skipping the lock is acceptable. */
    if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY);
    ota_state_t st = s_state;
    if (s_mx) xSemaphoreGive(s_mx);
    return st;
}

uint8_t ota_get_progress_pct(void)
{
    return s_progress;
}

const char *ota_get_error(void)
{
    return s_error;
}

char ota_get_active_bank(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part) return '?';
    return (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? 'A' : 'B';
}

bool ota_is_accepted(void)
{
    int32_t cnt = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, OTA_FAIL_KEY, 0, &cnt);
    return (cnt == 0);
}

/* ============================================================
 * T13 task — web-asset ZIP extraction
 *
 * ZIP LOCAL FILE HEADER layout (little-endian):
 *   Offset  Size  Field
 *   0       4     Signature 0x04034B50 ("PK\x03\x04")
 *   4       2     Version needed
 *   6       2     General purpose bit flag
 *   8       2     Compression method (0=store, 8=deflate)
 *   10      2     Last mod time
 *   12      2     Last mod date
 *   14      4     CRC-32
 *   18      4     Compressed size
 *   22      4     Uncompressed size
 *   26      2     File name length
 *   28      2     Extra field length
 *   30      n     File name
 *   30+n    m     Extra field
 *   30+n+m  c     File data (compressed_size bytes)
 * ============================================================ */

/* Signature constants */
#define ZIP_SIG_LOCAL    0x04034B50UL  /* Local file header  PK\x03\x04 */
#define ZIP_SIG_CENTRAL  0x02014B50UL  /* Central dir entry  PK\x01\x02 */
#define ZIP_SIG_END      0x06054B50UL  /* End-of-central-dir PK\x05\x06 */
#define ZIP_SIG_DD       0x08074B50UL  /* Data descriptor    PK\x07\x08 */

#define ZIP_METHOD_STORE    0
#define ZIP_METHOD_DEFLATE  8

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Walk ZIP local file headers sequentially, writing each STORE entry to
 * the specified LittleFS partition.  Returns number of files written (≥0)
 * or -1 on error (error description written to err_buf).
 */
static int extract_zip_store(const uint8_t *buf, size_t buf_len,
                              lfs_partition_t part,
                              char *err_buf, size_t err_len)
{
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;
    int files_written  = 0;

    while (p + 30 <= end) {
        uint32_t sig = rd32(p);

        /* Stop at Central Directory or EOCD */
        if (sig == ZIP_SIG_CENTRAL || sig == ZIP_SIG_END) {
            break;
        }
        if (sig == ZIP_SIG_DD) {
            /* Data descriptor without local header — skip 16 bytes */
            p += 16;
            continue;
        }
        if (sig != ZIP_SIG_LOCAL) {
            snprintf(err_buf, err_len,
                     "unexpected ZIP signature 0x%08lX at offset %u",
                     (unsigned long)sig, (unsigned)(p - buf));
            return -1;
        }

        uint16_t method     = rd16(p + 8);
        uint32_t comp_size  = rd32(p + 18);
        uint32_t uncomp_size = rd32(p + 22);
        uint16_t name_len   = rd16(p + 26);
        uint16_t extra_len  = rd16(p + 28);

        const uint8_t *fname_ptr = p + 30;
        const uint8_t *data_ptr  = fname_ptr + name_len + extra_len;

        /* Validate that data fits within the buffer. */
        if (data_ptr + comp_size > end) {
            snprintf(err_buf, err_len, "ZIP entry extends beyond buffer boundary");
            return -1;
        }

        /* Skip directory entries (name ends with '/'). */
        bool is_dir = (name_len > 0 && fname_ptr[name_len - 1] == '/');

        if (!is_dir) {
            if (method != ZIP_METHOD_STORE) {
                snprintf(err_buf, err_len,
                         "compressed ZIP entry (method %u) is not supported. "
                         "Repack with: zip -0 assets.zip *.html *.css *.js",
                         (unsigned)method);
                return -1;
            }

            /* Build LittleFS path: /basename (strip any leading directory). */
            char path[64] = "/";
            const uint8_t *base = fname_ptr;
            /* Search for the last '/' in the entry name. */
            for (uint16_t i = 0; i < name_len; i++) {
                if (fname_ptr[i] == '/') base = fname_ptr + i + 1;
            }
            uint16_t base_len = (uint16_t)(fname_ptr + name_len - base);
            if (base_len == 0) {
                /* Edge case: entry is a directory described without trailing '/'. */
                p = data_ptr + comp_size;
                continue;
            }
            if (base_len > 62) base_len = 62;
            memcpy(path + 1, base, base_len);
            path[1 + base_len] = '\0';

            lfs_status_t wst = littlefs_write(part, path, data_ptr, uncomp_size);
            if (wst != LFS_OK) {
                snprintf(err_buf, err_len,
                         "LittleFS write failed for %s (err %d)", path, (int)wst);
                return -1;
            }
            ESP_LOGI(TAG, "[T13] Extracted %s (%u B)", path, (unsigned)uncomp_size);
            files_written++;
        }

        p = data_ptr + comp_size;
    }

    return files_written;
}

void task_ota_manager(void *pvParameters)
{
    (void)pvParameters;

    /* Work with the module-level ZIP buffer — we own it. */
    const uint8_t *zip_buf   = s_zip_buf;
    const size_t   zip_size  = s_zip_total;

    ESP_LOGI(TAG, "[T13] Asset extraction starting (%u B)", (unsigned)zip_size);

    /* Determine which LittleFS partition is currently INACTIVE. */
    lfs_partition_t active_lfs   = littlefs_active_partition();
    lfs_partition_t inactive_lfs = (active_lfs == LFS_PARTITION_A)
                                   ? LFS_PARTITION_B
                                   : LFS_PARTITION_A;

    ESP_LOGI(TAG, "[T13] Active LittleFS: %c  →  writing to: %c",
             (active_lfs == LFS_PARTITION_A) ? 'A' : 'B',
             (inactive_lfs == LFS_PARTITION_A) ? 'A' : 'B');

    bool         ok       = false;
    bool         lfs_open = false;
    lfs_status_t lfs_st;   /* hoisted: declaration must precede all gotos */

    /* Ensure the inactive partition is not already mounted (e.g. retry path).
     * We do NOT format/erase before writing: littlefs_write() truncates each
     * file in-place, so every asset is fully replaced.  The web-asset filenames
     * are stable (index.html, app.js, style.css, manifest.json) so no orphans
     * can accumulate.  Erasing a 1 MB partition via esp_partition_erase_range()
     * takes ~10 s and trips the task watchdog — avoid it here. */
    littlefs_unmount(inactive_lfs);

    lfs_st = littlefs_mount(inactive_lfs);
    if (lfs_st != LFS_OK) {
        /* alpha.6.24 — first-time format fallback. On a fresh chip (or after
         * the lfs0/lfs1 partitions were wiped by `esptool erase_region`) the
         * inactive partition contains random flash content; littlefs_mount
         * rightly refuses with LFS_ERR_CORRUPT. Format and re-mount.
         *
         * This path is benign on production hardware: the active partition
         * always contains a valid LittleFS image (we just booted from the
         * paired OTA bank), and the inactive partition is the one being
         * overwritten — formatting it loses nothing operationally.
         *
         * Note: littlefs_format() calls esp_littlefs_format(), which under the
         * hood does the same erase pass we shy away from above. The difference
         * is that this is the genuine first-write path — we have no choice but
         * to pay the ~10 s erase cost. T13 is a transient task (not WDT-
         * subscribed via esp_task_wdt_add), so the long erase is safe here in
         * a way it wouldn't be inside the steady-state asset-write loop. */
        ESP_LOGW(TAG, "[T13] inactive LFS mount failed (%d) — formatting first-time",
                 (int)lfs_st);
        lfs_status_t fmt_st = littlefs_format(inactive_lfs);
        if (fmt_st != LFS_OK) {
            ESP_LOGE(TAG, "[T13] littlefs_format(%c) failed: %d",
                     (inactive_lfs == LFS_PARTITION_A) ? 'A' : 'B', (int)fmt_st);
            set_error_locked("inactive LittleFS format failed");
            goto t13_done;
        }
        lfs_st = littlefs_mount(inactive_lfs);
        if (lfs_st != LFS_OK) {
            ESP_LOGE(TAG, "[T13] post-format remount failed: %d", (int)lfs_st);
            set_error_locked("inactive LittleFS remount after format failed");
            goto t13_done;
        }
        ESP_LOGI(TAG, "[T13] inactive LFS formatted + mounted");
    }
    lfs_open = true;

    /* --- Extract all STORE entries ------------------------------------ */
    {
        char zip_err[80] = {};
        int  nfiles = extract_zip_store(zip_buf, zip_size,
                                        inactive_lfs, zip_err, sizeof(zip_err));
        if (nfiles < 0) {
            set_error_locked(zip_err);
            goto t13_done;
        }
        if (nfiles == 0) {
            set_error_locked("no files extracted from ZIP");
            goto t13_done;
        }
        ESP_LOGI(TAG, "[T13] Extracted %d file(s) from ZIP", nfiles);
        update_progress(75, 100);
    }

    /* /manifest.json travels INSIDE the uploaded ZIP — it is built into the
     * archive by tools/build_release.ps1 from $VERSION at ZIP-build time, so
     * the version that ends up on the active LittleFS partition reflects
     * the ZIP that was actually extracted (not the firmware that happens to
     * be running). T13 used to overwrite manifest.json with the running
     * firmware's NVS version here — that defeated the mismatch detector
     * because asset_version would always equal fw_ver regardless of what
     * ZIP was uploaded. */
    if (littlefs_exists(inactive_lfs, "/manifest.json")) {
        ESP_LOGI(TAG, "[T13] /manifest.json present in ZIP — preserved as-is");
    } else {
        ESP_LOGW(TAG, "[T13] /manifest.json NOT in ZIP — asset_version will report '?'");
    }
    update_progress(90, 100);

    ok = true;

    /* Asset-only OTA fix-up (1.17.3).
     *
     * When this OTA session uploaded ONLY new web assets (no firmware), the
     * historical behaviour was: write assets to the inactive LFS, then ask
     * the bootloader to switch to the inactive firmware bank. That assumed
     * BOTH firmware banks already held valid firmware. After a clean
     * `pio run -t upload` (which only writes to a single bank), the OTHER
     * bank may hold a stale or unbootable image, so the switch triggered
     * a rollback; the user then ended up back on the original bank — and
     * therefore on the OLD active LFS — with the just-uploaded assets
     * stranded on the inactive partition.
     *
     * Fix: when no firmware was uploaded in this session, ALSO mirror the
     * new assets to the active LFS, and skip the boot-partition switch.
     * The reboot below then comes back on the same bank with fresh assets
     * on the partition T11 actually mounts.
     *
     * For PAIRED firmware+asset OTA the dual-bank rollback property must be
     * preserved: the assets and firmware on each bank must stay paired so a
     * rollback to the previous bank yields a self-consistent firmware+assets
     * pair. Therefore the mirror runs ONLY on the asset-only path. */
    if (ok && s_ota_part == NULL) {
        ESP_LOGI(TAG, "[T13] Asset-only OTA — mirroring to active LFS %c",
                 (active_lfs == LFS_PARTITION_A) ? 'A' : 'B');
        lfs_status_t mst = littlefs_mount(active_lfs);
        if (mst != LFS_OK) {
            ESP_LOGW(TAG, "[T13] Active LFS mount failed for mirror — "
                          "new assets only on inactive partition");
        } else {
            char zip_err2[80] = {};
            int nfiles2 = extract_zip_store(zip_buf, zip_size,
                                            active_lfs, zip_err2, sizeof(zip_err2));
            if (nfiles2 > 0) {
                /* alpha.6.13: buffer grown from 16 to 32 — modern alpha tag
                 * strings like "2.0.0-alpha.6.13" are 17 chars including NUL
                 * and overflow the original 16-byte buffer with -fpermissive
                 * promoted to error under espidf hardening flags. 32 gives
                 * headroom for future tag patterns up to "2.0.0-alpha.10.99". */
                char fw_ver[32] = FIRMWARE_VERSION;
                nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, fw_ver, sizeof(fw_ver));
                char manifest[128];
                snprintf(manifest, sizeof(manifest),
                         "{\"asset_version\":\"%s\",\"checksum\":\"\"}", fw_ver);
                littlefs_write(active_lfs, "/manifest.json",
                               (const uint8_t *)manifest, strlen(manifest));
                ESP_LOGI(TAG, "[T13] Active LFS mirrored OK (%d file(s))", nfiles2);
            } else {
                ESP_LOGW(TAG, "[T13] Active LFS mirror skipped: %s", zip_err2);
            }
            /* Do NOT unmount — T11 still has this partition mounted for
             * serving requests. littlefs_mount() above was a safe no-op. */
        }
    }

t13_done:
    if (lfs_open) {
        littlefs_unmount(inactive_lfs);
    }

    /* Free the ZIP buffer regardless of outcome. */
    heap_caps_free(s_zip_buf);
    s_zip_buf = NULL;

    if (ok && s_ota_part) {
        /* Firmware-plus-assets OTA: switch the boot partition to the bank we
         * verified during the firmware-upload step. Both firmware and web
         * assets activate together on the reboot below. */
        esp_err_t err = esp_ota_set_boot_partition(s_ota_part);
        if (err != ESP_OK) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
            set_error_locked(msg);
            ok = false;
        }
    }
    /* else: asset-only path — boot partition is intentionally left alone.
     * The mirror above guarantees the active LFS now holds the new assets. */

    if (ok) {
        ESP_LOGI(TAG, "[T13] Asset OTA complete — reboot in 1 s");
        post_log(16);   /* 16 = OTA asset-complete (a.6.35.3 re-numbering) */
        s_progress = 100;
        schedule_reboot(1000);
    } else {
        ESP_LOGE(TAG, "[T13] Asset OTA failed: %s", s_error);
        post_log(17);   /* 17 = OTA asset-fail (a.6.35.3 re-numbering) */
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
    }

    vTaskDelete(NULL);
}
