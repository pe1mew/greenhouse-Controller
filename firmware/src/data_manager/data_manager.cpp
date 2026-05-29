/**
 * @file data_manager.cpp
 * @brief T4 — Data Manager task implementation (Phase 1).
 *
 * Central data store: NVS load, RTC read/write, sunrise/sunset, Q4/Q6
 * consumers, TN1/TN2/TN4 task notifications.
 *
 * ## Thread-safety contract
 *  - s_cfg  is always accessed under MX4.
 *  - s_meas is always accessed under MX2.
 *  - s_ring is always accessed under MX3.
 *  - MX1 is used for all DS1307 I2C operations.
 *  - Mutexes are acquired with a bounded timeout (200 ms max); on timeout,
 *    a warning is logged and the operation is skipped rather than blocking
 *    indefinitely.
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 1
 *  - design/tasks.md T4
 *  - design/technicalSoftwareDesignSpecification.md §5.x T4
 *
 * @author  Greenhouse Controller project
 */

#include "data_manager.h"
#include "sunrise.h"
#include "../event_logger/event_logger.h"
#include "../relay_controller/relay_controller.h"
#include "../status_post/status_post.h"  /* T14_NOTIFY_CFG_CHANGED (a.6.35.1) */
#include "../network_manager/network_manager.h"  /* rc.1.5.4 — nm_is_sntp_synced() */

#include "nvs_config.h"
#include "ds1307_rtc.h"
#include "littlefs_storage.h"
#include "../system_id/system_id.h"   /* unit_id at boot (gh#17, since 1.18.3) */

/* alpha.6.7 — dropped vestigial #include <Arduino.h> and <WiFi.h>.
 * The 3 WiFi.* call sites in dm_status_snapshot() are rewritten below
 * to use esp_wifi_sta_get_ap_info + esp_netif_get_ip_info directly.
 * Pulls in esp_wifi.h + esp_netif.h replacing the Arduino WiFi shim. */
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_system.h>     /* esp_reset_reason() — boot-reason event (1.17.31) */
#include <esp_task_wdt.h>   /* WDT subscription (1.17.29 / gh#13) */
#include <esp_timer.h>
#include <esp_core_dump.h>  /* a.6.35.6 — boot-time check + erase from web GUI */
#include <time.h>
#include <sys/time.h>    /* settimeofday() — alpha.6.7, was via Arduino.h */
#include <string.h>

static const char *TAG = "T4";

/* ============================================================
 * NVS factory-default values  → config/cfg_defaults.h (single source of truth)
 * Validation bounds (min/max)  → config/cfg_limits.h
 * ============================================================ */
#include "cfg_defaults.h"
#include "cfg_limits.h"

/* ============================================================
 * NVS key string literals
 * ============================================================ */

/* Climate namespace */
static const char K_T_MIN_DAY[]    = "t_min_day";
static const char K_T_MAX_DAY[]    = "t_max_day";
static const char K_T_MIN_NGT[]    = "t_min_ngt";
static const char K_T_MAX_NGT[]    = "t_max_ngt";
static const char K_RH_MIN_DAY[]   = "rh_min_day";
static const char K_RH_MAX_DAY[]   = "rh_max_day";
static const char K_RH_MIN_NGT[]   = "rh_min_ngt";
static const char K_RH_MAX_NGT[]   = "rh_max_ngt";
static const char K_HYST_T[]       = "hyst_t";
static const char K_HYST_RH[]      = "hyst_rh";
static const char K_RH_CTRL_EN[]   = "rh_ctrl_en";
static const char K_CR_PRIORITY[]  = "cr_priority";
static const char K_AVG_WIN_T[]    = "avg_win_t";
static const char K_AVG_WIN_RH[]   = "avg_win_rh";

/* Wind namespace */
static const char K_V_MAX[]          = "v_max";
static const char K_DIR_EXCL_LOW[]   = "dir_excl_low";
static const char K_DIR_EXCL_HIGH[]  = "dir_excl_high";
static const char K_WIND_PROT_EN[]   = "wind_prot_en";

/* Motor namespace */
static const char K_TRAVEL_M1[]       = "travel_m1";
static const char K_TRAVEL_M2[]       = "travel_m2";
static const char K_TRAVEL_M3[]       = "travel_m3";
static const char K_DWELL_OPEN_M1[]   = "dwell_open_m1";
static const char K_DWELL_OPEN_M2[]   = "dwell_open_m2";
static const char K_DWELL_OPEN_M3[]   = "dwell_open_m3";
static const char K_DWELL_CLOSE_M1[]  = "dwell_close_m1";
static const char K_DWELL_CLOSE_M2[]  = "dwell_close_m2";
static const char K_DWELL_CLOSE_M3[]  = "dwell_close_m3";

/* System namespace */
static const char K_POLL_INTERVAL[]    = "poll_interval";
static const char K_SESSION_TIMEOUT[]  = "session_timeout";
static const char K_AP_TIMEOUT[]       = "ap_timeout";
static const char K_LAT_DEG[]          = "lat_deg";
static const char K_LAT_FRAC[]         = "lat_frac";
static const char K_LON_DEG[]          = "lon_deg";
static const char K_LON_FRAC[]         = "lon_frac";
static const char K_LED_DAY_BRT[]      = "led_day_brt";
static const char K_LED_NITE_BRT[]     = "led_nite_brt";
static const char K_LED_NITE_FROM[]    = "led_nite_from";
static const char K_LED_NITE_TO[]      = "led_nite_to";
static const char K_TZ_STR[]           = "tz_str";

/* Web-tab / status-website (system namespace; NVS key max 15 chars + NUL) */
static const char K_STATUS_URL[]       = "status_url";
static const char K_STATUS_SECRET[]    = "status_secret";
static const char K_STATUS_INTERVAL[]  = "status_intv_s";   /* 13 chars */
static const char K_STATUS_ENABLE[]    = "status_enable";
static const char K_STATUS_EXPOSE[]    = "status_expose";
static const char K_LOG_UPLOAD_H[]     = "log_upload_h";
static const char K_LOG_UPLOAD_M[]     = "log_upload_m";
static const char K_LOG_UPLOAD_ROT[]   = "log_upload_rot";
static const char K_LOG_LAST_UP[]      = "log_last_up";

/* rc.1.5.0 (gh#28) — operating-mode persistence. NVS-backed so a power blip
 * during a deliberate STANDBY does not silently re-enable climate control on
 * reboot. Stored as i32: 0 = AUTOMATIC, 1 = STANDBY. Other modes (MOTOR_ALARM,
 * WIND_OVERRIDE, CALIBRATING) are runtime conditions and are NOT persisted. */
static const char K_MODE_STANDBY[]     = "mode_standby";

/* ============================================================
 * Module-private state
 * ============================================================ */

/** @brief NVS-backed configuration shadow.  Protected by MX4. */
static cfg_shadow_t   s_cfg;

/** @brief Latest sensor reading from T5.  Protected by MX2. */
static sensor_reading_t s_meas;

/** @brief True once at least one Q6 message has been received. */
static bool s_meas_valid;

/** @brief Sensor history ring buffer (7.2 KB BSS).  Protected by MX3. */
static dm_ring_buf_t s_ring;

/** @brief Cached "is a coredump sitting in flash from a previous panic?"
 *
 * Set once at boot from `esp_core_dump_image_check()` (cheap flash CRC check,
 * not worth repeating). Cleared by `dm_coredump_clear()` after T11 erases
 * the partition via `/api/coredump/erase`. Read by:
 *
 *  - `dm_status_snapshot()`  → fills `status_snapshot_t::coredump_available`
 *    so the canonical JSON emits the `coredump_available` mode flag and the
 *    GUI Alarms card renders the badge.
 *  - `dm_coredump_size_bytes()` → exposed for the GUI to display the dump
 *    size before the operator decides to download.
 *
 * No mutex: single 1-byte atomic load/store. Reads from any task; writes
 * from the boot-sequence + the erase handler (both serialised by being
 * one-shot from T11's worker thread).
 *
 * Since 2.0.0-a.6.35.6. */
static volatile bool   s_coredump_present  = false;
static volatile size_t s_coredump_size_b   = 0u;

/** @brief Re-read RTC every this many main-loop ticks (≈ RTC_POLL_TICKS s). */
#define RTC_POLL_TICKS  60u

/* ============================================================
 * Internal helper — RTC datetime → Unix UTC timestamp
 * ============================================================ */

/**
 * @brief Convert a DS1307 rtc_datetime_t (UTC) to a Unix timestamp.
 *
 * Manual implementation that avoids mktime()/timegm() portability
 * concerns and the TZ-environment dependence of timegm(). Counts days
 * since 1970-01-01 by summing complete years (with the Gregorian leap
 * rule) and complete months in the current year, then adds days,
 * hours, minutes, seconds. Supports years 1970–2099.
 *
 * @param dt  Datetime in UTC; not validated. Caller must pass a
 *            sensible struct from rtc_get_time().
 * @return    Unix UTC seconds since 1970-01-01 00:00:00.
 * @note   Returns 0 for `dt->year < 1970` (the year-loop is empty);
 *         not exercised in normal operation because the DS1307
 *         driver clamps reads.
 */
static uint32_t rtc_dt_to_unix(const rtc_datetime_t *dt)
{
    /* Days-per-month lookup; index 1–12 (index 0 unused). */
    static const uint8_t days_in_month[13] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    uint32_t days = 0u;

    /* Count complete years since 1970. */
    for (uint16_t y = 1970u; y < dt->year; y++) {
        bool leap = ((y % 4u == 0u) && ((y % 100u != 0u) || (y % 400u == 0u)));
        days += leap ? 366u : 365u;
    }

    /* Count complete months in the current year. */
    bool leap_cur = ((dt->year % 4u == 0u) &&
                     ((dt->year % 100u != 0u) || (dt->year % 400u == 0u)));
    for (uint8_t m = 1u; m < dt->month; m++) {
        days += days_in_month[m];
        if (m == 2u && leap_cur) { days++; }
    }

    /* Add complete days in the current month (day is 1-based). */
    days += (uint32_t)dt->day - 1u;

    return days * 86400u
           + (uint32_t)dt->hour   * 3600u
           + (uint32_t)dt->minute * 60u
           + (uint32_t)dt->second;
}

/* ============================================================
 * Internal helper — recompute sunrise/sunset and is_daytime
 * ============================================================ */

/**
 * @brief Recompute the cached sunrise/sunset and is_daytime values.
 *
 * Reads `s_cfg.lat_*`, `s_cfg.lon_*`, and `s_cfg.current_unix_ts`, then
 * writes `s_cfg.is_daytime`, `s_cfg.sunrise_mins_utc`, and
 * `s_cfg.sunset_mins_utc`. Called every time the timestamp changes
 * (after each RTC re-read and after every NTP sync) and whenever
 * lat/lon are modified via Q4.
 *
 * @warning Must be called with MX4 held — or at boot before MX4 is
 *          contested. Touches `s_cfg` directly.
 * @see    sunrise_calc(), sunrise_is_daytime()
 */
/* rc.1.4.0 — LOG_SUN change-detect cache. Initialised to a sentinel so the
 * first update_sun_times() call after boot always emits (real sunrise/sunset
 * values are in 0..1439, never -1). See model/logUpdatePlan.md §3.3. */
static int32_t s_last_logged_sunrise_min = -1;
static int32_t s_last_logged_sunset_min  = -1;

