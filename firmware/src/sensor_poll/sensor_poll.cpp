/**
 * @file sensor_poll.cpp
 * @brief T5 — Sensor Poll task implementation (Phase 3).
 *
 * Modbus RTU master for the FG6485A (T/RH) and S200 (wind) sensors.
 * Polls at the configured interval (15–120 s, default 30 s), maintains
 * per-sensor sliding averages, builds a sensor_reading_t, and overwrites Q6.
 * LOG_SENSOR is posted by T4 (data_manager) on receipt of the Q6 update —
 * not by T5 — to avoid duplicate log entries (FR-LG09: one snapshot per
 * poll interval).
 *
 * ## Fault handling
 *  - Each sensor is read up to twice per cycle (one immediate retry).
 *  - On two consecutive failures: set EG1_BIT_SENSOR_FAULT_T (T/RH) or
 *    EG1_BIT_SENSOR_FAULT_W (wind); post LOG_ALARM to Q3; log at WARN.
 *  - Fault bits are edge-triggered: the LOG_ALARM is posted only once on
 *    onset and once on clearance, not on every failed poll.
 *  - On first successful read after a fault: clear the EG1 bit and post a
 *    LOG_ALARM clearance event.
 *
 * ## Sliding average algorithm
 *  Window size (samples) = avg_win_x_min × 60 / poll_interval_s,
 *  clamped to [1, SP_AVG_DEPTH=360].
 *
 *  - T, RH, wind speed: arithmetic running-sum circular buffer.
 *  - Wind direction: unit-vector (sin/cos) circular buffer; mean reconstructed
 *    via atan2() to handle the 0°/360° discontinuity correctly.
 *
 *  Window sizes are re-evaluated on every poll cycle.  If a window size
 *  changes (user updated avg_win_t or avg_win_rh via config), the affected
 *  context is reset to zero and re-warms over the next N poll cycles.
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 3
 *  - design/tasks.md T5
 *  - design/technicalSoftwareDesignSpecification.md §5.x T5
 *  - FRS FR-SE01–FR-SE04, FR-LG09
 *
 * @author  Greenhouse Controller project
 */

/* alpha.6.8 — dropped <Arduino.h>. FreeRTOS handles (Q6, EG1, task primitives)
 * arrive via "../types/app_types.h" which includes freertos/{FreeRTOS,queue,
 * task,event_groups,semphr}.h. <math.h>, <string.h>, <time.h>, and <esp_log.h>
 * are explicit below — they were transitively available via Arduino.h pre-port
 * but the port now spells them out so we don't depend on the wrapper. */
#include <esp_log.h>

#include "sensor_poll.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"

#include "fg6485a.h"
#include "s200.h"
#include "modbus_rtu.h"

#include <math.h>
#include <string.h>
#include <time.h>

static const char *TAG = "T5_SEN";

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Maximum samples in any sliding-average buffer (matches ring-buffer depth). */
#define SP_AVG_DEPTH  360u

/** Minimum poll interval enforced by T5 regardless of NVS value (seconds). */
#define SP_POLL_MIN_S  15

/** Maximum poll interval enforced by T5 (seconds). */
#define SP_POLL_MAX_S  120

/** Delay between the first and second Modbus read attempt (ms). */
#define SP_RETRY_DELAY_MS  100u

/* =========================================================================
 * Sliding-average context types
 * ========================================================================= */

/**
 * @brief Arithmetic running-sum circular buffer.
 *
 * Used for temperature, humidity, and wind speed.
 * Oldest entry is at slot (head - count) wrapping within SP_AVG_DEPTH.
 * When the buffer is full (count == win), oldest = slot (head - win).
 */
typedef struct {
    float    buf[SP_AVG_DEPTH]; /**< Circular history of raw samples           */
    uint16_t head;              /**< Next-write slot index (0 … SP_AVG_DEPTH−1)*/
    uint16_t count;             /**< Valid entries in active window (≤ win)    */
    float    sum;               /**< Running sum of all entries in count        */
} avg_ctx_t;

