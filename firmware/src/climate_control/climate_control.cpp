/**
 * @file climate_control.cpp
 * @brief The T6 Climate Control task: everything around the control law —
 *        the inhibit mask, the snapshots, the model call, the commands to T2
 *        and the SD log row. The law itself is `drivers/ventModel`.
 *
 * ## T6 task structure
 *
 * T6 wakes on TN2 (task notification from T4 after each new Q6 reading).
 * On every wake it:
 *   1. Reads EG1 flags; skips evaluation while WIND_OVERRIDE, MOTOR_ALARM,
 *      SENSOR_FAULT_T, STANDBY or CALIBRATING is set (the reason for each is
 *      at step 2 of the task loop).
 *   2. Snapshots cfg_shadow_t under MX4; snapshots sensor_reading_t under MX2.
 *   3. Selects the active T and RH setpoints from is_daytime.
 *   4. Fills `vent_in_t` and calls the control MODEL, which decides. Since
 *      2.12.0 the law lives in `drivers/ventModel` behind
 *      design/ventModelContract.md; this task owns everything around it.
 *   5. Applies the model's answer: per-channel CMD_CLOSE / CMD_OPEN posted to
 *      Q1 for any window whose actual state does not already match the end
 *      state asked for, CLOSE before OPEN. Level-triggered, run on every cycle
 *      so dwell-deferred commands are retried automatically.
 *   6. Logs a MODE_CHANGE event when the resolved step changes.
 *
 * ## State variables
 *
 * One task-local `vent_state_t` — the model's memory, owned by this task, which
 * for `stepped` holds the per-source steps T6 used to keep in two ints. It is
 * reset at boot and whenever an inhibit begins (see step 1), so that when the
 * inhibit clears T6 starts fresh (T2's boot CLOSE_ALL keeps the actual window
 * position known). `last_logged_step` keeps the SD row edge-triggered.
 *
 * ## Q1 command encoding
 *
 * window_cmd_t fields for T6:
 *   .source  = SRC_T6
 *   .action  = CMD_OPEN  / CMD_CLOSE / CMD_CLOSE_ALL
 *   .channel = 1/2/3 for per-channel commands; 0 for CMD_CLOSE_ALL
 *
 * Each cycle, T6 reconciles actual T2 state against the END STATE the model
 * asked for per window: any channel not already in (or moving toward) that
 * state gets a single CMD_OPEN or CMD_CLOSE, every closing command before any
 * opening one. Channels already satisfied get nothing. Level-triggered design
 * means dwell-deferred commands are retried automatically until T2 accepts
 * them. Who decides is the model's business; the ordering and the actuator's
 * limits are this task's (contract §3).
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
#include "vent_model.h"   /* the control law, behind design/ventModelContract.md */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "T6_CLI";

/* -----------------------------------------------------------------------
 * The control law lives in drivers/ventModel — plan §5c, 2.12.0
 *
 * What used to sit here — VENT_STEP_TABLE, step_from_deviation(),
 * vent_step_channels(), vent_step_required_t(), vent_step_required_rh() and
 * vent_resolve_conflict() — is now `vent_model_stepped`, compiled from
 * drivers/ventModel/src through the component proxy. It moved unchanged: the
 * library was written as a faithful copy on 2026-09-17 and kept in step by hand
 * until this release, precisely so this switch could be shown to change no
 * decision (§5c's replay gate).
 *
 * What stays here is the caller's side of the contract: the EG1 inhibit mask,
 * the snapshots, the model call, the actuator limits, the command ORDER
 * (narrowing before widening), Q1, the log row and the state resets.
 *
 * T6 holds exactly one `vent_state_t`. It is the model's memory — for
 * `stepped`, the per-source steps this task used to keep in two ints — and the
 * caller owns it: reset at boot, at an inhibit onset, and on a mode change once
 * mode 2 exists.
 * ----------------------------------------------------------------------- */

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
 *                 apply_model_output()).
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
 * apply_model_output() — drive T2's channel states toward what the law wants
 * ----------------------------------------------------------------------- */

