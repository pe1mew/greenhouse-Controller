/**
 * @file climate_control.cpp
 * @brief Graduated ventilation implementation — step table, evaluation
 *        functions, conflict resolution, and T6 task stub.
 *
 * @author  Greenhouse Controller project
 */

#include "climate_control.h"
#include "../types/app_types.h"
#include <stdint.h>
#include <stdbool.h>

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
 * Public API implementations
 * ----------------------------------------------------------------------- */

uint8_t vent_step_channels(int step)
{
    if (step < 0 || step > NUM_VENT_STEPS) {
        return 0;
    }
    return VENT_STEP_TABLE[step];
}

int vent_step_required_t(int16_t t_avg, int16_t t_max, int16_t hyst_t,
                          int current_step)
{
    int deviation = (int)t_avg - (int)t_max;
    return step_from_deviation(deviation, (int)hyst_t, current_step);
}

int vent_step_required_rh(int16_t rh_avg, int16_t rh_max, int16_t rh_min,
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

int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority)
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
 * T6 task — stub (Phase 6 implementation)
 *
 * Full implementation: Phase 6 of firmwareImplementationPlan.md.
 * The stub keeps the scheduler happy and holds the task in a blocked state.
 * ----------------------------------------------------------------------- */

void task_climate_control(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 6: block on TN2 / TN3; read MX2 + MX4; call evaluation
     * functions; post incremental window commands to Q1; update current steps. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
