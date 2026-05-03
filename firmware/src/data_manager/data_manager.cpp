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

#include "nvs_config.h"
#include "ds1307_rtc.h"

#include <Arduino.h>
#include <esp_log.h>
#include <time.h>
#include <string.h>

static const char *TAG = "T4";

/* ============================================================
 * NVS factory-default values
 * ============================================================ */

/* Climate */
#define DEF_T_MIN_DAY      15
#define DEF_T_MAX_DAY      26
#define DEF_T_MIN_NGT      12
#define DEF_T_MAX_NGT      22
#define DEF_RH_MIN_DAY     40
#define DEF_RH_MAX_DAY     80
#define DEF_RH_MIN_NGT     50
#define DEF_RH_MAX_NGT     85
#define DEF_HYST_T          2
#define DEF_HYST_RH         5
#define DEF_RH_CTRL_EN      1
#define DEF_CR_PRIORITY     0
#define DEF_AVG_WIN_T       1
#define DEF_AVG_WIN_RH      1

/* Wind */
#define DEF_V_MAX           7   /**< Wind speed threshold (m/s) */
#define DEF_DIR_EXCL_LOW    0   /**< No exclusion zone by default */
#define DEF_DIR_EXCL_HIGH   0
#define DEF_WIND_PROT_EN    1

/* Motor dwell — travel defaults come from MOTOR_MN_TRAVEL_S_DEFAULT (app_types.h) */
#define DEF_DWELL_OPEN_MIN  0
#define DEF_DWELL_CLOSE_MIN 0

/* System */
#define DEF_POLL_INTERVAL_S      60
#define DEF_SESSION_TIMEOUT_MIN   5
#define DEF_AP_TIMEOUT_MIN       30
#define DEF_LAT_DEG              52   /**< Netherlands default latitude */
#define DEF_LAT_FRAC              0
#define DEF_LON_DEG               5   /**< Netherlands default longitude */
#define DEF_LON_FRAC              0
#define DEF_LED_DAY_BRT         200
#define DEF_LED_NITE_BRT         20
#define DEF_LED_NITE_FROM        22
#define DEF_LED_NITE_TO           6
#define DEF_TZ_STR    "CET-1CEST,M3.5.0,M10.5.0/3"

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

/** @brief Re-read RTC every this many main-loop ticks (≈ RTC_POLL_TICKS s). */
#define RTC_POLL_TICKS  60u

/* ============================================================
 * Internal helper — RTC datetime → Unix UTC timestamp
 *
 * Manual implementation; avoids mktime()/timegm() portability concerns.
 * Supports years 1970–2099.
 * ============================================================ */

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
 *
 * Must be called with MX4 held (or at boot before MX4 is contested).
 * ============================================================ */

static void update_sun_times(void)
{
    float lat = (float)s_cfg.lat_deg + (float)s_cfg.lat_frac / 1000.0f;
    float lon = (float)s_cfg.lon_deg + (float)s_cfg.lon_frac / 1000.0f;
    int32_t ts = (int32_t)s_cfg.current_unix_ts;

    s_cfg.is_daytime = sunrise_is_daytime(ts, lat, lon);
    sunrise_calc(ts, lat, lon, &s_cfg.sunrise_mins_utc, &s_cfg.sunset_mins_utc);
}

/* ============================================================
 * Internal helper — read DS1307, seed system clock, update MX4
 * ============================================================ */

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
 * ============================================================ */

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

static void nvs_load_wind(void)
{
    int32_t v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_V_MAX,         DEF_V_MAX,         &v); s_cfg.v_max         = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_DIR_EXCL_LOW,  DEF_DIR_EXCL_LOW,  &v); s_cfg.dir_excl_low  = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_DIR_EXCL_HIGH, DEF_DIR_EXCL_HIGH, &v); s_cfg.dir_excl_high = (int16_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_WIND_PROT_EN,  DEF_WIND_PROT_EN,  &v); s_cfg.wind_prot_en  = (int16_t)v;
}

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

    for (uint8_t i = 0u; i < 3u; i++) {
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, ktr[i], def_tr[i],           &v); s_cfg.travel_s[i]        = (int16_t)v;
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, kdo[i], DEF_DWELL_OPEN_MIN,  &v); s_cfg.dwell_open_min[i]  = (int16_t)v;
        nvs_cfg_get_i32_or_default(NVS_NS_MOTOR, kdc[i], DEF_DWELL_CLOSE_MIN, &v); s_cfg.dwell_close_min[i] = (int16_t)v;
    }
}

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

/* ============================================================
 * Internal helper — apply a Q4 config_update_t
 *
 * Returns true on success (NVS written + shadow updated).
 * config_update_t carries only int32_t values; string-type keys
 * (tz_str) are not handled through Q4 — the web server writes them
 * to NVS directly (Phase 9) and T4 re-reads on next boot.
 * ============================================================ */

