/**
 * @file safety_monitor.cpp
 * @brief T3 — Safety Monitor implementation (Phase 4).
 *
 * Wind safety evaluation: speed threshold and direction exclusion zone.
 * Issues CMD_CLOSE_ALL / CMD_RESUME to Q1.  Maintains EG1.WIND_OVERRIDE.
 *
 * See safety_monitor.h for the queue-table and the Q3 log-event mapping.
 *
 * ## Implementation notes
 *
 *  1. T4 sends TN1 as `xTaskNotify(task_t3, 1u, eSetBits)`.  T3 receives
 *     it with `ulTaskNotifyTake(pdTRUE, …)` which treats the value as a
 *     binary count: non-zero → wake + clear.  Rapid back-to-back
 *     notifications merge safely; T3 always reads the latest data from T4.
 *
 *  2. Q1 posts use timeout 0 (non-blocking) per design — T3 must never block
 *     on the actuation queue.  Q1 has depth 8; a full queue would require
 *     >8 pending unprocessed commands, which should not occur in practice.
 *
 *  3. Direction zone with dir_excl_low == dir_excl_high (zero width) is
 *     treated as disabled (returns false).  This means the direction check
 *     can be suppressed by setting both bounds to the same value.
 *
 *  4. MOTOR_ALARM: T3 evaluates normally and posts to Q1.  T2 discards Q1
 *     commands while EG1.MOTOR_ALARM is set; EG1.WIND_OVERRIDE is maintained
 *     correctly by T3 regardless.
 *
 *  5. WDT subscription (gh#13, since 1.17.29): T3 is safety-critical.  The
 *     ulTaskNotifyTake uses a 2 s timeout instead of portMAX_DELAY so the
 *     task can kick the WDT even when no new sensor reading has arrived
 *     (a stuck T4 would otherwise wedge T3 silently).
 *
 * @author  Greenhouse Controller project
 */

/* alpha.6.11 — dropped <Arduino.h>. T3 has no Arduino-specific calls;
 * FreeRTOS primitives (ulTaskNotifyTake, xQueueSend, xEventGroup*)
 * arrive via app_types.h's transitive FreeRTOS includes. */
#include <esp_log.h>
#include <esp_task_wdt.h>   /* WDT subscription (1.17.29 / gh#13) */

#include "safety_monitor.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"

static const char *TAG = "T3_WIND";

/* -----------------------------------------------------------------------
 * Direction exclusion zone check
 * ----------------------------------------------------------------------- */

/**
 * @brief Test whether a wind direction falls inside the configured exclusion arc.
 *
 * Returns true if dir_deg falls inside the [excl_low, excl_high] arc on the
 * 0–359° circle.
 *
 * Wrap-through-0° is handled when excl_low > excl_high:
 *   e.g. excl_low=330, excl_high=30  →  zone covers 330°–359° ∪ 0°–30°
 *
 * Zero-width zone (excl_low == excl_high) → disabled; returns false.
 * Negative bound (unset / invalid)        → disabled; returns false.
 *
 * @param  dir_deg    Current averaged wind direction, 0–359°.
 * @param  excl_low   Low bound of the exclusion arc (degrees, signed so the
 *                    "disabled / unset" sentinel −1 round-trips).
 * @param  excl_high  High bound of the exclusion arc.
 * @return true if dir_deg is inside the active exclusion arc; false if the
 *         zone is disabled or dir_deg lies outside.
 */
static bool dir_in_exclusion_zone(uint16_t dir_deg,
                                  int16_t  excl_low,
                                  int16_t  excl_high)
{
    if (excl_low < 0 || excl_high < 0) return false;
    if (excl_low == excl_high)         return false;   /* zero-width = disabled */

    const uint16_t lo = (uint16_t)excl_low;
    const uint16_t hi = (uint16_t)excl_high;

    if (lo < hi) {
        return (dir_deg >= lo && dir_deg <= hi);
    }
    /* lo > hi: zone wraps through 0° */
    return (dir_deg >= lo || dir_deg <= hi);
}

/* -----------------------------------------------------------------------
 * Convenience: build a LOG_ALARM event_t from T3
 * ----------------------------------------------------------------------- */

/**
 * @brief Build a LOG_ALARM log_event_t pre-populated for T3 wind events.
 *
 * Sets event_type=LOG_ALARM, initiator=LOG_BY_SYSTEM, channel=0,
 * param_id=LOG_PARAM_NONE.  Caller supplies the timestamp and the two
 * payload fields whose meaning per condition is documented in the table
 * in safety_monitor.h.
 *
 * @param  ts  Unix timestamp (seconds) at which the event occurred.
 * @param  va  First payload (value_a) — meaning per onset/clearance kind.
 * @param  vb  Second payload (value_b) — meaning per onset/clearance kind.
 * @return Populated log_event_t ready for log_post().
 */