/**
 * @brief Convert cached UTC sunrise/sunset to local-time minutes-from-midnight.
 *
 * Computes the LT−GM offset from `time(NULL)` via `localtime_r`/`gmtime_r`
 * (same math as `dm_status_snapshot`); applies it to the cached UTC values
 * and normalises with the standard `+14400` (10 days of minutes) modulo
 * trick so westward (negative-offset) TZs stay positive.
 *
 * @param out_sunrise_local  Receives local-time sunrise minutes (0..1439).
 * @param out_sunset_local   Receives local-time sunset  minutes (0..1439).
 * @note  Pure helper — no MX4 take. Caller must already hold MX4 (or be
 *        in a boot-time pre-task-creation phase like update_sun_times()).
 */
static void sun_local_from_utc(int32_t *out_sunrise_local,
                               int32_t *out_sunset_local)
{
    long off_min = 0;
    time_t ts_now = (time_t)s_cfg.current_unix_ts;
    if (ts_now > 0) {
        struct tm lt, gm;
        localtime_r(&ts_now, &lt);
        gmtime_r(&ts_now, &gm);
        int day_diff = lt.tm_yday - gm.tm_yday;
        if      (lt.tm_year > gm.tm_year) day_diff =  1;
        else if (lt.tm_year < gm.tm_year) day_diff = -1;
        off_min = (long)day_diff * 1440L
                + (long)(lt.tm_hour - gm.tm_hour) * 60L
                + (long)(lt.tm_min  - gm.tm_min);
    }
    *out_sunrise_local = (int32_t)((s_cfg.sunrise_mins_utc + off_min + 14400L) % 1440);
    *out_sunset_local  = (int32_t)((s_cfg.sunset_mins_utc  + off_min + 14400L) % 1440);
}

/**
 * @brief rc.1.4.0 — Post a LOG_SUN row to Q3.
 *
 * Mirrors the `log_sys()` helper pattern: builds a minimal log_event_t and
 * hands it to log_post() for T9 to drain. Initiator is always LOG_BY_SYSTEM
 * (T4 owns the event; per logUpdatePlan §3.5 the matching SETPT,*,0,21,*,*
 * row already carries the operator identity when an operator coordinate
 * edit indirectly triggered the recompute).
 *
 * @param sunrise_min  Local-time sunrise minutes-from-midnight (0..1439).
 * @param sunset_min   Local-time sunset  minutes-from-midnight (0..1439).
 */
static void log_sun_event(int32_t sunrise_min, int32_t sunset_min)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SUN;
    ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
    ev.channel    = 0u;
    ev.param_id   = (uint8_t)LOG_PARAM_NONE;
    ev.value_a    = (int16_t)sunrise_min;
    ev.value_b    = (int16_t)sunset_min;
    log_post(&ev);
}

static void update_sun_times(void)
{
    float lat = (float)s_cfg.lat_deg + (float)s_cfg.lat_frac / 1000.0f;
    float lon = (float)s_cfg.lon_deg + (float)s_cfg.lon_frac / 1000.0f;
    int32_t ts = (int32_t)s_cfg.current_unix_ts;

    s_cfg.is_daytime = sunrise_is_daytime(ts, lat, lon);
    sunrise_calc(ts, lat, lon, &s_cfg.sunrise_mins_utc, &s_cfg.sunset_mins_utc);

    /* rc.1.4.0 — emit LOG_SUN whenever the cached values change. Compares
     * the LOCAL-time minutes-from-midnight (consistent with how the values
     * are exposed via /api/status and how the plotter consumes them).
     *
     * In steady-state operation this fires:
     *   - Once at boot (sentinel -1 vs first real value)
     *   - Once per local midnight rollover (sunrise/sunset shift 1-2 min/day)
     *   - Once per operator lat/lon edit via Q4
     *
     * See model/logUpdatePlan.md §3.3 for the emission-trigger table. */
    int32_t sunrise_local, sunset_local;
    sun_local_from_utc(&sunrise_local, &sunset_local);
    if (sunrise_local != s_last_logged_sunrise_min ||
        sunset_local  != s_last_logged_sunset_min) {
        log_sun_event(sunrise_local, sunset_local);
        s_last_logged_sunrise_min = sunrise_local;
        s_last_logged_sunset_min  = sunset_local;
    }
}

/* ============================================================
 * Internal helper — read DS1307, seed system clock, update MX4
 * ============================================================ */

/**
 * @brief Read the DS1307 RTC, seed the ESP-IDF system clock, refresh MX4.
 *
 * Sequence:
 *   1. Acquire MX1 (200 ms timeout) → rtc_get_time() → release.
 *   2. Convert UTC datetime to Unix seconds via rtc_dt_to_unix().
 *   3. settimeofday() so time(NULL) returns valid UTC before NTP completes.
 *   4. Acquire MX4 → write current_unix_ts + recompute sun times → release.
 *
 * Mutex timeouts log a warning and skip the operation; no fatal errors.
 * Called once at T4 boot and again every RTC_POLL_TICKS main-loop ticks
 * (~60 s) to compensate for clock drift between NTP corrections.
 *
 * @note Has no effect on the DS1307 itself — read-only.
 */
static void read_rtc_and_seed_clock(void)
{
    rtc_datetime_t dt;
    rtc_status_t   st;

    /* Acquire I2C bus (MX1). */
    if (xSemaphoreTake(MX1, pdMS_TO_TICKS(200u)) != pdTRUE) {
        ESP_LOGW(TAG, "MX1 timeout — RTC read skipped");
        return;
    }
    st = rtc_get_time(&dt);
    xSemaphoreGive(MX1);

    if (st != RTC_OK) {
        ESP_LOGW(TAG, "RTC read failed (st=%d)", (int)st);
        return;
    }

    uint32_t unix_ts = rtc_dt_to_unix(&dt);

    /* Seed the ESP-IDF system clock so time(NULL) is valid before NTP. */
    struct timeval tv = { .tv_sec = (time_t)unix_ts, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Update the MX4 shadow. */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) == pdTRUE) {
        s_cfg.current_unix_ts = unix_ts;
        update_sun_times();
        xSemaphoreGive(MX4);
    } else {
        ESP_LOGW(TAG, "MX4 timeout — RTC timestamp not written to shadow");
    }

    ESP_LOGI(TAG, "RTC: %04u-%02u-%02u %02u:%02u:%02u UTC  unix=%lu  daytime=%s",
             (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
             (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second,
             (unsigned long)unix_ts,
             s_cfg.is_daytime ? "yes" : "no");
}

/* ============================================================
 * Internal helper — load NVS namespaces into s_cfg at boot
 *
 * Called before the scheduler is contested; no mutex needed.
 * Each missing key is silently replaced with its factory default
 * (defined in cfg_defaults.h) so a fresh device boots without errors.
 * ============================================================ */

/** @brief Load NVS_NS_CLIMATE keys into the s_cfg climate fields. */
static void nvs_load_climate(void)
{
    int32_t v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_T_MIN_DAY,   DEF_T_MIN_DAY,   &v); s_cfg.t_min_day  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_T_MAX_DAY,   DEF_T_MAX_DAY,   &v); s_cfg.t_max_day  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_T_MIN_NGT,   DEF_T_MIN_NGT,   &v); s_cfg.t_min_ngt  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_T_MAX_NGT,   DEF_T_MAX_NGT,   &v); s_cfg.t_max_ngt  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_RH_MIN_DAY,  DEF_RH_MIN_DAY,  &v); s_cfg.rh_min_day = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_RH_MAX_DAY,  DEF_RH_MAX_DAY,  &v); s_cfg.rh_max_day = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_RH_MIN_NGT,  DEF_RH_MIN_NGT,  &v); s_cfg.rh_min_ngt = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_RH_MAX_NGT,  DEF_RH_MAX_NGT,  &v); s_cfg.rh_max_ngt = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_HYST_T,      DEF_HYST_T,      &v); s_cfg.hyst_t     = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_HYST_RH,     DEF_HYST_RH,     &v); s_cfg.hyst_rh    = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_RH_CTRL_EN,  DEF_RH_CTRL_EN,  &v); s_cfg.rh_ctrl_en  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_CR_PRIORITY, DEF_CR_PRIORITY, &v); s_cfg.cr_priority = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_AVG_WIN_T,   DEF_AVG_WIN_T,   &v); s_cfg.avg_win_t   = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, K_AVG_WIN_RH,  DEF_AVG_WIN_RH,  &v); s_cfg.avg_win_rh  = (int16_t)v;
}

/** @brief Load NVS_NS_WIND keys into the s_cfg wind fields. */
static void nvs_load_wind(void)
{
    int32_t v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_V_MAX,         DEF_V_MAX,         &v); s_cfg.v_max         = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_DIR_EXCL_LOW,  DEF_DIR_EXCL_LOW,  &v); s_cfg.dir_excl_low  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_DIR_EXCL_HIGH, DEF_DIR_EXCL_HIGH, &v); s_cfg.dir_excl_high = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_WIND_PROT_EN,  DEF_WIND_PROT_EN,  &v); s_cfg.wind_prot_en  = (int16_t)v;
}

/**
 * @brief Load NVS_NS_MOTOR keys for all 3 motor channels into s_cfg.
 *
 * Iterates over M1/M2/M3 reading `travel_mX`, `dwell_open_mX`, and
 * `dwell_close_mX` from NVS. Per-channel keys are stored as separate
 * NVS entries (not arrays) because the NVS API doesn't natively
 * support int16 arrays; the parallel-arrays pattern keeps the code
 * compact without sacrificing per-channel granularity.
 */
static void nvs_load_motor(void)
{
    int32_t v;

    /* Parallel arrays for motor 1/2/3 key names and factory defaults. */
    static const char * const ktr[]  = { K_TRAVEL_M1,      K_TRAVEL_M2,      K_TRAVEL_M3      };
    static const char * const kdo[]  = { K_DWELL_OPEN_M1,  K_DWELL_OPEN_M2,  K_DWELL_OPEN_M3  };
    static const char * const kdc[]  = { K_DWELL_CLOSE_M1, K_DWELL_CLOSE_M2, K_DWELL_CLOSE_M3 };
    static const int32_t def_tr[]    = {
        MOTOR_M1_TRAVEL_S_DEFAULT,
        MOTOR_M2_TRAVEL_S_DEFAULT,
        MOTOR_M3_TRAVEL_S_DEFAULT
    };
    static const int32_t def_do[3] = {
        DEF_DWELL_OPEN_M1_S,
        DEF_DWELL_OPEN_M2_S,
        DEF_DWELL_OPEN_M3_S
    };
    static const int32_t def_dc[3] = {
        DEF_DWELL_CLOSE_M1_S,
        DEF_DWELL_CLOSE_M2_S,
        DEF_DWELL_CLOSE_M3_S
    };

    for (uint8_t i = 0u; i < 3u; i++) {
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, ktr[i], def_tr[i], &v); s_cfg.travel_s[i]        = (int16_t)v;
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, kdo[i], def_do[i], &v); s_cfg.dwell_open_min[i]  = (int16_t)v;
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, kdc[i], def_dc[i], &v); s_cfg.dwell_close_min[i] = (int16_t)v;
    }
}

