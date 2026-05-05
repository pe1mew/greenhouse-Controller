/**
 * @file event_logger.cpp
 * @brief Q3 drop-oldest log helper and T9 task — Phase 5 implementation.
 *
 * ## Architecture
 *
 * ### log_post() / log_take_dropped_count()
 * Already implemented (Gap H).  See event_logger.h for design rationale.
 *
 * ### T9 task
 * Sole consumer of Q3.  Each wake cycle:
 *  1. Blocks on `xQueueReceive(Q3, portMAX_DELAY)` until at least one
 *     event is available.
 *  2. Drains all remaining immediately-available events (non-blocking).
 *  3. For each event:
 *     a. Appends binary record to NVS ring buffer (always).
 *     b. Appends CSV line to the current SD file (if SD is mounted).
 *  4. After the drain pass: reads and resets the drop counter.  If > 0,
 *     posts a synthetic LOG_SYSTEM event directly to Q3 (not via
 *     log_post() — avoids re-entrant eviction).
 *
 * ### SD log rotation
 * Files are named `/ghc_NNNN.csv` (4-digit zero-padded sequential index).
 * The current index is persisted in NVS (`log/file_idx`) so it survives
 * a reboot.  Rotation triggers when the current file reaches 512 KB.  The
 * oldest file is deleted when the total count exceeds 10.
 *
 * ### NVS fallback
 * If `storage_init()` fails at startup, T9 operates in NVS-only mode.
 * A LOG_SYSTEM event is emitted on SD failure (FR-LG07, FR-LG08).  T9
 * does not retry the SD mount after a failure — a reboot is required to
 * re-attempt mounting.
 *
 * @author  Greenhouse Controller project
 */

#include <Arduino.h>

#include "event_logger.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"

#include "nvs_config.h"
#include "sd_storage.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "T9_LOG";

/* -----------------------------------------------------------------------
 * SD rotation parameters
 * ----------------------------------------------------------------------- */
/** Rotate to a new file when the current one reaches this many bytes. */
#define SD_ROTATE_BYTES    (512UL * 1024UL)

/** Maximum number of log files retained on the SD card. */
#define SD_MAX_FILES       10

/** Length of a FAT32 filename string including leading '/' and NUL. */
#define SD_FILENAME_LEN    16    /* "/ghc_9999.csv\0" = 15 chars + NUL */

/** NVS key (in NVS_NS_LOG namespace) for the current SD file index. */
#define NVS_KEY_FILE_IDX   "file_idx"

/** CSV header line written at the start of every new log file. */
#define CSV_HEADER  "timestamp,type,initiator,ch,param,value_a,value_b\n"

/* -----------------------------------------------------------------------
 * Module state
 * ----------------------------------------------------------------------- */
static bool     s_sd_ok        = false;   /**< true iff SD card is mounted */
static uint32_t s_file_idx     = 1;       /**< current SD file sequential index */
static char     s_cur_filename[SD_FILENAME_LEN]; /**< e.g. "/ghc_0001.csv" */

/* -----------------------------------------------------------------------
 * Drop counter — tracks events lost due to Q3 overflow
 *
 * Protected by a FreeRTOS spinlock so that concurrent callers from
 * different tasks increment it safely without blocking.  On ESP32-S3 the
 * spinlock is a 32-bit CAS instruction; the critical section is sub-
 * microsecond.
 * ----------------------------------------------------------------------- */
static portMUX_TYPE      g_drop_mux     = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_q3_dropped   = 0;

/* -----------------------------------------------------------------------
 * log_post() — the single entry point for all Q3 producers
 * ----------------------------------------------------------------------- */

void log_post(const log_event_t *evt)
{
    /* --- Common path: queue has space --- */
    if (xQueueSend(Q3, evt, 0) == pdPASS) {
        return;
    }

    /* --- Queue is full: evict the oldest entry to make room --- */
    log_event_t discard;
    xQueueReceive(Q3, &discard, 0);   /* removes oldest; result ignored */

    /* Count the evicted entry as a dropped event. */
    portENTER_CRITICAL(&g_drop_mux);
    g_q3_dropped++;
    portEXIT_CRITICAL(&g_drop_mux);

    /* --- Retry send: may fail if a concurrent sender took the freed slot ---
     *
     * The gap between xQueueReceive and the retry xQueueSend is not atomic.
     * A concurrent caller can win the freed slot, leaving this retry without
     * space.  This is an acknowledged race: extremely rare given Q3's depth
     * (32) and T9's drain rate.  A mutex around the whole sequence would
     * eliminate it but adds latency in T3 and T6; deferred until profiling
     * demonstrates it is needed.
     */
    if (xQueueSend(Q3, evt, 0) != pdPASS) {
        /* New event also lost — count it. */
        portENTER_CRITICAL(&g_drop_mux);
        g_q3_dropped++;
        portEXIT_CRITICAL(&g_drop_mux);
    }
}

