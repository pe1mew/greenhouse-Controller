/**
 * @file climate_control.h
 * @brief Graduated ventilation step table, evaluation functions, and T6 task.
 *
 * ## Subsystem role
 * T6 is the project's *climate-control* task. It owns no hardware directly;
 * it consumes the latest sensor reading (via the MX2-protected snapshot
 * published by T4 on Q6 reception) and the configuration shadow (MX4-protected,
 * also owned by T4), then posts open/close commands to the relay-controller
 * task T2 via Q1. T6 wakes only when T4 notifies it (TN2) after each new
 * sensor reading — its cadence is therefore the sensor poll interval
 * (`poll_interval_s`, typically 30–3600 s).
 *
 * ## Position in the task graph
 * T4 (Data Manager) → TN2 → T6 (Climate Control) → Q1 → T2 (Relay Controller)
 *                                          ↓
 *                                          Q3 → T9 (Event Logger)
 *
 * EG1 also gates T6 evaluation: while WIND_OVERRIDE (T3), MOTOR_ALARM or
 * CALIBRATING (T2), SENSOR_FAULT_T (T5) or STANDBY (the operator, via T4) is
 * set, T6 produces no commands — the flag's owner controls window position.
 *
 * ## Graduated ventilation (FR-C09, FR-C10, Gap G)
 *
 * Windows are opened in up to NUM_VENT_STEPS cumulative steps proportional
 * to the deviation of the measured value from the active setpoint.  The
 * channel assignment per step is a compile-time table in the law
 * (drivers/ventModel/src/vent_model_stepped.cpp since 2.12.0):
 *
 *   Step 1 — M1 only
 *   Step 2 — M1 + M2
 *   Step 3 — M1 + M2 + M3
 *
 * ### Step selection algorithm
 * The hysteresis band (hyst_t or hyst_rh) is divided evenly into
 * NUM_VENT_STEPS sub-bands above the setpoint:
 *
 *   step_width = hyst / NUM_VENT_STEPS   (minimum 1)
 *
 *   deviation = value - setpoint_max
 *   required_step = clamp( ceil(deviation / step_width), 0, NUM_VENT_STEPS )
 *
 * ### Close-hysteresis guard
 * Once any step > 0 is active, T6 will NOT reduce to step 0 until the
 * measured value drops below (setpoint_max − hyst).  This prevents
 * oscillation near the setpoint.  Step reductions within the active
 * range (1 → 2 → 1, etc.) are applied immediately.
 *
 * ### RH close demand (Gap G design decision)
 * Humidity control can demand either OPEN (RH > RH_max) or CLOSE
 * (RH < RH_min).  When humidity demands CLOSE the required step is
 * always 0 — graduated closing is not implemented.  This keeps conflict
 * resolution symmetric: T and RH demands are both expressed as a step
 * number (0 = close, 1–N = open at step N) with the special sentinel
 * VENT_STEP_NEUTRAL (−1) meaning "no demand from this source."
 *
 * ### State T6 must maintain
 * T6 keeps one `vent_state_t` — the model's memory, owned by the caller
 * (contract §2).  For `stepped` it holds the per-source steps this task used
 * to keep in two task-local integers (current_step_t, current_step_rh).  It is
 * reset when any EG1 inhibit begins (see the task's Inhibit behaviour), which
 * reproduces those two integers going to 0; T2's boot CLOSE_ALL ensures actual
 * window positions are known.  T6 also keeps `last_logged_step`, which is its
 * own and not the model's: it keeps the SD row edge-triggered.
 *
 * ### Incremental command posting
 * T6 computes the delta between the current channel mask and the newly
 * resolved channel mask and posts only the changed channels to Q1:
 *   - Channels in new_mask  & ~cur_mask → CMD_OPEN
 *   - Channels in cur_mask  & ~new_mask → CMD_CLOSE
 *   - new_mask == 0 (full close)        → CMD_CLOSE_ALL (single command)
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include "../types/app_types.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Sentinel returned by vent_step_required_rh() when RH is within
 *        the acceptable range and neither open nor close is demanded.
 *
 * vent_resolve_conflict() treats this as "RH has no vote" — the temperature
 * step wins unconditionally. Distinguishing NEUTRAL from a genuine `step=0`
 * close demand is essential: a NEUTRAL RH should NOT veto temperature-driven
 * opening, but a genuine close demand should.
 */