/** @brief Load NVS_NS_SYSTEM core keys (poll interval, location, TZ, LED) into s_cfg. */
static void nvs_load_system(void)
{
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_POLL_INTERVAL,   DEF_POLL_INTERVAL_S,     &s_cfg.poll_interval_s);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_SESSION_TIMEOUT, DEF_SESSION_TIMEOUT_MIN, &s_cfg.session_timeout_min);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_AP_TIMEOUT,      DEF_AP_TIMEOUT_MIN,      &s_cfg.ap_timeout_min);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LAT_DEG,         DEF_LAT_DEG,             &s_cfg.lat_deg);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LAT_FRAC,        DEF_LAT_FRAC,            &s_cfg.lat_frac);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LON_DEG,         DEF_LON_DEG,             &s_cfg.lon_deg);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LON_FRAC,        DEF_LON_FRAC,            &s_cfg.lon_frac);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LED_DAY_BRT,     DEF_LED_DAY_BRT,         &s_cfg.led_day_brt);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LED_NITE_BRT,    DEF_LED_NITE_BRT,        &s_cfg.led_nite_brt);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LED_NITE_FROM,   DEF_LED_NITE_FROM,       &s_cfg.led_nite_from);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LED_NITE_TO,     DEF_LED_NITE_TO,         &s_cfg.led_nite_to);
    nvs_cfg_get_str_or_default(NVS_NS_SYSTEM, K_TZ_STR,          DEF_TZ_STR,
                                s_cfg.tz_str, sizeof(s_cfg.tz_str));
}

/**
 * @brief rc.1.5.0 (gh#28) — load persisted STANDBY flag and seed EG1.
 *
 * Reads `system/mode_standby` (default 0). If non-zero, sets
 * EG1_BIT_STANDBY so T6 starts gated on its very first tick — the unit
 * comes back up in STANDBY exactly as it was when power was lost.
 *
 * Idempotent: safe to call at any point during boot; nothing else reads
 * EG1_BIT_STANDBY until T6 enters its main loop, which happens after T4
 * has run this helper.
 */
static void nvs_load_mode(void)
{
    int32_t v = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_MODE_STANDBY, 0, &v);
    if (v != 0 && EG1 != NULL) {
        xEventGroupSetBits(EG1, EG1_BIT_STANDBY);
        ESP_LOGI(TAG, "[T4] STANDBY restored from NVS (gh#28)");
    }
}

/**
 * @brief Load NVS_NS_SYSTEM web-tab/status-website keys into s_cfg.
 *
 * Separated from nvs_load_system() because dm_reload_web_cfg() needs to
 * refresh just this subset after the /api/web POST handler writes new
 * values directly to NVS.
 *
 * @see dm_reload_web_cfg()
 */
static void nvs_load_web(void)
{
    nvs_cfg_get_str_or_default(NVS_NS_SYSTEM, K_STATUS_URL,    DEF_STATUS_URL,
                                s_cfg.status_url,    sizeof(s_cfg.status_url));
    nvs_cfg_get_str_or_default(NVS_NS_SYSTEM, K_STATUS_SECRET, DEF_STATUS_SECRET,
                                s_cfg.status_secret, sizeof(s_cfg.status_secret));
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_STATUS_INTERVAL, DEF_STATUS_INTERVAL_S, &s_cfg.status_interval_s);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_STATUS_ENABLE,   DEF_STATUS_ENABLE,     &s_cfg.status_enable);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_STATUS_EXPOSE,   DEF_STATUS_EXPOSE,     &s_cfg.status_expose);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LOG_UPLOAD_H,    DEF_LOG_UPLOAD_H,      &s_cfg.log_upload_h);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LOG_UPLOAD_M,    DEF_LOG_UPLOAD_M,      &s_cfg.log_upload_m);
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_LOG_UPLOAD_ROT,  DEF_LOG_UPLOAD_ROT,    &s_cfg.log_upload_rot);
    nvs_cfg_get_str_or_default(NVS_NS_SYSTEM, K_LOG_LAST_UP,    DEF_LOG_LAST_UP,
                                s_cfg.log_last_up, sizeof(s_cfg.log_last_up));
}

/* ============================================================
 * cfg_clamp — enforce per-key validation bounds on Q4 values.
 * ============================================================ */

/**
 * @brief Clamp a Q4 config value to the per-key bounds in cfg_limits.h.
 *
 * Each known namespace/key pair has a documented [min, max] from
 * cfg_limits.h. Values outside the range are pulled to the nearest
 * endpoint and a warning row is logged so the operator can see the
 * correction. Unknown keys are passed through unchanged — the caller
 * later detects them as "not in shadow" and only the NVS write proceeds.
 *
 * @param ns    NVS namespace string (e.g. NVS_NS_CLIMATE).
 * @param key   NVS key string (e.g. "t_max_day").
 * @param v     Raw value from Q4.
 * @return      Clamped value (== v if already in range, or for unknown keys).
 * @note Clamping is permissive — a typo on `ns`/`key` returns the value
 *       unchanged and lets the downstream NVS write proceed.
 */
static int32_t cfg_clamp(const char *ns, const char *key, int32_t v)
{
#define _CLAMP(lo, hi)                                                       \
    do {                                                                     \
        int32_t _lo = (lo), _hi = (hi);                                      \
        if      (v < _lo) { ESP_LOGW(TAG, "cfg clamp %s/%s: %ld → %ld (min)", \
                                     ns, key, (long)v, (long)_lo); v = _lo; } \
        else if (v > _hi) { ESP_LOGW(TAG, "cfg clamp %s/%s: %ld → %ld (max)", \
                                     ns, key, (long)v, (long)_hi); v = _hi; } \
    } while (0)

    if (strcmp(ns, NVS_NS_CLIMATE) == 0) {
        if      (strcmp(key, K_T_MAX_DAY)  == 0) _CLAMP(CFG_MIN_T_MAX_DAY,  CFG_MAX_T_MAX_DAY);
        else if (strcmp(key, K_T_MIN_DAY)  == 0) _CLAMP(CFG_MIN_T_MIN_DAY,  CFG_MAX_T_MIN_DAY);
        else if (strcmp(key, K_T_MAX_NGT)  == 0) _CLAMP(CFG_MIN_T_MAX_NGT,  CFG_MAX_T_MAX_NGT);
        else if (strcmp(key, K_T_MIN_NGT)  == 0) _CLAMP(CFG_MIN_T_MIN_NGT,  CFG_MAX_T_MIN_NGT);
        else if (strcmp(key, K_RH_MAX_DAY) == 0) _CLAMP(CFG_MIN_RH_MAX,     CFG_MAX_RH_MAX);
        else if (strcmp(key, K_RH_MIN_DAY) == 0) _CLAMP(CFG_MIN_RH_MIN,     CFG_MAX_RH_MIN);
        else if (strcmp(key, K_RH_MAX_NGT) == 0) _CLAMP(CFG_MIN_RH_MAX,     CFG_MAX_RH_MAX);
        else if (strcmp(key, K_RH_MIN_NGT) == 0) _CLAMP(CFG_MIN_RH_MIN,     CFG_MAX_RH_MIN);
        else if (strcmp(key, K_HYST_T)     == 0) _CLAMP(CFG_MIN_HYST_T,     CFG_MAX_HYST_T);
        else if (strcmp(key, K_HYST_RH)    == 0) _CLAMP(CFG_MIN_HYST_RH,    CFG_MAX_HYST_RH);
        else if (strcmp(key, K_AVG_WIN_T)  == 0) _CLAMP(CFG_MIN_AVG_WIN,    CFG_MAX_AVG_WIN);
        else if (strcmp(key, K_AVG_WIN_RH) == 0) _CLAMP(CFG_MIN_AVG_WIN,    CFG_MAX_AVG_WIN);

    } else if (strcmp(ns, NVS_NS_WIND) == 0) {
        if      (strcmp(key, K_V_MAX)         == 0) _CLAMP(CFG_MIN_V_MAX, CFG_MAX_V_MAX);
        else if (strcmp(key, K_DIR_EXCL_LOW)  == 0) _CLAMP(CFG_MIN_DIR,   CFG_MAX_DIR);
        else if (strcmp(key, K_DIR_EXCL_HIGH) == 0) _CLAMP(CFG_MIN_DIR,   CFG_MAX_DIR);

    } else if (strcmp(ns, NVS_NS_MOTOR) == 0) {
        static const char * const ktr[] = { K_TRAVEL_M1,      K_TRAVEL_M2,      K_TRAVEL_M3      };
        static const char * const kdo[] = { K_DWELL_OPEN_M1,  K_DWELL_OPEN_M2,  K_DWELL_OPEN_M3  };
        static const char * const kdc[] = { K_DWELL_CLOSE_M1, K_DWELL_CLOSE_M2, K_DWELL_CLOSE_M3 };
        for (uint8_t i = 0u; i < 3u; i++) {
            if (strcmp(key, ktr[i]) == 0) { _CLAMP(CFG_MIN_TRAVEL_S,      CFG_MAX_TRAVEL_S);      break; }
            if (strcmp(key, kdo[i]) == 0) { _CLAMP(CFG_MIN_DWELL_OPEN_S,  CFG_MAX_DWELL_OPEN_S);  break; }
            if (strcmp(key, kdc[i]) == 0) { _CLAMP(CFG_MIN_DWELL_CLOSE_S, CFG_MAX_DWELL_CLOSE_S); break; }
        }

    } else if (strcmp(ns, NVS_NS_SYSTEM) == 0) {
        if      (strcmp(key, K_POLL_INTERVAL)   == 0) _CLAMP(CFG_MIN_POLL_S,            CFG_MAX_POLL_S);
        else if (strcmp(key, K_SESSION_TIMEOUT) == 0) _CLAMP(CFG_MIN_TIMEOUT_MIN,       CFG_MAX_TIMEOUT_MIN);
        else if (strcmp(key, K_AP_TIMEOUT)      == 0) _CLAMP(CFG_MIN_AP_TIMEOUT,        CFG_MAX_TIMEOUT_MIN);
        else if (strcmp(key, K_STATUS_INTERVAL) == 0) _CLAMP(CFG_MIN_STATUS_INTERVAL_S, CFG_MAX_STATUS_INTERVAL_S);
        else if (strcmp(key, K_STATUS_ENABLE)   == 0) _CLAMP(0, 1);
        else if (strcmp(key, K_STATUS_EXPOSE)   == 0) _CLAMP(0, 0x3F);
        else if (strcmp(key, K_LOG_UPLOAD_H)    == 0) _CLAMP(CFG_MIN_HOUR,              CFG_MAX_HOUR);
        else if (strcmp(key, K_LOG_UPLOAD_M)    == 0) _CLAMP(CFG_MIN_MINUTE,            CFG_MAX_MINUTE);
        else if (strcmp(key, K_LOG_UPLOAD_ROT)  == 0) _CLAMP(0, 1);
    }

#undef _CLAMP
    return v;
}

/* ============================================================
 * ns/key → log_param_id_t mapping (a.6.35.5 audit logging).
 * ============================================================ */

/**
 * @brief Map an NVS namespace/key pair to its LOG_PARAM_* id for audit rows.
 *
 * Used by apply_config_update() to fill log_event_t::param_id when a Q4
 * config change is committed. For motor dwell keys also writes the 1-based
 * channel index (1/2/3) to *out_channel so the audit row carries which
 * window the change affected.
 *
 * @param ns           NVS namespace string.
 * @param key          NVS key string.
 * @param out_channel  If non-NULL, set to 1/2/3 for motor keys, 0 otherwise.
 * @return The matching LOG_PARAM_* id, or LOG_PARAM_NONE for keys that are
 *         deliberately not enumerated (e.g. `travel_mX`, session/AP timeouts).
 *         LOG_PARAM_NONE means "change still applied, no audit row".
 */
