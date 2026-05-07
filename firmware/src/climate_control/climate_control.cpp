/**
 * @file climate_control.cpp
 * @brief Graduated ventilation implementation — step table, evaluation
 *        functions, conflict resolution, and T6 Climate Control task (Phase 6).
 *
 * ## T6 task structure
 *
 * T6 wakes on TN2 (task notification from T4 after each new Q6 reading).
 * On every wake it:
 *   1. Reads EG1 flags; skips evaluation if WIND_OVERRIDE, MOTOR_ALARM, or
 *      SENSOR_FAULT_T is set (window commands must not fight T3 or T2).
 *   2. Snapshots cfg_shadow_t under MX4; snapshots sensor_reading_t under MX2.
 *   3. Selects the active T and RH setpoints from is_daytime.
 *   4. Evaluates vent_step_required_t() and vent_step_required_rh().
 *   5. Resolves the two steps with vent_resolve_conflict().
 *   6. Converts old and new steps to channel bitmasks; posts only the
 *      incremental delta to Q1 via window_cmd_t.
 *   7. Logs a MODE_CHANGE event on every step change.
 *
 * ## State variables
 *
 * Two task-local statics:
 *   current_step_t  — last step T6 commanded for temperature.
 *   current_step_rh — last step T6 commanded for humidity.
 *
 * Both are reset to 0 on transitions to WIND_OVERRIDE or MOTOR_ALARM so that
 * when the flag clears T6 starts fresh from step 0 (T2's boot CLOSE_ALL keeps
 * the actual window position known).
 *
 * ## Q1 command encoding
 *
 * window_cmd_t fields for T6:
 *   .source  = SRC_T6
 *   .action  = CMD_OPEN  / CMD_CLOSE / CMD_CLOSE_ALL
 *   .channel = 1/2/3 for per-channel commands; 0 for CMD_CLOSE_ALL
 *
 * Only channels that changed are sent; no-op transitions post nothing.
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <esp_log.h>

#include "climate_control.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "T6_CLI";

/* -----------------------------------------------------------------------
 * Compile-time step → channel-mask table (Gap G)
 *
 * Index 0 is step 0 (all closed); indices 1..NUM_VENT_STEPS are the
 * cumulative channel masks added at each step.
 *
 * Step 1 — M1 only
 * Step 2 — M1 + M2
 * Step 3 — M1 + M2 + M3
 *
 * The table is sized NUM_VENT_STEPS + 1 to include the step-0 entry.
 * ----------------------------------------------------------------------- */
static const uint8_t VENT_STEP_TABLE[NUM_VENT_STEPS + 1] = {
    0,                               /* step 0 — all closed               */
    VENT_CH_M1,                      /* step 1 — M1 only                  */
    VENT_CH_M1 | VENT_CH_M2,        /* step 2 — M1 + M2                  */
    VENT_CH_M1 | VENT_CH_M2 | VENT_CH_M3, /* step 3 — M1 + M2 + M3      */
};

/* -----------------------------------------------------------------------
 * Internal helper: core graduation algorithm
 *
 * Computes the required step given:
 *   deviation   = value − setpoint_max  (may be negative)
 *   hyst        = hysteresis band (> 0)
 *   current_step = step currently commanded
 *
 * Step selection:
 *   step_width = max(hyst / NUM_VENT_STEPS, 1)
 *   raw_step   = ceil(deviation / step_width)
 *   clamped    = clamp(raw_step, 0, NUM_VENT_STEPS)
 *
 * Close-hysteresis guard:
 *   Once any step > 0 is active, do NOT step down to 0 until
 *   deviation <= −hyst  (i.e. value < setpoint_max − hyst).
 *   Step reductions within 1..NUM_VENT_STEPS are applied immediately.
 *
 * Returns: 0..NUM_VENT_STEPS
 * ----------------------------------------------------------------------- */
