/**
 * @file data_manager.h
 * @brief T4 — Data Manager public API.
 *
 * T4 is the project's authoritative data hub. It is the only task that
 * *writes* to the configuration shadow (MX4), the latest-measurement
 * snapshot (MX2), and the historical ring buffer (MX3). Every other task
 * either reads through the snapshot getters declared in this header
 * (`dm_cfg_snapshot`, `dm_meas_snapshot`, `dm_ring_read`, `dm_get_*`) or
 * posts updates back via Q4 (config_update_t) and Q6 (sensor_reading_t).
 *
 * ## Inputs (queues consumed)
 *  - Q6 — sensor_reading_t from T5 (xQueueOverwrite, depth 1).
 *  - Q4 — config_update_t from T8 (LCD), T10 (WiFi), T11 (web).
 *
 * ## Outputs (notifications + queue posts)
 *  - Q3 — log_event_t for LOG_SENSOR (every Q6), LOG_SETPOINT
 *         (every Q4 with a known param_id), LOG_SYSTEM (boot, unit_id,
 *         coredump-detected).
 *  - TN1 — task_t3 (Safety Monitor) on each new sensor reading.
 *  - TN2 — task_t6 (Climate Control) on each new sensor reading.
 *  - xTaskNotify(task_t14, T14_NOTIFY_CFG_CHANGED) on web-cfg change
 *    (via dm_reload_web_cfg() — not yet bound to a TN-N label).
 *
 * ## Mutexes owned (writer)
 *  - MX1 — DS1307 I2C bus  (shared with T8's LCD I2C; brief critical sections)
 *  - MX2 — current measurement snapshot
 *  - MX3 — sensor history ring buffer
 *  - MX4 — NVS configuration shadow
 *
 * T4 is the sole owner of:
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
#include <time.h>    /* time_t — used by dm_set_manual_time (alpha.6.7) */
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

/* TN5 (DM_NOTIFY_RELOAD_WEB) used to live here. The /api/web POST handler
 * now reloads the cfg shadow synchronously via dm_reload_web_cfg() — async
 * notification left a window where the next GET could read stale values. */

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

    /* ---- Status website / web-tab settings (NVS_NS_SYSTEM, T14) ---- */
    char     status_url[129];     /**< Endpoint URL ("" = disabled) */
    char     status_secret[65];   /**< Shared secret for sourceidentifier header */
    int32_t  status_interval_s;   /**< POST cycle (s); 60–300 */
    int32_t  status_enable;       /**< Master enable (0 = off) */
    int32_t  status_expose;       /**< Bitmask: bits 0..5 = climate/wind/windows/mode/sun/system */
    int32_t  log_upload_h;        /**< Daily log-upload local hour (0–23) */
    int32_t  log_upload_m;        /**< Daily log-upload local minute (0–59) */
    int32_t  log_upload_rot;      /**< Also upload on T9 CSV rotation (0/1) */
    char     log_last_up[33];     /**< Last uploaded log filename (T14 owns) */

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
 *
 * Performs the boot sequence (NVS load, TZ apply, RTC seed, sunrise compute,
 * boot-reason log event, coredump check) then enters the main event loop.
 * The loop blocks on Q6 with a 1 s timeout and on each tick drains Q4,
 * checks for TN4 (NTP sync), and re-reads the RTC every ~60 s.
 *
 * @param pvParameters  Unused; pass NULL.
 * @note   Subscribes to esp_task_wdt; Q6's 1 s receive timeout keeps the
 *         WDT happy even when no sensor data is flowing.
 * @warning Must be started AFTER nvs_cfg_init(), MX1/MX2/MX3/MX4 creation,
 *          and Q4/Q6 creation.
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
 * @note   Safe to call from any task. T6 calls this once per evaluation
 *         cycle; T11 calls it from the web-server's async worker.
 */
void dm_cfg_snapshot(cfg_shadow_t *out);

/**
 * @brief Copy the latest sensor measurement under MX2.
 *
 * Acquires MX2 (100 ms timeout), copies sensor_reading_t by value, releases.
 * On timeout writes zeros to *out and *valid_out = false.
 *
 * @param out        Caller-allocated sensor_reading_t to fill.
 * @param valid_out  Set to true if at least one Q6 message has been received
 *                   since boot.  May be NULL if caller does not need it.
 * @note   Always check `*valid_out` (or the equivalent) before consuming
 *         the fields — at boot, before T5 has run, the buffer is zeros.
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
 * @note   On MX3 timeout (500 ms) the function returns 0 copied; check
 *         `*read_out` rather than the buffer.
 * @warning sizeof(sensor_reading_t) is non-trivial — sizing `buf` for the
 *          whole ring (DM_RING_DEPTH entries) costs ~7 KB. Read in chunks.
 * @see    dm_ring_count() — use before this to compute offset for newest N.
 */