static log_param_id_t ns_key_to_log_id(const char *ns, const char *key,
                                       uint8_t *out_channel)
{
    if (out_channel) { *out_channel = 0u; }

    if (strcmp(ns, NVS_NS_CLIMATE) == 0) {
        if (strcmp(key, K_T_MIN_DAY)   == 0) return LOG_PARAM_T_MIN_DAY;
        if (strcmp(key, K_T_MAX_DAY)   == 0) return LOG_PARAM_T_MAX_DAY;
        if (strcmp(key, K_T_MIN_NGT)   == 0) return LOG_PARAM_T_MIN_NGT;
        if (strcmp(key, K_T_MAX_NGT)   == 0) return LOG_PARAM_T_MAX_NGT;
        if (strcmp(key, K_RH_MIN_DAY)  == 0) return LOG_PARAM_RH_MIN_DAY;
        if (strcmp(key, K_RH_MAX_DAY)  == 0) return LOG_PARAM_RH_MAX_DAY;
        if (strcmp(key, K_RH_MIN_NGT)  == 0) return LOG_PARAM_RH_MIN_NGT;
        if (strcmp(key, K_RH_MAX_NGT)  == 0) return LOG_PARAM_RH_MAX_NGT;
        if (strcmp(key, K_HYST_T)      == 0) return LOG_PARAM_HYST_T;
        if (strcmp(key, K_HYST_RH)     == 0) return LOG_PARAM_HYST_RH;
        if (strcmp(key, K_RH_CTRL_EN)  == 0) return LOG_PARAM_RH_CTRL_EN;
        if (strcmp(key, K_CR_PRIORITY) == 0) return LOG_PARAM_CR_PRIORITY;
        if (strcmp(key, K_AVG_WIN_T)   == 0) return LOG_PARAM_AVG_WIN_T;
        if (strcmp(key, K_AVG_WIN_RH)  == 0) return LOG_PARAM_AVG_WIN_RH;
        return LOG_PARAM_NONE;
    }
    if (strcmp(ns, NVS_NS_WIND) == 0) {
        if (strcmp(key, K_V_MAX)         == 0) return LOG_PARAM_V_MAX;
        if (strcmp(key, K_DIR_EXCL_LOW)  == 0) return LOG_PARAM_DIR_EXCL_LOW;
        if (strcmp(key, K_DIR_EXCL_HIGH) == 0) return LOG_PARAM_DIR_EXCL_HI;
        if (strcmp(key, K_WIND_PROT_EN)  == 0) return LOG_PARAM_WIND_PROT_EN;
        return LOG_PARAM_NONE;
    }
    if (strcmp(ns, NVS_NS_MOTOR) == 0) {
        static const char * const kdo[] = { K_DWELL_OPEN_M1,  K_DWELL_OPEN_M2,  K_DWELL_OPEN_M3  };
        static const char * const kdc[] = { K_DWELL_CLOSE_M1, K_DWELL_CLOSE_M2, K_DWELL_CLOSE_M3 };
        for (uint8_t i = 0u; i < 3u; i++) {
            if (strcmp(key, kdo[i]) == 0) {
                if (out_channel) { *out_channel = (uint8_t)(i + 1u); }
                return LOG_PARAM_DWELL_OPEN;
            }
            if (strcmp(key, kdc[i]) == 0) {
                if (out_channel) { *out_channel = (uint8_t)(i + 1u); }
                return LOG_PARAM_DWELL_CLOSE;
            }
        }
        /* travel_m{1,2,3} not enumerated in log_param_id_t — falls to NONE.
         * Motor travel time is set during commissioning and rarely changes;
         * not surfacing it via SETPT is a deliberate choice from the C1..C22
         * table in logAnalysis.md. */
        return LOG_PARAM_NONE;
    }
    if (strcmp(ns, NVS_NS_SYSTEM) == 0) {
        if (strcmp(key, K_POLL_INTERVAL)  == 0) return LOG_PARAM_POLL_INTV;
        if (strcmp(key, K_LAT_DEG)        == 0 ||
            strcmp(key, K_LAT_FRAC)       == 0 ||
            strcmp(key, K_LON_DEG)        == 0 ||
            strcmp(key, K_LON_FRAC)       == 0) return LOG_PARAM_LAT_LON;
        if (strcmp(key, K_STATUS_INTERVAL) == 0) return LOG_PARAM_STATUS_INTV;
        if (strcmp(key, K_STATUS_ENABLE)   == 0) return LOG_PARAM_STATUS_ENABLE;
        if (strcmp(key, K_STATUS_EXPOSE)   == 0) return LOG_PARAM_STATUS_EXPOSE;
        if (strcmp(key, K_LOG_UPLOAD_H)    == 0) return LOG_PARAM_LOG_UPLOAD_H;
        if (strcmp(key, K_LOG_UPLOAD_M)    == 0) return LOG_PARAM_LOG_UPLOAD_M;
        if (strcmp(key, K_LOG_UPLOAD_ROT)  == 0) return LOG_PARAM_LOG_UPLOAD_ROT;
        /* session_timeout_min / ap_timeout_min / led_* not enumerated. */
        return LOG_PARAM_NONE;
    }
    return LOG_PARAM_NONE;
}

/* ============================================================
 * Internal helper — apply a Q4 config_update_t
 * ============================================================ */

/**
 * @brief Validate, persist, and apply a single Q4 config change.
 *
 * Pipeline:
 *   1. cfg_clamp() → in-range value.
 *   2. nvs_cfg_set_i32() → persistent storage.
 *   3. MX4 critical section: capture old value, write the in-RAM shadow,
 *      call update_sun_times() if location changed.
 *   4. Outside MX4: log INFO line + LOG_SETPOINT audit row to Q3 (skipped
 *      for keys whose ns_key_to_log_id() returns LOG_PARAM_NONE).
 *
 * Since a.6.35.5: emits a LOG_SETPOINT audit row to Q3 after every
 * successful shadow update. The Q4 message carries the initiator
 * (LOG_BY_FARMER / LOG_BY_ADMIN from the LCD-UI session, LOG_BY_WEB
 * from the web server, or 0 / LOG_BY_SYSTEM from legacy callers that
 * didn't set it). Old → new (clamped) values are captured under the
 * same MX4 critical section that writes the shadow.
 *
 * @param upd  Validated Q4 message (caller is T4's main loop after
 *             xQueueReceive). Must be non-NULL.
 * @return     true if the NVS write succeeded; the shadow may still be
 *             stale if MX4 timed out (logged but not returned).
 * @note   config_update_t carries only int32_t values; string-type keys
 *         (tz_str) are not handled through Q4 — the web server writes
 *         them to NVS directly and emits its own audit row (T11-side,
 *         with initiator=LOG_BY_WEB).
 * @warning NVS write happens BEFORE the MX4 update. On MX4 timeout the
 *          persistent state is correct but the shadow is briefly stale.
 */
