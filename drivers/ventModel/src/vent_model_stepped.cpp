/**
 * @file vent_model_stepped.cpp
 * @brief Mode 1 — the stepped ventilation law, behind the model interface.
 *
 * ## Version 2 (gh#84, 2026-09-27): the humidity branch and the conflict rule
 *
 * v1 let a dry house close every window against any heat demand under
 * `cr_priority` 1, and let humidity open the house at night under 1 and 2
 * with nothing to stop it cooling the crop. Under 0 humidity could never open
 * a window on its own. v2, prototyped in the simulator before this change
 * (`model/closedloop/humidityControl.md`, "The package, prototyped"):
 *
 * 1. **Dryness never closes against heat.** If the temperature wants to open
 *    and humidity votes 0 (below `rh_min`), the temperature's step wins under
 *    every priority. As a consequence the "too dry" vote can no longer change
 *    any decision, so `rh_min` is inert under every priority, as it already
 *    was under 0.
 * 2. **Humidity may open the house on its own** only under priority 1 or 2,
 *    and only from `t_min` + STEPPED_FLOOR_MARGIN_C. An opening that is
 *    already live holds down to STEPPED_FLOOR_HYST_C below that floor.
 * 3. **Capped at M1:** humidity alone never reaches M2 or M3.
 * 4. **A close guard at `rh_max`**: a live humidity vote holds at 1 until the
 *    average is max(hyst_rh / 3, 1) under the ceiling. The branch had none.
 * 5. **Priority 2 behaves exactly as priority 1.** Rule 1 removed the only
 *    conflict in which they differed.
 *
 * At priority 0 v2 decides exactly as v1 (rule 1 returns what priority 0
 * returned; humidity alone is still refused). The constants are the law's
 * own, provisional until they are keys (contract §3, "Adding a tunable").
 *
 * ## Provenance
 *
 * v1 was extracted from the live firmware,
 * `firmware/src/climate_control/climate_control.cpp` at 2.9.1 (commit
 * aed852c), with the arithmetic untouched. **The line numbers below refer to
 * that commit.** The file no longer holds these functions: 2.12.0 moved T6
 * onto this library and deleted its inline copy, so this is the only copy.
 *
 * | here | there, at aed852c |
 * |---|---|
 * | `VENT_STEP_TABLE`          | `:80`  |
 * | `step_from_deviation()`    | `:121` |
 * | `vent_step_channels()`     | `:175` |
 * | `vent_step_required_t()`   | `:195` |
 * | `vent_step_required_rh()`  | `:220` (v2 adds the close guard at `rh_max`) |
 * | `vent_resolve_conflict()`  | `:263` (v2 replaces the priority switch) |
 * | the want-open vs actual comparison in `step()` | `reconcile_to_step()`, `:401` |
 *
 * The replay (`model/vent_step_replay.py`) scores this law against real SD
 * logs, and must still reproduce 5C88's logged decisions: at priority 0 v2
 * decides as v1 did.
 *
 * ## What it does not do
 *
 * No `VENT_ACT_TARGET`: the stepped law is binary on all three windows, which
 * is exactly what makes it mode 1. No safety, no queue, no logging, no clock —
 * see the contract.
 *
 * ## Part-open windows (interface 2)
 *
 * A window at rest part-open (`VENT_WIN_PART_OPEN`, M3 under mode 2) is at
 * neither end, so this law drives it to the end it wants, in both directions,
 * and never HOLDs it: holding would leave M3 part-open while the step says
 * OPEN.
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
/* ---------------------------------------------------------------------------
 * v2's constants (gh#84). The law's own, provisional until they are keys.
 * ------------------------------------------------------------------------- */
#define STEPPED_FLOOR_MARGIN_C  2   /* humidity may open alone from t_min + this */
#define STEPPED_FLOOR_HYST_C    1   /* a live humidity-only opening holds this far
                                     * below the floor, so the whole-degree
                                     * average does not flicker M1 across it */
#define STEPPED_RH_ALONE_CAP    1   /* a humidity-only opening: M1 at most */

#define ST_STEP_T   0   /* last temperature step this model commanded  */
#define ST_STEP_RH  1   /* last humidity step this model commanded     */
#define ST_STEP      2  /* last resolved step, kept for the caller's log */

/** Why the model decided what it did. Reported in `vent_out_t::reason`,
 *  which T6 prints to the serial console. The SD log does not carry it: T6's
 *  row holds only step, step_t and step_rh (contract §3, "Logging"). Append
 *  only, never renumber, all the same: graded reports these in its low bits. */