/**
 * @brief Command T2 so each window reaches the end state the model asked for.
 *
 * Was `reconcile_to_step()`, which computed the channel mask itself; the model
 * now returns one desired END STATE per window and this applies them. Called every T6
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
 * @param out   The model's answer: one desired end state per window.
 * @see   t2_get_window_states(), post_q1(), design/ventModelContract.md
 */
static void apply_model_output(const vent_out_t *out, const window_state_t *actual)
{
    /* `actual` is the SAME snapshot the model was given. Reading T2 a second
     * time here would let a window change state between the decision and its
     * command, so a model that asked for a CLOSE could have it silently
     * dropped. The inline law read the states once for exactly this reason. */

    /* Post CLOSE first (narrowing before widening is safer). */
    for (uint8_t ch = 0; ch < 3; ch++) {
        const window_state_t a = actual[ch];
        const bool currently_open_or_opening = (a == WIN_OPEN || a == WIN_MOVING_OPEN);
        if (out->win[ch].action == VENT_ACT_CLOSE && currently_open_or_opening) {
            post_q1(CMD_CLOSE, (uint8_t)(ch + 1));
            ESP_LOGI(TAG, "[T6] → CMD_CLOSE ch=%u (step %d, actual=%d)",
                     (unsigned)(ch + 1), (int)out->step, (int)a);
        }
    }
    for (uint8_t ch = 0; ch < 3; ch++) {
        const window_state_t a = actual[ch];
        const bool currently_closed_or_closing = (a == WIN_CLOSED || a == WIN_MOVING_CLOSE);
        if (out->win[ch].action == VENT_ACT_OPEN && currently_closed_or_closing) {
            post_q1(CMD_OPEN, (uint8_t)(ch + 1));
            ESP_LOGI(TAG, "[T6] → CMD_OPEN  ch=%u (step %d, actual=%d)",
                     (unsigned)(ch + 1), (int)out->step, (int)a);
        }
    }
    /* A TARGET for a digital window is a model error (contract §3). Mode 1
     * never asks for one; the check is here because the contract says the
     * caller enforces the actuator limits, and mode 2 will add the case where
     * a TARGET is legitimate for M3 alone. */
    for (uint8_t ch = 0; ch < 3; ch++) {
        if (out->win[ch].action == VENT_ACT_TARGET) {
            ESP_LOGE(TAG, "[T6] model asked for a TARGET on ch=%u, which this build "
                          "cannot drive — treated as HOLD", (unsigned)(ch + 1));
        }
    }
}

/* -----------------------------------------------------------------------
 * fill_model_input() — everything the law may read, and nothing else
 * ----------------------------------------------------------------------- */

/**
 * @brief Build `vent_in_t` from the snapshots T6 already takes.
 *
 * The contract's §3a: the caller resolves day/night, floors the hysteresis so
 * a law may divide by it, and combines `rh_ctrl_en` with the sensor's validity,
 * so the model never learns where a value came from.
 *
 * Mode 1 reads only a part of this; the rest is filled because the struct is
 * the interface, not this law's parameter list. `cap` is DIGITAL for all three
 * windows in this release: M3 becomes LINEAR in mode 2, from T17's capability
 * through T4's pass-through, and nothing else changes here.
 */
