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
 *   6. Reconciles T2 actual window states to the resolved step's channel
 *      mask: per-channel CMD_CLOSE / CMD_OPEN posted to Q1 for any channel
 *      whose actual state does not already match. Level-triggered, run on
 *      every cycle so dwell-deferred commands are retried automatically.
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
 * Each cycle, T6 reconciles actual T2 state against the desired channel
 * mask: any channel whose actual state does not already match gets a
 * single CMD_OPEN or CMD_CLOSE; channels already in (or moving toward) the
 * desired state get nothing. Level-triggered design means dwell-deferred
 * commands are retried automatically until T2 accepts them.
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
/* alpha.6.10 — dropped <Arduino.h>. T6 has no Arduino-specific calls;
 * FreeRTOS primitives (ulTaskNotifyTake, xQueueSend, xEventGroupGetBits)
 * arrive transitively via app_types.h, and ESP-IDF logging + WDT are
 * already explicit below. */
#include <esp_log.h>
#include <esp_task_wdt.h>   /* WDT subscription (1.17.29 / gh#13) */

#include "climate_control.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../relay_controller/relay_controller.h"   /* t2_get_window_states */

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
 * ----------------------------------------------------------------------- */

/**
 * @brief Compute the required ventilation step from a value's deviation
 *        above its setpoint.
 *
 * Shared by both the temperature and the humidity branches — both demand
 * graduated OPEN with the same integer-ceiling algorithm and the same
 * close-hysteresis guard. The function is purely arithmetic (no I/O, no
 * mutexes) and is safe to call from any context.
 *
 * Step selection:
 *   step_width = max(hyst / NUM_VENT_STEPS, 1)
 *   raw_step   = (deviation > 0) ? ceil(deviation / step_width) : 0
 *   clamped    = clamp(raw_step, 0, NUM_VENT_STEPS)
 *
 * Close-hysteresis guard:
 *   Once any step > 0 is active, do NOT step down to 0 until
 *   deviation <= −hyst  (i.e. value < setpoint_max − hyst). Step reductions
 *   within 1..NUM_VENT_STEPS are applied immediately.
 *
 * @param deviation    value − setpoint_max (may be negative).
 * @param hyst         Hysteresis band; floor-clamped to 1 internally to
 *                     avoid division by zero.
 * @param current_step Step currently commanded; used only for the
 *                     close-hysteresis guard.
 * @return Required step in 0..NUM_VENT_STEPS. 0 means "close"; values >0
 *         denote progressively wider opening per VENT_STEP_TABLE.
 * @note  Returns 1 (not 0) when current_step>0 and the close threshold has
 *        not yet been crossed — a deliberate "stay slightly open" rather
 *        than oscillate around the setpoint.
 */
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

/**
 * @brief Map a step number (0..NUM_VENT_STEPS) to its channel bitmask.
 *
 * Bounds-checks step against the table; returns 0 (all closed) for
 * out-of-range inputs so a corrupt step value cannot drive arbitrary
 * channels.
 *
 * @param step  Step number; 0 = all closed, 1..NUM_VENT_STEPS = lookup.
 * @return      Bitmask combining VENT_CH_M1/M2/M3 bits, or 0.
 */
static uint8_t vent_step_channels(int step)
{
    if (step < 0 || step > NUM_VENT_STEPS) {
        return 0;
    }
    return VENT_STEP_TABLE[step];
}

/**
 * @brief Compute the temperature branch's required ventilation step.
 *
 * Thin wrapper over step_from_deviation(): deviation = t_avg − t_max,
 * hysteresis = hyst_t.
 *
 * @param t_avg        Sliding-average temperature (°C).
 * @param t_max        Active max-temperature setpoint (°C, day or night).
 * @param hyst_t       Temperature hysteresis band (°C, must be >0).
 * @param current_step Step currently commanded for temperature.
 * @return Required step 0..NUM_VENT_STEPS.
 */
static int vent_step_required_t(int16_t t_avg, int16_t t_max, int16_t hyst_t,
                                 int current_step)
{
    int deviation = (int)t_avg - (int)t_max;
    return step_from_deviation(deviation, (int)hyst_t, current_step);
}