/* -----------------------------------------------------------------------
 * log_take_dropped_count() — read and reset the drop counter
 *
 * Called by T9 only.  The portENTER_CRITICAL ensures that no in-flight
 * log_post() increment races with the reset: on exit the counter is 0
 * and `count` holds the pre-reset value.
 * ----------------------------------------------------------------------- */

uint32_t log_take_dropped_count(void)
{
    portENTER_CRITICAL(&g_drop_mux);
    uint32_t count = g_q3_dropped;
    g_q3_dropped   = 0;
    portEXIT_CRITICAL(&g_drop_mux);
    return count;
}

/* =======================================================================
 * T9 internal helpers
 * ======================================================================= */

/**
 * @brief Build the SD file path for a given sequential index.
 */
static void make_filename(uint32_t idx, char *buf, size_t len)
{
    snprintf(buf, len, "/ghc_%04u.csv", (unsigned)idx);
}

/**
 * @brief Return a short ASCII name for a log_type_t value.
 */
static const char *evt_type_str(uint8_t t)
{
    switch ((log_type_t)t) {
        case LOG_SENSOR:      return "SENSOR";
        case LOG_RELAY:       return "RELAY";
        case LOG_MODE_CHANGE: return "MODE";
        case LOG_SETPOINT:    return "SETPT";
        case LOG_SESSION:     return "SESSION";
        case LOG_ALARM:       return "ALARM";
        case LOG_SYSTEM:      return "SYSTEM";
        default:              return "UNKNWN";
    }
}

/**
 * @brief Return a short ASCII name for a log_initiator_t value.
 */
static const char *initiator_str(uint8_t i)
{
    switch ((log_initiator_t)i) {
        case LOG_BY_SYSTEM: return "SYS";
        case LOG_BY_FARMER: return "FARMER";
        case LOG_BY_ADMIN:  return "ADMIN";
        case LOG_BY_MQTT:   return "MQTT";
        case LOG_BY_WEB:    return "WEB";
        default:            return "UNK";
    }
}

/**
 * @brief Format one log_event_t as a NUL-terminated CSV line.
 *
 * Line format (always ends with '\n'):
 *   timestamp,type,initiator,ch,param,value_a,value_b\n
 *
 * @param evt  Event to format.
 * @param buf  Destination buffer (must be ≥ 64 bytes; 80 is safe).
 * @param len  Size of @p buf.
 */
static void build_csv_line(const log_event_t *evt, char *buf, size_t len)
{
    snprintf(buf, len,
             "%lu,%s,%s,%u,%u,%d,%d\n",
             (unsigned long)evt->timestamp,
             evt_type_str(evt->event_type),
             initiator_str(evt->initiator),
             (unsigned)evt->channel,
             (unsigned)evt->param_id,
             (int)evt->value_a,
             (int)evt->value_b);
}

/**
 * @brief Advance to the next SD log file and delete the oldest if needed.
 *
 * Increments the sequential file index, persists it in NVS, creates the
 * new file with a CSV header, and deletes the file that is now more than
 * SD_MAX_FILES positions behind the current index.
 */
static void rotate_sd_file(void)
{
    s_file_idx++;
    nvs_cfg_set_i32(NVS_NS_LOG, NVS_KEY_FILE_IDX, (int32_t)s_file_idx);
    make_filename(s_file_idx, s_cur_filename, sizeof(s_cur_filename));

    /* Write CSV header to the new (initially empty) file. */
    storage_status_t rc = storage_sd_write_append(s_cur_filename, CSV_HEADER);
    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "Rotate: header write to %s failed (%d)", s_cur_filename, rc);
        s_sd_ok = false;
        return;
    }

    /* Delete the oldest file when we now hold more than SD_MAX_FILES. */
    if (s_file_idx > SD_MAX_FILES) {
        char oldest[SD_FILENAME_LEN];
        make_filename(s_file_idx - SD_MAX_FILES, oldest, sizeof(oldest));
        storage_status_t del_rc = storage_sd_delete(oldest);
        if (del_rc == STORAGE_OK) {
            ESP_LOGI(TAG, "Rotated: %s created, deleted %s", s_cur_filename, oldest);
        } else {
            /* NOT_FOUND is expected once the file index wraps past 10. */
            ESP_LOGI(TAG, "Rotated: %s created (oldest %s absent, code %d)",
                     s_cur_filename, oldest, del_rc);
        }
    } else {
        ESP_LOGI(TAG, "Rotated to %s", s_cur_filename);
    }
}

/**
 * @brief Write one event as a CSV line to the current SD file.
 *
 * Checks the file size after writing and rotates if the 512 KB limit is
 * reached.  On any write error, clears `s_sd_ok` and emits a warning so
 * that subsequent events fall back to NVS-only.
 */