/**
 * @brief Unit-vector running-sum buffer for wind direction.
 *
 * Stores per-sample sin(θ) and cos(θ) in parallel circular buffers.
 * Mean direction = atan2(Σsin, Σcos) × 180/π, adjusted to [0, 360).
 * This correctly handles the 0°/360° wrap that arithmetic averaging cannot.
 */
typedef struct {
    float    sin_buf[SP_AVG_DEPTH]; /**< sin(θ) history                        */
    float    cos_buf[SP_AVG_DEPTH]; /**< cos(θ) history                        */
    uint16_t head;                  /**< Next-write slot index                  */
    uint16_t count;                 /**< Valid entries in active window (≤ win) */
    float    sum_sin;               /**< Running sum of sin components          */
    float    sum_cos;               /**< Running sum of cos components          */
} dir_avg_ctx_t;

/* =========================================================================
 * Module-private state (BSS — zero-initialised at startup)
 * ========================================================================= */

static avg_ctx_t     s_avg_t;   /**< Sliding average — temperature (°C)    */
static avg_ctx_t     s_avg_rh;  /**< Sliding average — relative humidity   */
static avg_ctx_t     s_avg_ws;  /**< Sliding average — wind speed (m/s)    */
static dir_avg_ctx_t s_avg_wd;  /**< Sliding average — wind direction (°)  */

/** Previous window sizes — used to detect config changes and reset contexts. */
static uint16_t s_win_t_last  = 0u;
static uint16_t s_win_rh_last = 0u;
static uint16_t s_win_w_last  = 0u;

/* =========================================================================
 * Sliding-average helpers
 * ========================================================================= */

/**
 * @brief Push one scalar sample into an arithmetic running-sum context.
 *
 * When count < win: appends sample and increments count.
 * When count >= win (buffer full): evicts the oldest sample from the running
 * sum before appending the new one; count stays at win.
 *
 * Eviction index: (head - win + SP_AVG_DEPTH) % SP_AVG_DEPTH, which is the
 * slot that is exactly win positions before the current head in the circular
 * buffer — i.e. the sample that will fall out of the window.
 *
 * @param ctx  Context to update (must not be NULL).
 * @param val  New sample value.
 * @param win  Active window size in samples, already clamped [1, SP_AVG_DEPTH].
 */
static void avg_push(avg_ctx_t *ctx, float val, uint16_t win)
{
    if (ctx->count >= win) {
        uint16_t tail = (uint16_t)((ctx->head + SP_AVG_DEPTH - win) % SP_AVG_DEPTH);
        ctx->sum -= ctx->buf[tail];
        /* count remains at win */
    } else {
        ctx->count++;
    }
    ctx->buf[ctx->head] = val;
    ctx->sum            += val;
    ctx->head            = (uint16_t)((ctx->head + 1u) % SP_AVG_DEPTH);
}

/**
 * @brief Return the arithmetic mean of all entries in the context.
 * @return Mean value, or 0.0f if count == 0.
 */
static float avg_get(const avg_ctx_t *ctx)
{
    if (ctx->count == 0u) return 0.0f;
    return ctx->sum / (float)ctx->count;
}

/**
 * @brief Push one wind-direction sample into the unit-vector context.
 *
 * Converts degrees to radians, decomposes into sin/cos, maintains parallel
 * running-sum circular buffers with the same eviction logic as avg_push().
 *
 * @param ctx  Context to update.
 * @param deg  Wind direction in degrees [0, 360).
 * @param win  Active window size, clamped [1, SP_AVG_DEPTH].
 */
static void dir_avg_push(dir_avg_ctx_t *ctx, float deg, uint16_t win)
{
    const float rad = deg * ((float)M_PI / 180.0f);
    const float s   = sinf(rad);
    const float c   = cosf(rad);

    if (ctx->count >= win) {
        uint16_t tail = (uint16_t)((ctx->head + SP_AVG_DEPTH - win) % SP_AVG_DEPTH);
        ctx->sum_sin -= ctx->sin_buf[tail];
        ctx->sum_cos -= ctx->cos_buf[tail];
    } else {
        ctx->count++;
    }
    ctx->sin_buf[ctx->head] = s;
    ctx->cos_buf[ctx->head] = c;
    ctx->sum_sin             += s;
    ctx->sum_cos             += c;
    ctx->head                = (uint16_t)((ctx->head + 1u) % SP_AVG_DEPTH);
}