static log_event_t make_wind_log(uint32_t ts, int16_t va, int16_t vb)
{
    log_event_t e;
    e.timestamp  = ts;
    e.event_type = (uint8_t)LOG_ALARM;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.channel    = 0;
    e.param_id   = (uint8_t)LOG_PARAM_NONE;
    e.value_a    = va;
    e.value_b    = vb;
    return e;
}

/* -----------------------------------------------------------------------
 * T3 task
 * ----------------------------------------------------------------------- */

/**
 * @brief T3 task body — wind safety state machine.
 *
 * Loop structure:
 *  1. Kick the task WDT.
 *  2. Block on TN1 with a 2 s timeout (continue + re-kick if it expires).
 *  3. Snapshot the latest sensor + cfg state from T4.
 *  4. Short-circuit if wind protection is disabled (clear any stale override).
 *  5. Evaluate speed-unsafe / dir-unsafe; safe-fail to "unsafe" on
 *     EG1_BIT_SENSOR_FAULT_W.
 *  6. Drive the safe ↔ unsafe transition: set/clear EG1_BIT_WIND_OVERRIDE,
 *     post CMD_CLOSE_ALL or CMD_RESUME to Q1, log LOG_ALARM to Q3.
 *
 * `alarm_active` is a task-local mirror of EG1_BIT_WIND_OVERRIDE used to
 * detect transitions cheaply without re-reading the event group every
 * iteration.
 *
 * @param  pvParameters  Unused; pass NULL.
 * @warning Subscribes to the task WDT; the 2 s notify timeout is required
 *          for WDT progress when no new sensor reading is available.  Do
 *          not increase it without re-tuning the WDT timeout window.
 */
