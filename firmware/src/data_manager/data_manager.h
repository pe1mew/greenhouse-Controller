/**
 * @file data_manager.h
 * @brief T4 — Data Manager public API.
 *
 * Central data store for the greenhouse controller.  T4 is the sole owner of:
 *   - MX4 — NVS-backed configuration shadow (cfg_shadow_t)
 *   - MX2 — latest sensor measurement (sensor_reading_t)
 *   - MX3 — historical sensor ring buffer (dm_ring_buf_t, DM_RING_DEPTH entries)
 *
 * ## Boot sequence
 *  1. Load all NVS namespaces into cfg_shadow_t.
 *  2. Apply TZ string → setenv/tzset.
 *  3. Read DS1307 RTC under MX1 → rtc_dt_to_unix() → settimeofday().
 *  4. Compute sunrise/sunset and is_daytime.
 *
 * ## Main loop events
 *  - **Q6**  (xQueueReceive, 1 s timeout): new sensor_reading_t from T5 →
 *            update MX2 + MX3 ring → post LOG_SENSOR to Q3 →
 *            xTaskNotify T3 (TN1) and T6 (TN2).
 *  - **Q4**  (non-blocking drain): config_update_t from T8/T11 →
 *            validate → NVS write → update MX4 shadow.
 *  - **TN4** (xTaskNotifyWait, non-blocking): NTP sync confirmed by T10 →
 *            read time(NULL) → rtc_set_time() under MX1 → update MX4.
 *  - **Periodic** (~60 s): re-read DS1307 → refresh current_unix_ts,
 *            is_daytime, sunrise/sunset.
 *
 * ## Getter functions
 *  Thread-safe getters are provided for T1, T2, T3, T6.
 *  All getters acquire the appropriate mutex internally.
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 1
 *  - design/tasks.md T4
 *  - design/technicalSoftwareDesignSpecification.md §5.x T4
 *  - FRS §5.3a (FR-MA01–FR-MA08), FR-DN02–FR-DN05
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../types/app_types.h"

/* ============================================================
 * T4 task notification bits
 * ============================================================ */

/**
 * @brief TN4 — T10 → T4: NTP sync confirmed.
 * T10 calls xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits) after
 * configTime() synchronises the system clock.  T4 then writes the NTP
 * time to the DS1307 RTC.
 */
#define DM_NOTIFY_NTP_SYNCED  (1u << 3)

/* ============================================================
 * Configuration shadow struct  (MX4-protected)
 *
 * Populated from NVS at boot; updated on Q4 config_update_t reception.
 * All other tasks read this struct via dm_cfg_snapshot() or the
 * individual convenience getters.  Direct access to the module-private
 * s_cfg variable is forbidden outside data_manager.cpp.
 * ============================================================ */

typedef struct {
    /* ---- Climate (NVS_NS_CLIMATE = "climate") ---- */
    int16_t  t_min_day;      /**< Min temperature day setpoint   (°C, C1)  */
    int16_t  t_max_day;      /**< Max temperature day setpoint   (°C, C2)  */
    int16_t  t_min_ngt;      /**< Min temperature night setpoint (°C, C3)  */
    int16_t  t_max_ngt;      /**< Max temperature night setpoint (°C, C4)  */
    int16_t  rh_min_day;     /**< Min humidity day setpoint      (%, C5)   */
    int16_t  rh_max_day;     /**< Max humidity day setpoint      (%, C6)   */
    int16_t  rh_min_ngt;     /**< Min humidity night setpoint    (%, C7)   */
    int16_t  rh_max_ngt;     /**< Max humidity night setpoint    (%, C8)   */
    int16_t  hyst_t;         /**< Temperature hysteresis band    (°C, C9)  */
    int16_t  hyst_rh;        /**< Humidity hysteresis band       (%, C10)  */
    int16_t  rh_ctrl_en;     /**< Humidity control enable (0=off, 1=on, C11) */
    int16_t  cr_priority;    /**< Conflict-resolution priority
                               *  0 = T-first (default), 1 = RH-first,
                               *  2 = deviation-based  (C12)               */
    int16_t  avg_win_t;      /**< T sliding-average window (minutes, C13)  */
    int16_t  avg_win_rh;     /**< RH sliding-average window (minutes, C14) */

    /* ---- Wind (NVS_NS_WIND = "wind") ---- */
    int16_t  v_max;          /**< Wind speed threshold   (m/s, C15)  */
    int16_t  dir_excl_low;   /**< Excl. zone lower bound (°,  C16)  */
    int16_t  dir_excl_high;  /**< Excl. zone upper bound (°,  C17)  */
    int16_t  wind_prot_en;   /**< Wind protection enable (0=off, 1=on) */

    /* ---- Motor (NVS_NS_MOTOR = "motor") ---- */
    int16_t  travel_s[3];         /**< Full-travel time per channel (s)
                                    *  Index 0=M1, 1=M2, 2=M3 (C18)    */
    int16_t  dwell_open_min[3];   /**< Min hold at OPEN before CLOSE accepted
                                    *  (minutes, C18)                   */
    int16_t  dwell_close_min[3];  /**< Min hold at CLOSED before OPEN accepted
                                    *  (minutes, C19)                   */

    /* ---- System (NVS_NS_SYSTEM = "system") ---- */
    int32_t  poll_interval_s;     /**< Sensor poll interval  (s, C20)       */
    int32_t  session_timeout_min; /**< LCD session timeout   (minutes)       */
    int32_t  ap_timeout_min;      /**< WiFi AP auto-shutdown (minutes)       */
    int32_t  lat_deg;             /**< Latitude integer-degrees, North >0    */
    int32_t  lat_frac;            /**< Latitude milli-degrees (0–999, C21)   */
    int32_t  lon_deg;             /**< Longitude integer-degrees, East >0    */
    int32_t  lon_frac;            /**< Longitude milli-degrees (0–999, C21)  */
    int32_t  led_day_brt;         /**< RGB LED day brightness  (0–255)       */
    int32_t  led_nite_brt;        /**< RGB LED night brightness (0–255)      */
    int32_t  led_nite_from;       /**< Night brightness start hour (0–23)    */
    int32_t  led_nite_to;         /**< Night brightness end hour  (0–23)     */
    char     tz_str[64];          /**< POSIX TZ string (e.g. "CET-1CEST,...")*/

    /* ---- Derived (computed by T4; not stored in NVS) ---- */
    bool     is_daytime;          /**< true = between sunrise and sunset      */
    uint32_t current_unix_ts;     /**< Last RTC/NTP timestamp (Unix UTC s)    */
    int32_t  sunrise_mins_utc;    /**< Today's sunrise (minutes UTC midnight) */
    int32_t  sunset_mins_utc;     /**< Today's sunset  (minutes UTC midnight) */
} cfg_shadow_t;

