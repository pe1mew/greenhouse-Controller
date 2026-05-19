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
 * ### SD log file naming
 * Files are named `YYYYMMDDHHMMSS.csv` where the timestamp encodes the
 * moment the file was created (UTC).  Lexicographic sort = chronological
 * order.  At most SD_MAX_FILES (10) files are retained; the lexicographically
 * oldest is deleted when a rotation would exceed this limit.
 *
 * ### CSV line format
 * Header:  timestamp,type,initiator,ch,param,value_a,value_b
 * Example: 2025-06-07T14:30:22,SENSOR,SYS,0,0,235,650
 * The timestamp field uses ISO 8601 (UTC).
 *
 * ### Startup / resume
 * On mount, T9 scans the SD root for files matching the 14-digit timestamp
 * pattern.  The lexicographically largest (most recent) file is resumed if
 * its size is below SD_ROTATE_BYTES; otherwise a new file is created.
 * Old sequential-index files (`ghc_NNNN.csv`) are ignored by the scan
 * filter and will not interfere with the new naming scheme.
 *
 * ### Free-space guard
 * After each rotation T9 checks available SD space.  If free < SD_FREE_MIN_BYTES
 * and the file count is above SD_MIN_FILES, the oldest file is deleted to
 * reclaim space.  If the count is already at SD_MIN_FILES and space is still
 * low, SD logging is suspended and NVS fallback is activated.  Additionally,
 * if a write returns STORAGE_ERR_FULL, a single oldest-file deletion is
 * attempted before falling back to NVS-only mode.
 *
 * ### NVS fallback and SD automount
 * If `storage_init()` fails at startup, T9 operates in NVS-only mode.
 * A LOG_SYSTEM event is emitted on SD failure (FR-LG07, FR-LG08).  While
 * `s_sd_ok` is false, the main event loop uses a 60-second receive timeout
 * and calls `event_logger_sd_remount()` on each expiry, so a card inserted
 * after boot is picked up automatically within one minute.  The admin web-GUI
 * mount button is still available for an immediate manual remount.
 *
 * @author  Greenhouse Controller project
 */

/* alpha.6.6 — dropped vestigial #include <Arduino.h>. The file uses no
 * Arduino types — only ESP-IDF (esp_log, esp_task_wdt via FreeRTOS),
 * stdlib (time.h, string.h, stdio.h, ctype.h), and project headers
 * (LIB-7 nvs, LIB-8 sd_storage, system_id, app_types). The
 * `dm_get_unix_time()` dependency on T4 is satisfied via a stub
 * (firmware/src/data_manager/data_manager_stub.cpp) until T4 itself
 * activates in Phase 6.7+.
 *
 * `esp_log.h` was previously pulled in transitively through Arduino.h;
 * with Arduino removed it has to be included explicitly. */
#include <esp_log.h>
#include <time.h>

#include "event_logger.h"
#include "../types/app_types.h"          /* task_t14 handle */
#include "../data_manager/data_manager.h"
#include "../status_post/status_post.h"  /* T14_NOTIFY_LOG_ROTATED (a.6.35) */
#include "../system_id/system_id.h"   /* unit_id in SD preamble (gh#17) */

#include "nvs_config.h"
#include "sd_storage.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "T9_LOG";

/* -----------------------------------------------------------------------
 * SD rotation parameters
 * ----------------------------------------------------------------------- */
/** Rotate to a new file when the current one reaches this many bytes. */
#define SD_ROTATE_BYTES    (512UL * 1024UL)

/** Maximum number of log files retained on the SD card. */
#define SD_MAX_FILES       10u

/** Minimum number of files to retain; never delete below this floor. */
#define SD_MIN_FILES       3u

/** Suspend (or reclaim) when free space drops below this many bytes. */
#define SD_FREE_MIN_BYTES  (2UL * 1024UL * 1024UL)

/**
 * Length of an SD filename string including leading '/' and NUL.
 * "/YYYYMMDDHHMMSS.csv" = 19 printable chars + '\0' = 20.  24 gives margin.
 */
#define SD_FILENAME_LEN    24

/**
 * Length of the name-only part (no leading '/') including NUL.
 * "YYYYMMDDHHMMSS.csv" = 18 chars + '\0' = 19.  20 gives margin.
 */
#define SD_NAME_ONLY_LEN   20