static bool apply_config_update(const config_update_t *upd)
{
    /* Write to NVS first; abort if NVS write fails. */
    nvs_cfg_status_t ns = nvs_cfg_set_i32(upd->ns, upd->key, upd->value);
    if (ns != NVS_CFG_OK) {
        ESP_LOGW(TAG, "Q4 NVS write failed  ns=%.15s key=%.15s val=%ld  err=%d",
                 upd->ns, upd->key, (long)upd->value, (int)ns);
        return false;
    }

    /* Update the in-RAM shadow under MX4. */
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(200u)) != pdTRUE) {
        ESP_LOGW(TAG, "MX4 timeout in apply_config_update — NVS written but shadow stale");
        return false;
    }

    bool updated  = true;
    int16_t v16   = (int16_t)upd->value;
    int32_t v32   = upd->value;
    const char *ns_str  = upd->ns;
    const char *key_str = upd->key;

    if (strcmp(ns_str, NVS_NS_CLIMATE) == 0) {
        if      (strcmp(key_str, K_T_MIN_DAY)   == 0) s_cfg.t_min_day   = v16;
        else if (strcmp(key_str, K_T_MAX_DAY)   == 0) s_cfg.t_max_day   = v16;
        else if (strcmp(key_str, K_T_MIN_NGT)   == 0) s_cfg.t_min_ngt   = v16;
        else if (strcmp(key_str, K_T_MAX_NGT)   == 0) s_cfg.t_max_ngt   = v16;
        else if (strcmp(key_str, K_RH_MIN_DAY)  == 0) s_cfg.rh_min_day  = v16;
        else if (strcmp(key_str, K_RH_MAX_DAY)  == 0) s_cfg.rh_max_day  = v16;
        else if (strcmp(key_str, K_RH_MIN_NGT)  == 0) s_cfg.rh_min_ngt  = v16;
        else if (strcmp(key_str, K_RH_MAX_NGT)  == 0) s_cfg.rh_max_ngt  = v16;
        else if (strcmp(key_str, K_HYST_T)      == 0) s_cfg.hyst_t      = v16;
        else if (strcmp(key_str, K_HYST_RH)     == 0) s_cfg.hyst_rh     = v16;
        else if (strcmp(key_str, K_RH_CTRL_EN)  == 0) s_cfg.rh_ctrl_en  = v16;
        else if (strcmp(key_str, K_CR_PRIORITY) == 0) s_cfg.cr_priority = v16;
        else if (strcmp(key_str, K_AVG_WIN_T)   == 0) s_cfg.avg_win_t   = v16;
        else if (strcmp(key_str, K_AVG_WIN_RH)  == 0) s_cfg.avg_win_rh  = v16;
        else { updated = false; }

    } else if (strcmp(ns_str, NVS_NS_WIND) == 0) {
        if      (strcmp(key_str, K_V_MAX)         == 0) s_cfg.v_max         = v16;
        else if (strcmp(key_str, K_DIR_EXCL_LOW)  == 0) s_cfg.dir_excl_low  = v16;
        else if (strcmp(key_str, K_DIR_EXCL_HIGH) == 0) s_cfg.dir_excl_high = v16;
        else if (strcmp(key_str, K_WIND_PROT_EN)  == 0) s_cfg.wind_prot_en  = v16;
        else { updated = false; }

    } else if (strcmp(ns_str, NVS_NS_MOTOR) == 0) {
        static const char * const ktr[] = { K_TRAVEL_M1,      K_TRAVEL_M2,      K_TRAVEL_M3      };
        static const char * const kdo[] = { K_DWELL_OPEN_M1,  K_DWELL_OPEN_M2,  K_DWELL_OPEN_M3  };
        static const char * const kdc[] = { K_DWELL_CLOSE_M1, K_DWELL_CLOSE_M2, K_DWELL_CLOSE_M3 };
        updated = false;
        for (uint8_t i = 0u; i < 3u; i++) {
            if (strcmp(key_str, ktr[i]) == 0) { s_cfg.travel_s[i]        = v16; updated = true; break; }
            if (strcmp(key_str, kdo[i]) == 0) { s_cfg.dwell_open_min[i]  = v16; updated = true; break; }
            if (strcmp(key_str, kdc[i]) == 0) { s_cfg.dwell_close_min[i] = v16; updated = true; break; }
        }

    } else if (strcmp(ns_str, NVS_NS_SYSTEM) == 0) {
        if      (strcmp(key_str, K_POLL_INTERVAL)   == 0) s_cfg.poll_interval_s     = v32;
        else if (strcmp(key_str, K_SESSION_TIMEOUT)  == 0) s_cfg.session_timeout_min = v32;
        else if (strcmp(key_str, K_AP_TIMEOUT)       == 0) s_cfg.ap_timeout_min      = v32;
        else if (strcmp(key_str, K_LAT_DEG)          == 0) { s_cfg.lat_deg  = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LAT_FRAC)         == 0) { s_cfg.lat_frac = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LON_DEG)          == 0) { s_cfg.lon_deg  = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LON_FRAC)         == 0) { s_cfg.lon_frac = v32; update_sun_times(); }
        else if (strcmp(key_str, K_LED_DAY_BRT)      == 0) s_cfg.led_day_brt         = v32;
        else if (strcmp(key_str, K_LED_NITE_BRT)     == 0) s_cfg.led_nite_brt        = v32;
        else if (strcmp(key_str, K_LED_NITE_FROM)    == 0) s_cfg.led_nite_from       = v32;
        else if (strcmp(key_str, K_LED_NITE_TO)      == 0) s_cfg.led_nite_to         = v32;
        else { updated = false; }
    } else {
        updated = false;
    }

    xSemaphoreGive(MX4);

    if (updated) {
        ESP_LOGI(TAG, "Q4 applied: %.15s/%.15s = %ld", ns_str, key_str, (long)upd->value);
    } else {
        ESP_LOGW(TAG, "Q4 key not in shadow: %.15s/%.15s = %ld  (NVS written; shadow unchanged)",
                 ns_str, key_str, (long)upd->value);
    }
    return true;
}