/* ============================================================
 * Sensor history ring buffer  (MX3-protected)
 * ============================================================ */

/** @brief Number of sensor readings stored in the history ring buffer. */
#define DM_RING_DEPTH  360u

/**
 * @brief Sensor history ring buffer.
 *
 * Stores the most recent DM_RING_DEPTH sensor_reading_t records.
 * Populated by T4 on each Q6 reception.  Protected by MX3.
 *
 * @note  sizeof(dm_ring_buf_t) ≈ 7 KB.  Do not allocate on the stack.
 *        Use dm_ring_read() to retrieve entries in small batches.
 */
typedef struct {
    sensor_reading_t entries[DM_RING_DEPTH]; /**< Circular store (newest at head−1) */
    uint16_t head;    /**< Next-write slot index (0 … DM_RING_DEPTH−1)          */
    uint16_t count;   /**< Number of valid entries currently stored (0 … DM_RING_DEPTH) */
} dm_ring_buf_t;

/* ============================================================
 * Task entry point
 * ============================================================ */

/**
 * @brief T4 — Data Manager task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_data_manager(void *pvParameters);

/* ============================================================
 * Thread-safe getter API  (callable from any task)
 * ============================================================ */

/**
 * @brief Copy the full configuration shadow under MX4.
 *
 * Acquires MX4 (200 ms timeout), copies cfg_shadow_t by value, releases.
 * On timeout falls back to a lock-free copy (may be transiently stale).
 *
 * @param out  Caller-allocated cfg_shadow_t to fill.
 */
void dm_cfg_snapshot(cfg_shadow_t *out);

/**
 * @brief Copy the latest sensor measurement under MX2.
 *
 * @param out        Caller-allocated sensor_reading_t to fill.
 * @param valid_out  Set to true if at least one Q6 message has been received
 *                   since boot.  May be NULL if caller does not need it.
 */
void dm_meas_snapshot(sensor_reading_t *out, bool *valid_out);

/**
 * @brief Read up to @p count entries from the history ring buffer.
 *
 * Entries are returned oldest-first.  @p offset=0 is the oldest surviving
 * entry; @p offset=count-1 is the most recent.  Protected by MX3.
 *
 * @param offset     Logical read offset (0 = oldest).
 * @param buf        Caller-allocated buffer for up to @p count entries.
 * @param count      Maximum entries to copy.
 * @param read_out   Receives the actual number of entries copied.
 */
void dm_ring_read(uint16_t offset, sensor_reading_t *buf,
                  uint16_t count, uint16_t *read_out);

/* ---- Convenience single-field getters ---- */

/** @brief Return true if current time falls between sunrise and sunset.
 *  Thread-safe (MX4, 100 ms timeout; returns true on timeout). */
bool dm_get_is_daytime(void);

/** @brief Return the last-known Unix UTC timestamp (seconds since 1970-01-01).
 *  Thread-safe (MX4, 100 ms timeout; returns 0 on timeout). */
uint32_t dm_get_unix_time(void);

/** @brief Return the configured sensor poll interval (seconds).
 *  Thread-safe (MX4, 100 ms timeout). */
int32_t dm_get_poll_interval_s(void);

/**
 * @brief Return motor full-travel time for the given channel (0-based).
 *
 * Returns the factory default for @p channel ≥ 3.
 * Thread-safe (MX4, 100 ms timeout).
 *
 * @param channel  0 = M1, 1 = M2, 2 = M3.
 * @return         Travel time in seconds.
 */
int16_t dm_get_travel_s(uint8_t channel);

/**
 * @brief Return OPEN dwell time for the given channel (0-based, minutes).
 *  Thread-safe (MX4, 100 ms timeout). */
int16_t dm_get_dwell_open_min(uint8_t channel);

/**
 * @brief Return CLOSE dwell time for the given channel (0-based, minutes).
 *  Thread-safe (MX4, 100 ms timeout). */
int16_t dm_get_dwell_close_min(uint8_t channel);

/**
 * @brief Read RGB LED brightness and night-schedule from MX4.
 *
 * All output pointers are optional (may be NULL).
 * Thread-safe (MX4, 100 ms timeout).
 *
 * @param day_brt_out    Day brightness  (0–255).
 * @param nite_brt_out   Night brightness (0–255).
 * @param nite_from_out  Night-start hour (0–23).
 * @param nite_to_out    Night-end hour   (0–23).
 */
void dm_get_led_config(uint8_t *day_brt_out, uint8_t *nite_brt_out,
                       uint8_t *nite_from_out, uint8_t *nite_to_out);