/** CSV header line written at the start of every new log file. */
#define CSV_HEADER  "timestamp,type,initiator,ch,param,value_a,value_b\n"

/* -----------------------------------------------------------------------
 * Module state
 * ----------------------------------------------------------------------- */
static bool s_sd_ok = false;                    /**< true iff SD logging is active */
static char s_cur_filename[SD_FILENAME_LEN];    /**< active file, with leading '/' */

/**
 * Most recently *rotated-away* CSV filename (no leading '/'). Empty until
 * the first rotation of the boot. Read by T14's upload-on-rotation path
 * via event_logger_last_rotated(); written by rotate_sd_file() under
 * s_closed_mux. Thread-safety: short critical section copying a small
 * fixed-size string.
 */
static portMUX_TYPE s_closed_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_last_closed[SD_NAME_ONLY_LEN] = {};

/* -----------------------------------------------------------------------
 * Drop counter — tracks events lost due to Q3 overflow
 * ----------------------------------------------------------------------- */
static portMUX_TYPE      g_drop_mux   = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_q3_dropped = 0;

/* -----------------------------------------------------------------------
 * Force-rotate request (T14 → T9 hand-off, since 1.17.28)
 *
 * Set by event_logger_force_rotate(); polled by T9's main loop after each
 * drain pass. T9 calls rotate_sd_file() and clears the flag. T14 polls
 * back via event_logger_force_rotate() until the flag clears or its
 * timeout expires.
 * ----------------------------------------------------------------------- */
static portMUX_TYPE      s_rotate_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     s_force_rotate_req = false;

/* -----------------------------------------------------------------------
 * log_post() — single entry point for all Q3 producers
 * ----------------------------------------------------------------------- */

void log_post(const log_event_t *evt)
{
    if (xQueueSend(Q3, evt, 0) == pdPASS) {
        return;
    }

    log_event_t discard;
    xQueueReceive(Q3, &discard, 0);

    portENTER_CRITICAL(&g_drop_mux);
    g_q3_dropped++;
    portEXIT_CRITICAL(&g_drop_mux);

    if (xQueueSend(Q3, evt, 0) != pdPASS) {
        portENTER_CRITICAL(&g_drop_mux);
        g_q3_dropped++;
        portEXIT_CRITICAL(&g_drop_mux);
    }
}

/* -----------------------------------------------------------------------
 * log_take_dropped_count()
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
 * @brief Return true if @p name matches the timestamp filename pattern.
 *
 * A valid log filename is exactly 14 decimal digits followed by ".csv"
 * (total 18 characters, e.g. "20250607143022.csv").  Files with any other
 * naming pattern — including old sequential-index files (`ghc_NNNN.csv`) —
 * are silently skipped by all scan operations.
 */
