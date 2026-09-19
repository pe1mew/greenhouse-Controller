/**
 * @file vent_model_stepped.cpp
 * @brief Mode 1 — the stepped ventilation law, behind the model interface.
 *
 * ## Provenance: this is a COPY, not a rewrite
 *
 * Every decision function below is copied from the live firmware,
 * `firmware/src/climate_control/climate_control.cpp` at 2.9.1 (commit
 * aed852c), with the arithmetic untouched:
 *
 * | here | there |
 * |---|---|
 * | `VENT_STEP_TABLE`          | `:80`  |
 * | `step_from_deviation()`    | `:121` |
 * | `vent_step_channels()`     | `:175` |
 * | `vent_step_required_t()`   | `:195` |
 * | `vent_step_required_rh()`  | `:220` |
 * | `vent_resolve_conflict()`  | `:263` |
 * | the want-open vs actual comparison in `step()` | `reconcile_to_step()`, `:401` |
 *
 * **The firmware is unaffected and still runs its own inline copy.** The two
 * are kept in step by hand until T6 is refactored onto this interface (2.11.0,
 * plan §5b/§5c). Change one, change the other, and re-run both this library's
 * host tests and the replay.
 *
 * ## Why it exists before it is used
 *
 * 1. It is the reference implementation of the contract — the shape a new
 *    model copies.
 * 2. It is the equivalence baseline: when T6 does move onto this interface,
 *    this model must reproduce the logged decisions of real SD data at least as
 *    well as today's replay does (96.8 % of 378 decisions), and reproduce
 *    `LOG_MODE_CHANGE` param 0 byte for byte. A difference in mode 2 can then
 *    be attributed to the new law rather than to the refactor.
 *
 * ## What it does not do
 *
 * No `VENT_ACT_TARGET`: the stepped law is binary on all three windows, which
 * is exactly what makes it mode 1. No safety, no queue, no logging, no clock —
 * see the contract.
 *
 * ## The one deliberate difference from the firmware
 *
 * The firmware recomputes the *previous* resolved step each cycle from its
 * stored per-source steps and logs when that differs (`climate_control.cpp:580`).
 * Here the model reports the step it resolved and the caller logs on change.
 * The two agree except in one case: when `cr_priority` or `rh_ctrl_en` changes
 * between two cycles, the recomputed "previous" step can shift, so a log row
 * may appear or be suppressed. **No command differs** — only the timing of a
 * log row, in a case that needs an operator to change a setting mid-run.
 *
 * ## One addition the firmware does not have yet
 *
 * `VENT_WIN_PART_OPEN` (interface 2). The firmware has no part-open state until
 * T2 gains one in 2.11.0, so no firmware input maps to it and the equivalence
 * above is untouched. A part-open window is at neither end, so mode 1 drives it
 * to the end it wants, in both directions, and never HOLDs it: holding would
 * leave M3 part-open while the step says OPEN. When T2 gains the state,
 * `reconcile_to_step()` must do the same.
 */

#include "vent_model.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Local mirrors of firmware constants
 *
 * Deliberately re-declared rather than included: this file may not see
 * project headers (contract §4). Keep the values identical to
 * `climate_control.h` (VENT_CH_*) and `app_types.h` (NUM_VENT_STEPS).
 * ------------------------------------------------------------------------- */
#define VENT_CH_M1  (1u << 0)
#define VENT_CH_M2  (1u << 1)
#define VENT_CH_M3  (1u << 2)

/** Step → channel-mask table. Index 0 is step 0 (all closed); 1..3 are the
 *  cumulative masks. Copied from climate_control.cpp:80. */
static const uint8_t VENT_STEP_TABLE[VENT_STEPS_MAX + 1] = {
    0,                                     /* step 0 — all closed  */
    VENT_CH_M1,                            /* step 1 — M1 only     */
    VENT_CH_M1 | VENT_CH_M2,               /* step 2 — M1 + M2     */
    VENT_CH_M1 | VENT_CH_M2 | VENT_CH_M3,  /* step 3 — M1 + M2 + M3 */
};

/* ---------------------------------------------------------------------------
 * Model state layout
 *
 * `vent_state_t` is an opaque int32 array, so name the slots here. Zeroed
 * state is the firmware's own boot state: `current_step_t = 0`,
 * `current_step_rh = 0` (climate_control.cpp:464), so a reset() reproduces a
 * reboot exactly.
 * ------------------------------------------------------------------------- */
#define ST_STEP_T   0   /* last temperature step this model commanded  */
#define ST_STEP_RH  1   /* last humidity step this model commanded     */
#define ST_STEP      2  /* last resolved step, kept for the caller's log */

/** Why the model decided what it did. Reported in `vent_out_t::reason` and
 *  written to the SD log, so these codes are part of the log's meaning: append
 *  only, never renumber. */