static void write_to_sd(const log_event_t *evt)
{
    char csv_line[80];
    build_csv_line(evt, csv_line, sizeof(csv_line));

    storage_status_t rc = storage_sd_write_append(s_cur_filename, csv_line);
    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "SD write failed (%d) — falling back to NVS-only", rc);
        s_sd_ok = false;

        /* Emit a LOG_SYSTEM event so the SD failure is visible in NVS log. */
        log_event_t sys_evt;
        memset(&sys_evt, 0, sizeof(sys_evt));
        sys_evt.timestamp  = dm_get_unix_time();
        sys_evt.event_type = (uint8_t)LOG_SYSTEM;
        sys_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
        sys_evt.value_a    = (int16_t)(-1);  /* −1 = SD write failure */
        log_post(&sys_evt);
        return;
    }

    /* Rotate on size threshold. */
    uint32_t sz = storage_sd_file_size(s_cur_filename);
    if (sz >= SD_ROTATE_BYTES) {
        rotate_sd_file();
    }
}

/**
 * @brief Persist one event to NVS and (if available) SD card.
 */
static void process_event(const log_event_t *evt)
{
    /* Always write to NVS ring buffer (circular, overwrites oldest). */
    nvs_log_append(evt, sizeof(log_event_t));

    /* Write to SD if the card is currently available. */
    if (s_sd_ok) {
        write_to_sd(evt);
    }
}

/* =======================================================================
 * T9 task — Phase 5 full implementation
 * ======================================================================= */

void task_event_logger(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "[T9] task alive");

    /* ----------------------------------------------------------------
     * SD card initialisation
     * ---------------------------------------------------------------- */
    storage_status_t sd_rc = storage_init();
    if (sd_rc == STORAGE_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "[T9] SD card mounted");
    } else {
        s_sd_ok = false;
        ESP_LOGW(TAG, "[T9] SD not available (code %d) — NVS-only mode", sd_rc);
    }

    /* ----------------------------------------------------------------
     * Recover or initialise the SD file index from NVS.
     *
     * NVS_NS_LOG / NVS_KEY_FILE_IDX stores the sequential index of the
     * last-used log file.  On first boot the key is absent; default to 1.
     * ---------------------------------------------------------------- */
    int32_t idx_stored = 0;
    if (nvs_cfg_get_i32(NVS_NS_LOG, NVS_KEY_FILE_IDX, &idx_stored) == NVS_CFG_OK
            && idx_stored >= 1) {
        s_file_idx = (uint32_t)idx_stored;
    } else {
        s_file_idx = 1;
        nvs_cfg_set_i32(NVS_NS_LOG, NVS_KEY_FILE_IDX, 1);
    }
    make_filename(s_file_idx, s_cur_filename, sizeof(s_cur_filename));
    ESP_LOGI(TAG, "[T9] current log file: %s", s_cur_filename);

    /* Write CSV header if the current file is new (zero-length or absent). */
    if (s_sd_ok && storage_sd_file_size(s_cur_filename) == 0) {
        storage_status_t hdr_rc = storage_sd_write_append(s_cur_filename, CSV_HEADER);
        if (hdr_rc != STORAGE_OK) {
            ESP_LOGW(TAG, "[T9] CSV header write failed (%d) — NVS-only", hdr_rc);
            s_sd_ok = false;
        }
    }

    /* ----------------------------------------------------------------
     * Main event loop
     *
     * Structure:
     *   1. Block until at least one event arrives (portMAX_DELAY).
     *   2. Drain all immediately-available events (non-blocking).
     *   3. After each drain pass, check and surface the drop counter.
     * ---------------------------------------------------------------- */
    for (;;) {
        log_event_t evt;

        /* Block until the first event of this drain pass is available. */
        if (xQueueReceive(Q3, &evt, portMAX_DELAY) != pdTRUE) {
            /* Should never happen with portMAX_DELAY; guard against it. */
            continue;
        }
        process_event(&evt);

        /* Drain any further events that arrived without blocking. */
        while (xQueueReceive(Q3, &evt, 0) == pdTRUE) {
            process_event(&evt);
        }

        /* --- Drop-counter surfacing (FR-LG05 equivalent) ---
         *
         * Use xQueueSend directly (NOT log_post) to avoid a re-entrant
         * call to the evict-and-retry logic inside log_post().  A second
         * overflow while we are posting the drop event would be counted
         * the next drain pass.
         */
        uint32_t dropped = log_take_dropped_count();
        if (dropped > 0) {
            ESP_LOGW(TAG, "[T9] Q3 overflow: %u event(s) dropped", (unsigned)dropped);

            log_event_t sys_evt;
            memset(&sys_evt, 0, sizeof(sys_evt));
            sys_evt.timestamp  = dm_get_unix_time();
            sys_evt.event_type = (uint8_t)LOG_SYSTEM;
            sys_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
            /* value_a = drop count (clamped to int16_t range). */
            sys_evt.value_a    = (int16_t)(dropped > 32767u ? 32767 : (int16_t)dropped);

            /* Direct send — no eviction; if Q3 is still full the synthetic
             * event is lost silently (acceptable: the overflow is already
             * known and the next drain pass will catch any further drops). */
            xQueueSend(Q3, &sys_evt, 0);
        }
    }
}