static bool is_ts_filename(const char *name)
{
    if (!name || strlen(name) != 18) return false;
    for (int i = 0; i < 14; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    return strncmp(name + 14, ".csv", 4) == 0;
}

/**
 * @brief Create an SD filename from the current local time.
 *
 * Produces a path of the form "/YYYYMMDDHHMMSS.csv" in @p buf.
 * Local time is used so that filenames are human-readable without
 * timezone conversion when browsing the card directly.
 */
static void make_ts_filename(char *buf, size_t len)
{
    time_t now = (time_t)dm_get_unix_time();
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    snprintf(buf, len, "/%04d%02d%02d%02d%02d%02d.csv",
             tm_local.tm_year + 1900,
             tm_local.tm_mon  + 1,
             tm_local.tm_mday,
             tm_local.tm_hour,
             tm_local.tm_min,
             tm_local.tm_sec);
}

/**
 * @brief Scan the SD root and return a comma-separated list of matching
 *        timestamp-pattern CSV filenames (name only, no leading '/').
 *
 * @param list_buf  Destination buffer.
 * @param list_len  Size of @p list_buf.
 * @return true if the scan succeeded (even if no files were found).
 */
static bool sd_scan(char *list_buf, size_t list_len)
{
    list_buf[0] = '\0';
    if (!s_sd_ok && !storage_sd_available()) return false;

    char raw[512];
    if (storage_sd_list_csv(".csv", raw, sizeof(raw)) != STORAGE_OK) return false;

    /* Re-filter: keep only files matching the 14-digit timestamp pattern. */
    size_t pos = 0;
    const char *tok = raw;
    while (*tok) {
        const char *end = strchr(tok, ',');
        size_t flen = end ? (size_t)(end - tok) : strlen(tok);
        if (flen > 0 && flen < SD_NAME_ONLY_LEN) {
            char name[SD_NAME_ONLY_LEN];
            memcpy(name, tok, flen);
            name[flen] = '\0';
            if (is_ts_filename(name) && pos + flen + 2 < list_len) {
                memcpy(list_buf + pos, name, flen);
                pos += flen;
                list_buf[pos++] = ',';
                list_buf[pos]   = '\0';
            }
        }
        if (!end) break;
        tok = end + 1;
    }
    return true;
}

/**
 * @brief Count the comma-separated entries in a scan list.
 */
static uint32_t scan_count(const char *list)
{
    uint32_t n = 0;
    const char *tok = list;
    while (*tok) {
        const char *end = strchr(tok, ',');
        size_t flen = end ? (size_t)(end - tok) : strlen(tok);
        if (flen > 0) n++;
        if (!end) break;
        tok = end + 1;
    }
    return n;
}

/**
 * @brief Find the lexicographically smallest or largest name in a scan list.
 *
 * @param list       Comma-separated list from sd_scan().
 * @param find_max   true = find newest (lex max); false = find oldest (lex min).
 * @param out        Destination for the found name (no leading '/').
 * @param out_len    Size of @p out.
 * @return true if a name was found; false if the list was empty.
 */
static bool scan_find(const char *list, bool find_max,
                      char *out, size_t out_len)
{
    out[0] = '\0';
    const char *tok = list;
    while (*tok) {
        const char *end  = strchr(tok, ',');
        size_t      flen = end ? (size_t)(end - tok) : strlen(tok);
        if (flen > 0 && flen < out_len) {
            char candidate[SD_NAME_ONLY_LEN];
            memcpy(candidate, tok, flen);
            candidate[flen] = '\0';
            if (out[0] == '\0' ||
                (find_max ? strcmp(candidate, out) > 0
                          : strcmp(candidate, out) < 0)) {
                memcpy(out, candidate, flen + 1);
            }
        }
        if (!end) break;
        tok = end + 1;
    }
    return out[0] != '\0';
}

/**
 * @brief Delete the lexicographically oldest timestamp CSV file on the SD card.
 * @return true on success.
 */
static bool delete_oldest(void)
{
    char list[512];
    if (!sd_scan(list, sizeof(list))) return false;

    char oldest[SD_NAME_ONLY_LEN];
    if (!scan_find(list, false, oldest, sizeof(oldest))) return false;

    char path[SD_FILENAME_LEN];
    snprintf(path, sizeof(path), "/%s", oldest);
    bool ok = (storage_sd_delete(path) == STORAGE_OK);
    if (ok) ESP_LOGI(TAG, "[T9] Deleted oldest log file %s", path);
    return ok;
}

/**
 * @brief Check free space; if below SD_FREE_MIN_BYTES, delete the oldest
 *        file to reclaim space.  If already at SD_MIN_FILES floor and still
 *        low, suspend SD logging (s_sd_ok = false) and emit a LOG_SYSTEM event.
 */
static void check_free_space(void)
{
    if (storage_sd_free_bytes() >= SD_FREE_MIN_BYTES) return;

    char list[512];
    sd_scan(list, sizeof(list));
    uint32_t count = scan_count(list);

    if (count > SD_MIN_FILES) {
        if (delete_oldest()) {
            ESP_LOGW(TAG, "[T9] SD low space: deleted oldest (%u files remaining)",
                     (unsigned)(count - 1u));
            return;   /* freed one file; enough room to continue */
        }
    }

    /* At retention floor or deletion failed — suspend. */
    ESP_LOGW(TAG, "[T9] SD low space at retention floor (%u files) — suspending",
             (unsigned)count);
    s_sd_ok = false;

    log_event_t sys_evt;
    memset(&sys_evt, 0, sizeof(sys_evt));
    sys_evt.timestamp  = dm_get_unix_time();
    sys_evt.event_type = (uint8_t)LOG_SYSTEM;
    sys_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    sys_evt.value_a    = (int16_t)(-2);   /* −2 = SD low-space suspension */
    log_post(&sys_evt);
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
 * Line format: ISO-8601-timestamp,type,initiator,ch,param,value_a,value_b\n
 * Example:     2026-05-19T13:30:22,SENSOR,SYS,0,0,235,650
 *
 * Since a.6.35.3 the timestamp is **local time** (per the POSIX TZ env
 * variable that T10 maintains via setenv("TZ", cfg.tz_str) + tzset()).
 * This matches the SD card filename convention (make_ts_filename, which has
 * always used localtime_r) so the filename's wall-clock and the inside-the-
 * file row timestamps are now consistent. Operator-side a CSV downloaded at
 * 13:30 local time will show rows stamped 13:30 local time, not 11:30 UTC.
 *
 * @param evt  Event to format.
 * @param buf  Destination buffer (≥ 80 bytes recommended).
 * @param len  Size of @p buf.
 */
static void build_csv_line(const log_event_t *evt, char *buf, size_t len)
{
    time_t ts = (time_t)evt->timestamp;
    struct tm tm_local;
    localtime_r(&ts, &tm_local);
    char ts_str[20];   /* "YYYY-MM-DDTHH:MM:SS\0" */
    strftime(ts_str, sizeof(ts_str), "%Y-%m-%dT%H:%M:%S", &tm_local);

    snprintf(buf, len,
             "%s,%s,%s,%u,%u,%d,%d\n",
             ts_str,
             evt_type_str(evt->event_type),
             initiator_str(evt->initiator),
             (unsigned)evt->channel,
             (unsigned)evt->param_id,
             (int)evt->value_a,
             (int)evt->value_b);
}

/**
 * @brief Advance to the next SD log file.
 *
 * Creates a new file named with the current UTC timestamp, writes the CSV
 * header, and deletes the lexicographically oldest timestamp file if the
 * total count now exceeds SD_MAX_FILES.  Calls check_free_space() after
 * the rotation.
 */
static void rotate_sd_file(void)
{
    /* Capture the soon-to-be-closed filename for T14 upload-on-rotation
     * before make_ts_filename overwrites s_cur_filename. We strip the
     * leading '/' so callers receive the bare name (matches the spec's
     * URL-friendly path scheme used by storage_sd_list_csv). */
    if (s_cur_filename[0] != '\0') {
        const char *bare = (s_cur_filename[0] == '/') ? s_cur_filename + 1 : s_cur_filename;
        portENTER_CRITICAL(&s_closed_mux);
        strncpy(s_last_closed, bare, sizeof(s_last_closed) - 1u);
        s_last_closed[sizeof(s_last_closed) - 1u] = '\0';
        portEXIT_CRITICAL(&s_closed_mux);
    }

    make_ts_filename(s_cur_filename, sizeof(s_cur_filename));

    storage_status_t rc = storage_sd_write_append(s_cur_filename, CSV_HEADER);
    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "[T9] Rotate: header write to %s failed (%d)",
                 s_cur_filename, (int)rc);
        s_sd_ok = false;
        return;
    }

    /* Unit-id preamble row (gh#17, since 1.18.3). Every new SD log file
     * starts with a self-identifying LOG_SYSTEM value_a=11 row so a
     * downloaded CSV is always traceable to the unit that produced it,
     * regardless of how many files later get downloaded out-of-order.
     * Cost: ~50 bytes per rotation. The format matches build_csv_line()
     * exactly so the row is indistinguishable from one that came through
     * Q3 — operators / parsers see no difference.
     *
     * Written directly (not via Q3 + log_post) because we want it to land
     * synchronously after the header, before any "real" rotation event
     * (e.g. the force-rotate marker T14 emits) reaches T9's drain. */
    log_event_t id_evt = {};
    id_evt.timestamp  = (uint32_t)time(NULL);
    id_evt.event_type = (uint8_t)LOG_SYSTEM;
    id_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    id_evt.channel    = 0u;
    id_evt.param_id   = (uint8_t)LOG_PARAM_NONE;
    id_evt.value_a    = (int16_t)11;
    id_evt.value_b    = (int16_t)system_unit_id_u16();
    char id_line[80];
    build_csv_line(&id_evt, id_line, sizeof(id_line));
    (void)storage_sd_write_append(s_cur_filename, id_line);
    /* If the unit-id append fails, swallow it — the file is still usable
     * for normal CSV writes. The boot-time LOG_SYSTEM value_a=11 in T4
     * provides a fallback identification path. */

    /* Enforce SD_MAX_FILES ceiling. */
    char list[512];
    if (sd_scan(list, sizeof(list))) {
        uint32_t count = scan_count(list);
        if (count > SD_MAX_FILES) {
            char oldest[SD_NAME_ONLY_LEN];
            if (scan_find(list, false, oldest, sizeof(oldest))) {
                char path[SD_FILENAME_LEN];
                snprintf(path, sizeof(path), "/%s", oldest);
                storage_sd_delete(path);
                ESP_LOGI(TAG, "[T9] Rotated to %s, deleted %s", s_cur_filename, path);
            }
        } else {
            ESP_LOGI(TAG, "[T9] Rotated to %s (%u files)", s_cur_filename, (unsigned)count);
        }
    }

    /* Proactive free-space check. */
    check_free_space();

    /* a.6.35 — wake T14 to consider uploading the just-closed file. T14's
     * handler reads cfg.log_upload_rot and silently consumes the notification
     * if rotation-uploads are disabled. NULL-safe: at very early boot T9
     * may rotate before T14 is spawned; xTaskNotify with a NULL handle would
     * crash, so we skip when task_t14 isn't populated yet. */
    if (task_t14 != NULL) {
        xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits);
    }
}