/**
 * @brief Reconstruct circular mean wind direction in degrees [0, 360).
 * @return Mean direction, or 0.0f if count == 0.
 */
static float dir_avg_get(const dir_avg_ctx_t *ctx)
{
    if (ctx->count == 0u) return 0.0f;
    float deg = atan2f(ctx->sum_sin, ctx->sum_cos) * (180.0f / (float)M_PI);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

/**
 * @brief Width of the smallest arc containing every direction sample.
 *
 * Reconstructs each per-sample angle from the sin/cos buffers, sorts them,
 * then finds the largest gap between consecutive sorted angles (including
 * the wrap-around gap from the last back to the first). The variation is
 * the complement of that gap: `360 - max_gap`.
 *
 * Examples:
 *   samples 100°/130°/160°       → variation = 60° (one continuous sector)
 *   samples 5°/355°/10°           → variation = 15° (wraps across north)
 *   samples 100°/280°             → variation = 180° (opposing winds)
 *   all samples identical         → variation = 0°
 *
 * @return Variation in degrees [0, 360), or 0 when count < 2.
 */
static float dir_avg_variation(const dir_avg_ctx_t *ctx)
{
    if (ctx->count < 2u) return 0.0f;

    /* Reconstruct each in-window angle from its stored sin/cos pair.
     * The valid samples are the most recent `count` entries written into
     * the circular buffer — walk from the oldest forward. */
    float angles[SP_AVG_DEPTH];
    const uint16_t n = ctx->count;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t idx = (uint16_t)((ctx->head + SP_AVG_DEPTH - n + i) % SP_AVG_DEPTH);
        float deg = atan2f(ctx->sin_buf[idx], ctx->cos_buf[idx]) * (180.0f / (float)M_PI);
        if (deg < 0.0f) deg += 360.0f;
        angles[i] = deg;
    }

    /* Insertion sort — N is small (≤ SP_AVG_DEPTH, typically 12–30) so the
     * O(N²) cost is negligible and we avoid pulling in qsort. */
    for (uint16_t i = 1u; i < n; i++) {
        float key = angles[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && angles[j] > key) {
            angles[j + 1] = angles[j];
            j--;
        }
        angles[j + 1] = key;
    }

    /* Largest gap between consecutive sorted angles, including the wrap. */
    float max_gap = 0.0f;
    for (uint16_t i = 0u; i + 1u < n; i++) {
        float gap = angles[i + 1u] - angles[i];
        if (gap > max_gap) max_gap = gap;
    }
    float wrap = 360.0f - angles[n - 1u] + angles[0];
    if (wrap > max_gap) max_gap = wrap;

    float variation = 360.0f - max_gap;
    if (variation < 0.0f)   variation = 0.0f;
    if (variation >= 360.0f) variation = 359.0f;
    return variation;
}

/**
 * @brief Compute window size in samples, clamped to [1, SP_AVG_DEPTH].
 *
 * @param win_min  Window duration in minutes (from cfg_shadow_t).
 * @param poll_s   Poll interval in seconds (must be ≥ 1).
 */
static uint16_t calc_win(int32_t win_min, int32_t poll_s)
{
    if (poll_s < 1)    poll_s = 1;
    if (win_min < 1)   win_min = 1;
    int32_t w = (win_min * 60) / poll_s;
    if (w < 1)             return 1u;
    if (w > (int32_t)SP_AVG_DEPTH) return (uint16_t)SP_AVG_DEPTH;
    return (uint16_t)w;
}

/* =========================================================================
 * Log helpers
 * ========================================================================= */