static bool apply_config_update(const config_update_t *upd)
{
    /* Clamp to valid range before touching NVS or the shadow struct.
     * cfg_clamp() logs a warning for every out-of-range value. */
    const int32_t clamped = cfg_clamp(upd->ns, upd->key, upd->value);

    /* Write to NVS first; abort if NVS write fails. */
    nvs_cfg_status_t ns = nvs_cfg_set_i32(upd->ns, upd->key, clamped);
    if (ns != NVS_CFG_OK) {
        ESP_LOGW(TAG, "Q4 NVS write failed  ns=%.15s key=%.15s val=%ld  err=%d",
                 upd->ns, upd->key, (long)upd->value, (int)ns);
        return false;
    }

    /* Update the in-RAM shadow under MX4. Also reads the *old* value of the
     * same key in the same critical section so the audit row below carries
     * an accurate old→new pair regardless of any racing reader. */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) != pdTRUE) {
        ESP_LOGW(TAG, "MX4 timeout in apply_config_update — NVS written but shadow stale");
        return false;
    }

    bool    updated = true;
    int32_t old_val = 0;            /* captured before the shadow write */
    int16_t v16     = (int16_t)clamped;
    int32_t v32     = clamped;
    const char *ns_str  = upd->ns;
    const char *key_str = upd->key;

    if (strcmp(ns_str, NVS_NS_CLIMATE) == 0) {
        if      (strcmp(key_str, K_T_MIN_DAY)   == 0) { old_val = s_cfg.t_min_day;   s_cfg.t_min_day   = v16; }
        else if (strcmp(key_str, K_T_MAX_DAY)   == 0) { old_val = s_cfg.t_max_day;   s_cfg.t_max_day   = v16; }
        else if (strcmp(key_str, K_T_MIN_NGT)   == 0) { old_val = s_cfg.t_min_ngt;   s_cfg.t_min_ngt   = v16; }
        else if (strcmp(key_str, K_T_MAX_NGT)   == 0) { old_val = s_cfg.t_max_ngt;   s_cfg.t_max_ngt   = v16; }
        else if (strcmp(key_str, K_RH_MIN_DAY)  == 0) { old_val = s_cfg.rh_min_day;  s_cfg.rh_min_day  = v16; }
        else if (strcmp(key_str, K_RH_MAX_DAY)  == 0) { old_val = s_cfg.rh_max_day;  s_cfg.rh_max_day  = v16; }
        else if (strcmp(key_str, K_RH_MIN_NGT)  == 0) { old_val = s_cfg.rh_min_ngt;  s_cfg.rh_min_ngt  = v16; }
        else if (strcmp(key_str, K_RH_MAX_NGT)  == 0) { old_val = s_cfg.rh_max_ngt;  s_cfg.rh_max_ngt  = v16; }
        else if (strcmp(key_str, K_HYST_T)      == 0) { old_val = s_cfg.hyst_t;      s_cfg.hyst_t      = v16; }
        else if (strcmp(key_str, K_HYST_RH)     == 0) { old_val = s_cfg.hyst_rh;     s_cfg.hyst_rh     = v16; }
        else if (strcmp(key_str, K_RH_CTRL_EN)  == 0) { old_val = s_cfg.rh_ctrl_en;  s_cfg.rh_ctrl_en  = v16; }
        else if (strcmp(key_str, K_CR_PRIORITY) == 0) { old_val = s_cfg.cr_priority; s_cfg.cr_priority = v16; }
        else if (strcmp(key_str, K_AVG_WIN_T)   == 0) { old_val = s_cfg.avg_win_t;   s_cfg.avg_win_t   = v16; }
        else if (strcmp(key_str, K_AVG_WIN_RH)  == 0) { old_val = s_cfg.avg_win_rh;  s_cfg.avg_win_rh  = v16; }
        else { updated = false; }

    } else if (strcmp(ns_str, NVS_NS_WIND) == 0) {
        if      (strcmp(key_str, K_V_MAX)         == 0) { old_val = s_cfg.v_max;         s_cfg.v_max         = v16; }
        else if (strcmp(key_str, K_DIR_EXCL_LOW)  == 0) { old_val = s_cfg.dir_excl_low;  s_cfg.dir_excl_low  = v16; }
        else if (strcmp(key_str, K_DIR_EXCL_HIGH) == 0) { old_val = s_cfg.dir_excl_high; s_cfg.dir_excl_high = v16; }
        else if (strcmp(key_str, K_WIND_PROT_EN)  == 0) { old_val = s_cfg.wind_prot_en;  s_cfg.wind_prot_en  = v16; }
        else { updated = false; }

    } else if (strcmp(ns_str, NVS_NS_MOTOR) == 0) {
        static const char * const ktr[] = { K_TRAVEL_M1,      K_TRAVEL_M2,      K_TRAVEL_M3      };
        static const char * const kdo[] = { K_DWELL_OPEN_M1,  K_DWELL_OPEN_M2,  K_DWELL_OPEN_M3  };
        static const char * const kdc[] = { K_DWELL_CLOSE_M1, K_DWELL_CLOSE_M2, K_DWELL_CLOSE_M3 };
        updated = false;
        for (uint8_t i = 0u; i < 3u; i++) {
            if (strcmp(key_str, ktr[i]) == 0) { old_val = s_cfg.travel_s[i];        s_cfg.travel_s[i]        = v16; updated = true; break; }
            if (strcmp(key_str, kdo[i]) == 0) { old_val = s_cfg.dwell_open_min[i];  s_cfg.dwell_open_min[i]  = v16; updated = true; break; }
            if (strcmp(key_str, kdc[i]) == 0) { old_val = s_cfg.dwell_close_min[i]; s_cfg.dwell_close_min[i] = v16; updated = true; break; }
        }

    } else if (strcmp(ns_str, NVS_NS_SYSTEM) == 0) {
        if      (strcmp(key_str, K_POLL_INTERVAL)   == 0) { old_val = s_cfg.poll_interval_s;     s_cfg.poll_interval_s     = v32; }
        else if (strcmp(key_str, K_SESSION_TIMEOUT) == 0) { old_val = s_cfg.session_timeout_min; s_cfg.session_timeout_min = v32; }
        else if (strcmp(key_str, K_AP_TIMEOUT)      == 0) { old_val = s_cfg.ap_timeout_min;      s_cfg.ap_timeout_min      = v32; }
        else if (strcmp(key_str, K_LAT_DEG)         == 0) { old_val = s_cfg.lat_deg;  s_cfg.lat_deg  = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LAT_FRAC)        == 0) { old_val = s_cfg.lat_frac; s_cfg.lat_frac = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LON_DEG)         == 0) { old_val = s_cfg.lon_deg;  s_cfg.lon_deg  = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LON_FRAC)        == 0) { old_val = s_cfg.lon_frac; s_cfg.lon_frac = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LED_DAY_BRT)     == 0) { old_val = s_cfg.led_day_brt;   s_cfg.led_day_brt   = v32; }
        else if (strcmp(key_str, K_LED_NITE_BRT)    == 0) { old_val = s_cfg.led_nite_brt;  s_cfg.led_nite_brt  = v32; }
        else if (strcmp(key_str, K_LED_NITE_FROM)   == 0) { old_val = s_cfg.led_nite_from; s_cfg.led_nite_from = v32; }
        else if (strcmp(key_str, K_LED_NITE_TO)     == 0) { old_val = s_cfg.led_nite_to;   s_cfg.led_nite_to   = v32; }
        else if (strcmp(key_str, K_STATUS_INTERVAL) == 0) { old_val = s_cfg.status_interval_s; s_cfg.status_interval_s = v32; }
        else if (strcmp(key_str, K_STATUS_ENABLE)   == 0) { old_val = s_cfg.status_enable;     s_cfg.status_enable     = v32; }
        else if (strcmp(key_str, K_STATUS_EXPOSE)   == 0) { old_val = s_cfg.status_expose;     s_cfg.status_expose     = v32; }
        else if (strcmp(key_str, K_LOG_UPLOAD_H)    == 0) { old_val = s_cfg.log_upload_h;      s_cfg.log_upload_h      = v32; }
        else if (strcmp(key_str, K_LOG_UPLOAD_M)    == 0) { old_val = s_cfg.log_upload_m;      s_cfg.log_upload_m      = v32; }
        else if (strcmp(key_str, K_LOG_UPLOAD_ROT)  == 0) { old_val = s_cfg.log_upload_rot;    s_cfg.log_upload_rot    = v32; }
        else { updated = false; }
    } else {
        updated = false;
    }

    xSemaphoreGive(MX4);

    if (updated) {
        if (clamped != upd->value) {
            ESP_LOGI(TAG, "Q4 applied: %.15s/%.15s = %ld (clamped from %ld)",
                     ns_str, key_str, (long)clamped, (long)upd->value);
        } else {
            ESP_LOGI(TAG, "Q4 applied: %.15s/%.15s = %ld", ns_str, key_str, (long)clamped);
        }

        /* a.6.35.5 — emit the audit row. Only when ns/key maps to a
         * documented log_param_id_t; admin-internal keys (session_timeout,
         * ap_timeout, led_*) update silently. */
        uint8_t channel = 0u;
        log_param_id_t pid = ns_key_to_log_id(ns_str, key_str, &channel);
        if (pid != LOG_PARAM_NONE) {
            log_event_t ev = {};
            ev.timestamp  = (uint32_t)time(NULL);
            ev.event_type = (uint8_t)LOG_SETPOINT;
            /* Default to LOG_BY_SYSTEM when an older caller forgot to set
             * the initiator — surfaces missing attribution rather than
             * silently mis-attributing to LOG_BY_FARMER (which is the zero
             * value in some older log_initiator_t orderings). */
            ev.initiator  = (upd->initiator != 0u) ? upd->initiator
                                                   : (uint8_t)LOG_BY_SYSTEM;
            ev.channel    = channel;
            ev.param_id   = (uint8_t)pid;
            /* Clamp old_val and clamped to int16 range for the log payload.
             * Values that don't fit (e.g. led_day_brt up to 255) still log
             * within int16 — the parser knows the unit per param_id. */
            ev.value_a    = (int16_t)old_val;
            ev.value_b    = (int16_t)clamped;
            log_post(&ev);
        }
    } else {
        ESP_LOGW(TAG, "Q4 key not in shadow: %.15s/%.15s = %ld  (NVS written; shadow unchanged)",
                 ns_str, key_str, (long)clamped);
    }
    return true;
}

/* ============================================================
 * Internal helper — handle a sensor_reading_t from Q6
 * ============================================================ */

/**
 * @brief Process a single Q6 sensor_reading_t: snapshot, ring, log, notify.
 *
 * Four-step fan-out called by the main loop whenever Q6 delivers a fresh
 * reading from T5 (Sensor Poll):
 *  1. Update MX2 (latest measurement) with a 50 ms acquire timeout.
 *  2. Append to MX3 ring buffer (head advance, count saturates at DEPTH).
 *  3. Post a LOG_SENSOR row to Q3 via log_post() (FR-LG09).
 *  4. Notify T3 (TN1 — new wind data) and T6 (TN2 — new sensor data).
 *
 * @param r  Validated sensor reading; not NULL. Caller is the main loop
 *           after a successful xQueueReceive on Q6.
 * @warning Mutex timeouts here do NOT abort the function — each step is
 *          independent, so a brief MX2 stall does not block MX3 or the
 *          downstream task notifications.
 */
static void handle_sensor_reading(const sensor_reading_t *r)
{
    /* 1. Update MX2 (current measurement). */
    if (xSemaphoreTake(MX2, pdMS_TO_TICKS(50u)) == pdTRUE) {
        memcpy(&s_meas, r, sizeof(s_meas));
        s_meas_valid = true;
        xSemaphoreGive(MX2);
    } else {
        ESP_LOGW(TAG, "MX2 timeout — current measurement not updated");
    }

    /* 2. Append to MX3 history ring buffer. */
    if (xSemaphoreTake(MX3, pdMS_TO_TICKS(50u)) == pdTRUE) {
        s_ring.entries[s_ring.head] = *r;
        s_ring.head = (uint16_t)((s_ring.head + 1u) % (uint16_t)DM_RING_DEPTH);
        if (s_ring.count < (uint16_t)DM_RING_DEPTH) {
            s_ring.count++;
        }
        xSemaphoreGive(MX3);
    } else {
        ESP_LOGW(TAG, "MX3 timeout — ring buffer entry dropped");
    }

    /* 3. Post LOG_SENSOR_HR snapshot to Q3 (FR-LG09; rc.1.4.0 — replaces the
     *    legacy LOG_SENSOR single-row format with three channel-discriminated
     *    sub-rows per sample, per model/logUpdatePlan.md §2.
     *
     *    Channel 0 — T + RH at 0.1 °C precision   (raw values, not sliding avg)
     *    Channel 1 — wind speed × 10 + direction  (raw values)
     *    Channel 2 — packed window-state bitmask  (see logUpdatePlan §2.2)
     *
     *    LOG_SENSOR (the legacy single-row format) is sunset — no longer
     *    emitted. Its enum value remains in `log_type_t` so historical SD
     *    files served via /api/log/download keep their stable type-column
     *    string. */
    log_event_t evt = {};
    evt.timestamp  = r->timestamp;
    evt.event_type = (uint8_t)LOG_SENSOR_HR;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.param_id   = (uint8_t)LOG_PARAM_NONE;

    /* Channel 0 — T + RH (raw, 0.1 °C precision in value_a). */
    evt.channel = 0u;
    evt.value_a = r->temperature_c10;
    evt.value_b = (int16_t)r->humidity_pct;
    log_post(&evt);

    /* Channel 1 — wind speed (m/s × 10) + direction (deg). */
    evt.channel = 1u;
    evt.value_a = (int16_t)r->wind_speed_ms10;
    evt.value_b = (int16_t)r->wind_dir_deg;
    log_post(&evt);

    /* Channel 2 — packed window-state bitmask (see logUpdatePlan §2.2). */
    evt.channel = 2u;
    evt.value_a = t2_get_window_bitmask();
    evt.value_b = 0;
    log_post(&evt);

    /* 4. Notify T3 (TN1 — new wind data) and T6 (TN2 — new sensor data). */
    if (task_t3 != NULL) {
        xTaskNotify(task_t3, 1u, eSetBits);
    }
    if (task_t6 != NULL) {
        xTaskNotify(task_t6, 1u, eSetBits);
    }
}

/* ============================================================
 * Internal helper — handle TN4 (NTP sync confirmed by T10)
 * ============================================================ */

/**
 * @brief Handle a TN4 notification: write the freshly-NTP'd time to DS1307.
 *
 * Triggered when T10 (Network Manager) calls xTaskNotify(task_t4,
 * DM_NOTIFY_NTP_SYNCED, eSetBits) after configTime() succeeds. By that
 * point ESP-IDF's POSIX clock holds the correct UTC; this function reads
 * time(NULL), converts to DS1307 datetime fields, and writes to the RTC
 * under MX1 so the device retains correct time across power cycles.
 *
 * Also refreshes the MX4 shadow's current_unix_ts and recomputes sun
 * times because NTP correction is typically a few seconds more accurate
 * than the previously-loaded RTC value.
 *
 * @note   If time(NULL) returns an implausible pre-2000 value the DS1307
 *         write is skipped to avoid corrupting the RTC with epoch zero.
 */