/**
 * @brief Write one event as a CSV line to the current SD file.
 *
 * On STORAGE_ERR_FULL, attempts to delete the oldest log file and retry
 * the write before falling back to NVS-only mode.  Rotates to a new file
 * when the 512 KB threshold is reached.
 */
static void write_to_sd(const log_event_t *evt)
{
    char csv_line[80];
    build_csv_line(evt, csv_line, sizeof(csv_line));

    storage_status_t rc = storage_sd_write_append(s_cur_filename, csv_line);

    /* On full/IO error, attempt to reclaim space by deleting the oldest file. */
    if (rc == STORAGE_ERR_FULL || rc == STORAGE_ERR_IO) {
        char list[512];
        if (sd_scan(list, sizeof(list)) && scan_count(list) > SD_MIN_FILES) {
            if (delete_oldest()) {
                ESP_LOGW(TAG, "[T9] SD full: reclaimed space, retrying write");
                rc = storage_sd_write_append(s_cur_filename, csv_line);
            }
        }
    }

    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "[T9] SD write failed (%d) — NVS-only", (int)rc);
        s_sd_ok = false;

        log_event_t sys_evt;
        memset(&sys_evt, 0, sizeof(sys_evt));
        sys_evt.timestamp  = dm_get_unix_time();
        sys_evt.event_type = (uint8_t)LOG_SYSTEM;
        sys_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
        sys_evt.value_a    = (int16_t)(-1);   /* −1 = SD write failure */
        log_post(&sys_evt);
        return;
    }

    /* Rotate when the size threshold is reached. */
    uint32_t sz = storage_sd_file_size(s_cur_filename);
    if (sz >= SD_ROTATE_BYTES) {
        rotate_sd_file();
    }
}