static int step_from_deviation(int deviation, int hyst, int current_step)
{
    /* Compute step width; floor to 1 to avoid division by zero. */
    int step_width = hyst / NUM_VENT_STEPS;
    if (step_width < 1) {
        step_width = 1;
    }

    /* Raw required step: ceil(deviation / step_width).
     * Use integer ceiling: for positive deviation only.
     * For zero or negative deviation the required step is 0. */
    int raw_step;
    if (deviation <= 0) {
        raw_step = 0;
    } else {
        /* Integer ceiling division for positive integers: (a + b - 1) / b */
        raw_step = (deviation + step_width - 1) / step_width;
    }

    /* Clamp to valid range. */
    if (raw_step > NUM_VENT_STEPS) {
        raw_step = NUM_VENT_STEPS;
    }
    if (raw_step < 0) {
        raw_step = 0;
    }

    /* Close-hysteresis guard:
     * If currently at step > 0 and raw_step == 0, only allow the step-down
     * when value has fallen below (setpoint_max − hyst), i.e. deviation <= −hyst. */
    if (current_step > 0 && raw_step == 0) {
        if (deviation > -hyst) {
            /* Not yet below close threshold; hold at step 1 (minimum open). */
            return 1;
        }
    }

    return raw_step;
}

/* -----------------------------------------------------------------------
 * Internal helpers (only called within this translation unit)
 * ----------------------------------------------------------------------- */

static uint8_t vent_step_channels(int step)
{
    if (step < 0 || step > NUM_VENT_STEPS) {
        return 0;
    }
    return VENT_STEP_TABLE[step];
}

static int vent_step_required_t(int16_t t_avg, int16_t t_max, int16_t hyst_t,
                                 int current_step)
{
    int deviation = (int)t_avg - (int)t_max;
    return step_from_deviation(deviation, (int)hyst_t, current_step);
}

static int vent_step_required_rh(int16_t rh_avg, int16_t rh_max, int16_t rh_min,
                                  int16_t hyst_rh, bool rh_ctrl_en,
                                  int current_step)
{
    if (!rh_ctrl_en) {
        return VENT_STEP_NEUTRAL;
    }

    if (rh_avg > rh_max) {
        /* Too humid — graduated OPEN using same algorithm as temperature. */
        int deviation = (int)rh_avg - (int)rh_max;
        return step_from_deviation(deviation, (int)hyst_rh, current_step);
    }

    if (rh_avg < rh_min) {
        /* Too dry — demand full CLOSE (Gap G design decision).
         * Graduated closing is NOT implemented; step 0 = full close to keep
         * conflict resolution symmetric.  vent_resolve_conflict() treats
         * step_rh == 0 as a genuine close demand from RH. */
        return 0;
    }

    /* RH is within [RH_min, RH_max] — no demand from humidity side. */
    return VENT_STEP_NEUTRAL;
}

static int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority)
{
    /* Rule 1: RH has no vote — return temperature step unchanged. */
    if (step_rh == VENT_STEP_NEUTRAL) {
        return step_t;
    }

    /* Rule 2: Both demand OPEN — more ventilation satisfies both.
     * Take the higher step regardless of cr_priority. */
    if (step_t > 0 && step_rh > 0) {
        return (step_t > step_rh) ? step_t : step_rh;
    }

    /* Rule 3: No conflict — both agree on the same step. */
    if (step_t == step_rh) {
        return step_t;
    }

    /* Rule 4: Genuine conflict (one OPEN, one CLOSE=0). Apply cr_priority.
     *   0 = CR_TEMP_FIRST  : temperature wins → return step_t
     *   1 = CR_RH_FIRST    : humidity wins    → return step_rh (may be 0)
     *   2 = CR_DEVIATION   : higher step wins (more ventilation) */
    switch (cr_priority) {
        case 0:  /* CR_TEMP_FIRST */
        default:
            return step_t;

        case 1:  /* CR_RH_FIRST */
            return step_rh;

        case 2:  /* CR_DEVIATION — higher step = more open = safer choice */
            return (step_t > step_rh) ? step_t : step_rh;
    }
}

/* -----------------------------------------------------------------------
 * post_q1() — send one window_cmd_t to Q1 (non-blocking, warn on full)
 * ----------------------------------------------------------------------- */
static void post_q1(cmd_action_t action, uint8_t channel)
{
    window_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action  = action;
    cmd.channel = channel;
    cmd.source  = SRC_T6;

    if (xQueueSend(Q1, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[T6] Q1 full — command action=%d ch=%u dropped",
                 (int)action, (unsigned)channel);
    }
}

/* -----------------------------------------------------------------------
 * post_log_mode() — emit a LOG_MODE_CHANGE record to Q3
 *
 * value_a = resolved step (0..NUM_VENT_STEPS)
 * value_b = packed: high byte = step_t, low byte = step_rh (cast to int16)
 * ----------------------------------------------------------------------- */