/**
 * @brief Post a LOG_ALARM event to Q3 for a sensor fault change.
 *
 * Since 2.0.0-a.6.35.3: encoding re-cut to avoid logparser collisions.
 *
 * Pre-fix: `value_a` carried both the sensor type and the onset/clear sign
 * (±1 = T/RH, ±2 = wind). With `channel=0` this aliased to motor-alarm
 * (`va=1, vb=0` per relay_controller.cpp) and wind-override sensor-fault
 * (`va=-1`) — the logparser had no way to disambiguate, so every T/RH
 * onset surfaced as "MOTOR ALARM: triggered" in the parsed output. Real
 * field captures during OTAs showed all three sensor_poll alarm shapes
 * being misclassified.
 *
 * Post-fix: `channel` carries the sensor type, `value_a` carries the
 * onset/clear edge:
 *   - channel = 4, value_a = 1 → T/RH sensor fault TRIGGERED
 *   - channel = 4, value_a = 0 → T/RH sensor fault CLEARED
 *   - channel = 5, value_a = 1 → wind  sensor fault TRIGGERED
 *   - channel = 5, value_a = 0 → wind  sensor fault CLEARED
 *
 * Channels 1/2/3 are motor channels (RELAY events); 4 and 5 are reserved
 * here for sensor faults. The logparser checks `ch` first when decoding
 * ALARM rows and only falls back to the motor/wind-override decoder when
 * `ch ∈ {0,1,2,3}`.
 *
 * @param sensor_kind  4 = T/RH, 5 = wind.
 * @param onset        true = fault triggered, false = fault cleared.
 */
static void post_sensor_alarm(uint8_t sensor_kind, bool onset)
{
    log_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.timestamp  = dm_get_unix_time();
    evt.event_type = (uint8_t)LOG_ALARM;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.channel    = sensor_kind;             /* 4 = T/RH, 5 = wind */
    evt.value_a    = onset ? 1 : 0;
    /* value_b stays 0 — sensor faults are binary on/off. */
    log_post(&evt);
}

/* =========================================================================
 * Uint8 clamp helper
 * ========================================================================= */
static inline uint8_t clamp_u8(float v)
{
    long r = lroundf(v);
    if (r < 0)   return 0u;
    if (r > 255) return 255u;
    return (uint8_t)r;
}

/* =========================================================================
 * T5 task entry point
 * ========================================================================= */