enum {
    STEPPED_REASON_NO_DATA   = 0,  /**< no valid temperature — nothing decided */
    STEPPED_REASON_T_ONLY    = 1,  /**< humidity cast no vote */
    STEPPED_REASON_BOTH_OPEN = 2,  /**< both wanted open; the higher step won */
    STEPPED_REASON_AGREE     = 3,  /**< both asked for the same step */
    STEPPED_REASON_CONFLICT  = 4,  /**< one open, one closed: v2's rules decided
                                    *   (dryness yields to heat; humidity alone
                                    *   opens under 1 or 2, floored and capped) */
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
 * @brief Humidity branch. Extracted from climate_control.cpp:220 at aed852c.
 *
 * Disabled → no vote. Above max → graduated open, same algorithm as
 * temperature. v2: a live vote then holds at 1 until the average is
 * max(hyst_rh / 3, 1) under max (rule 4, the close guard the branch never
 * had). Below min → step 0, a close demand; since v2 it can change no
 * decision (rule 1). Within the band → no vote.
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

    /* v2 rule 4: a live vote holds at step 1 until the average is the
     * ladder's own step width below the ceiling. */
    {
        const int band = ((int)hyst_rh / VENT_STEPS_MAX < 1) ? 1
                       : (int)hyst_rh / VENT_STEPS_MAX;
        if (current_step > 0 && (int)rh_avg > (int)rh_max - band) {
            return 1;
        }
    }

    if (rh_avg < rh_min) {
        return 0;
    }

    return VENT_STEP_NONE;
}

/**
 * @brief Resolve the two demands into one step. Extracted from
 *        climate_control.cpp:263 at aed852c; v2 replaces the priority switch.
 *
 * 1. Humidity abstains → temperature's step.
 * 2. Both want open → the higher step, whatever `cr_priority` says.
 * 3. They agree → that step.
 * 4. v2 rule 1: the temperature wants to open and humidity votes 0 (too dry)
 *    → the temperature's step, under every priority.
 * 5. What is left is humidity alone wanting to open: priority 0 → no (0);
 *    priority 1 or 2 → min(step_rh, STEPPED_RH_ALONE_CAP) if `floor_ok`,
 *    else 0.
 *
 * @param floor_ok  the temperature is at or above the humidity floor (see
 *                  stepped_step()); only case 5 reads it.
 */
static int vent_resolve_conflict(int step_t, int step_rh, uint8_t cr_priority,
                                 bool floor_ok)
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

    /* v2 rule 1 (gh#84): dryness never closes against heat. */
    if (step_t > 0 && step_rh == 0) {
        return step_t;
    }

    /* What is left: the temperature is idle and humidity wants to open. */
    if (cr_priority != 1 && cr_priority != 2) {
        return step_t;                              /* 0: temperature first */
    }
    /* Rules 2, 3 and 5: 1 and 2 alike -- above the floor, M1 at most. */
    if (!floor_ok) {
        return step_t;
    }
    return (step_rh < STEPPED_RH_ALONE_CAP) ? step_rh : STEPPED_RH_ALONE_CAP;
}

/**
 * @brief Classify which branch of vent_resolve_conflict() applied.
 *
 * Separate from the resolver on purpose, as it was when the resolver was a
 * byte-for-byte copy of the firmware's: the reason is derived alongside the
 * decision rather than woven into it. Both of v2's new cases (rule 1, and
 * humidity alone) still classify as CONFLICT, one axis open and one closed.
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

    /* v2 rule 2: humidity may open the house on its own only from t_min +
     * STEPPED_FLOOR_MARGIN_C. The reading lags the air by minutes, T5
     * averages on top, and an unheated house keeps cooling after M1 shuts, so
     * stopping at t_min itself lets the house fall below it. An opening
     * already live (the last decision opened on humidity alone) holds down to
     * STEPPED_FLOOR_HYST_C below the floor. Both slots are zero after
     * reset(), so no opening is live after a reset. t_min is a whole number
     * of degrees, so the division is exact. */
    const int  t_floor    = (int)(in->t_min_c10 / 10) + STEPPED_FLOOR_MARGIN_C;
    const bool alone_live = st->v[ST_STEP] > 0 && st->v[ST_STEP_T] == 0;
    const bool floor_ok   = (int)in->t_avg_c >= t_floor ||
        (alone_live && (int)in->t_avg_c >= t_floor - STEPPED_FLOOR_HYST_C);

    const int resolved = vent_resolve_conflict(step_t, step_rh, in->cr_priority,
                                               floor_ok);
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
    2,          /* v2 (gh#84): the humidity branch and the conflict rule */
    stepped_reset,
    stepped_step,
};

const vent_model_t *vent_model_stepped(void)
{
    return &s_stepped;
}