void dm_ring_read(uint16_t offset, sensor_reading_t *buf,
                  uint16_t count, uint16_t *read_out);

/** @brief Return the number of entries currently stored in the ring buffer.
 *  Thread-safe (MX3, 500 ms timeout; returns 0 on timeout).
 *  Use before dm_ring_read() to compute the correct offset for the newest N
 *  entries: offset = (avail > n) ? avail - n : 0. */
uint16_t dm_ring_count(void);

/* ---- Convenience single-field getters ---- */

/** @brief Return the last-known Unix UTC timestamp (seconds since 1970-01-01).
 *  Thread-safe (MX4, 100 ms timeout; returns 0 on timeout). */
uint32_t dm_get_unix_time(void);

/** @brief Return the configured sensor poll interval (seconds).
 *  Thread-safe (MX4, 100 ms timeout). */
int32_t dm_get_poll_interval_s(void);

/**
 * @brief Fill an aggregated controller status snapshot.
 *
 * Reads MX2 (latest measurement), MX4 (config + derived state) and the
 * relay-controller spinlock once each, releasing all locks before returning.
 * Safe to call from any task. Used by both the local web UI and T14.
 *
 * @param out  Caller-allocated status_snapshot_t to fill. Zero-initialised
 *             on entry; missing data appears as 0 / false / "".
 */
void dm_status_snapshot(status_snapshot_t *out);

/**
 * @brief Reload the status-website / web-tab NVS keys into the cfg shadow.
 *
 * Called by the /api/web POST handler after it has written the new keys
 * directly via nvs_cfg_set_str / nvs_cfg_set_i32. Synchronous — takes MX4
 * itself, calls the internal nvs_load_web(), releases MX4, and returns. The
 * next dm_cfg_snapshot() reflects the fresh values immediately.
 */
void dm_reload_web_cfg(void);

/**
 * @brief Persist the most recently uploaded log filename.
 *
 * Called by T14 after a successful log upload. Writes log_last_up to NVS and
 * updates the cfg shadow under MX4 in one step.
 *
 * @param filename  Bare filename (no path), e.g. "20260507143022.csv".
 *                  Truncated to fit cfg_shadow_t::log_last_up.
 */
void dm_set_log_last_up(const char *filename);

/**
 * @brief Set the system clock and DS1307 RTC to the given Unix UTC timestamp.
 *
 * Called by T8 (LCD UI) after manual date/time entry.
 *  1. Updates the POSIX system clock via settimeofday().
 *  2. Writes the time to the DS1307 RTC under MX1.
 *  3. Updates current_unix_ts in the MX4 configuration shadow.
 *
 * @param unix_ts  New time as Unix UTC epoch (seconds since 1970-01-01 UTC).
 *                 The caller converts user-entered local time to UTC via
 *                 mktime() (with TZ already set by geolocation or NVS).
 */
void dm_set_manual_time(time_t unix_ts);

/* ============================================================
 * Coredump accessors (a.6.35.6)
 *
 * T4 calls esp_core_dump_image_check() once during boot. If a coredump
 * from the previous panic is present in flash, T4 caches the fact + size
 * and emits a LOG_SYSTEM row (value_a=18). The cached state drives both
 * the canonical JSON's `coredump_available` mode flag and the GUI's blue
 * Alarms-card badge. T11's /api/coredump endpoints query and clear the
 * cached state via the accessors below.
 * ============================================================ */

/**
 * @brief Returns true iff a valid coredump was detected in flash at boot.
 *
 * Read-only, lock-free (single-byte volatile load). Cleared by
 * dm_coredump_clear() after T11 successfully erases the coredump partition.
 */
bool dm_coredump_present(void);

/**
 * @brief Size in bytes of the stored coredump as reported by
 *        esp_core_dump_image_get(). Zero when none is present.
 */
size_t dm_coredump_size_bytes(void);

/**
 * @brief Clear the cached "coredump present" flag.
 *
 * Called by T11 immediately after a successful esp_core_dump_image_erase()
 * from the /api/coredump/erase handler. Drops the GUI badge and the
 * canonical JSON flag on the next status snapshot. Does NOT touch flash —
 * the caller is responsible for the actual partition erase.
 */
void dm_coredump_clear(void);