#define VENT_STEP_NEUTRAL  (-1)

/* -----------------------------------------------------------------------
 * Channel bitmask helpers (bit 0 = M1, bit 1 = M2, bit 2 = M3)
 * ----------------------------------------------------------------------- */

/** @brief Bitmask bit for motor channel M1 (window 1). */
#define VENT_CH_M1  (1u << 0)
/** @brief Bitmask bit for motor channel M2 (window 2). */
#define VENT_CH_M2  (1u << 1)
/** @brief Bitmask bit for motor channel M3 (window 3). */
#define VENT_CH_M3  (1u << 2)

/* -----------------------------------------------------------------------
 * T6 task entry point
 * ----------------------------------------------------------------------- */

/**
 * @brief T6 — Climate Control task (Phase 6, fully implemented).
 *
 * Wakes on TN2 (ulTaskNotifyTake from T4 after every new Q6 reading).
 *
 * ### Per-wake sequence
 *  1. **EG1 gate** — skips evaluation while any inhibit is set (see Inhibit
 *     behaviour below).  Resets the model's state on inhibit onset so
 *     re-evaluation starts from step 0 when the flag clears.
 *  2. **Snapshot** — dm_cfg_snapshot() under MX4; dm_meas_snapshot() under MX2.
 *  3. **Setpoint selection** — selects t_max, rh_max, rh_min from is_daytime.
 *  4. **The decision** — fill_model_input() fills a `vent_in_t` and the model
 *     decides. Since 2.12.0 the law is `drivers/ventModel`, behind
 *     design/ventModelContract.md, not code in this module: step evaluation
 *     and conflict resolution moved there unchanged, and the formulas above
 *     are what `vent_model_stepped` computes.
 *  5. **Apply** — apply_model_output(): every T6 cycle, compare the model's
 *     desired END STATE per window against the T2 states the model was given,
 *     and post CMD_CLOSE / CMD_OPEN for any channel not already there.
 *     Level-triggered, so commands lost to T2 dwell are retried until they
 *     take effect.  Every CLOSE first, then the OPENs.
 *  6. **Logging** — LOG_MODE_CHANGE posted to Q3 only on step changes.
 *  7. **State update** — the model's `vent_state_t`, owned by this task.
 *
 * ### Inhibit behaviour
 * While any of EG1 WIND_OVERRIDE, MOTOR_ALARM, SENSOR_FAULT_T, STANDBY or
 * CALIBRATING is set, T6 posts nothing to Q1. T3 and T2 issue their own
 * CLOSE_ALL for the override and the motor alarm respectively. CALIBRATING
 * joined the list in 2.9.2 (gh#79): T2 reads Q1 only between its blocking
 * sweeps, so what T6 posted during one piled up in the 8-deep queue. T6 steps
 * are reset to 0 when an inhibit begins, so that on clearance it re-evaluates
 * from scratch.
 *
 * @param pvParameters  Unused; pass NULL.
 * @note   T6 subscribes to the task watchdog (esp_task_wdt_add). The TN2
 *         wait uses a 2 s timeout so the WDT is kicked even when sensor
 *         readings are sparse (poll interval can be up to 3600 s).
 * @warning T6 must run AFTER T2 and T4 are created (it queries
 *          t2_get_window_states() and calls dm_*_snapshot()).
 * @see    task_data_manager()  — produces the TN2 notification
 * @see    task_relay_controller() — owner of Q1 and the window state machine
 * @see    t2_get_window_states() — used for level-triggered reconciliation
 */
void task_climate_control(void *pvParameters);

/**
 * @brief T6's record of M3's last target and how it ended (2026-09-21).
 *
 * What the law is told in `win[2].last_target_x10` / `last_result`, for the
 * bench diag route. Read without a lock: the fields are written by T6 alone,
 * each is atomic, and a reader between two of them sees at worst a new target
 * with the previous result for one poll -- acceptable for a diagnostic.
 */
typedef struct {
    int16_t  last_target_x10;   /**< -1 = none commanded since boot */
    uint8_t  last_result;       /**< vent_result_t: 0 NONE (outstanding),
                                 *   1 DONE, 2 FAIL_TIMEOUT, 3 FAIL_FAULT,
                                 *   4 ABORTED */
    uint32_t taken_at_post;     /**< t2_get_taken(M3) when it went out */
} cc_m3_target_t;

void cc_get_m3_target(cc_m3_target_t *out);