enum {
    STEPPED_REASON_NO_DATA   = 0,  /**< no valid temperature — nothing decided */
    STEPPED_REASON_T_ONLY    = 1,  /**< humidity cast no vote */
    STEPPED_REASON_BOTH_OPEN = 2,  /**< both wanted open; the higher step won */
    STEPPED_REASON_AGREE     = 3,  /**< both asked for the same step */
    STEPPED_REASON_CONFLICT  = 4,  /**< one open, one closed; cr_priority decided */
};

/* ---------------------------------------------------------------------------
 * The decision functions — copied verbatim in behaviour
 * ------------------------------------------------------------------------- */

/**
 * @brief Required step from a value's deviation above its setpoint.
 *
 * Copy of climate_control.cpp:121.
 *
 *   step_width = max(hyst / VENT_STEPS_MAX, 1)
 *   raw_step   = (deviation > 0) ? ceil(deviation / step_width) : 0
 *   clamped    = clamp(raw_step, 0, VENT_STEPS_MAX)
 *
 * Close-hysteresis guard: once any step > 0 is active, do not step down to 0
 * until deviation <= -hyst. Reductions within 1..max are immediate. The guard
 * returning 1 rather than 0 is the "stay slightly open rather than oscillate"
 * decision, and it is why `hyst_t` governs both the step width and the close
 * margin — the conflation the campaign quantified as a bad trade (F8).
 */
static int step_from_deviation(int deviation, int hyst, int current_step)
{
    int step_width = hyst / VENT_STEPS_MAX;
    if (step_width < 1) {
        step_width = 1;
    }

    int raw_step;
    if (deviation <= 0) {
        raw_step = 0;
    } else {
        /* Integer ceiling for positive integers: (a + b - 1) / b */
        raw_step = (deviation + step_width - 1) / step_width;
    }

    if (raw_step > VENT_STEPS_MAX) {
        raw_step = VENT_STEPS_MAX;
    }
    if (raw_step < 0) {
        raw_step = 0;
    }

    if (current_step > 0 && raw_step == 0) {
        if (deviation > -hyst) {
            return 1;           /* not yet below the close threshold */
        }
    }

    return raw_step;
}

/** Step → channel bitmask, bounds-checked. Copy of climate_control.cpp:175. */
static uint8_t vent_step_channels(int step)
{
    if (step < 0 || step > VENT_STEPS_MAX) {
        return 0;
    }
    return VENT_STEP_TABLE[step];
}

/** Temperature branch. Copy of climate_control.cpp:195 — whole °C in, as the
 *  firmware compares whole degrees. */
static int vent_step_required_t(int16_t t_avg, int16_t t_max, int16_t hyst_t,
                                int current_step)
{
    int deviation = (int)t_avg - (int)t_max;
    return step_from_deviation(deviation, (int)hyst_t, current_step);
}

/**
 * @brief Humidity branch. Copy of climate_control.cpp:220.
 *
 * Disabled → no vote. Above max → graduated open, same algorithm as
 * temperature. Below min → step 0, a genuine close demand (graduated closing
 * is deliberately not implemented). Within the band → no vote.
 */
static int vent_step_required_rh(int16_t rh_avg, int16_t rh_max, int16_t rh_min,
                                 int16_t hyst_rh, bool rh_ctrl_en,
                                 int current_step)
{
    if (!rh_ctrl_en) {
        return VENT_STEP_NONE;
    }

    if (rh_avg > rh_max) {
        int deviation = (int)rh_avg - (int)rh_max;
        return step_from_deviation(deviation, (int)hyst_rh, current_step);
    }

    if (rh_avg < rh_min) {
        return 0;
    }

    return VENT_STEP_NONE;
}

/**
 * @brief Resolve the two demands into one step. Copy of climate_control.cpp:263.
 *
 * 1. Humidity abstains → temperature's step.
 * 2. Both want open → the higher step, whatever `cr_priority` says.
 * 3. They agree → that step.
 * 4. Genuine conflict → `cr_priority`: 0 = temperature, 1 = humidity,
 *    2 = the higher step.
 */
static int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority)
{
    if (step_rh == VENT_STEP_NONE) {
        return step_t;
    }

    if (step_t > 0 && step_rh > 0) {
        return (step_t > step_rh) ? step_t : step_rh;
    }

    if (step_t == step_rh) {
        return step_t;
    }

    switch (cr_priority) {
        case 0:  /* temperature first */
        default:
            return step_t;

        case 1:  /* humidity first */
            return step_rh;

        case 2:  /* the larger demand wins */
            return (step_t > step_rh) ? step_t : step_rh;
    }
}

/**
 * @brief Classify which branch of vent_resolve_conflict() applied.
 *
 * Separate from the resolver on purpose: the resolver stays a byte-for-byte
 * copy of the firmware's, and the reason code is derived alongside it rather
 * than woven into it. Reading the same conditions twice costs nothing here and
 * keeps the copied function auditable against its original.
 */