/**
 * @brief Compute the humidity branch's required ventilation step.
 *
 * Three branches:
 *   - rh_ctrl_en == false → VENT_STEP_NEUTRAL (RH abstains from voting).
 *   - rh_avg > rh_max     → graduated OPEN, same algorithm as temperature.
 *   - rh_avg < rh_min     → step 0 (full CLOSE; graduated closing not
 *                            implemented — Gap G design decision).
 *   - within band         → VENT_STEP_NEUTRAL.
 *
 * @param rh_avg        Sliding-average humidity (%RH).
 * @param rh_max        Active max-humidity setpoint (%RH).
 * @param rh_min        Active min-humidity setpoint (%RH).
 * @param hyst_rh       Humidity hysteresis band (%RH, must be >0).
 * @param rh_ctrl_en    Master enable for humidity control (cfg.rh_ctrl_en).
 * @param current_step  Step currently commanded for humidity.
 * @return VENT_STEP_NEUTRAL (no vote), 0 (close), or 1..NUM_VENT_STEPS.
 */
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

/**
 * @brief Resolve temperature and humidity step demands to a single step.
 *
 * Four-rule decision tree:
 *   1. RH abstains (VENT_STEP_NEUTRAL) → return step_t.
 *   2. Both demand OPEN (step_t>0 AND step_rh>0) → take the higher step
 *      regardless of cr_priority (more ventilation satisfies both).
 *   3. No conflict (step_t == step_rh) → return either.
 *   4. Genuine conflict (one wants OPEN, the other CLOSE) → apply
 *      cr_priority: 0=T-first, 1=RH-first, 2=deviation-based (higher wins).
 *
 * @param step_t        Temperature branch step (0..NUM_VENT_STEPS).
 * @param step_rh       Humidity branch step (VENT_STEP_NEUTRAL,
 *                      0, or 1..NUM_VENT_STEPS).
 * @param cr_priority   cfg.cr_priority (0/1/2; see cfg_shadow_t).
 * @return Resolved step 0..NUM_VENT_STEPS.
 */
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

/**
 * @brief Build a window_cmd_t with source=SRC_T6 and post it to Q1.
 *
 * Non-blocking send (0-tick timeout). If Q1 is full the command is dropped
 * and a warning is logged — the level-triggered reconciliation loop in T6
 * will retry on the next cycle, so a single drop is recoverable.
 *
 * @param action   CMD_OPEN, CMD_CLOSE, or CMD_CLOSE_ALL (CLOSE_ALL is
 *                 reserved for safety events; T6 itself avoids it — see
 *                 reconcile_to_step()).
 * @param channel  1, 2, 3 for per-channel commands; 0 for CMD_CLOSE_ALL.
 * @warning Caller must keep channel in range [0..3]; T2 logs an error and
 *          drops out-of-range channels.
 */
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
 * ----------------------------------------------------------------------- */

/**
 * @brief Emit a LOG_MODE_CHANGE event to Q3 via log_post().
 *
 * Encodes both per-branch demands plus the resolved step into a single
 * log row so the SD-log parser can reconstruct the full decision context.
 *
 *   value_a = resolved step (0..NUM_VENT_STEPS)
 *   value_b = packed: high byte = step_t, low byte = step_rh
 *
 * Each per-branch step is clamped to int8 range before packing so an
 * out-of-range source value cannot corrupt the int16 encoding.
 *
 * @param resolved_step  Final step posted to T2 (0..NUM_VENT_STEPS).
 * @param step_t         Temperature branch raw demand.
 * @param step_rh        Humidity branch raw demand (may be VENT_STEP_NEUTRAL).
 * @see   log_post()
 */
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
 * reconcile_to_step() — drive T2 channel states toward the desired step
 * ----------------------------------------------------------------------- */

/**
 * @brief Drive T2's per-channel state toward the channel mask for `step`.
 *
 * Replaces the previous edge-triggered apply_step_delta(). Called every T6
 * cycle (level-triggered) so that commands lost to T2's post-open/close
 * dwell are re-issued automatically once dwell expires. The previous
 * delta-only design dropped any CMD_CLOSE that arrived while a window was
 * still in its post-open dwell, leaving windows stuck OPEN until the next
 * step transition; reconciling every cycle removes that failure mode.
 *
 * Sequence: CLOSE-first then OPEN. Narrowing before widening keeps the
 * total open area monotone-decreasing in transient states — safer when a
 * step transition is interrupted (e.g. wind override fires mid-cycle).
 *
 * Idempotency is provided by T2's ch_start_open() / ch_start_close()
 * (relay_controller.cpp): a CMD_OPEN posted while the channel is already
 * OPEN/MOVING_OPEN/GAP_TO_OPEN is a no-op; likewise CMD_CLOSE on a channel
 * already CLOSED/MOVING_CLOSE/GAP_TO_CLOSE. Posting a command for the
 * opposite direction during travel triggers the standard 2 s reversal gap.
 *
 * CMD_CLOSE_ALL is intentionally NOT used here. CMD_CLOSE_ALL bypasses the
 * per-channel post-open dwell, causing rapid close after a brief opening —
 * undesirable when temperature rebounds quickly. CMD_CLOSE_ALL is reserved
 * for safety events (wind override in T3, motor alarm in T2).
 *
 * @param step  Target step 0..NUM_VENT_STEPS; out-of-range maps to mask 0.
 * @see   t2_get_window_states(), vent_step_channels(), post_q1()
 */
