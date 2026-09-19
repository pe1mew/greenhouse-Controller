/**
 * @file vent_model_graded.cpp
 * @brief Mode 2 — a first candidate for the graded law: M1 and M2 as mode 1,
 *        M3 proportional with a rate limit.
 *
 * ## Status: a CANDIDATE, not a decision
 *
 * Which law mode 2 runs is the plan's open decision 9. This is the simplest
 * candidate the plan names — "a proportional map with a rate limit" — written
 * so the closed-loop simulator (model/closedloop/) can score it against the
 * stepped law. Its constants are provisional: the contract (§3, "Adding a
 * tunable of your own") says to keep them in this file until they are keys.
 *
 * ## The law in one paragraph (contract §9 item 1)
 *
 * The embedded stepped law decides the step exactly as mode 1 does: the same
 * temperature and humidity branches, hysteresis, conflict rule and reason. M1
 * and M2 follow its first two steps, window for window. M3 opens only from
 * step 2 on. Its aperture is proportional to the temperature excess above
 * t_max: shut at +1.5 °C, fully open at +3.5 °C, so half open where mode 1
 * would open it fully. Humidity that demands step 3 on its own opens M3 fully,
 * as in mode 1. A slow actuator with a late reading must not be chased: M3
 * is moved only for a correction of at least 10 %, by at most 25 % per move,
 * and not within 10 minutes of its last drive. That covers the loop's dead
 * time, how long the controller's reading takes to show what a move did: the
 * sensor lags the air by 3.5-5.5 min (NS-10), T5 averages on top of that, and
 * a 25 % move takes 44 s in production. In the closed loop 5 min damped the
 * swing about as well, with a third more M3 drives
 * (model/closedloop/gradedCandidate.md).
 *
 * ## Why it embeds the stepped law
 *
 * A difference against mode 1 is then M3's doing and nothing else's, and no
 * arithmetic is copied: M1 and M2's thresholds, the close guard, the humidity
 * branch and cr_priority are the stepped model's own code, called through its
 * public interface. Its memory lives in this model's state (slots 0..5); a
 * host test runs both in lockstep to catch the day it needs more.
 *
 * ## Written for API 1 and API 2
 *
 * It reads neither the minimum-interval field (the caller enforces the
 * interval; its name and width change in API 2) nor any window state beyond
 * those API 1 has. M3's decisions come from pos_x10, and M1 and M2's come
 * from the stepped model.
 */

#include "vent_model.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * The candidate's constants. Provisional: keys when mode 2 is adopted.
 * ------------------------------------------------------------------------- */
#define M3_START_C10       15      /* M3 starts to open at t_max + 1.5 °C (0.1 °C) */
#define M3_FULL_C10        35      /* and is fully open at t_max + 3.5 °C */
#define M3_MOVE_MIN_X10    100     /* no correction under 10 %, except to an end */
#define M3_STEP_MAX_X10    250     /* the rate limit: at most 25 % per move */
#define M3_HOLD_MS         600000u /* and no move within 10 min of the last drive */
#define M3_POS_STALE_MS    90000u  /* T17 reads every 30 s at rest; older is stale */

/* ---------------------------------------------------------------------------
 * State: the stepped model's memory, then ours. Zeroed = boot: no target held.
 * ------------------------------------------------------------------------- */
#define STEPPED_SLOTS      6       /* v[0..5], the embedded stepped model's own */
#define ST_M3_TGT          6       /* M3's target, 0.1 % */
#define ST_M3_HAVE         7       /* 1 once a target is held */

/* Why, as logged (append only): stepped's reason in the low bits, M3's in the high. */
#define GRADED_M3_DIGITAL  0x10u   /* M3 is not linear: mode 1's M3 */
#define GRADED_M3_NO_POS   0x20u   /* position unknown or stale: M3 held */
#define GRADED_M3_HELD     0x40u   /* a move is due but within the hold time */
#define GRADED_M3_REBASED  0x80u   /* the last command timed out or was taken over */