/**
 * @brief Persist one event to the SD card (if available).
 *
 * 2.0.0-alpha.6.5: the NVS-backed event-log ringbuffer (gh#22) was retired
 * as redundant with the SD CSV. The `nvs_log_append(evt, sizeof(log_event_t))`
 * call that lived here is gone. Events are now SD-only; if SD is absent or
 * the mount has failed, the event is dropped (and counted via the existing
 * `s_dropped` accumulator surfaced as a LOG_SYSTEM post on the next drain).
 */
static void process_event(const log_event_t *evt)
{
    if (s_sd_ok) {
        write_to_sd(evt);
    }
}

/* =======================================================================
 * Shared startup / remount helper
 * ======================================================================= */

/**
 * @brief Scan the SD card for timestamp log files and set s_cur_filename.
 *
 * Resumes the most recent (lexicographically largest) file if its size is
 * below SD_ROTATE_BYTES.  Creates a new timestamp file otherwise.
 *
 * @return true if s_cur_filename now points to a usable file.
 */
static bool sd_open_active_file(void)
{
    char list[512];
    bool have_list = sd_scan(list, sizeof(list));

    char newest[SD_NAME_ONLY_LEN] = { '\0' };
    bool found = have_list && scan_find(list, true, newest, sizeof(newest));

    if (found) {
        char path[SD_FILENAME_LEN];
        snprintf(path, sizeof(path), "/%s", newest);
        if (storage_sd_file_size(path) < SD_ROTATE_BYTES) {
            strncpy(s_cur_filename, path, sizeof(s_cur_filename) - 1);
            s_cur_filename[sizeof(s_cur_filename) - 1] = '\0';
            ESP_LOGI(TAG, "[T9] Resuming log file %s", s_cur_filename);
            return true;
        }
    }

    /* No suitable existing file — create a fresh one. */
    make_ts_filename(s_cur_filename, sizeof(s_cur_filename));
    storage_status_t rc = storage_sd_write_append(s_cur_filename, CSV_HEADER);
    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "[T9] Failed to create %s (%d)", s_cur_filename, (int)rc);
        return false;
    }
    ESP_LOGI(TAG, "[T9] Created new log file %s", s_cur_filename);
    return true;
}