static void reconcile_to_step(int step)
{
    window_state_t actual[3];
    t2_get_window_states(actual);

    uint8_t desired = vent_step_channels(step);

    /* Post CLOSE first (narrowing before widening is safer). */
    for (uint8_t ch = 0; ch < 3; ch++) {
        bool want_open = ((desired >> ch) & 1u) != 0;
        window_state_t a = actual[ch];
        bool currently_open_or_opening = (a == WIN_OPEN || a == WIN_MOVING_OPEN);
        if (!want_open && currently_open_or_opening) {
            post_q1(CMD_CLOSE, (uint8_t)(ch + 1));
            ESP_LOGI(TAG, "[T6] → CMD_CLOSE ch=%u (target step %d, actual=%d)",
                     (unsigned)(ch + 1), step, (int)a);
        }
    }
    for (uint8_t ch = 0; ch < 3; ch++) {
        bool want_open = ((desired >> ch) & 1u) != 0;
        window_state_t a = actual[ch];
        bool currently_closed_or_closing = (a == WIN_CLOSED || a == WIN_MOVING_CLOSE);
        if (want_open && currently_closed_or_closing) {
            post_q1(CMD_OPEN, (uint8_t)(ch + 1));
            ESP_LOGI(TAG, "[T6] → CMD_OPEN  ch=%u (target step %d, actual=%d)",
                     (unsigned)(ch + 1), step, (int)a);
        }
    }
}

/* -----------------------------------------------------------------------
 * T6 task — Climate Control (Phase 6)
 * ----------------------------------------------------------------------- */

/**
 * @brief T6 task entry point — see climate_control.h for the full
 *        per-wake sequence and EG1 inhibit semantics.
 *
 * Implementation notes (not duplicated in the header):
 *  - Subscribes to esp_task_wdt with a 2 s TN2-wait timeout so the WDT is
 *    kicked even when the sensor poll interval is long (up to 3600 s).
 *  - Reconciliation is level-triggered every wake (dwell-deferred T2
 *    commands are retried automatically). Mode-change logging stays
 *    edge-triggered so the SD log keeps one row per actual transition.
 *  - prev_inhibited tracks the EG1 inhibit edges so the inhibit-onset
 *    reset of current_step_t/rh happens exactly once.
 *
 * @param pvParameters Unused; pass NULL.
 */
void task_climate_control(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to WDT (1.17.29 / gh#13). T4 notifies T6 on each sensor
     * reading (interval 30–3600 s) — too sparse for portMAX_DELAY under a
     * 5 s WDT. Use a 2 s receive timeout; on timeout reset WDT and continue
     * (no notification, no work). */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "[T6] task alive");

    /* Task-local state — tracks last commanded step from each source.
     * Initialised to 0; T2's boot CLOSE_ALL ensures windows are CLOSED. */
    int current_step_t  = 0;
    int current_step_rh = 0;

    /* Track whether we were inhibited on the previous cycle so we can log
     * mode transitions (inhibit onset / inhibit clearance). */
    bool prev_inhibited = false;

    for (;;) {
        esp_task_wdt_reset();
        /* ----------------------------------------------------------------
         * 1. Block on TN2 — T4 notifies after every new Q6 reading.
         *    2 s timeout: T6 has nothing to do between sensor updates, but
         *    must wake periodically to kick the WDT (1.17.29).
         * ---------------------------------------------------------------- */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
            continue;
        }

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

        /* Level-triggered: reconcile every cycle so dwell-deferred commands
         * are retried until they land. Mode-change logging stays edge-
         * triggered to preserve event-log semantics. */
        reconcile_to_step(resolved);
        if (resolved != prev_resolved) {
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