static void handle_ntp_sync(void)
{
    time_t now = time(NULL);
    if (now < 100000L) {
        /* System time not yet valid (epoch close to zero). */
        ESP_LOGW(TAG, "TN4 received but time(NULL)=%ld — system clock not yet set, skipping DS1307 write",
                 (long)now);
        return;
    }

    /* Convert Unix UTC time to DS1307 fields. */
    struct tm t;
    gmtime_r(&now, &t);

    rtc_datetime_t dt;
    dt.year        = (uint16_t)(t.tm_year + 1900);
    dt.month       = (uint8_t)(t.tm_mon  + 1);
    dt.day         = (uint8_t)t.tm_mday;
    dt.hour        = (uint8_t)t.tm_hour;
    dt.minute      = (uint8_t)t.tm_min;
    dt.second      = (uint8_t)t.tm_sec;
    dt.day_of_week = (uint8_t)(t.tm_wday + 1);   /* 1=Sunday */

    /* Write to DS1307 under MX1. */
    rtc_status_t st = RTC_ERR_COMM;
    if (xSemaphoreTake(MX1, pdMS_TO_TICKS(500u)) == pdTRUE) {
        st = rtc_set_time(&dt);
        xSemaphoreGive(MX1);
    } else {
        ESP_LOGW(TAG, "TN4: MX1 timeout — DS1307 write skipped");
        return;
    }

    if (st == RTC_OK) {
        ESP_LOGI(TAG, "TN4: DS1307 updated → %04u-%02u-%02u %02u:%02u:%02u UTC",
                 (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
                 (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    } else {
        ESP_LOGW(TAG, "TN4: DS1307 write failed (st=%d)", (int)st);
    }

    /* Update MX4 timestamps (NTP is more accurate than RTC; use time(NULL) directly). */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) == pdTRUE) {
        s_cfg.current_unix_ts = (uint32_t)now;
        update_sun_times();
        xSemaphoreGive(MX4);
    }
}

/* ============================================================
 * T4 task entry point
 * ============================================================ */

/**
 * @brief T4 task entry point — see data_manager.h for the contract.
 *
 * Implementation outline (boot + main loop):
 *   Boot
 *     1. esp_task_wdt_add() to subscribe to the watchdog.
 *     2. Zero s_cfg/s_meas/s_ring and clear s_meas_valid.
 *     3. nvs_load_climate/wind/motor/system/web() — full NVS pull.
 *     4. setenv("TZ", ...) + tzset().
 *     5. read_rtc_and_seed_clock() — DS1307 → POSIX clock + MX4.
 *     6. Emit boot-reason LOG_SYSTEM row (since 1.17.31; previously
 *        emitted from main.cpp at epoch zero).
 *     7. Emit unit-id LOG_SYSTEM row (gh#17, since 1.18.3).
 *     8. esp_core_dump_image_check() — cache + log if present (a.6.35.6).
 *   Main loop (1 s tick)
 *     - xQueueReceive(Q6, 1000 ms) → handle_sensor_reading().
 *     - drain Q4 → apply_config_update().
 *     - check TN4 → handle_ntp_sync().
 *     - every RTC_POLL_TICKS s → read_rtc_and_seed_clock().
 *     - esp_task_wdt_reset() at the top of every iteration.
 */
void task_data_manager(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to WDT (1.17.29 / gh#13). T4's main loop blocks on Q6 with
     * a 1 s timeout, well under the 5 s WDT window. */
    esp_task_wdt_add(NULL);

    /* ----------------------------------------------------------------
     * Boot phase: initialise all module state.
     * No mutex needed here — other tasks do not yet contest MX2/MX3/MX4.
     * ---------------------------------------------------------------- */
    memset(&s_cfg,  0, sizeof(s_cfg));
    memset(&s_meas, 0, sizeof(s_meas));
    memset(&s_ring, 0, sizeof(s_ring));
    s_meas_valid = false;

    /* Load all NVS namespaces into the config shadow. */
    nvs_load_climate();
    nvs_load_wind();
    nvs_load_motor();
    nvs_load_system();
    nvs_load_web();
    /* rc.1.5.0 (gh#28) — seed EG1_BIT_STANDBY from persisted state so the
     * unit comes back up in STANDBY after a reboot if that's what the
     * operator last chose. Must run after EG1 is created (it is — system
     * globals are constructed before any task starts) and before T6 enters
     * its main loop (it does — T4 runs `nvs_load_*` here in boot phase). */
    nvs_load_mode();

    /* Apply the stored TZ string so local-time functions are correct. */
    setenv("TZ", s_cfg.tz_str, 1);
    tzset();

    ESP_LOGI(TAG, "NVS loaded — poll=%lds  t_max_day=%d°C  v_max=%d m/s  tz=%s",
             (long)s_cfg.poll_interval_s,
             (int)s_cfg.t_max_day,
             (int)s_cfg.v_max,
             s_cfg.tz_str);

    /* Read DS1307; seed system clock so time(NULL) returns valid UTC. */
    read_rtc_and_seed_clock();

    /* Log the boot-reason event. Previously emitted from main.cpp::setup()
     * before any RTC read, which left the SD-log row stamped 1970-01-01
     * (confirmed in the 2026-05-13 capture: four POWERON boots all came
     * out at epoch zero).  Moved here in 1.17.31 so the timestamp reflects
     * the boot's actual wall-clock (per the DS1307; NTP correction follows
     * a few seconds later if WiFi is up).
     *
     * Encoding (see event_logger.h LOG_SYSTEM value_a table):
     *   value_a = 5    BOOT marker
     *   value_b       = esp_reset_reason_t (1=POWERON, 3=SW, 4=PANIC,
     *                   5=INT_WDT, 6=TASK_WDT, 7=WDT, 8=DEEPSLEEP,
     *                   9=BROWNOUT, ...)
     * esp_reset_reason() is cached by ESP-IDF — returns the same value
     * regardless of when in the boot it is called. */
    {
        log_event_t boot_evt = {};
        boot_evt.timestamp  = s_cfg.current_unix_ts;
        boot_evt.event_type = (uint8_t)LOG_SYSTEM;
        boot_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
        boot_evt.channel    = 0u;
        boot_evt.param_id   = (uint8_t)LOG_PARAM_NONE;
        boot_evt.value_a    = (int16_t)5;
        boot_evt.value_b    = (int16_t)esp_reset_reason();
        log_post(&boot_evt);
    }

    /* Unit-id event (gh#17, since 1.18.3) — emitted once per boot, immediately
     * after the boot-reason row so the operator sees both rows in the same
     * second-resolution timestamp on the SD log. value_b carries the 16-bit ID
     * cast through int16_t: top bit of mac[4] aliases as a negative number,
     * which the parser reinterprets via (uint16_t) before rendering %04X. */
    {
        log_event_t id_evt = {};
        id_evt.timestamp  = s_cfg.current_unix_ts;
        id_evt.event_type = (uint8_t)LOG_SYSTEM;
        id_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
        id_evt.channel    = 0u;
        id_evt.param_id   = (uint8_t)LOG_PARAM_NONE;
        id_evt.value_a    = (int16_t)11;
        id_evt.value_b    = (int16_t)system_unit_id_u16();
        log_post(&id_evt);
    }

    /* a.6.35.6 — boot-time coredump check. esp_core_dump_image_check() reads
     * the coredump partition header + checksum and returns ESP_OK if a valid
     * dump from a previous panic is stored. If yes, cache the fact + size,
     * and emit a LOG_SYSTEM row so the operator sees "by the way, a coredump
     * is sitting in flash from last boot" in the SD log without having to
     * notice the badge in the GUI. value_b carries the size in KB (rounded
     * up). The dump stays in flash until the operator downloads + erases
     * via /api/coredump in T11. */
    {
        esp_err_t cd_check = esp_core_dump_image_check();
        if (cd_check == ESP_OK) {
            size_t cd_addr = 0u;
            size_t cd_size = 0u;
            if (esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK && cd_size > 0u) {
                s_coredump_present = true;
                s_coredump_size_b  = cd_size;
                ESP_LOGW(TAG, "Coredump from previous boot detected: %u bytes at flash 0x%06x",
                         (unsigned)cd_size, (unsigned)cd_addr);

                log_event_t cd_evt = {};
                cd_evt.timestamp  = s_cfg.current_unix_ts;
                cd_evt.event_type = (uint8_t)LOG_SYSTEM;
                cd_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
                cd_evt.channel    = 0u;
                cd_evt.param_id   = (uint8_t)LOG_PARAM_NONE;
                cd_evt.value_a    = (int16_t)18;   /* coredump available at boot */
                cd_evt.value_b    = (int16_t)((cd_size + 1023u) / 1024u); /* KB rounded up */
                log_post(&cd_evt);
            } else {
                ESP_LOGW(TAG, "esp_core_dump_image_check OK but image_get failed — treating as absent");
            }
        } else {
            ESP_LOGI(TAG, "No coredump present (esp_core_dump_image_check=%d)", (int)cd_check);
        }
    }

    /* ----------------------------------------------------------------
     * Main loop
     * ---------------------------------------------------------------- */
    uint32_t tick_count    = 0u;
    uint32_t last_rtc_tick = 0u;

    for (;;) {
        esp_task_wdt_reset();   /* WDT kick (1.17.29 / gh#13) */
        /* ---- 1. Block up to 1 s on Q6 for a new sensor reading. ---- */
        sensor_reading_t reading;
        if (xQueueReceive(Q6, &reading, pdMS_TO_TICKS(1000u)) == pdTRUE) {
            handle_sensor_reading(&reading);
        }
        tick_count++;

        /* ---- 2. Drain Q4 (config updates). Non-blocking. ---- */
        config_update_t upd;
        while (xQueueReceive(Q4, &upd, 0) == pdTRUE) {
            apply_config_update(&upd);
        }

        /* ---- 3. Check TN4 (NTP sync notification from T10). Non-blocking. ---- */
        uint32_t notif_val = 0u;
        if (xTaskNotifyWait(0u, DM_NOTIFY_NTP_SYNCED, &notif_val, 0u) == pdTRUE) {
            if (notif_val & DM_NOTIFY_NTP_SYNCED) {
                handle_ntp_sync();
                last_rtc_tick = tick_count;  /* fresh time — skip next periodic re-read */
            }
        }

        /* ---- 4. Periodic RTC re-read (~every 60 s). ---- */
        if ((tick_count - last_rtc_tick) >= RTC_POLL_TICKS) {
            read_rtc_and_seed_clock();
            last_rtc_tick = tick_count;
        }
    }
}

/* ============================================================
 * Public getter implementations
 * ============================================================ */

/** @brief Thread-safe full-cfg copy under MX4 (see data_manager.h). */
void dm_cfg_snapshot(cfg_shadow_t *out)
{
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) == pdTRUE) {
        memcpy(out, &s_cfg, sizeof(s_cfg));
        xSemaphoreGive(MX4);
    } else {
        /* Fallback: lock-free copy — may be transiently inconsistent. */
        ESP_LOGW(TAG, "dm_cfg_snapshot: MX4 timeout — lock-free fallback");
        memcpy(out, &s_cfg, sizeof(s_cfg));
    }
}

/** @brief Thread-safe latest-measurement copy under MX2 (see data_manager.h). */
void dm_meas_snapshot(sensor_reading_t *out, bool *valid_out)
{
    if (xSemaphoreTake(MX2, pdMS_TO_TICKS(100u)) == pdTRUE) {
        memcpy(out, &s_meas, sizeof(s_meas));
        if (valid_out != NULL) { *valid_out = s_meas_valid; }
        xSemaphoreGive(MX2);
    } else {
        memset(out, 0, sizeof(sensor_reading_t));
        if (valid_out != NULL) { *valid_out = false; }
    }
}