static void post_log_mode(int resolved_step, int step_t, int step_rh)
{
    log_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.timestamp  = dm_get_unix_time();
    evt.event_type = (uint8_t)LOG_MODE_CHANGE;
    evt.initiator  = (uint8_t)LOG_BY_SYSTEM;
    evt.channel    = 0;
    evt.param_id   = 0;
    evt.value_a    = (int16_t)resolved_step;
    /* Pack step_t (high byte) and step_rh (low byte) into value_b.
     * step_rh may be VENT_STEP_NEUTRAL (−1); clamp to −1..3 for int8. */
    int8_t b_t  = (int8_t)(step_t  < -128 ? -128 : (step_t  > 127 ? 127 : step_t));
    int8_t b_rh = (int8_t)(step_rh < -128 ? -128 : (step_rh > 127 ? 127 : step_rh));
    evt.value_b = (int16_t)(((uint16_t)(uint8_t)b_t << 8) | (uint8_t)b_rh);
    log_post(&evt);
}

/* -----------------------------------------------------------------------
 * apply_step_delta() — post incremental Q1 commands for step change
 *
 * new_step and old_step are expressed as resolved steps (0..NUM_VENT_STEPS).
 * Only changed channels are sent.  Per-channel CMD_CLOSE / CMD_OPEN commands
 * are used for all transitions, including step → 0 (full close).
 *
 * CMD_CLOSE_ALL is intentionally NOT used here.  CMD_CLOSE_ALL bypasses the
 * per-channel post-open dwell enforced by T2, causing windows to close
 * immediately regardless of dwell_open_s, which produces rapid oscillation
 * when the temperature quickly rebounds after closing.  CMD_CLOSE_ALL is
 * reserved for safety events (wind override in T3, motor alarm in T2).
 * ----------------------------------------------------------------------- */
static void apply_step_delta(int old_step, int new_step)
{
    if (old_step == new_step) {
        return;  /* Nothing to do */
    }

    uint8_t old_mask = vent_step_channels(old_step);
    uint8_t new_mask = vent_step_channels(new_step);

    /* Per-channel open/close commands for channels that changed.
     * Applies to full-close (new_mask == 0) as well as partial step changes. */
    uint8_t open_bits  = (uint8_t)(new_mask & ~old_mask);
    uint8_t close_bits = (uint8_t)(old_mask & ~new_mask);

    /* Post CLOSE commands first (narrowing before widening is safer). */
    for (uint8_t ch = 1; ch <= 3; ch++) {
        if (close_bits & (1u << (ch - 1u))) {
            post_q1(CMD_CLOSE, ch);
            ESP_LOGI(TAG, "[T6] → CMD_CLOSE ch=%u (step %d → %d)",
                     (unsigned)ch, old_step, new_step);
        }
    }
    for (uint8_t ch = 1; ch <= 3; ch++) {
        if (open_bits & (1u << (ch - 1u))) {
            post_q1(CMD_OPEN, ch);
            ESP_LOGI(TAG, "[T6] → CMD_OPEN  ch=%u (step %d → %d)",
                     (unsigned)ch, old_step, new_step);
        }
    }
}

/* -----------------------------------------------------------------------
 * T6 task — Climate Control (Phase 6)
 * ----------------------------------------------------------------------- */