/* =======================================================================
 * SD mount / unmount helpers — called by T11 web-server endpoints
 * ======================================================================= */

bool event_logger_sd_remount(void)
{
    if (s_sd_ok) return true;

    storage_status_t rc = storage_init();
    if (rc != STORAGE_OK) {
        ESP_LOGW(TAG, "[T9] SD remount failed (%d)", (int)rc);
        return false;
    }

    /* gh#14 (since 1.17.32): belt-and-braces after storage_init().
     * The driver's storage_init() guards against this case directly via
     * SD.totalBytes()==0, but the cost of double-checking here is one
     * extra accessor call and the benefit is that if the SD library's
     * cached state slips through both layers — extremely unlikely but
     * possible — s_sd_ok still doesn't flip to true. */
    if (storage_sd_total_bytes() == 0u) {
        ESP_LOGW(TAG, "[T9] SD remount reported OK but total=0 — treating as absent");
        storage_sd_unmount();
        return false;
    }

    if (!sd_open_active_file()) {
        storage_sd_unmount();
        return false;
    }

    s_sd_ok = true;
    ESP_LOGI(TAG, "[T9] SD remounted — logging on %s", s_cur_filename);
    return true;
}

void event_logger_sd_unmount(void)
{
    s_sd_ok = false;
    storage_sd_unmount();
    ESP_LOGI(TAG, "[T9] SD unmounted via web request");
}

/* =======================================================================
 * Public — synchronous LOG_SYSTEM helper (since 2.0.0-a.6.34)
 *
 * Bypasses Q3 to guarantee the row reaches the SD file before the caller
 * returns. See event_logger.h for the rationale (T13 fallback audit row).
 * ======================================================================= */

bool event_logger_post_sync(int16_t value_a, int16_t value_b)
{
    if (!s_sd_ok || s_cur_filename[0] == '\0') {
        return false;
    }

    log_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_SYSTEM;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.value_a    = value_a;
    evt.value_b    = value_b;

    char csv_line[80];
    build_csv_line(&evt, csv_line, sizeof(csv_line));
    return storage_sd_write_append(s_cur_filename, csv_line) == STORAGE_OK;
}

/* =======================================================================
 * Public — rotation-tracking helpers (T14)
 * ======================================================================= */

bool event_logger_last_rotated(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }

    portENTER_CRITICAL(&s_closed_mux);
    strncpy(out, s_last_closed, cap - 1u);
    out[cap - 1u] = '\0';
    portEXIT_CRITICAL(&s_closed_mux);

    return out[0] != '\0';
}