/**
 * @brief Build the aggregated status snapshot. See data_manager.h.
 *
 * Aggregates state from four sources into a single output struct:
 *   - MX2 latest measurement (via dm_meas_snapshot)
 *   - MX4 cfg shadow         (via dm_cfg_snapshot, plus derived setpoints)
 *   - relay-controller spinlock window states (via t2_get_window_states)
 *   - EG1 event-group bits   (raw + decoded operating mode)
 *   - WiFi STA info          (esp_wifi_sta_get_ap_info + esp_netif_get_ip_info)
 *   - LittleFS /manifest.json asset_version (cached after first read)
 *
 * Each underlying lock is taken and released independently — no critical
 * section spans more than one source, so this can safely be called from
 * any task without lock-order concerns.
 *
 * @param out  Caller-allocated status_snapshot_t; zero-initialised on entry.
 *             Missing data appears as 0 / false / "" (e.g. before T5
 *             produces its first reading).
 * @note   The asset version is cached after first successful manifest read
 *         (s_asset_ver_loaded / s_asset_ver) — the manifest doesn't change
 *         at runtime, only across an OTA reboot.
 */
void dm_status_snapshot(status_snapshot_t *out)
{
    if (out == NULL) { return; }
    memset(out, 0, sizeof(*out));

    /* Latest measurement (MX2-protected; fast path). */
    sensor_reading_t meas;
    bool meas_valid = false;
    dm_meas_snapshot(&meas, &meas_valid);
    if (meas_valid) {
        /* T5 populates the ×10 fields directly from the FG6485A's float-
         * precision reading (rc.1.3.1). Prior to rc.1.3.1 the integer
         * `temperature_c` field was multiplied by 10 here, which could only
         * produce values ending in .0 — the operator-visible "stuck at .0"
         * bug. The current path uses the c10 fields verbatim, preserving
         * the sensor's native 0.1 °C resolution end-to-end. */
        out->t_c10              = meas.temperature_c10;
        out->t_avg_c10          = meas.t_avg_c10;
        out->rh_pct             = meas.humidity_pct;
        out->rh_avg_pct         = meas.rh_avg_pct;
        out->w_ms10             = meas.wind_speed_ms10;
        out->w_avg_ms10         = meas.wind_speed_avg_ms10;
        out->w_dir_deg          = meas.wind_dir_deg;
        out->w_avg_dir_deg      = meas.wind_dir_avg_deg;
        out->w_dir_variation_deg = meas.wind_dir_variation_deg;
    }

    /* Config / derived state (MX4-protected). */
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);
    out->is_daytime         = cfg.is_daytime;
    /* Active climate setpoints — currently-in-force day-or-night values.
     * The local web GUI and the canonical status JSON surface these so the
     * operator can see, at a glance, which threshold is gating ventilation
     * right now without mentally consulting the time of day. */
    if (cfg.is_daytime) {
        out->t_max_active  = cfg.t_max_day;
        out->rh_max_active = (uint8_t)cfg.rh_max_day;
        out->rh_min_active = (uint8_t)cfg.rh_min_day;
    } else {
        out->t_max_active  = cfg.t_max_ngt;
        out->rh_max_active = (uint8_t)cfg.rh_max_ngt;
        out->rh_min_active = (uint8_t)cfg.rh_min_ngt;
    }
    out->rh_ctrl_enabled = (cfg.rh_ctrl_en != 0);
    /* Wind protection has a dedicated cfg boolean (`wind_prot_en`) that gates
     * the whole T3 safety_monitor subsystem — both the speed and direction
     * branches. Setting v_max ≤ 0 only disables the speed branch; setting
     * wind_prot_en=0 disables everything (and T3 clears EG1.WIND_OVERRIDE
     * if it was set). The badge maps to the operator-visible "off the whole
     * subsystem" decision, which is wind_prot_en. */
    out->wind_protect_enabled = (cfg.wind_prot_en != 0);
    /* a.6.35.6 — coredump-available indicator, derived from the boot-time
     * esp_core_dump_image_check() result. Drives the canonical JSON's
     * `coredump_available` mode flag and the GUI's blue Alarms-card badge. */
    out->coredump_available = s_coredump_present;
    out->ts_unix            = cfg.current_unix_ts;
    /* rc.1.5.4 — read the same s_sntp_synced latch network_manager's
     * snapshot_state() uses for the LCD path. The previous
     * `cfg.current_unix_ts > 1700000000UL` test was fooled by T4's own
     * DS1307 RTC pre-seed: the cached unix_ts is plausible at boot
     * regardless of SNTP success, so the web GUI's /api/status would
     * report `ntp_synced=true` and show "NTP synced" while the LCD
     * (correctly) showed "RTC". The accessor returns the canonical flag
     * driven only by `esp_sntp_get_sync_status() == COMPLETED`. */
    out->ntp_synced         = nm_is_sntp_synced();
    out->update_interval_s  = (uint16_t)(cfg.status_interval_s > 0 ? cfg.status_interval_s
                                                                    : DEF_STATUS_INTERVAL_S);

    /* Belt-and-braces TZ reapply. configTime() inside T10's NTP sync resets
     * TZ to "UTC0" and there is a brief window where localtime_r() returns
     * UTC. We reapply the configured TZ here so both time_iso (local ISO)
     * and the sunrise/sunset conversion below are deterministic regardless
     * of what the NTP sync path is doing. The strcmp gate avoids the
     * setenv-allocate-free churn when TZ already matches — this snapshot
     * runs every 2 s from the WS push. */
    if (cfg.tz_str[0] != '\0') {
        const char *cur = getenv("TZ");
        if (cur == NULL || strcmp(cur, cfg.tz_str) != 0) {
            setenv("TZ", cfg.tz_str, 1);
            tzset();
        }
    }

    /* Local-time ISO-8601 + UTC→local minutes-from-midnight for sun tile.
     * The dashboard renders sunrise_min/sunset_min verbatim as HH:MM, so the
     * payload must carry LOCAL minutes — not UTC. Newlib's `struct tm` does
     * not expose tm_gmtoff, so derive the offset from localtime vs. gmtime
     * of the same instant; this naturally tracks DST. */
    long off_min = 0;
    if (out->ts_unix > 1000000UL) {
        time_t ts = (time_t)out->ts_unix;
        struct tm lt, gm;
        localtime_r(&ts, &lt);
        gmtime_r(&ts, &gm);
        strftime(out->time_iso, sizeof(out->time_iso), "%Y-%m-%dT%H:%M:%S", &lt);

        /* Compute LT − GM in minutes. Use yday delta for the day-boundary
         * case; collapse to ±1 across a year boundary (rare but possible). */
        int day_diff = lt.tm_yday - gm.tm_yday;
        if      (lt.tm_year > gm.tm_year) day_diff =  1;
        else if (lt.tm_year < gm.tm_year) day_diff = -1;
        off_min = (long)day_diff * 1440L
                + (long)(lt.tm_hour - gm.tm_hour) * 60L
                + (long)(lt.tm_min  - gm.tm_min);
    } else {
        strncpy(out->time_iso, "—", sizeof(out->time_iso) - 1u);
    }
    /* +14400 (10 days of minutes) keeps modulo positive for any plausible
     * offset including negative (west-of-UTC) timezones. */
    out->sunrise_mins_local = (int32_t)((cfg.sunrise_mins_utc + off_min + 14400L) % 1440);
    out->sunset_mins_local  = (int32_t)((cfg.sunset_mins_utc  + off_min + 14400L) % 1440);

    /* Window states via the relay-controller spinlock-protected getter. */
    t2_get_window_states(out->win);

    /* Mode is derived from EG1 in priority order. rc.1.5.0 (gh#28) inserts
     * STANDBY below WIND_OVERRIDE — safety always wins, but a deliberate
     * operator pause beats the default AUTOMATIC tile. CALIBRATING is
     * orthogonal (early-boot transient): the LCD Scherm 3 renders
     * "Window Cal." directly from the EG1 bit rather than going through
     * op_mode_t, which keeps the canonical-JSON consumers (4-value enum)
     * undisturbed. */
    EventBits_t eg1 = xEventGroupGetBits(EG1);
    out->eg1_bits = (uint32_t)eg1;
    if      (eg1 & EG1_BIT_MOTOR_ALARM)   { out->mode = MODE_MOTOR_ALARM;   }
    else if (eg1 & EG1_BIT_WIND_OVERRIDE) { out->mode = MODE_WIND_OVERRIDE; }
    else if (eg1 & EG1_BIT_STANDBY)       { out->mode = MODE_STANDBY;      }
    else                                  { out->mode = MODE_AUTOMATIC;    }

    /* Network (alpha.6.7 — IDF replacement for Arduino WiFi.* calls).
     *
     * esp_wifi_sta_get_ap_info() returns ESP_OK only when the STA is
     * associated to an AP — same semantics as Arduino's WiFi.isConnected().
     * On success we read the IP from the default WiFi-STA netif and the
     * RSSI from the AP-info record (no separate WiFi.RSSI() call needed). */
    {
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            esp_netif_t *netif =
                esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip = {};
            if (netif != NULL &&
                esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
                snprintf(out->ip, sizeof(out->ip),
                         IPSTR, IP2STR(&ip.ip));
            }
            out->rssi = (int16_t)ap_info.rssi;
        }
    }

    /* Firmware version + uptime. */
    strncpy(out->fw, FIRMWARE_VERSION, sizeof(out->fw) - 1u);
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);

    /* Asset version from /manifest.json on the active LittleFS partition.
     * Cached after the first successful read — manifest doesn't change at
     * runtime, only across an OTA reboot. The manifest format written by
     * task_t13_assets is a flat JSON object: {"asset_version":"X.Y.Z",...}
     *
     * If the manifest cannot be read or parsed, "?" is reported. The web UI
     * compares fw against assets and shows an explicit "MISMATCH" badge so
     * a stale-LFS situation after a partial OTA is visible at a glance. */
    /* alpha.6.24 — bumped 16 → 24 to match status_snapshot_t::assets[24].
     * Same "2.0.0-alpha.6.X" truncation trap that hit fw[16] in alpha.6.17.1. */
    static char s_asset_ver[24] = {};
    static bool s_asset_ver_loaded = false;
    if (!s_asset_ver_loaded) {
        s_asset_ver_loaded = true;
        char manifest[128] = {};
        if (littlefs_read(littlefs_active_partition(), "/manifest.json",
                          manifest, sizeof(manifest)) == LFS_OK) {
            const char *needle = "\"asset_version\":\"";
            const char *p = strstr(manifest, needle);
            if (p) {
                p += strlen(needle);
                const char *end = strchr(p, '"');
                if (end && end > p) {
                    size_t n = (size_t)(end - p);
                    if (n >= sizeof(s_asset_ver)) n = sizeof(s_asset_ver) - 1u;
                    memcpy(s_asset_ver, p, n);
                    s_asset_ver[n] = '\0';
                }
            }
        }
        if (s_asset_ver[0] == '\0') {
            strncpy(s_asset_ver, "?", sizeof(s_asset_ver) - 1u);
        }
        ESP_LOGI(TAG, "Asset version (from manifest.json): %s", s_asset_ver);
    }
    strncpy(out->assets, s_asset_ver, sizeof(out->assets) - 1u);
}

/**
 * @brief Read up to @p count entries from the MX3-protected ring buffer.
 *        See data_manager.h.
 *
 * Locates the oldest entry from the ring head and copies `to_copy =
 * min(count, available - offset)` entries forward (wrap-aware). On MX3
 * timeout no entries are copied and *read_out is set to 0.
 */