void task_climate_control(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T6] task alive");

    /* Task-local state — tracks last commanded step from each source.
     * Initialised to 0; T2's boot CLOSE_ALL ensures windows are CLOSED. */
    int current_step_t  = 0;
    int current_step_rh = 0;

    /* Track whether we were inhibited on the previous cycle so we can log
     * mode transitions (inhibit onset / inhibit clearance). */
    bool prev_inhibited = false;

    for (;;) {
        /* ----------------------------------------------------------------
         * 1. Block on TN2 — T4 notifies after every new Q6 reading.
         *    portMAX_DELAY: T6 has nothing to do between sensor updates.
         * ---------------------------------------------------------------- */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* ----------------------------------------------------------------
         * 2. Check EG1 inhibit flags.
         *    Skip evaluation while any of these are active:
         *      WIND_OVERRIDE  — T3 has forced all windows closed
         *      MOTOR_ALARM    — T2 has de-energised all relays (emergency)
         *      SENSOR_FAULT_T — T/RH sensor unreliable; cannot evaluate T
         * ---------------------------------------------------------------- */
        EventBits_t bits = xEventGroupGetBits(EG1);
        bool inhibited = (bits & (EG1_BIT_WIND_OVERRIDE |
                                  EG1_BIT_MOTOR_ALARM   |
                                  EG1_BIT_SENSOR_FAULT_T)) != 0;

        if (inhibited) {
            if (!prev_inhibited) {
                /* Transition into inhibited state — reset steps so T6
                 * re-evaluates from scratch when inhibit clears. */
                current_step_t  = 0;
                current_step_rh = 0;
                ESP_LOGI(TAG, "[T6] inhibited (EG1=0x%02lx) — evaluation suspended",
                         (unsigned long)bits);
            }
            prev_inhibited = true;
            continue;
        }

        if (prev_inhibited) {
            ESP_LOGI(TAG, "[T6] inhibit cleared — resuming evaluation from step 0");
        }
        prev_inhibited = false;

        /* ----------------------------------------------------------------
         * 3. Snapshot configuration and current measurement.
         * ---------------------------------------------------------------- */
        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);

        sensor_reading_t meas;
        bool meas_valid = false;
        dm_meas_snapshot(&meas, &meas_valid);

        if (!meas_valid) {
            /* No sensor data yet — wait for the first Q6 message. */
            ESP_LOGD(TAG, "[T6] no measurement yet — skipping");
            continue;
        }

        /* ----------------------------------------------------------------
         * 4. Select active setpoints (day vs. night).
         * ---------------------------------------------------------------- */
        int16_t t_max  = cfg.is_daytime ? cfg.t_max_day  : cfg.t_max_ngt;
        int16_t rh_max = cfg.is_daytime ? cfg.rh_max_day : cfg.rh_max_ngt;
        int16_t rh_min = cfg.is_daytime ? cfg.rh_min_day : cfg.rh_min_ngt;

        /* Defensively ensure hysteresis values are positive; fall back to 1
         * so step_from_deviation never divides by zero. */
        int16_t hyst_t  = (cfg.hyst_t  > 0) ? cfg.hyst_t  : 1;
        int16_t hyst_rh = (cfg.hyst_rh > 0) ? cfg.hyst_rh : 1;

        bool rh_ctrl_en = (cfg.rh_ctrl_en != 0);

        /* ----------------------------------------------------------------
         * 5. Evaluate required steps.
         * ---------------------------------------------------------------- */
        int step_t  = vent_step_required_t(meas.t_avg_c, t_max, hyst_t,
                                           current_step_t);
        int step_rh = vent_step_required_rh(meas.rh_avg_pct, rh_max, rh_min,
                                            hyst_rh, rh_ctrl_en,
                                            current_step_rh);

        /* ----------------------------------------------------------------
         * 6. Resolve conflict → single resolved step.
         * ---------------------------------------------------------------- */
        int resolved = vent_resolve_conflict(step_t, step_rh,
                                             (uint8_t)cfg.cr_priority);

        ESP_LOGI(TAG,
                 "[T6] T_avg=%d t_max=%d hyst=%d → step_t=%d | "
                 "RH_avg=%u rh_max=%d rh_min=%d hyst=%d rh_en=%d → step_rh=%d | "
                 "resolved=%d (was cur_t=%d cur_rh=%d)",
                 (int)meas.t_avg_c, (int)t_max, (int)hyst_t, step_t,
                 (unsigned)meas.rh_avg_pct, (int)rh_max, (int)rh_min,
                 (int)hyst_rh, (int)rh_ctrl_en, step_rh,
                 resolved, current_step_t, current_step_rh);

        /* ----------------------------------------------------------------
         * 7. Compute previous resolved step and apply incremental delta.
         * ---------------------------------------------------------------- */
        int prev_resolved = vent_resolve_conflict(current_step_t,
                                                   (current_step_rh == 0 && !rh_ctrl_en)
                                                       ? VENT_STEP_NEUTRAL
                                                       : current_step_rh,
                                                   (uint8_t)cfg.cr_priority);

        if (resolved != prev_resolved) {
            apply_step_delta(prev_resolved, resolved);
            post_log_mode(resolved, step_t, step_rh);
        }

        /* ----------------------------------------------------------------
         * 8. Update state.
         * ---------------------------------------------------------------- */
        current_step_t  = step_t;
        /* Preserve VENT_STEP_NEUTRAL semantics: if rh returned NEUTRAL,
         * keep the rh step at NEUTRAL so future hysteresis is evaluated
         * correctly from the NEUTRAL baseline. */
        current_step_rh = step_rh;
    }
}