bool event_logger_force_rotate(uint32_t timeout_ms)
{
    /* Refuse early if SD logging is currently inactive: rotation has no
     * meaning without an active file, and we'd otherwise spin to timeout. */
    if (!s_sd_ok) { return false; }

    /* Raise the request flag. T9's drain loop checks this after each pass. */
    portENTER_CRITICAL(&s_rotate_mux);
    s_force_rotate_req = true;
    portEXIT_CRITICAL(&s_rotate_mux);

    /* Post a synthetic marker to Q3 to (a) wake T9 if it is blocked on
     * receive, and (b) leave a visible "why was this file closed?" trail
     * in the file that is about to be rotated away. The marker uses
     * value_a=6 per the LOG_SYSTEM encoding table in event_logger.h. */
    log_event_t marker = {};
    marker.timestamp  = (uint32_t)time(NULL);
    marker.event_type = (uint8_t)LOG_SYSTEM;
    marker.initiator  = (uint8_t)LOG_BY_WEB;
    marker.value_a    = 6;
    marker.value_b    = 0;
    log_post(&marker);

    /* Poll for completion. Resolution = 100 ms; well under the typical
     * 5 s timeout T14 passes for this call. */
    const TickType_t start         = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        portENTER_CRITICAL(&s_rotate_mux);
        bool still_pending = s_force_rotate_req;
        portEXIT_CRITICAL(&s_rotate_mux);
        if (!still_pending) { return true; }
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            ESP_LOGW(TAG, "[T9] force-rotate timeout after %lu ms",
                     (unsigned long)timeout_ms);
            /* Leave the flag set — T9 will process when it gets a chance.
             * The caller (T14) treats timeout as "no rotation observed in
             * time" and falls back to whatever newest_closed currently is. */
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool event_logger_newest_closed(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }
    out[0] = '\0';

    /* Determine the active file's bare name (no leading '/') so we can skip
     * it in the scan. If SD logging is inactive, no file is "active". */
    char active_bare[SD_NAME_ONLY_LEN] = {};
    if (s_sd_ok && s_cur_filename[0] != '\0') {
        const char *bare = (s_cur_filename[0] == '/') ? s_cur_filename + 1 : s_cur_filename;
        strncpy(active_bare, bare, sizeof(active_bare) - 1u);
        active_bare[sizeof(active_bare) - 1u] = '\0';
    }

    char list[512];
    if (!sd_scan(list, sizeof(list))) {
        /* SD unavailable — fall back to in-memory rotation record. */
        return event_logger_last_rotated(out, cap);
    }

    /* Walk the scan list, lex-max excluding the active filename. */
    const char *tok = list;
    while (*tok) {
        const char *end  = strchr(tok, ',');
        size_t      flen = end ? (size_t)(end - tok) : strlen(tok);
        if (flen > 0 && flen < cap) {
            char candidate[SD_NAME_ONLY_LEN];
            memcpy(candidate, tok, flen);
            candidate[flen] = '\0';
            if (active_bare[0] && strcmp(candidate, active_bare) == 0) {
                /* skip the active file */
            } else if (out[0] == '\0' || strcmp(candidate, out) > 0) {
                strncpy(out, candidate, cap - 1u);
                out[cap - 1u] = '\0';
            }
        }
        if (!end) break;
        tok = end + 1;
    }

    if (out[0] == '\0') {
        /* SD scan found nothing — try the in-memory record. */
        return event_logger_last_rotated(out, cap);
    }
    return true;
}

/* -----------------------------------------------------------------------
 * event_logger_next_pending — a.6.35.2 multi-file upload helper
 *
 * Scans the SD card and returns the lex-smallest closed CSV whose name is
 * strictly greater than @p after. T14's upload_pending walks this in a loop,
 * advancing `after` to each successful upload, so a backlog of missed files
 * (e.g. WiFi outage that spanned a rotation) gets drained in chronological
 * order on the next trigger. Closed-file enumeration is identical to
 * event_logger_newest_closed (same sd_scan() + active-file exclusion);
 * only the selection predicate differs: smallest > after, vs lex-max.
 * ----------------------------------------------------------------------- */
bool event_logger_next_pending(const char *after, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }
    out[0] = '\0';
    if (after == NULL) { after = ""; }

    /* Active file's bare name — exclude from results. If SD logging is
     * inactive, no file is active. */
    char active_bare[SD_NAME_ONLY_LEN] = {};
    if (s_sd_ok && s_cur_filename[0] != '\0') {
        const char *bare = (s_cur_filename[0] == '/') ? s_cur_filename + 1 : s_cur_filename;
        strncpy(active_bare, bare, sizeof(active_bare) - 1u);
        active_bare[sizeof(active_bare) - 1u] = '\0';
    }

    char list[512];
    if (!sd_scan(list, sizeof(list))) {
        /* SD unavailable / scan failed — no enumeration possible. Unlike
         * newest_closed we do NOT fall back to event_logger_last_rotated
         * here, because the caller's intent is "walk all pending in order"
         * and an in-memory fallback can't provide that. */
        return false;
    }

    /* Walk the comma-separated scan list, find smallest candidate > after. */
    const char *tok = list;
    while (*tok) {
        const char *end  = strchr(tok, ',');
        size_t      flen = end ? (size_t)(end - tok) : strlen(tok);
        if (flen > 0 && flen < cap) {
            char candidate[SD_NAME_ONLY_LEN];
            memcpy(candidate, tok, flen);
            candidate[flen] = '\0';
            bool is_active   = (active_bare[0] && strcmp(candidate, active_bare) == 0);
            bool is_eligible = !is_active && (strcmp(candidate, after) > 0);
            if (is_eligible && (out[0] == '\0' || strcmp(candidate, out) < 0)) {
                strncpy(out, candidate, cap - 1u);
                out[cap - 1u] = '\0';
            }
        }
        if (!end) break;
        tok = end + 1;
    }

    return out[0] != '\0';
}

