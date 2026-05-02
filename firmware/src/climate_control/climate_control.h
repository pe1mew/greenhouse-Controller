/**
 * @file climate_control.h
 * @brief Graduated ventilation step table, evaluation functions, and T6 task.
 *
 * ## Graduated ventilation (FR-C09, FR-C10, Gap G)
 *
 * Windows are opened in up to NUM_VENT_STEPS cumulative steps proportional
 * to the deviation of the measured value from the active setpoint.  The
 * channel assignment per step is a compile-time table in climate_control.cpp:
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
 * T6 keeps two task-local static integers (current_step_t, current_step_rh)
 * that track the step last commanded.  Both are reset to 0 on entry to
 * WIND_OVERRIDE or MANUAL_OVERRIDE; T2's boot CLOSE_ALL ensures actual
 * window positions are known.
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

/* -----------------------------------------------------------------------
 * Sentinel value returned by vent_step_required_rh() when RH is within
 * the acceptable range and neither open nor close is demanded.
 * vent_resolve_conflict() treats this as "RH has no vote".
 * ----------------------------------------------------------------------- */
#define VENT_STEP_NEUTRAL  (-1)

/* -----------------------------------------------------------------------
 * Channel bitmask helpers (bit 0 = M1, bit 1 = M2, bit 2 = M3)
 * ----------------------------------------------------------------------- */
#define VENT_CH_M1  (1u << 0)
#define VENT_CH_M2  (1u << 1)
#define VENT_CH_M3  (1u << 2)

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/**
 * @brief Return the relay channel bitmask for a ventilation step.
 *
 * Bit 0 = M1, bit 1 = M2, bit 2 = M3.
 *
 * @param step  0 = no channels (fully closed);
 *              1..NUM_VENT_STEPS = cumulative channel set.
 * @return      Channel bitmask, or 0 for step == 0 or out-of-range.
 */
uint8_t vent_step_channels(int step);

/**
 * @brief Compute the required ventilation step from temperature.
 *
 * Steps up immediately when T_avg exceeds T_max in increments of
 * (hyst_t / NUM_VENT_STEPS).  Steps down within the active range
 * (1–NUM_VENT_STEPS) immediately.  Allows step-down to 0 only when
 * T_avg has fallen below (T_max − hyst_t) — the close-hysteresis guard.
 *
 * Temperature control only ever demands OPEN (step > 0) or NEUTRAL
 * (step 0 held by hysteresis guard until close threshold is reached).
 * It never produces VENT_STEP_NEUTRAL; it always returns 0..NUM_VENT_STEPS.
 *
 * @param t_avg         Sliding-average temperature, integer °C.
 * @param t_max         Upper temperature setpoint, integer °C.
 * @param hyst_t        Temperature hysteresis band, integer °C (> 0).
 * @param current_step  Step currently commanded (0..NUM_VENT_STEPS).
 * @return              Required step (0..NUM_VENT_STEPS).
 */
int vent_step_required_t(int16_t t_avg, int16_t t_max, int16_t hyst_t,
                          int current_step);

/**
 * @brief Compute the required ventilation step from humidity.
 *
 * Three distinct cases:
 *
 *   RH > RH_max (too humid)   → graduated OPEN, same algorithm as temperature.
 *   RH < RH_min (too dry)     → returns 0 (full close demand).
 *                                Graduated closing is NOT implemented (Gap G
 *                                design decision): closing is always to step 0
 *                                to keep conflict resolution symmetric.
 *   RH in [RH_min, RH_max]    → returns VENT_STEP_NEUTRAL (−1): RH has no
 *                                demand; vent_resolve_conflict() will ignore it.
 *   rh_ctrl_en == false        → always returns VENT_STEP_NEUTRAL.
 *
 * @param rh_avg        Sliding-average relative humidity, integer %.
 * @param rh_max        Upper humidity setpoint, integer %.
 * @param rh_min        Lower humidity setpoint, integer %.
 * @param hyst_rh       Humidity hysteresis band, integer % (> 0).
 * @param rh_ctrl_en    True if humidity-based control is enabled.
 * @param current_step  Step currently commanded (0..NUM_VENT_STEPS).
 * @return              VENT_STEP_NEUTRAL, 0 (close demand), or
 *                      1..NUM_VENT_STEPS (open demand).
 */
int vent_step_required_rh(int16_t rh_avg, int16_t rh_max, int16_t rh_min,
                           int16_t hyst_rh, bool rh_ctrl_en,
                           int current_step);

/**
 * @brief Resolve temperature and humidity ventilation demands into one step.
 *
 * Rules applied in order:
 *  1. If step_rh == VENT_STEP_NEUTRAL: return step_t unchanged.
 *  2. If both step_t > 0 and step_rh > 0 (both want OPEN): return the
 *     higher step regardless of cr_priority — both demands are satisfied
 *     by more ventilation.
 *  3. If step_t == step_rh: no conflict, return as-is.
 *  4. Genuine conflict (one OPEN, one CLOSE) — apply cr_priority:
 *       0 = CR_TEMP_FIRST  : temperature wins.
 *       1 = CR_RH_FIRST    : humidity wins (step_rh, which may be 0).
 *       2 = CR_DEVIATION   : higher step wins (more ventilation).
 *
 * @param step_t      Temperature step (0..NUM_VENT_STEPS).
 * @param step_rh     Humidity step (VENT_STEP_NEUTRAL, 0, or 1..NUM_VENT_STEPS).
 * @param cr_priority Conflict resolution mode from NVS `climate/cr_priority`.
 * @return            Resolved step (0..NUM_VENT_STEPS).
 */
int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority);

/* -----------------------------------------------------------------------
 * T6 task entry point
 * ----------------------------------------------------------------------- */

/**
 * @brief T6 — Climate Control task.
 *
 * Wakes on TN2 (new sensor data from T4) and TN3 (manual override from T2).
 * On each wake:
 *   1. Checks EG1 flags; skips evaluation if WIND_OVERRIDE, MANUAL_OVERRIDE,
 *      or SENSOR_FAULT_T is set.
 *   2. Reads T_avg, RH_avg, is_daytime, setpoints, hyst values from T4 (MX4).
 *   3. Calls vent_step_required_t() and vent_step_required_rh().
 *   4. Calls vent_resolve_conflict() to get the final step.
 *   5. Calls vent_step_channels() to convert step → channel bitmask.
 *   6. Posts incremental CMD_OPEN / CMD_CLOSE commands for changed channels
 *      to Q1 (or CMD_CLOSE_ALL if new step is 0).
 *   7. Updates current_step_t and current_step_rh.
 *
 * On TN3 (manual override): resets both current steps to 0, posts
 * CMD_CLOSE_ALL to Q1, starts calibration timer, then resumes above loop.
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_climate_control(void *pvParameters);