void dm_ring_read(uint16_t offset, sensor_reading_t *buf,
                  uint16_t count, uint16_t *read_out)
{
    uint16_t copied = 0u;

    if (xSemaphoreTake(MX3, pdMS_TO_TICKS(500u)) == pdTRUE) {
        uint16_t avail = s_ring.count;
        if (offset >= avail) {
            /* Nothing to read at this offset. */
        } else {
            uint16_t to_copy = avail - offset;
            if (to_copy > count) { to_copy = count; }

            /* Logical index 0 is the oldest entry.
             * Physical index of oldest = (head - count + DEPTH) % DEPTH. */
            uint16_t oldest = (uint16_t)((s_ring.head + (uint16_t)DM_RING_DEPTH - s_ring.count)
                                          % (uint16_t)DM_RING_DEPTH);
            for (uint16_t i = 0u; i < to_copy; i++) {
                uint16_t phys = (uint16_t)((oldest + offset + i) % (uint16_t)DM_RING_DEPTH);
                buf[i] = s_ring.entries[phys];
            }
            copied = to_copy;
        }
        xSemaphoreGive(MX3);
    }

    if (read_out != NULL) { *read_out = copied; }
}

/** @brief Return current ring buffer occupancy under MX3 (see data_manager.h). */
uint16_t dm_ring_count(void)
{
    uint16_t n = 0u;
    if (xSemaphoreTake(MX3, pdMS_TO_TICKS(500u)) == pdTRUE) {
        n = s_ring.count;
        xSemaphoreGive(MX3);
    }
    return n;
}

/** @brief Return the last-known Unix UTC timestamp from MX4 (see data_manager.h). */
uint32_t dm_get_unix_time(void)
{
    uint32_t v = 0u;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.current_unix_ts;
        xSemaphoreGive(MX4);
    }
    return v;
}

/** @brief Return the configured sensor poll interval from MX4 (see data_manager.h). */
int32_t dm_get_poll_interval_s(void)
{
    int32_t v = DEF_POLL_INTERVAL_S;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.poll_interval_s;
        xSemaphoreGive(MX4);
    }
    return v;
}

/**
 * @brief Refresh the web/status NVS keys into the cfg shadow. See data_manager.h.
 *
 * Takes MX4 with a 500 ms timeout, calls nvs_load_web(), releases, then
 * also notifies T14 via T14_NOTIFY_CFG_CHANGED so enable/URL/interval
 * changes take effect within ~1 s instead of after T14's 60 s idle period.
 */
void dm_reload_web_cfg(void)
{
    /* Synchronous: caller (typically the /api/web POST handler) blocks until
     * the shadow has been refreshed from NVS. Going via TN5/task-notification
     * leaves a window where the next /api/web GET (e.g. the 5 s tab refresh)
     * still reads the *previous* shadow values and the UI snaps back to the
     * old URL. NVS reads under MX4 are fast (<10 ms) so blocking the async
     * web context briefly is acceptable. */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(500u)) == pdTRUE) {
        nvs_load_web();
        xSemaphoreGive(MX4);
    } else {
        ESP_LOGW(TAG, "dm_reload_web_cfg: MX4 timeout — shadow may be stale");
    }

    /* a.6.35.1 — wake T14 so an enable / URL / interval change takes effect
     * within ~1 s of the /api/web POST rather than after the 60 s disabled-
     * branch idle. The notify bit also drives T14's "I just woke from
     * disabled — clear s_last_str so the GUI shows `—` until the first POST
     * completes" transition, which is what removes the confusing
     * `enable=1 last_post=DISABLED` window the operator was seeing. */
    if (task_t14 != NULL) {
        xTaskNotify(task_t14, T14_NOTIFY_CFG_CHANGED, eSetBits);
    }
}

/**
 * @brief Persist the most recently uploaded log filename. See data_manager.h.
 *
 * Two-step write: NVS first (so the value survives reboot even if MX4
 * times out next), then the MX4 shadow. The shadow update is best-effort;
 * MX4 timeout leaves the persistent NVS value correct and the shadow
 * mildly stale until the next reload.
 */
void dm_set_log_last_up(const char *filename)
{
    if (filename == NULL) { return; }

    /* Persist to NVS first; on success, update the in-RAM shadow. */
    nvs_cfg_status_t st = nvs_cfg_set_str(NVS_NS_SYSTEM, K_LOG_LAST_UP, filename);
    if (st != NVS_CFG_OK) {
        ESP_LOGW(TAG, "dm_set_log_last_up: NVS write failed (st=%d)", (int)st);
        return;
    }

    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) == pdTRUE) {
        strncpy(s_cfg.log_last_up, filename, sizeof(s_cfg.log_last_up) - 1u);
        s_cfg.log_last_up[sizeof(s_cfg.log_last_up) - 1u] = '\0';
        xSemaphoreGive(MX4);
    }
}

/* ============================================================
 * Coredump accessors (a.6.35.6)
 * ============================================================ */

/** @brief Returns the cached "coredump-present-at-boot" flag (lock-free volatile load). */
bool dm_coredump_present(void)
{
    return s_coredump_present;
}

/** @brief Returns the cached size of the stored coredump in bytes (0 if none). */
size_t dm_coredump_size_bytes(void)
{
    return s_coredump_size_b;
}

/** @brief Drop the cached coredump flag after T11 erases the partition. See data_manager.h. */
void dm_coredump_clear(void)
{
    /* Called by T11 after a successful /api/coredump/erase. The actual
     * partition erase happens in T11 via esp_core_dump_image_erase();
     * this just clears the cached "present" flag so the next status
     * snapshot omits the coredump_available mode flag and the GUI
     * Alarms-card badge disappears. */
    s_coredump_present = false;
    s_coredump_size_b  = 0u;
    ESP_LOGI(TAG, "dm_coredump_clear: cached state reset (post-erase)");
}

/**
 * @brief Set the system clock and DS1307 RTC from a Unix UTC timestamp.
 *        See data_manager.h.
 *
 * Three steps:
 *   1. settimeofday() — POSIX clock updated immediately.
 *   2. DS1307 write under MX1 — persistent across power cycles.
 *   3. MX4 shadow current_unix_ts refresh.
 *
 * Mutex timeouts on MX1 or MX4 are logged and skipped; the POSIX clock
 * always reflects the requested time regardless.
 */
void dm_set_manual_time(time_t unix_ts)
{
    /* 1. Update the POSIX system clock immediately */
    struct timeval tv = {};
    tv.tv_sec  = unix_ts;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    /* 2. Write UTC time to the DS1307 RTC under MX1 */
    struct tm t;
    gmtime_r(&unix_ts, &t);
    rtc_datetime_t dt = {};
    dt.year        = (uint16_t)(t.tm_year + 1900);
    dt.month       = (uint8_t) (t.tm_mon  + 1);
    dt.day         = (uint8_t)  t.tm_mday;
    dt.hour        = (uint8_t)  t.tm_hour;
    dt.minute      = (uint8_t)  t.tm_min;
    dt.second      = (uint8_t)  t.tm_sec;
    dt.day_of_week = (uint8_t) (t.tm_wday + 1);  /* 1 = Sunday */

    if (xSemaphoreTake(MX1, pdMS_TO_TICKS(500u)) == pdTRUE) {
        rtc_status_t st = rtc_set_time(&dt);
        xSemaphoreGive(MX1);
        if (st == RTC_OK) {
            ESP_LOGI(TAG, "Manual time set: %04u-%02u-%02u %02u:%02u:%02u UTC",
                     (unsigned)dt.year,  (unsigned)dt.month,  (unsigned)dt.day,
                     (unsigned)dt.hour,  (unsigned)dt.minute, (unsigned)dt.second);
        } else {
            ESP_LOGW(TAG, "rtc_set_time failed (st=%d) after manual time set", (int)st);
        }
    } else {
        ESP_LOGW(TAG, "MX1 timeout — DS1307 write skipped in dm_set_manual_time");
    }

    /* 3. Update the MX4 configuration shadow */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) == pdTRUE) {
        s_cfg.current_unix_ts = (uint32_t)unix_ts;
        xSemaphoreGive(MX4);
    }
}

/* ============================================================
 * rc.1.5.0 (gh#28) — STANDBY mode set/get
 * ============================================================ */

bool dm_get_standby(void)
{
    if (EG1 == NULL) { return false; }
    return (xEventGroupGetBits(EG1) & EG1_BIT_STANDBY) != 0;
}

void dm_set_standby(bool standby, log_initiator_t initiator, uint8_t channel)
{
    /* rc.1.5.1 — thin wrapper: keeps the original API stable for callers that
     * want the gh#28 recalibration-on-exit semantics (web POST /api/mode,
     * LCD Scherm 3 mode-toggle). */
    dm_set_standby_ex(standby, initiator, channel, true);
}

void dm_set_standby_ex(bool standby,
                        log_initiator_t initiator,
                        uint8_t channel,
                        bool recalibrate_on_clear)
{
    if (EG1 == NULL) { return; }

    const bool current = dm_get_standby();
    if (current == standby) {
        /* Idempotent — already in desired state. */
        return;
    }

    if (standby) {
        xEventGroupSetBits(EG1, EG1_BIT_STANDBY);
    } else {
        xEventGroupClearBits(EG1, EG1_BIT_STANDBY);
    }

    /* Persist to NVS (0/1) so the state survives reboot (gh#28 locked
     * decision). nvs_cfg_set_i32 is internally serialised by ESP-IDF NVS;
     * no extra mutex needed. */
    (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_MODE_STANDBY, standby ? 1 : 0);

    /* Audit-log the transition. LOG_MODE_CHANGE row:
     *   initiator = caller-supplied (LCD farmer/admin or web)
     *   channel   = surface hint (0=web, 1=LCD) — see dm_set_standby() doc
     *   value_a   = 1 enter STANDBY | 0 leave STANDBY
     *   value_b   = 0 reserved
     */
    {
        log_event_t ev = {};
        ev.timestamp  = (uint32_t)time(NULL);
        ev.event_type = (uint8_t)LOG_MODE_CHANGE;
        ev.initiator  = (uint8_t)initiator;
        ev.channel    = channel;
        ev.param_id   = (uint8_t)LOG_PARAM_NONE;
        ev.value_a    = (int16_t)(standby ? 1 : 0);
        ev.value_b    = 0;
        log_post(&ev);
    }

    ESP_LOGI(TAG, "[T4] STANDBY %s (init=%u, surface=%u, recal_on_clear=%d)",
             standby ? "ON" : "OFF",
             (unsigned)initiator, (unsigned)channel,
             (int)recalibrate_on_clear);

    /* On STANDBY exit AND recalibrate_on_clear requested: post CMD_RECALIBRATE
     * to Q1 so T2 re-runs the synchronous CLOSE_ALL sweep (sets
     * EG1_BIT_CALIBRATING for the duration). The "calibrate windows on
     * leave" decision was locked with the operator on 2026-05-26 — windows
     * return to a known CLOSED baseline before T6 resumes.
     *
     * rc.1.5.1 — when recalibrate_on_clear is false (admin manual-motor menu
     * exit per gh#29 + 2026-05-26 follow-up decision), skip the Q1 post:
     * the admin's deliberate per-channel positions are preserved and T6
     * takes its next decision based on the actual current state, not on a
     * forced CLOSED baseline.
     *
     * On STANDBY entry: no Q1 post regardless; windows stay where they are. */
    if (!standby && recalibrate_on_clear && Q1 != NULL) {
        window_cmd_t cmd = {};
        cmd.action  = CMD_RECALIBRATE;
        cmd.channel = 0u;
        cmd.source  = SRC_OPERATOR_MANUAL;   /* deliberate operator action */
        if (xQueueSend(Q1, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "[T4] Q1 full — CMD_RECALIBRATE dropped on STANDBY exit");
        }
    }
}
