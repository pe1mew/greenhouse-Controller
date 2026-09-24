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
 * Files are named `XXXX_YYYYMMDDHHMMSS.csv` where `XXXX` is the 4-hex
 * unit ID (gh#30, 2.0.1+) and the timestamp encodes the
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

/* rc.1.4.0 — rotation defaults bumped per model/logUpdatePlan.md §2.4. The
 * LOG_SENSOR sunset + three LOG_SENSOR_HR rows per sample ~3× the row volume
 * (~483 KB/day vs ~166 KB/day previously). The new defaults give ~63 days
 * of on-SD history at the rotation cap (30 files × 1 MB = 30 MB), comfortably
 * over-provisioned given daily T14 upload removes uploaded files anyway.
 * The previous defaults (512 KB / 10 files / 3 floor / 2 MB free) are
 * retained as comments for reference. */

/* SD_ROTATE_BYTES, SD_MAX_FILES, SD_MIN_FILES, SD_FREE_MIN_BYTES,
 * SD_FILENAME_LEN, SD_NAME_ONLY_LEN, SD_LIST_BUF_LEN — defined in event_logger.h */

/** @brief CSV header line written at the start of every new log file. */
#define CSV_HEADER  "timestamp,type,initiator,ch,param,value_a,value_b\n"

/* -----------------------------------------------------------------------
 * Module state
 * ----------------------------------------------------------------------- */

/** @brief true iff SD logging is active (card mounted, current file open). */
static bool s_sd_ok = false;

/**
 * @brief 2.4.9 (gh#61) — true when the operator deliberately released the card.
 *
 * `s_sd_ok == false` conflates two different situations: the card is
 * *unavailable* (absent at boot, failed, or swapped) and the card was
 * *released on request* via `POST /api/sd/unmount`. T9's 60 s retry exists for
 * the first — a card inserted after boot is picked up within a minute — and
 * used to fire for the second as well, silently remounting a card the admin had
 * just asked to be released.
 *
 * That mattered because `beheerderHandleiding.md` makes unmounting **mandatory**
 * before physically removing the card (:782, :1318, :1320-1322) and states no
 * deadline. The operator had about 60 seconds to open the enclosure and pull
 * the card before the firmware remounted it and resumed writing — which is
 * exactly the corruption the documented procedure exists to prevent.
 *
 * Set by event_logger_sd_unmount(), cleared by event_logger_sd_remount(), so a
 * deliberate release is honoured until an explicit mount request or a reboot.
 */
static bool s_sd_released = false;

/** @brief Active SD log filename including the leading '/' (e.g. "/20260507143022.csv"). */
static char s_cur_filename[SD_FILENAME_LEN];

/**
 * @brief Spinlock guarding @ref s_last_closed.
 *
 * Held only for the brief moment of copying a fixed-size filename buffer in
 * or out of static storage.
 */
static portMUX_TYPE s_closed_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Most recently *rotated-away* CSV filename (no leading '/').
 *
 * Empty until the first rotation of the boot. Read by T14's
 * upload-on-rotation path via event_logger_last_rotated(); written by
 * rotate_sd_file() under @ref s_closed_mux. Thread-safety: short critical
 * section copying a small fixed-size string.
 */
static char s_last_closed[SD_NAME_ONLY_LEN] = {};

/* -----------------------------------------------------------------------
 * Drop counter — tracks events lost due to Q3 overflow
 * ----------------------------------------------------------------------- */

/** @brief Spinlock protecting @ref g_q3_dropped against concurrent producers. */
static portMUX_TYPE      g_drop_mux   = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Cumulative count of Q3 events dropped since the last drain pass.
 *
 * Incremented by log_post() in the eviction path and the rare retry-fail
 * path; read and cleared atomically by log_take_dropped_count().
 */
static volatile uint32_t g_q3_dropped = 0;

/* -----------------------------------------------------------------------
 * Force-rotate request (T14 → T9 hand-off, since 1.17.28)
 *
 * Set by event_logger_force_rotate(); polled by T9's main loop after each
 * drain pass. T9 calls rotate_sd_file() and clears the flag. T14 polls
 * back via event_logger_force_rotate() until the flag clears or its
 * timeout expires.
 * ----------------------------------------------------------------------- */

/** @brief Spinlock guarding @ref s_force_rotate_req across T14/T9. */
static portMUX_TYPE      s_rotate_mux = portMUX_INITIALIZER_UNLOCKED;

/** @brief Force-rotate hand-off flag; raised by T14, cleared by T9 after rotate_sd_file(). */
static volatile bool     s_force_rotate_req = false;

/* -----------------------------------------------------------------------
 * log_post() — single entry point for all Q3 producers
 *
 * Implements the two-step evict-and-retry pattern documented in
 * event_logger.h. The fast path is a single non-blocking xQueueSend; the
 * slow path evicts the oldest entry, counts the drop, and retries once.
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
 * log_take_dropped_count() — read-and-reset under spinlock; see header
 * for the calling convention (T9 only, once per drain pass).
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
 * 2.0.1 (gh#30) — two valid forms are now recognised:
 *
 *  - New prefixed form (since 2.0.1):
 *      4 hex chars + '_' + 14 decimal digits + ".csv"  (total 23 chars,
 *      e.g. "5C88_20260529164050.csv")
 *  - Legacy un-prefixed form (pre-2.0.1):
 *      14 decimal digits + ".csv"  (total 18 chars,
 *      e.g. "20250607143022.csv")
 *
 * Both forms participate in all scan operations (rotation, oldest-delete,
 * boot-time resume).  Newly-created files use the prefixed form so the
 * unit ID is self-evident in cross-unit log archives; existing un-prefixed
 * files on a card from a prior firmware version continue to be honoured.
 * Files with any other naming pattern — including very old sequential-
 * index files (`ghc_NNNN.csv`) — are silently skipped.
 *
 * @param  name  Bare filename (no leading '/'). May be NULL.
 * @return true if @p name matches either accepted form; false otherwise.
 */
static bool is_ts_filename(const char *name)
{
    if (!name) return false;
    const size_t n = strlen(name);

    /* New prefixed form: 4 hex + '_' + 14 digits + ".csv" = 23 chars. */
    if (n == 23) {
        for (int i = 0; i < 4; i++) {
            if (!isxdigit((unsigned char)name[i])) return false;
        }
        if (name[4] != '_') return false;
        for (int i = 5; i < 19; i++) {
            if (!isdigit((unsigned char)name[i])) return false;
        }
        return strncmp(name + 19, ".csv", 4) == 0;
    }

    /* Legacy un-prefixed form: 14 digits + ".csv" = 18 chars. */
    if (n == 18) {
        for (int i = 0; i < 14; i++) {
            if (!isdigit((unsigned char)name[i])) return false;
        }
        return strncmp(name + 14, ".csv", 4) == 0;
    }

    return false;
}

/**
 * @brief Create an SD filename from the current local time.
 *
 * 2.0.1 (gh#30) — produces a path of the form "/XXXX_YYYYMMDDHHMMSS.csv"
 * in @p buf, where `XXXX` is the unit ID hex (`system_unit_id_str`) and
 * the timestamp is local time.  The unit-ID prefix lets cross-unit log
 * archives (e.g. when CSVs from multiple controllers are merged for
 * analysis) be visually self-attributing without having to inspect the
 * first BOOT row of each file.
 *
 * Local time is used so filenames are human-readable without timezone
 * conversion when browsing the card directly.  T10 keeps the POSIX TZ
 * environment variable up to date from `cfg.tz_str`, so localtime_r()
 * honours the configured zone.
 *
 * @param  buf  Destination buffer; populated with a NUL-terminated path.
 * @param  len  Capacity of @p buf in bytes; 32 (SD_FILENAME_LEN) is
 *              sufficient for the prefixed form.
 *
 * @note Calls dm_get_unix_time() (T4 helper); safe before NTP sync because
 *       the RTC is seeded from NVS at boot.  Calls system_unit_id_str()
 *       which reads cached eFuse-MAC state; safe before WiFi init.
 */
static void make_ts_filename(char *buf, size_t len)
{
    char id[5] = {0};                                  /* "XXXX" + NUL */
    system_unit_id_str(id, sizeof(id));

    time_t now = (time_t)dm_get_unix_time();
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    snprintf(buf, len, "/%s_%04d%02d%02d%02d%02d%02d.csv",
             id,
             tm_local.tm_year + 1900,
             tm_local.tm_mon  + 1,
             tm_local.tm_mday,
             tm_local.tm_hour,
             tm_local.tm_min,
             tm_local.tm_sec);
}

/**
 * @brief Is this log file one of OURS?
 *
 * The name carries the unit id (`XXXX_YYYYMMDDHHMMSS.csv`), because the dev rig
 * takes swappable modules and one card collects both of their histories. A
 * LEGACY un-prefixed file (14 digits) counts as ours: it can only come from an
 * era when one unit owned the card, and somebody has to be allowed to retire
 * it.
 */
static bool name_is_mine(const char *name)
{
    char id[5] = {0};
    system_unit_id_str(id, sizeof(id));
    if (strlen(name) == 18u) { return true; }          /* legacy, un-prefixed */
    return (strncmp(name, id, 4) == 0) && name[4] == '_';
}

/** What one pass over the card found (gh#82). */
typedef struct {
    uint32_t total;                       /**< every timestamp-pattern file */
    uint32_t mine;                        /**< ...that belongs to this unit */
    char oldest_mine[SD_NAME_ONLY_LEN];   /**< "" when this unit has none */
    char newest_mine[SD_NAME_ONLY_LEN];
} log_scan_t;

static void log_scan_cb(const char *name, void *ctx)
{
    log_scan_t *sc = (log_scan_t *)ctx;
    if (!is_ts_filename(name)) { return; }
    sc->total++;
    if (!name_is_mine(name)) { return; }
    sc->mine++;
    /* Within one unit's prefix a plain strcmp IS chronological order, because
     * the rest of the name is a fixed-width timestamp. Across units it is not,
     * which is why every comparison here is made among our own files only
     * (the accepted rule: sort on unit, then on date-time). */
    if (sc->oldest_mine[0] == '\0' || strcmp(name, sc->oldest_mine) < 0) {
        snprintf(sc->oldest_mine, sizeof(sc->oldest_mine), "%s", name);
    }
    if (sc->newest_mine[0] == '\0' || strcmp(name, sc->newest_mine) > 0) {
        snprintf(sc->newest_mine, sizeof(sc->newest_mine), "%s", name);
    }
}

/**
 * @brief Count this unit's log files and find its oldest and newest, in one
 *        pass that CANNOT truncate (gh#82).
 *
 * Every decision T9 makes about files -- how many there are, which to delete,
 * which to resume -- used to come from a comma-separated list in a fixed
 * buffer. `storage_sd_list_csv()` drops the names that do not fit and still
 * returns STORAGE_OK, and the names it drops are the ones the directory lists
 * last, which on FAT are roughly the newest. Past ~30 files that made the
 * count saturate at exactly SD_MAX_FILES, so `count > SD_MAX_FILES` was never
 * true and **retention silently stopped**; the boot resume, choosing the
 * "largest" name from the same partial view, could append today's rows to a
 * file named weeks ago (2344, 2026-09-24).
 *
 * @return false if the card could not be scanned at all.
 */
static bool log_scan(log_scan_t *out)
{
    memset(out, 0, sizeof(*out));
    return storage_sd_foreach_csv(".csv", log_scan_cb, out) == STORAGE_OK;
}

/**
 * @brief Delete the lexicographically oldest timestamp CSV file on the SD card.
 *
 * Used by both check_free_space() (proactive reclaim) and write_to_sd()
 * (reactive reclaim on STORAGE_ERR_FULL). Skips non-timestamp files via the
 * is_ts_filename() filter inside log_scan(), and only this unit's files.
 *
 * @return true on successful deletion; false if no candidates were found
 *         or the underlying storage_sd_delete() call failed.
 */
static bool delete_oldest(void)
{
    log_scan_t sc;
    if (!log_scan(&sc) || sc.oldest_mine[0] == '\0') return false;

    char path[SD_FILENAME_LEN];
    snprintf(path, sizeof(path), "/%s", sc.oldest_mine);
    bool ok = (storage_sd_delete(path) == STORAGE_OK);
    if (ok) ESP_LOGI(TAG, "[T9] Deleted oldest log file %s", path);
    return ok;
}

/**
 * @brief Enforce the free-space guard rail after a rotation.
 *
 * If `storage_sd_free_bytes()` is below SD_FREE_MIN_BYTES, deletes the
 * lex-oldest file to reclaim space.  If the file count is already at the
 * SD_MIN_FILES retention floor and free space is still low, suspends SD
 * logging (clears @ref s_sd_ok) and emits a LOG_SYSTEM event with
 * `value_a = -2` so the suspension is visible to operators.
 *
 * @note No-op when SD has plenty of free space (the common case).
 */
static void check_free_space(void)
{
    if (storage_sd_free_bytes() >= SD_FREE_MIN_BYTES) return;

    /* gh#82: this unit's own file count, from a scan that cannot truncate. The
     * retention floor is per unit, so a module never eats another module's
     * history to make room for its own. */
    log_scan_t sc;
    (void)log_scan(&sc);
    const uint32_t count = sc.mine;

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
 *
 * @param  t  Raw event_type byte from a log_event_t.
 * @return Static string ("SENSOR", "RELAY", "MODE", ...). Unknown values
 *         return "UNKNWN" so the CSV line is never malformed.
 */
static const char *evt_type_str(uint8_t t)
{
    switch ((log_type_t)t) {
        case LOG_SENSOR:      return "SENSOR";     /* reserved; no emitter since rc.1.4.0 */
        case LOG_RELAY:       return "RELAY";
        case LOG_MODE_CHANGE: return "MODE";
        case LOG_SETPOINT:    return "SETPT";
        case LOG_SESSION:     return "SESSION";
        case LOG_ALARM:       return "ALARM";
        case LOG_SYSTEM:      return "SYSTEM";
        case LOG_SENSOR_HR:   return "SENSOR_HR";   /* rc.1.4.0 — see logUpdatePlan §2 */
        case LOG_SUN:         return "SUN";         /* rc.1.4.0 — see logUpdatePlan §3 */
        case LOG_PIN_AUTH:    return "PIN_AUTH";    /* 2.5.0 (gh#58) — failed PIN / lockout */
        default:              return "UNKNWN";
    }
}

/**
 * @brief Return a short ASCII name for a log_initiator_t value.
 *
 * @param  i  Raw initiator byte from a log_event_t.
 * @return Static string ("SYS", "FARMER", "ADMIN", "MQTT", "WEB").
 *         Unknown values return "UNK".
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
 * Sequence:
 *  -# Captures the bare name of the soon-to-be-closed file in
 *     @ref s_last_closed (under @ref s_closed_mux) so T14 can find it.
 *  -# Generates a new timestamp filename via make_ts_filename().
 *  -# Writes the CSV header (CSV_HEADER) to the new file.
 *  -# Writes the unit-id preamble row (gh#17) — a LOG_SYSTEM value_a=11
 *     event with `value_b = system_unit_id_u16()` — so every downloaded
 *     CSV is self-identifying.
 *  -# If the file count now exceeds SD_MAX_FILES, deletes the
 *     lexicographically oldest timestamp file.
 *  -# Calls check_free_space() to enforce the SD_FREE_MIN_BYTES guard.
 *  -# Notifies T14 (@ref task_t14) via xTaskNotify(T14_NOTIFY_LOG_ROTATED)
 *     so the upload-on-rotation path can consider the just-closed file.
 *
 * @note If the header write fails, @ref s_sd_ok is cleared and SD logging
 *       is suspended; the failure also surfaces through write_to_sd()'s
 *       subsequent attempts.
 * @note NULL-safe with respect to @ref task_t14 — early-boot rotations
 *       before T14 is spawned skip the notification.
 * @see  event_logger_force_rotate
 * @see  event_logger_last_rotated
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

    /* Enforce the SD_MAX_FILES ceiling -- PER UNIT (gh#82). The count comes
     * from a scan that cannot truncate; the old one saturated at exactly
     * SD_MAX_FILES, so this test could never fire and nothing was ever
     * deleted once the card passed the cap. */
    log_scan_t sc;
    if (log_scan(&sc)) {
        uint32_t trimmed = 0u;
        /* Trim back TO the cap, a few per rotation. Deleting exactly one while
         * creating exactly one leaves a card that is ALREADY over the cap
         * sitting there for ever -- and 2344's was, by weeks, because the count
         * came from a truncated scan and the test could never fire (gh#82).
         * Re-scan after each delete so the next "oldest" is the real one, and
         * stop at SD_TRIM_PER_ROTATION so no rotation becomes an unlink storm. */
        while (sc.mine > SD_MAX_FILES && trimmed < SD_TRIM_PER_ROTATION &&
               sc.oldest_mine[0] != '\0') {
            char path[SD_FILENAME_LEN];
            snprintf(path, sizeof(path), "/%s", sc.oldest_mine);
            if (storage_sd_delete(path) != STORAGE_OK) { break; }
            trimmed++;
            ESP_LOGI(TAG, "[T9] retention: deleted %s", path);
            if (!log_scan(&sc)) { break; }
        }
        ESP_LOGI(TAG, "[T9] Rotated to %s (%u of ours, %u on the card, %u trimmed)",
                 s_cur_filename, (unsigned)sc.mine, (unsigned)sc.total,
                 (unsigned)trimmed);
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
 * On STORAGE_ERR_FULL or STORAGE_ERR_IO, attempts to delete the oldest log
 * file and retry the write before falling back to NVS-only mode (clears
 * @ref s_sd_ok and emits a LOG_SYSTEM event with `value_a = -1`).  When the
 * post-write file size reaches SD_ROTATE_BYTES, calls rotate_sd_file().
 *
 * @param  evt  Event to format and append. Must not be NULL.
 *
 * @note The retry-on-full path is bounded: a single oldest-file deletion
 *       per write attempt, no retry on a second failure.
 */
static void write_to_sd(const log_event_t *evt)
{
    char csv_line[80];
    build_csv_line(evt, csv_line, sizeof(csv_line));

    storage_status_t rc = storage_sd_write_append(s_cur_filename, csv_line);

    /* On full/IO error, attempt to reclaim space by deleting the oldest file. */
    if (rc == STORAGE_ERR_FULL || rc == STORAGE_ERR_IO) {
        /* gh#82: our own count, from the non-truncating scan. */
        log_scan_t sc;
        if (log_scan(&sc) && sc.mine > SD_MIN_FILES) {
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
 *
 * @param  evt  Event to process. Must not be NULL.
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
 * @brief Scan the SD card for timestamp log files and set @ref s_cur_filename.
 *
 * Resumes the most recent (lexicographically largest) file if its size is
 * below SD_ROTATE_BYTES.  Creates a new timestamp file (with CSV header)
 * otherwise.  Shared between T9 startup and event_logger_sd_remount().
 *
 * @return true if @ref s_cur_filename now points to a usable file; false
 *         if file creation failed (header write error).
 */
static bool sd_open_active_file(void)
{
    /* gh#82: OUR newest file, from a scan that cannot truncate. Taking the
     * largest name from a truncated list meant resuming into a file that was
     * merely the newest of the ones that survived truncation -- or, on a card
     * shared with the other module, into that module's file. */
    log_scan_t sc;
    bool found = log_scan(&sc) && sc.newest_mine[0] != '\0';

    if (found) {
        char path[SD_FILENAME_LEN];
        snprintf(path, sizeof(path), "/%s", sc.newest_mine);
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

/**
 * @brief Attempt to mount the SD card and re-enable SD logging in T9.
 *
 * See event_logger.h for the full description. Belt-and-braces total-bytes
 * check (gh#14) guards against drivers that report mount success on an
 * effectively absent card.
 */
bool event_logger_active_file(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }
    out[0] = '\0';
    if (!s_sd_ok || s_cur_filename[0] == '\0') { return false; }
    const char *bare = (s_cur_filename[0] == '/') ? s_cur_filename + 1 : s_cur_filename;
    snprintf(out, cap, "%s", bare);
    return out[0] != '\0';
}

bool event_logger_sd_remount(void)
{
    if (s_sd_ok) return true;

    /* gh#61 — an explicit mount request is the operator taking the card back
     * into service, so it clears the release latch regardless of whether the
     * mount below succeeds. Leaving it set on failure would strand a unit whose
     * card was released and then reinserted. */
    s_sd_released = false;

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

/**
 * @brief Stop SD logging in T9 and unmount the SD card.
 *
 * See event_logger.h. Clears @ref s_sd_ok first so T9 will not race on a
 * card that is being torn down.
 */
void event_logger_sd_unmount(void)
{
    s_sd_ok = false;
    /* gh#61 — latch the release so T9's 60 s retry does not remount the card
     * under an operator who is on their way to physically remove it. */
    s_sd_released = true;
    storage_sd_unmount();
    ESP_LOGW(TAG, "[T9] SD released on request — automount suppressed until an "
                  "explicit mount or reboot (gh#61)");
}

/* =======================================================================
 * Public — synchronous LOG_SYSTEM helper (since 2.0.0-a.6.34)
 *
 * Bypasses Q3 to guarantee the row reaches the SD file before the caller
 * returns. See event_logger.h for the rationale (T13 fallback audit row).
 * ======================================================================= */

/**
 * @brief Synchronously write a LOG_SYSTEM event row to the current SD file.
 *
 * Full rationale in event_logger.h. Bypasses Q3 / T9 entirely so that the
 * row reaches the SD card before the caller returns — required when a
 * subsequent esp_restart() would otherwise cut off T9 before it drains.
 *
 * @param  value_a    Subtype encoding from the LOG_SYSTEM value_a table.
 * @param  value_b    Subtype payload (count, id, sub-code; depends on value_a).
 * @param  initiator  Attribution for the row. 2.6.0 (gh#59) added this
 *                    parameter: the factory-reset row (value_a=26) is an
 *                    operator action and must not read as LOG_BY_SYSTEM.
 * @param  channel    Channel/surface hint, 0 when not applicable.
 * @return true if the row was appended; false if SD is unmounted or the
 *         write failed.
 */
bool event_logger_post_sync(int16_t value_a, int16_t value_b,
                            log_initiator_t initiator, uint8_t channel)
{
    if (!s_sd_ok || s_cur_filename[0] == '\0') {
        return false;
    }

    log_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_SYSTEM;
    evt.initiator  = (uint8_t)initiator;
    evt.channel    = channel;
    evt.value_a    = value_a;
    evt.value_b    = value_b;

    char csv_line[80];
    build_csv_line(&evt, csv_line, sizeof(csv_line));
    return storage_sd_write_append(s_cur_filename, csv_line) == STORAGE_OK;
}

/* =======================================================================
 * Public — rotation-tracking helpers (T14)
 * ======================================================================= */

/**
 * @brief Return the most recently rotated-away CSV filename to T14.
 *
 * Reads @ref s_last_closed under @ref s_closed_mux. See event_logger.h.
 */
bool event_logger_last_rotated(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }

    portENTER_CRITICAL(&s_closed_mux);
    strncpy(out, s_last_closed, cap - 1u);
    out[cap - 1u] = '\0';
    portEXIT_CRITICAL(&s_closed_mux);

    return out[0] != '\0';
}

/**
 * @brief Force T9 to rotate the active SD log file (T14 daily-upload path).
 *
 * Raises @ref s_force_rotate_req under @ref s_rotate_mux, posts a synthetic
 * LOG_SYSTEM(value_a=6) marker via log_post() to (a) wake T9 from
 * `xQueueReceive(portMAX_DELAY)` and (b) leave a "why was this file closed?"
 * trail in the outgoing file, then polls every 100 ms until T9 clears the
 * flag or @p timeout_ms elapses.
 *
 * @note On timeout the request flag is intentionally left set — T9 will
 *       still rotate when it next gets CPU time; the caller simply did not
 *       observe completion in its budget.
 */
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

/**
 * @brief Return the lex-newest closed (non-active) CSV name on SD.
 *
 * Used by T14 daily-fallback when no rotation occurred this boot. Falls back
 * to @ref s_last_closed if the SD scan finds no candidate. Full contract in
 * event_logger.h.
 */
/** One closed file of ours, chosen while scanning (gh#82). */
typedef struct {
    char        active[SD_NAME_ONLY_LEN];  /**< skip the file being written */
    const char *after;                     /**< NULL, or "strictly greater than" */
    bool        want_max;                  /**< true: newest; false: next after */
    char        best[SD_NAME_ONLY_LEN];
} pick_t;

/** The 14-digit timestamp inside a log name, prefixed or legacy (gh#82). */
static const char *name_ts(const char *name)
{
    return (strlen(name) == 23u && name[4] == '_') ? name + 5 : name;
}

static void pick_cb(const char *name, void *ctx)
{
    pick_t *p = (pick_t *)ctx;
    if (!is_ts_filename(name) || !name_is_mine(name)) { return; }
    if (p->active[0] != '\0' && strcmp(name, p->active) == 0) { return; }
    /* The upload watermark is a point in TIME, not a name: compare the
     * timestamps. T14 persists the last file it uploaded, and on the dev rig
     * that can be the OTHER module's -- 2344 found `FDA4_20260916192045.csv`
     * there on 2026-09-24, because the enumerator used to take the lexicographic
     * maximum across every unit and `FDA4_` sorts above `2344_`. Comparing whole
     * names would then find nothing pending for ever, since all of this unit's
     * names sort below that one, and the backlog would never drain (gh#82). */
    if (p->after != NULL && p->after[0] != 0 &&
        strcmp(name_ts(name), name_ts(p->after)) <= 0) { return; }
    if (p->best[0] == '\0') {
        snprintf(p->best, sizeof(p->best), "%s", name);
        return;
    }
    const int c = strcmp(name, p->best);
    if ((p->want_max && c > 0) || (!p->want_max && c < 0)) {
        snprintf(p->best, sizeof(p->best), "%s", name);
    }
}

/** Fill @p p with the active file to exclude, and pick over the card. */
static bool pick_closed(pick_t *p)
{
    p->best[0] = '\0';
    p->active[0] = '\0';
    if (s_sd_ok && s_cur_filename[0] != '\0') {
        const char *bare = (s_cur_filename[0] == '/') ? s_cur_filename + 1 : s_cur_filename;
        /* strncpy, not snprintf: the path buffer is longer than this field,
         * and a bounded copy says so to the compiler as well as the reader. */
        strncpy(p->active, bare, sizeof(p->active) - 1u);
        p->active[sizeof(p->active) - 1u] = 0;
    }
    return storage_sd_foreach_csv(".csv", pick_cb, p) == STORAGE_OK;
}

bool event_logger_newest_closed(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }
    out[0] = '\0';

    /* gh#82: our newest closed file, from a scan that cannot truncate. Sizing
     * the old list to SD_MAX_FILES names (gh#42) only moved the cliff from ~21
     * files to ~30: past the cap the newest names fell off again and the
     * upload path stalled on a stale file. */
    pick_t p;
    p.after = NULL;
    p.want_max = true;
    if (!pick_closed(&p)) {
        /* SD unavailable — fall back to in-memory rotation record. */
        return event_logger_last_rotated(out, cap);
    }
    if (p.best[0] != '\0') {
        snprintf(out, cap, "%s", p.best);
    }

    if (out[0] == '\0') {
        /* SD scan found nothing — try the in-memory record. */
        return event_logger_last_rotated(out, cap);
    }
    return true;
}

/**
 * @brief Return the smallest closed CSV name strictly greater than @p after.
 *
 * a.6.35.2 multi-file upload helper. Scans the SD card and returns the
 * lex-smallest closed CSV whose name is strictly greater than @p after.
 * T14's upload_pending walks this in a loop, advancing `after` to each
 * successful upload, so a backlog of missed files (e.g. WiFi outage that
 * spanned a rotation) gets drained in chronological order on the next
 * trigger. Closed-file enumeration is identical to
 * event_logger_newest_closed() (same non-truncating scan + active-file exclusion);
 * only the selection predicate differs: smallest > after, vs lex-max.
 *
 * Unlike event_logger_newest_closed() this routine does *not* fall back to
 * the in-memory @ref s_last_closed record on SD failure, because the
 * caller's intent is "walk all pending in order" — and an in-memory
 * fallback cannot satisfy that.
 *
 * @see event_logger.h for the full @param/@return contract.
 */
bool event_logger_next_pending(const char *after, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return false; }
    out[0] = '\0';
    if (after == NULL) { after = ""; }

    /* gh#82: the next one of ours after @p after, from a scan that cannot
     * truncate. Unlike newest_closed this does NOT fall back to the in-memory
     * record on SD failure: the caller's intent is "walk all pending in
     * order", which an in-memory record cannot satisfy. */
    pick_t p;
    p.after = after;
    p.want_max = false;
    if (!pick_closed(&p)) {
        return false;
    }
    if (p.best[0] != '\0') {
        snprintf(out, cap, "%s", p.best);
    }

    return out[0] != '\0';
}

/* =======================================================================
 * T9 task
 * ======================================================================= */

/**
 * @brief T9 — Event Logger task body.
 *
 * See event_logger.h for the full responsibility statement.
 *
 * Lifecycle:
 *  -# Mount the SD card via storage_init(); if it succeeds and
 *     sd_open_active_file() opens a usable file, set @ref s_sd_ok = true.
 *  -# Enter the infinite main loop:
 *     - Block on xQueueReceive(Q3) — `portMAX_DELAY` when SD is healthy,
 *       60 s timeout when SD is absent (the timeout drives automount
 *       retries).
 *     - process_event() each received event (writes to SD if mounted).
 *     - Drain any further immediately-available events non-blocking.
 *     - Attempt SD automount once per minute while @ref s_sd_ok is false,
 *       even if Q3 keeps the receive busy.
 *     - Read log_take_dropped_count(); if non-zero, synthesise a LOG_SYSTEM
 *       row reporting the drop count and post it directly to Q3 (not via
 *       log_post() — avoids re-entrant eviction; see header design notes).
 *     - Honour any pending @ref s_force_rotate_req from T14.
 *
 * @param  pvParameters  Unused; pass NULL.
 *
 * @note   This function never returns. It is a FreeRTOS task entry point.
 * @see    log_post
 * @see    task_t14 (status_post.cpp) — the rotation-notify recipient.
 */
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
            /* gh#61 — do not undo a deliberate unmount. s_sd_released is only
             * set by event_logger_sd_unmount(); an absent or failed card leaves
             * it false, so hot-insertion still works. */
            if (!s_sd_ok && !s_sd_released) {
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
         * the sensor poll keeps Q3 busy (e.g. poll_interval = 30 s).
         *
         * gh#61 — s_sd_released is checked here too. This is the path that
         * actually fires in service: the sensor poll posts three rows every
         * 30 s, so the queue is never idle long enough for the timeout branch
         * above to run. The first 2.4.9 bench build guarded only that branch
         * and the card remounted itself 30 s after a deliberate unmount —
         * caught by the release's own 135 s verification step. */
        if (!s_sd_ok && !s_sd_released) {
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