static int clamp_i(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/** M3's temperature demand: the aperture, 0.1 %, for an excess in 0.1 °C. */
static int m3_aperture_for(int excess_c10)
{
    return clamp_i(((excess_c10 - M3_START_C10) * 1000) / (M3_FULL_C10 - M3_START_C10),
                   0, 1000);
}

/**
 * @brief M3 on its position: where to send it, rate-limited.
 *
 * Returns the desired end state: HOLD when M3 is where it should be or when
 * its position cannot be trusted, else TARGET, repeated on every call until
 * M3 is there (contract §3, "Ordering").
 */
static vent_win_out_t m3_linear(const vent_in_t *in, vent_state_t *st, int want,
                                uint8_t *flags)
{
    vent_win_out_t o = { VENT_ACT_HOLD, -1 };
    const vent_win_in_t *w = &in->win[2];

    if (w->pos_x10 < 0 || w->pos_x10 > 1000 || w->pos_age_ms > M3_POS_STALE_MS ||
        w->state == VENT_WIN_UNKNOWN) {
        *flags |= GRADED_M3_NO_POS;                 /* decide nothing on a guess */
        return o;
    }
    if (w->state == VENT_WIN_MOVING_OPEN || w->state == VENT_WIN_MOVING_CLOSE) {
        if (st->v[ST_M3_HAVE]) {                    /* on its way: repeat, never chase */
            o.action = VENT_ACT_TARGET;
            o.target_x10 = (int16_t)st->v[ST_M3_TGT];
        }
        return o;
    }

    /* At rest. Take the position as the target when none is held (after a
     * reset), or when the last command did not arrive: nothing is re-issued
     * blind, and the rules below decide afresh from where M3 is. */
    if (w->last_result == VENT_RES_FAIL_TIMEOUT || w->last_result == VENT_RES_ABORTED) {
        *flags |= GRADED_M3_REBASED;
        st->v[ST_M3_TGT] = w->pos_x10;
        st->v[ST_M3_HAVE] = 1;
    } else if (!st->v[ST_M3_HAVE]) {
        st->v[ST_M3_TGT] = w->pos_x10;
        st->v[ST_M3_HAVE] = 1;
    }

    const int tgt = (int)st->v[ST_M3_TGT];
    const int gap = want - tgt;
    const bool worth = (gap >= M3_MOVE_MIN_X10 || -gap >= M3_MOVE_MIN_X10 ||
                        (gap != 0 && (want == 0 || want == 1000)));
    if (worth) {
        if (w->ms_since_move < M3_HOLD_MS) {
            *flags |= GRADED_M3_HELD;
        } else {
            st->v[ST_M3_TGT] = tgt + clamp_i(gap, -M3_STEP_MAX_X10, M3_STEP_MAX_X10);
        }
    }

    const int now_tgt = (int)st->v[ST_M3_TGT];
    const int off = (now_tgt > w->pos_x10) ? now_tgt - w->pos_x10 : w->pos_x10 - now_tgt;
    if (off <= (int)in->m3_deadzone_x10) {
        return o;                                   /* there: the caller would drop it */
    }
    o.action = VENT_ACT_TARGET;
    o.target_x10 = (int16_t)now_tgt;
    return o;
}

/* ---------------------------------------------------------------------------
 * The interface
 * ------------------------------------------------------------------------- */

static void graded_reset(vent_state_t *st)
{
    if (st == NULL) { return; }
    memset(st, 0, sizeof(*st));
}

static void graded_step(const vent_in_t *in, vent_state_t *st, vent_out_t *out)
{
    if (in == NULL || st == NULL || out == NULL) { return; }

    /* The stepped model decides the step, on its own memory. */
    vent_state_t sub;
    memset(&sub, 0, sizeof(sub));
    memcpy(sub.v, st->v, STEPPED_SLOTS * sizeof(sub.v[0]));
    vent_model_stepped()->step(in, &sub, out);
    memcpy(st->v, sub.v, STEPPED_SLOTS * sizeof(sub.v[0]));

    if (!in->t_valid) {
        return;                                     /* stepped held everything: NO_DATA */
    }

    /* M1 and M2 keep stepped's decision; M3 is ours. */
    const int a_t = m3_aperture_for((int)in->t_avg_c10 - (int)in->t_max_c10);
    const int a_rh = (out->step_rh == VENT_STEP_NONE) ? -1
                   : ((out->step_rh >= VENT_STEPS_MAX) ? 1000 : 0);
    uint8_t flags = 0u;

    if (in->win[2].cap != VENT_CAP_LINEAR) {
        flags |= GRADED_M3_DIGITAL;                 /* stepped's M3, as in mode 1 */
    } else {
        int want = 0;
        if (out->step >= 2) {
            want = (a_rh > a_t) ? a_rh : a_t;
        }
        out->win[2] = m3_linear(in, st, want, &flags);
    }

    out->reason = (uint8_t)(out->reason | flags);
    out->demand_t_x10 = (int16_t)a_t;
    out->demand_rh_x10 = (int16_t)a_rh;
}

static const vent_model_t s_graded = {
    "graded",
    1,
    graded_reset,
    graded_step,
};

const vent_model_t *vent_model_graded(void)
{
    return &s_graded;
}