static void fill_model_input(vent_in_t *in, const cfg_shadow_t *cfg,
                             const sensor_reading_t *meas,
                             const window_state_t *actual)
{
    memset(in, 0, sizeof(*in));

    in->now_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    in->unix_time = (uint32_t)dm_get_unix_time();
    in->daytime   = cfg->is_daytime;

    in->t_c10         = meas->temperature_c10;
    in->t_avg_c10     = meas->t_avg_c10;
    in->t_avg_c       = meas->t_avg_c;          /* rounded by the sensor layer */
    in->rh_pct        = meas->humidity_pct;
    in->rh_avg_pct    = meas->rh_avg_pct;
    in->wind_ms10     = meas->wind_speed_ms10;
    in->wind_avg_ms10 = meas->wind_speed_avg_ms10;
    in->wind_dir_deg     = meas->wind_dir_deg;
    in->wind_dir_avg_deg = meas->wind_dir_avg_deg;
    in->wind_dir_var_deg = meas->wind_dir_variation_deg;

    /* T6 is not called at all while SENSOR_FAULT_T is set (the inhibit mask
     * above), so a reading that reaches here is valid. */
    in->t_valid    = true;
    in->rh_valid   = true;
    in->wind_valid = true;

    in->t_max_c10  = (int16_t)((cfg->is_daytime ? cfg->t_max_day  : cfg->t_max_ngt) * 10);
    in->rh_max_pct = (uint8_t)(cfg->is_daytime ? cfg->rh_max_day : cfg->rh_max_ngt);
    in->rh_min_pct = (uint8_t)(cfg->is_daytime ? cfg->rh_min_day : cfg->rh_min_ngt);
    in->hyst_t_c   = (uint8_t)((cfg->hyst_t  > 0) ? cfg->hyst_t  : 1);
    in->hyst_rh_pct= (uint8_t)((cfg->hyst_rh > 0) ? cfg->hyst_rh : 1);
    in->cr_priority = (uint8_t)cfg->cr_priority;
    in->rh_ctrl_en  = (cfg->rh_ctrl_en != 0);

    in->m3_deadzone_x10    = 0u;   /* mode 2 */
    in->m3_min_interval_ms = 0u;   /* mode 2, and its key does not exist yet */

    /* The two enums share their ordinals by design (vent_model.h), and T2
     * gains a part-open state in this release, so pin the mapping here: a
     * renumbering on either side must fail the build, not the greenhouse. */
    static_assert((int)WIN_UNKNOWN      == (int)VENT_WIN_UNKNOWN,      "window state 0");
    static_assert((int)WIN_CLOSED       == (int)VENT_WIN_CLOSED,       "window state 1");
    static_assert((int)WIN_MOVING_OPEN  == (int)VENT_WIN_MOVING_OPEN,  "window state 2");
    static_assert((int)WIN_OPEN         == (int)VENT_WIN_OPEN,         "window state 3");
    static_assert((int)WIN_MOVING_CLOSE == (int)VENT_WIN_MOVING_CLOSE, "window state 4");

    for (int ch = 0; ch < VENT_WINDOWS; ch++) {
        in->win[ch].state           = (vent_win_state_t)actual[ch];
        in->win[ch].cap             = VENT_CAP_DIGITAL;
        in->win[ch].pos_x10         = -1;
        in->win[ch].last_target_x10 = -1;
        in->win[ch].last_result     = VENT_RES_NONE;
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
 *  - prev_inhibited tracks the EG1 inhibit edges so the inhibit-onset reset of
 *    the model's state happens exactly once.
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

    /* The model and its memory. One model in this release -- `stepped`, mode 1
     * -- and T6 owns the state it keeps between calls (contract §2). The reset
     * at boot replaces the two step ints this task used to initialise here;
     * T2's boot CLOSE_ALL still ensures the windows are CLOSED. */
    const vent_model_t *model = vent_model_stepped();
    vent_state_t vstate;
    model->reset(&vstate);

    /* The last step written to the SD log, so the row stays edge-triggered.
     * It replaces recomputing the previous resolved step from the stored
     * per-source steps: the model reports the step it resolved, and the caller
     * logs on a change. The two differ only when cr_priority or rh_ctrl_en
     * changes between cycles, where a row may appear or be suppressed -- no
     * command differs (vent_model_stepped.cpp, "the one deliberate
     * difference").
     *
     * It starts at 0, not VENT_STEP_NONE, and returns to 0 at an inhibit onset:
     * the inline law began every run and every inhibit with current_step_t =
     * current_step_rh = 0, so its recomputed previous step was 0. A -1 baseline
     * would write an extra step-0 row at boot and after every wind override,
     * STANDBY exit and T2 calibration sweep -- rows logparser.py and
     * plot_daily.py read as ventilation decisions. */
    int last_logged_step = 0;

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
         *      WIND_OVERRIDE   — T3 has forced all windows closed
         *      MOTOR_ALARM     — T2 has de-energised all relays (emergency)
         *      SENSOR_FAULT_T  — T/RH sensor unreliable; cannot evaluate T
         *      STANDBY         — rc.1.5.0 (gh#28) operator-initiated pause.
         *                        rc.1.5.1 — also the gate during the admin
         *                        manual-motor menu (gh#29): the menu entry
         *                        auto-sets STANDBY so T6 stays paused for
         *                        as long as the admin is operating, and
         *                        clears it on exit. The transient
         *                        EG1_BIT_MANUAL_SESSION bit that rc.1.5.0
         *                        used has been removed — it cleared too
         *                        quickly (10 s idle dismiss) and let T6
         *                        reopen windows the admin had just closed.
         *      CALIBRATING     — gh#79 (2.9.2): T2 is in its blocking
         *                        CLOSE_ALL sweep (boot, STANDBY exit,
         *                        motor-alarm clearance) and reads Q1 only
         *                        when it returns, up to 176 s in
         *                        production. Posting meanwhile queued up to
         *                        three commands per wake into the 8-deep Q1,
         *                        where a full queue dropped T3's CLOSE_ALL.
         *                        The stale OPENs reached T2 after the sweep,
         *                        and only its stale clock (fixed in 2.9.2)
         *                        deferred them, by accident. T2's
         *                        CMD_RECALIBRATE comment always said T6 was
         *                        gated here; until 2.9.2 it was not.
         * ---------------------------------------------------------------- */
        EventBits_t bits = xEventGroupGetBits(EG1);
        bool inhibited = (bits & (EG1_BIT_WIND_OVERRIDE |
                                  EG1_BIT_MOTOR_ALARM   |
                                  EG1_BIT_SENSOR_FAULT_T|
                                  EG1_BIT_STANDBY       |
                                  EG1_BIT_CALIBRATING)) != 0;

        if (inhibited) {
            if (!prev_inhibited) {
                /* Transition into inhibited state — reset the model's memory
                 * so T6 re-evaluates from scratch when the inhibit clears. An
                 * integrator that survived an inhibit would wind up invisibly
                 * (contract §2), and `stepped`'s close-guard would otherwise
                 * hold a step the windows no longer have. */
                model->reset(&vstate);
                last_logged_step = 0;   /* the inline law's baseline; see above */
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
         * 4-6. Ask the law. Day/night selection, the hysteresis floor and the
         *      validity flags are the caller's job (contract §3a) and live in
         *      fill_model_input(); the decision itself is the model's.
         * ---------------------------------------------------------------- */
        /* One snapshot of T2's window states, shared by the decision and the
         * commands that follow it. */
        window_state_t actual[3];
        t2_get_window_states(actual);

        vent_in_t  in;
        vent_out_t out;
        memset(&out, 0, sizeof(out));
        fill_model_input(&in, &cfg, &meas, actual);
        model->step(&in, &vstate, &out);

        ESP_LOGI(TAG,
                 "[T6] %s: T_avg=%d t_max=%d hyst=%u → step_t=%d | "
                 "RH_avg=%u rh_max=%u rh_min=%u hyst=%u rh_en=%d → step_rh=%d | "
                 "resolved=%d reason=%u (last logged %d)",
                 model->name,
                 (int)in.t_avg_c, (int)(in.t_max_c10 / 10), (unsigned)in.hyst_t_c,
                 (int)out.step_t,
                 (unsigned)in.rh_avg_pct, (unsigned)in.rh_max_pct, (unsigned)in.rh_min_pct,
                 (unsigned)in.hyst_rh_pct, (int)in.rh_ctrl_en, (int)out.step_rh,
                 (int)out.step, (unsigned)out.reason, last_logged_step);

        /* ----------------------------------------------------------------
         * 7. Apply, and log on a change.
         *
         * Level-triggered: every cycle, so commands lost to T2's dwell are
         * retried until they land. The log row stays edge-triggered, and
         * byte-identical to what this task wrote before the model moved out:
         * value_a = the resolved step, value_b = step_t << 8 | step_rh.
         * ---------------------------------------------------------------- */
        apply_model_output(&out, actual);
        if ((int)out.step != last_logged_step) {
            post_log_mode((int)out.step, (int)out.step_t, (int)out.step_rh);
            last_logged_step = (int)out.step;
        }

    }
}