/* ============================================================
 * Internal helper — handle a sensor_reading_t from Q6
 * ============================================================ */

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

    /* 3. Post LOG_SENSOR snapshot to Q3 (FR-LG09). */
    log_event_t evt = {};
    evt.timestamp   = r->timestamp;
    evt.event_type  = (uint8_t)LOG_SENSOR;
    evt.initiator   = (uint8_t)LOG_BY_SYSTEM;
    evt.channel     = 0u;
    evt.param_id    = (uint8_t)LOG_PARAM_NONE;
    evt.value_a     = r->t_avg_c;
    evt.value_b     = (int16_t)r->rh_avg_pct;
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

void task_data_manager(void *pvParameters)
{
    (void)pvParameters;

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

    /* Log boot system event. */
    {
        log_event_t boot_evt = {};
        boot_evt.timestamp  = s_cfg.current_unix_ts;
        boot_evt.event_type = (uint8_t)LOG_SYSTEM;
        boot_evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
        log_post(&boot_evt);
    }

    /* ----------------------------------------------------------------
     * Main loop
     * ---------------------------------------------------------------- */
    uint32_t tick_count    = 0u;
    uint32_t last_rtc_tick = 0u;

    for (;;) {
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

bool dm_get_is_daytime(void)
{
    bool v = true;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.is_daytime;
        xSemaphoreGive(MX4);
    }
    return v;
}

uint32_t dm_get_unix_time(void)
{
    uint32_t v = 0u;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.current_unix_ts;
        xSemaphoreGive(MX4);
    }
    return v;
}

int32_t dm_get_poll_interval_s(void)
{
    int32_t v = DEF_POLL_INTERVAL_S;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.poll_interval_s;
        xSemaphoreGive(MX4);
    }
    return v;
}

int16_t dm_get_travel_s(uint8_t channel)
{
    if (channel >= 3u) { return (int16_t)MOTOR_M1_TRAVEL_S_DEFAULT; }
    int16_t v = (int16_t)MOTOR_M1_TRAVEL_S_DEFAULT;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.travel_s[channel];
        xSemaphoreGive(MX4);
    }
    return v;
}

int16_t dm_get_dwell_open_min(uint8_t channel)
{
    if (channel >= 3u) { return 0; }
    int16_t v = 0;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.dwell_open_min[channel];
        xSemaphoreGive(MX4);
    }
    return v;
}

int16_t dm_get_dwell_close_min(uint8_t channel)
{
    if (channel >= 3u) { return 0; }
    int16_t v = 0;
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        v = s_cfg.dwell_close_min[channel];
        xSemaphoreGive(MX4);
    }
    return v;
}

void dm_get_led_config(uint8_t *day_brt_out, uint8_t *nite_brt_out,
                       uint8_t *nite_from_out, uint8_t *nite_to_out)
{
    if (xSemaphoreTake(MX4, pdMS_TO_TICKS(100u)) == pdTRUE) {
        if (day_brt_out   != NULL) { *day_brt_out   = (uint8_t)s_cfg.led_day_brt;  }
        if (nite_brt_out  != NULL) { *nite_brt_out  = (uint8_t)s_cfg.led_nite_brt; }
        if (nite_from_out != NULL) { *nite_from_out = (uint8_t)s_cfg.led_nite_from; }
        if (nite_to_out   != NULL) { *nite_to_out   = (uint8_t)s_cfg.led_nite_to;  }
        xSemaphoreGive(MX4);
    }
}