/* =======================================================================
 * T9 task
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
        if (sd_open_active_file()) {
            s_sd_ok = true;
            ESP_LOGI(TAG, "[T9] SD ready");
        } else {
            ESP_LOGW(TAG, "[T9] SD mounted but file init failed — NVS-only");
        }
    } else {
        ESP_LOGW(TAG, "[T9] SD not available (code %d) — NVS-only", (int)sd_rc);
    }

    /* ----------------------------------------------------------------
     * Main event loop
     * ---------------------------------------------------------------- */
    TickType_t s_last_remount_ticks = xTaskGetTickCount();

    for (;;) {
        log_event_t evt;

        /* When SD is absent, wake up every 60 s to attempt automount.
         * When SD is active, block indefinitely — no polling overhead. */
        TickType_t wait = s_sd_ok ? portMAX_DELAY : pdMS_TO_TICKS(60000);

        if (xQueueReceive(Q3, &evt, wait) != pdTRUE) {
            /* Timeout — no event arrived; try to (re)mount the SD card. */
            if (!s_sd_ok) {
                s_last_remount_ticks = xTaskGetTickCount();
                if (event_logger_sd_remount()) {
                    ESP_LOGI(TAG, "[T9] SD automounted");
                }
            }
            continue;
        }
        process_event(&evt);

        while (xQueueReceive(Q3, &evt, 0) == pdTRUE) {
            process_event(&evt);
        }

        /* When events are flowing, the 60-s timeout above never fires.
         * Check elapsed time here so automount is attempted even while
         * the sensor poll keeps Q3 busy (e.g. poll_interval = 30 s). */
        if (!s_sd_ok) {
            TickType_t now = xTaskGetTickCount();
            if ((now - s_last_remount_ticks) >= pdMS_TO_TICKS(60000)) {
                s_last_remount_ticks = now;
                if (event_logger_sd_remount()) {
                    ESP_LOGI(TAG, "[T9] SD automounted");
                }
            }
        }

        /* Surface any Q3 drop events. */
        uint32_t dropped = log_take_dropped_count();
        if (dropped > 0) {
            ESP_LOGW(TAG, "[T9] Q3 overflow: %u event(s) dropped", (unsigned)dropped);

            log_event_t sys_evt;
            memset(&sys_evt, 0, sizeof(sys_evt));
            sys_evt.timestamp  = dm_get_unix_time();
            sys_evt.event_type = (uint8_t)LOG_SYSTEM;
            sys_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
            sys_evt.value_a    = (int16_t)(dropped > 32767u ? 32767 : (int16_t)dropped);

            xQueueSend(Q3, &sys_evt, 0);   /* direct — not via log_post() */
        }

        /* Honour an external force-rotate request (T14 daily-upload slot).
         * The marker event posted by event_logger_force_rotate() is already
         * in the file at this point — it was processed by the drain loop
         * above — so rotating now produces a closed file whose last entry
         * documents why it was closed. */
        portENTER_CRITICAL(&s_rotate_mux);
        bool need_rotate = s_force_rotate_req;
        portEXIT_CRITICAL(&s_rotate_mux);
        if (need_rotate && s_sd_ok) {
            ESP_LOGI(TAG, "[T9] force-rotate requested");
            rotate_sd_file();
            portENTER_CRITICAL(&s_rotate_mux);
            s_force_rotate_req = false;
            portEXIT_CRITICAL(&s_rotate_mux);
        }
    }
}