void task_safety_monitor(void *pvParameters)
{
    (void)pvParameters;

    bool alarm_active = false;   /* mirrors EG1.WIND_OVERRIDE */

    /* Subscribe to the task WDT (1.17.29 / gh#13). T3 is safety-critical:
     * if it hangs, wind protection stops working — we want a WDT-reset to
     * notice. T4 only notifies on each sensor poll (interval 30–3600 s), so
     * the receive uses a 2 s timeout instead of portMAX_DELAY; on timeout
     * we just kick the WDT and continue (no work to do without new data).
     * This means T3 wakes every 2 s in the idle case — cheap. */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "[T3] task alive");

    for (;;) {
        esp_task_wdt_reset();
        /* Take the TN1 notification with a 2 s timeout. If no notification
         * arrived during the window the loop iterates again to kick the WDT.
         * When a notification DOES arrive, we fall through and evaluate. */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
            continue;
        }

        /* ---- Snapshot latest state from T4 ---- */
        sensor_reading_t meas;
        bool meas_valid = false;
        dm_meas_snapshot(&meas, &meas_valid);

        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);

        uint32_t now = dm_get_unix_time();

        /* ---- Wind protection disabled ---------------------------------- */
        if (!cfg.wind_prot_en) {
            if (alarm_active) {
                /* Clear any active override now that wind protection is off */
                alarm_active = false;
                xEventGroupClearBits(EG1, EG1_BIT_WIND_OVERRIDE);

                window_cmd_t cmd = { CMD_RESUME, 0, SRC_T3 };
                xQueueSend(Q1, &cmd, 0);

                /* value_a = 0, value_b = 0: disabled-while-active clearance */
                log_event_t evt = make_wind_log(now, 0, 0);
                log_post(&evt);

                ESP_LOGI(TAG, "[T3] WIND_OVERRIDE cleared — wind protection disabled");
            }
            continue;
        }

        /* ---- Evaluate unsafe conditions -------------------------------- */
        const EventBits_t bits  = xEventGroupGetBits(EG1);
        const bool sensor_fault = (bits & EG1_BIT_SENSOR_FAULT_W) != 0;
        bool speed_unsafe       = false;
        bool dir_unsafe         = false;

        if (sensor_fault) {
            /*
             * SENSOR_FAULT_W active: safe-fail — treat wind as worst-case
             * (FR-W04, TSDS §5.12).  T5 has already posted the S3 fault event;
             * T3 records the WIND_OVERRIDE onset with value_a = -1.
             */
            speed_unsafe = true;
        } else if (meas_valid) {
            /* Speed threshold: compare wind_speed_avg_ms10 (m/s × 10) vs
             * v_max (m/s, integer) × 10.  Use int32 arithmetic to avoid
             * overflow on both sides before the comparison.
             * v_max <= 0 is treated as disabled (no speed-based closure).   */
            if (cfg.v_max > 0) {
                speed_unsafe = ((int32_t)meas.wind_speed_avg_ms10 >=
                                (int32_t)cfg.v_max * 10);
            }

            /* Direction exclusion zone */
            dir_unsafe = dir_in_exclusion_zone(meas.wind_dir_avg_deg,
                                               cfg.dir_excl_low,
                                               cfg.dir_excl_high);
        }

        const bool is_unsafe = speed_unsafe || dir_unsafe;

        /* ---- State machine -------------------------------------------- */

        if (is_unsafe && !alarm_active) {
            /* Transition: safe → unsafe ---------------------------------- */
            alarm_active = true;
            xEventGroupSetBits(EG1, EG1_BIT_WIND_OVERRIDE);

            /* Post CMD_CLOSE_ALL to T2 (non-blocking; T2 may discard if
             * MOTOR_ALARM is active, which is correct — relays are already
             * de-energised in that case).                                   */
            const window_cmd_t close_cmd = { CMD_CLOSE_ALL, 0, SRC_T3 };
            xQueueSend(Q1, &close_cmd, 0);

            /* Log alarm onset ------------------------------------------- */
            if (sensor_fault) {
                /* Fault-triggered: value_a = −1, value_b = 0 */
                log_event_t evt = make_wind_log(now, -1, 0);
                log_post(&evt);
                ESP_LOGW(TAG,
                         "[T3] WIND_OVERRIDE set — SENSOR_FAULT_W safe-fail");
            } else {
                if (speed_unsafe) {
                    /* W1: speed exceeded
                     * value_a = current speed × 10,  value_b = v_max × 10  */
                    log_event_t evt = make_wind_log(now,
                        (int16_t)meas.wind_speed_avg_ms10,
                        (int16_t)((int32_t)cfg.v_max * 10));
                    log_post(&evt);
                    ESP_LOGW(TAG,
                             "[T3] WIND_OVERRIDE set — speed %u.%u m/s >= v_max %d m/s",
                             (unsigned)(meas.wind_speed_avg_ms10 / 10),
                             (unsigned)(meas.wind_speed_avg_ms10 % 10),
                             (int)cfg.v_max);
                }
                if (dir_unsafe) {
                    /* W2: direction excluded
                     * value_a = current direction (deg),
                     * value_b = excl zone low bound (deg)                   */
                    log_event_t evt = make_wind_log(now,
                        (int16_t)meas.wind_dir_avg_deg,
                        cfg.dir_excl_low);
                    log_post(&evt);
                    ESP_LOGW(TAG,
                             "[T3] WIND_OVERRIDE set — dir %u° in excl zone [%d°–%d°]",
                             (unsigned)meas.wind_dir_avg_deg,
                             (int)cfg.dir_excl_low,
                             (int)cfg.dir_excl_high);
                }
            }

        } else if (!is_unsafe && alarm_active) {
            /* Transition: unsafe → safe ---------------------------------- */
            alarm_active = false;
            xEventGroupClearBits(EG1, EG1_BIT_WIND_OVERRIDE);

            const window_cmd_t resume_cmd = { CMD_RESUME, 0, SRC_T3 };
            xQueueSend(Q1, &resume_cmd, 0);

            /* W3: wind override ended
             * value_a = current speed × 10  (or 0 if no valid reading)
             * value_b = current direction   (or 0 if no valid reading)      */
            log_event_t evt = make_wind_log(now,
                meas_valid ? (int16_t)meas.wind_speed_avg_ms10 : 0,
                meas_valid ? (int16_t)meas.wind_dir_avg_deg    : 0);
            log_post(&evt);
            ESP_LOGI(TAG,
                     "[T3] WIND_OVERRIDE cleared — speed %u.%u m/s dir %u°",
                     meas_valid ? (unsigned)(meas.wind_speed_avg_ms10 / 10) : 0u,
                     meas_valid ? (unsigned)(meas.wind_speed_avg_ms10 % 10) : 0u,
                     meas_valid ? (unsigned)meas.wind_dir_avg_deg           : 0u);

        } else {
            /* No state change (steady safe or steady unsafe) */
            ESP_LOGD(TAG,
                     "[T3] eval: spd=%u.%u m/s dir=%u°%s%s",
                     meas_valid ? (unsigned)(meas.wind_speed_avg_ms10 / 10) : 0u,
                     meas_valid ? (unsigned)(meas.wind_speed_avg_ms10 % 10) : 0u,
                     meas_valid ? (unsigned)meas.wind_dir_avg_deg           : 0u,
                     speed_unsafe ? " SPEED!" : "",
                     dir_unsafe   ? " DIR!"   : "");
        }
    }
}
