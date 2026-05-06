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
#include <Arduino.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
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

/* Asset ZIP buffer (owned by ota_manager until passed to T13) */
static uint8_t *s_zip_buf   = NULL;
static size_t   s_zip_total = 0;
static size_t   s_zip_rcvd  = 0;

/* Fallback reboot timer: fires 120 s after ota_firmware_end() if no asset
 * upload follows.  Cancelled by ota_assets_begin(). */
static TimerHandle_t s_fallback_timer = NULL;

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

/* Fallback timer callback: asset upload never arrived — commit the firmware
 * update on its own by switching the boot partition and rebooting.
 * The LittleFS partition is NOT switched; the existing active LittleFS
 * continues to be used. */
static void fallback_reboot_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    s_fallback_timer = NULL;
    if (!s_ota_part) {
        ESP_LOGE(TAG, "[OTA] Fallback: s_ota_part is NULL — cannot switch partition");
        esp_restart();
        return;
    }
    ESP_LOGI(TAG, "[OTA] Fallback: no assets upload — committing firmware-only update");
    esp_err_t err = esp_ota_set_boot_partition(s_ota_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[OTA] Fallback: esp_ota_set_boot_partition: %s",
                 esp_err_to_name(err));
    }
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

/* ============================================================
 * Rollback management
 * ============================================================ */

void ota_check_rollback(void)
{
    ota_mx_init();

    int32_t fail_cnt = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, OTA_FAIL_KEY, 0, &fail_cnt);
    ESP_LOGI(TAG, "[OTA] Boot fail counter = %d", (int)fail_cnt);

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
    post_log(0);
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
     * Start a 120 s fallback timer so a firmware-only update (no assets)
     * still commits and reboots. */
    s_fallback_timer = xTimerCreate("ota_fallback",
                                    pdMS_TO_TICKS(120000),
                                    pdFALSE, NULL, fallback_reboot_cb);
    if (s_fallback_timer) {
        xTimerStart(s_fallback_timer, 0);
    } else {
        /* Timer create failed — fall back to immediate firmware-only commit. */
        ESP_LOGW(TAG, "[OTA] Fallback timer create failed — committing firmware now");
        err = esp_ota_set_boot_partition(s_ota_part);
        if (err == ESP_OK) {
            schedule_reboot(1000);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "esp_ota_set_boot_partition: %s",
                     esp_err_to_name(err));
            set_error_locked(msg);
            xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
            return false;
        }
    }

    ESP_LOGI(TAG,
        "[OTA] Firmware verified OK — awaiting web-asset upload "
        "(fallback reboot in 120 s if no assets received)");
    post_log(1);
    s_progress = 100;
    set_state_locked(OTA_STATE_FW_DONE);
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

    /* If a firmware upload just completed, cancel the fallback reboot timer
     * so the boot partition switch happens atomically after asset extraction. */
    if (s_fallback_timer) {
        xTimerStop(s_fallback_timer, 0);
        xTimerDelete(s_fallback_timer, 0);
        s_fallback_timer = NULL;
        ESP_LOGI(TAG, "[OTA] Fallback reboot timer cancelled — assets upload received");
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
    xSemaphoreTake(s_mx, portMAX_DELAY);
    ota_state_t st = s_state;
    xSemaphoreGive(s_mx);
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

    bool ok        = false;
    bool lfs_open  = false;

    /* Ensure the inactive partition is not already mounted (e.g. retry). */
    littlefs_unmount(inactive_lfs);

    lfs_status_t lfs_st = littlefs_mount(inactive_lfs);
    if (lfs_st != LFS_OK) {
        set_error_locked("inactive LittleFS mount failed");
        goto t13_done;
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

    /* --- Write manifest.json last (integrity sentinel) --------------- */
    {
        /* Firmware version: prefer live NVS over compile-time macro so the
         * manifest reflects what is actually booted. */
        char fw_ver[16] = FIRMWARE_VERSION;
        nvs_cfg_get_str(NVS_NS_SYSTEM, NVS_KEY_FW_VERSION, fw_ver, sizeof(fw_ver));

        char manifest[128];
        snprintf(manifest, sizeof(manifest),
                 "{\"asset_version\":\"%s\",\"checksum\":\"\"}", fw_ver);

        lfs_status_t wst = littlefs_write(inactive_lfs, "/manifest.json",
                                          (const uint8_t *)manifest,
                                          strlen(manifest));
        if (wst != LFS_OK) {
            set_error_locked("manifest.json write failed");
            goto t13_done;
        }
        ESP_LOGI(TAG, "[T13] Wrote /manifest.json: %s", manifest);
        update_progress(90, 100);
    }

    ok = true;

t13_done:
    if (lfs_open) {
        littlefs_unmount(inactive_lfs);
    }

    /* Free the ZIP buffer regardless of outcome. */
    heap_caps_free(s_zip_buf);
    s_zip_buf = NULL;

    if (ok) {
        /* Switch the active OTA boot partition to the bank paired with the
         * inactive LittleFS we just wrote.  Both firmware and web assets
         * switch together on the next reboot.
         *
         * Prefer s_ota_part (set by ota_firmware_end in the same session) so
         * the exact verified partition is used.  Fall back to
         * esp_ota_get_next_update_partition() when only assets are being
         * updated (firmware upload did not occur in this session). */
        const esp_partition_t *next_fw = s_ota_part
                                       ? s_ota_part
                                       : esp_ota_get_next_update_partition(NULL);
        if (!next_fw) {
            set_error_locked("no inactive firmware partition for bank switch");
            ok = false;
        } else {
            esp_err_t err = esp_ota_set_boot_partition(next_fw);
            if (err != ESP_OK) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
                set_error_locked(msg);
                ok = false;
            }
        }
    }

    if (ok) {
        ESP_LOGI(TAG, "[T13] Asset OTA complete — reboot in 1 s");
        post_log(2);
        s_progress = 100;
        schedule_reboot(1000);
    } else {
        ESP_LOGE(TAG, "[T13] Asset OTA failed: %s", s_error);
        post_log(-1);
        xEventGroupClearBits(EG1, EG1_BIT_OTA_IN_PROGRESS);
    }

    vTaskDelete(NULL);
}