void task_sensor_poll(void *pvParameters)
{
    (void)pvParameters;

    /* ---- Boot grace delay: lets USB-CDC re-enumerate so early messages are visible ---- */
    vTaskDelay(pdMS_TO_TICKS(8000));
    ESP_LOGI(TAG, "[T5] task alive — boot grace expired");

    /* ---- Initialise Modbus RTU driver (UART1, RS485 transceiver) ---- */
    ESP_LOGI(TAG, "[T5] calling modbus_init...");
    modbus_init();
    ESP_LOGI(TAG, "[T5] Modbus RTU initialised (%d baud) — init complete", MODBUS_BAUD);

    /* ---- Explicitly zero averaging contexts (belt + braces) ---- */
    memset(&s_avg_t,  0, sizeof(s_avg_t));
    memset(&s_avg_rh, 0, sizeof(s_avg_rh));
    memset(&s_avg_ws, 0, sizeof(s_avg_ws));
    memset(&s_avg_wd, 0, sizeof(s_avg_wd));

    bool t_fault_active = false;
    bool w_fault_active = false;

    uint32_t iter = 0u;

    for (;;) {
        iter++;

        /* ================================================================
         * Step 1 — Sleep for the configured poll interval
         * ================================================================ */
        int32_t poll_s = dm_get_poll_interval_s();
        if (poll_s < SP_POLL_MIN_S) poll_s = SP_POLL_MIN_S;
        if (poll_s > SP_POLL_MAX_S) poll_s = SP_POLL_MAX_S;

        vTaskDelay(pdMS_TO_TICKS((uint32_t)poll_s * 1000u));

        ESP_LOGI(TAG, "[T5] iter %lu — woke from %ld s delay", (unsigned long)iter, (long)poll_s);

        /* ================================================================
         * Step 2 — Refresh config snapshot; recompute window sizes
         * ================================================================ */
        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);

        int32_t poll_s_cfg = cfg.poll_interval_s;
        if (poll_s_cfg < 1) poll_s_cfg = 1;

        const uint16_t win_t  = calc_win(cfg.avg_win_t,  poll_s_cfg);
        const uint16_t win_rh = calc_win(cfg.avg_win_rh, poll_s_cfg);
        const uint16_t win_w  = win_t;   /* wind window tracks temperature window */

        /* Reset contexts on window-size change to avoid stale sums */
        if (win_t != s_win_t_last) {
            memset(&s_avg_t, 0, sizeof(s_avg_t));
            s_win_t_last = win_t;
            ESP_LOGI(TAG, "[T5] T avg window → %u sample(s)", (unsigned)win_t);
        }
        if (win_rh != s_win_rh_last) {
            memset(&s_avg_rh, 0, sizeof(s_avg_rh));
            s_win_rh_last = win_rh;
            ESP_LOGI(TAG, "[T5] RH avg window → %u sample(s)", (unsigned)win_rh);
        }
        if (win_w != s_win_w_last) {
            memset(&s_avg_ws, 0, sizeof(s_avg_ws));
            memset(&s_avg_wd, 0, sizeof(s_avg_wd));
            s_win_w_last = win_w;
            ESP_LOGI(TAG, "[T5] Wind avg window → %u sample(s)", (unsigned)win_w);
        }

        /* ================================================================
         * Step 3 — Poll FG6485A (T / RH), one retry on failure
         * ================================================================ */
        ESP_LOGI(TAG, "[T5] iter %lu — polling FG6485A", (unsigned long)iter);

        fg6485a_measurement_t tm;
        memset(&tm, 0, sizeof(tm));
        bool t_ok = false;

        for (int attempt = 0; attempt < 2 && !t_ok; attempt++) {
            if (attempt > 0) {
                vTaskDelay(pdMS_TO_TICKS(SP_RETRY_DELAY_MS));
            }
            t_ok = (fg6485a_read_measurements(FG6485A_DEFAULT_ADDR, &tm) == FG6485A_OK);
        }

        if (t_ok) {
            if (t_fault_active) {
                /* Fault cleared — update EG1, log once */
                xEventGroupClearBits(EG1, EG1_BIT_SENSOR_FAULT_T);
                t_fault_active = false;
                post_sensor_alarm(/*sensor_kind=*/4u, /*onset=*/false);
                ESP_LOGI(TAG, "[T5] T/RH sensor fault cleared (T=%.1f°C RH=%.1f%%)",
                         (double)tm.temperature_c, (double)tm.humidity_pct);
            }
            avg_push(&s_avg_t,  tm.temperature_c, win_t);
            avg_push(&s_avg_rh, tm.humidity_pct,  win_rh);
        } else {
            if (!t_fault_active) {
                /* Fault onset — update EG1, log once */
                xEventGroupSetBits(EG1, EG1_BIT_SENSOR_FAULT_T);
                t_fault_active = true;
                post_sensor_alarm(/*sensor_kind=*/4u, /*onset=*/true);
                ESP_LOGW(TAG, "[T5] T/RH sensor FAULT — two consecutive read failures");
            }
        }

        /* ================================================================
         * Step 4 — Poll S200 (wind speed / direction), one retry on failure
         * ================================================================ */
        ESP_LOGI(TAG, "[T5] iter %lu — polling S200", (unsigned long)iter);

        s200_measurement_t wm;
        memset(&wm, 0, sizeof(wm));
        bool w_ok = false;

        for (int attempt = 0; attempt < 2 && !w_ok; attempt++) {
            if (attempt > 0) {
                vTaskDelay(pdMS_TO_TICKS(SP_RETRY_DELAY_MS));
            }
            w_ok = (s200_read_measurements(S200_DEFAULT_ADDR, &wm) == S200_OK);
        }

        if (w_ok) {
            if (w_fault_active) {
                xEventGroupClearBits(EG1, EG1_BIT_SENSOR_FAULT_W);
                w_fault_active = false;
                post_sensor_alarm(/*sensor_kind=*/5u, /*onset=*/false);
                ESP_LOGI(TAG, "[T5] Wind sensor fault cleared (ws=%.1f m/s wd=%.0f°)",
                         (double)wm.wind_speed_avg_ms, (double)wm.wind_dir_avg_deg);
            }
            avg_push(&s_avg_ws, wm.wind_speed_avg_ms, win_w);
            dir_avg_push(&s_avg_wd, wm.wind_dir_avg_deg, win_w);
        } else {
            if (!w_fault_active) {
                xEventGroupSetBits(EG1, EG1_BIT_SENSOR_FAULT_W);
                w_fault_active = true;
                post_sensor_alarm(/*sensor_kind=*/5u, /*onset=*/true);
                ESP_LOGW(TAG, "[T5] Wind sensor FAULT — two consecutive read failures");
            }
        }

        /* ================================================================
         * Step 5 — Build sensor_reading_t
         *
         * Raw fields: instantaneous values from the sensor (or last known
         * average if the sensor is in fault — avoids a zero-gap in T4's ring).
         * Avg fields: sliding window output regardless of current read status.
         * ================================================================ */
        sensor_reading_t reading;
        memset(&reading, 0, sizeof(reading));

        /* Use the POSIX system clock so the timestamp always reflects the
         * actual moment of the reading.  dm_get_unix_time() returns a cached
         * value that T4 only refreshes every 60 s from the RTC; with a
         * poll_interval shorter than 60 s two consecutive polls would both
         * receive the same stale timestamp, producing duplicate rows in the
         * sensor history table. */
        reading.timestamp = (uint32_t)time(NULL);

        /* Raw T / RH */
        if (t_ok) {
            reading.temperature_c = (int16_t)lroundf(tm.temperature_c);
            reading.humidity_pct  = clamp_u8(tm.humidity_pct);
        } else {
            /* Carry forward the last known average to avoid a gap */
            reading.temperature_c = (int16_t)lroundf(avg_get(&s_avg_t));
            reading.humidity_pct  = clamp_u8(avg_get(&s_avg_rh));
        }

        /* Raw wind */
        if (w_ok) {
            reading.wind_speed_ms10 = (uint16_t)lroundf(wm.wind_speed_avg_ms * 10.0f);
            reading.wind_dir_deg    = (uint16_t)lroundf(wm.wind_dir_avg_deg);
        } else {
            reading.wind_speed_ms10 = (uint16_t)lroundf(avg_get(&s_avg_ws) * 10.0f);
            reading.wind_dir_deg    = (uint16_t)lroundf(dir_avg_get(&s_avg_wd));
        }

        /* Sliding averages */
        reading.t_avg_c              = (int16_t)lroundf(avg_get(&s_avg_t));
        reading.rh_avg_pct           = clamp_u8(avg_get(&s_avg_rh));
        reading.wind_speed_avg_ms10  = (uint16_t)lroundf(avg_get(&s_avg_ws) * 10.0f);
        reading.wind_dir_avg_deg     = (uint16_t)lroundf(dir_avg_get(&s_avg_wd));
        reading.wind_dir_variation_deg = (uint16_t)lroundf(dir_avg_variation(&s_avg_wd));

        /* ================================================================
         * Step 6 — Overwrite Q6 (depth-1; T4 reads via xQueueReceive)
         * ================================================================ */
        xQueueOverwrite(Q6, &reading);

        /* ================================================================
         * Step 7 — LOG_SENSOR is posted by T4 (data_manager) on receipt of
         * this Q6 update (FR-LG09: one snapshot per poll interval).
         * T5 does NOT post LOG_SENSOR directly — doing so would duplicate
         * the log entry (Finding 1, Phase 5 hardware verification).
         * ================================================================ */

        ESP_LOGI(TAG,
                 "[T5] T=%d°C RH=%u%% ws=%u.%u m/s wd=%u° | "
                 "avg T=%d RH=%u ws=%u.%u wd=%u° [win T=%u RH=%u W=%u]",
                 (int)reading.temperature_c,
                 (unsigned)reading.humidity_pct,
                 (unsigned)(reading.wind_speed_ms10 / 10u),
                 (unsigned)(reading.wind_speed_ms10 % 10u),
                 (unsigned)reading.wind_dir_deg,
                 (int)reading.t_avg_c,
                 (unsigned)reading.rh_avg_pct,
                 (unsigned)(reading.wind_speed_avg_ms10 / 10u),
                 (unsigned)(reading.wind_speed_avg_ms10 % 10u),
                 (unsigned)reading.wind_dir_avg_deg,
                 (unsigned)win_t,
                 (unsigned)win_rh,
                 (unsigned)win_w);
    }
}