static uint8_t resolve_reason(int step_t, int step_rh)
{
    if (step_rh == VENT_STEP_NONE)    { return STEPPED_REASON_T_ONLY; }
    if (step_t > 0 && step_rh > 0)    { return STEPPED_REASON_BOTH_OPEN; }
    if (step_t == step_rh)            { return STEPPED_REASON_AGREE; }
    return STEPPED_REASON_CONFLICT;
}

/* ---------------------------------------------------------------------------
 * The interface
 * ------------------------------------------------------------------------- */

static void stepped_reset(vent_state_t *st)
{
    if (st == NULL) { return; }
    memset(st, 0, sizeof(*st));
}

static void stepped_step(const vent_in_t *in, vent_state_t *st, vent_out_t *out)
{
    if (in == NULL || st == NULL || out == NULL) { return; }

    memset(out, 0, sizeof(*out));
    for (int i = 0; i < VENT_WINDOWS; i++) {
        out->win[i].action     = VENT_ACT_HOLD;
        out->win[i].target_x10 = -1;
    }
    out->step    = VENT_STEP_NONE;
    out->step_t  = VENT_STEP_NONE;
    out->step_rh = VENT_STEP_NONE;

    /* The firmware never reaches its decision without a valid measurement: T6
     * skips the cycle (climate_control.cpp:533) and a T/RH sensor fault
     * inhibits the task entirely. Hold everything and say why. */
    if (!in->t_valid) {
        out->reason = STEPPED_REASON_NO_DATA;
        return;
    }

    /* Setpoints arrive in 0.1 °C; the stepped law works in whole degrees, and
     * a threshold is always a whole number of degrees, so this division is
     * exact. The measured average comes ready-rounded in t_avg_c — see the
     * field's comment in vent_model.h for why it is not derived here. */
    const int16_t t_max   = (int16_t)(in->t_max_c10 / 10);
    const int16_t hyst_t  = (int16_t)((in->hyst_t_c   > 0) ? in->hyst_t_c   : 1);
    const int16_t hyst_rh = (int16_t)((in->hyst_rh_pct > 0) ? in->hyst_rh_pct : 1);

    /* Humidity needs its own validity: the firmware's inhibit mask covers the
     * T/RH sensor as one unit, so rh_valid false here means "do not vote". */
    const bool rh_en = in->rh_ctrl_en && in->rh_valid;

    const int step_t  = vent_step_required_t((int16_t)in->t_avg_c, t_max, hyst_t,
                                             (int)st->v[ST_STEP_T]);
    const int step_rh = vent_step_required_rh((int16_t)in->rh_avg_pct,
                                              (int16_t)in->rh_max_pct,
                                              (int16_t)in->rh_min_pct,
                                              hyst_rh, rh_en,
                                              (int)st->v[ST_STEP_RH]);

    const int resolved = vent_resolve_conflict(step_t, step_rh, in->cr_priority);
    const uint8_t mask = vent_step_channels(resolved);

    /* Desired end state per window. The caller orders the commands (every
     * narrowing move before any widening one) and drops the ones already
     * satisfied — copied from reconcile_to_step()'s comparison, which is why a
     * window in UNKNOWN is left alone: its position is not established, and
     * T2's boot calibration is what resolves that. */
    for (int ch = 0; ch < VENT_WINDOWS; ch++) {
        const bool want_open = ((mask >> ch) & 1u) != 0;
        const vent_win_state_t a = in->win[ch].state;

        /* PART_OPEN is at neither end, so it goes to whichever one is wanted. */
        const bool part = (a == VENT_WIN_PART_OPEN);
        if (!want_open && (a == VENT_WIN_OPEN || a == VENT_WIN_MOVING_OPEN || part)) {
            out->win[ch].action = VENT_ACT_CLOSE;
        } else if (want_open && (a == VENT_WIN_CLOSED || a == VENT_WIN_MOVING_CLOSE || part)) {
            out->win[ch].action = VENT_ACT_OPEN;
        } else {
            out->win[ch].action = VENT_ACT_HOLD;
        }
    }

    /* The log fields. step/step_t/step_rh keep LOG_MODE_CHANGE param 0
     * byte-identical; the continuous demands carry the same steps, so a reader
     * of mode 2's row sees a comparable figure from either model. */
    out->step          = (int8_t)resolved;
    out->step_t        = (int8_t)step_t;
    out->step_rh       = (int8_t)step_rh;
    out->reason        = resolve_reason(step_t, step_rh);
    out->demand_t_x10  = (int16_t)(step_t * 10);
    out->demand_rh_x10 = (int16_t)((step_rh == VENT_STEP_NONE) ? -1 : step_rh * 10);

    st->v[ST_STEP_T]  = step_t;
    st->v[ST_STEP_RH] = step_rh;
    st->v[ST_STEP]    = resolved;
}

static const vent_model_t s_stepped = {
    "stepped",
    1,
    stepped_reset,
    stepped_step,
};

const vent_model_t *vent_model_stepped(void)
{
    return &s_stepped;
}
