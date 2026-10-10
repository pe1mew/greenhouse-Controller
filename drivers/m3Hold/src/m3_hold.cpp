/**
 * @file m3_hold.cpp
 * @brief The LCD's tap-or-hold timing for M3 (gh#93). See m3_hold.h.
 *
 * Every duration is an unsigned difference of two 32-bit millisecond stamps,
 * so the tick counter wrapping (after 49.7 days of uptime) changes nothing.
 */
#include "m3_hold.h"

static m3h_out_t nothing(void)
{
    m3h_out_t o = { M3H_NOTHING, false, 0u };
    return o;
}

void m3h_reset(m3_hold_t *h)
{
    h->active   = false;
    h->opening  = false;
    h->key      = '\0';
    h->moving   = false;
    h->repeated = false;
    h->t0_ms    = 0u;
    h->last_ms  = 0u;
}

/* The press ends at `at_ms`: what it amounted to. */
static m3h_out_t end_press(m3_hold_t *h, uint32_t at_ms)
{
    m3h_out_t o;
    o.opening = h->opening;
    o.held_ms = (uint32_t)(at_ms - h->t0_ms);
    if (h->moving) {
        o.action = M3H_MOVE_STOP;
    } else if (o.held_ms < M3H_TAP_MAX_MS) {
        o.action = M3H_FULL;
    } else {
        /* 1 to 2 s, or 2 s and more without the move having started: the press
         * is over, so the move it would have made is nothing. */
        o.action = M3H_MID;
    }
    m3h_reset(h);
    return o;
}

/* No sign of the key for longer than T7's cadence allows: it was released. */
static bool lost(const m3_hold_t *h, uint32_t now_ms)
{
    if (h->repeated) {
        return (uint32_t)(now_ms - h->last_ms) > M3H_LOST_MS;
    }
    return (uint32_t)(now_ms - h->t0_ms) > (M3H_FIRST_REPEAT_MS + M3H_LOST_MS);
}

static m3h_out_t start_if_due(m3_hold_t *h, uint32_t now_ms)
{
    const uint32_t held = (uint32_t)(now_ms - h->t0_ms);
    if (h->moving || held < M3H_HOLD_MS) {
        return nothing();
    }
    h->moving = true;
    m3h_out_t o = { M3H_MOVE_START, h->opening, held };
    return o;
}

m3h_out_t m3h_press(m3_hold_t *h, char key, bool opening, uint32_t now_ms)
{
    m3h_out_t o = nothing();
    if (h->active) {
        o = end_press(h, h->last_ms);       /* its release never reached us */
    }
    h->active   = true;
    h->opening  = opening;
    h->key      = key;
    h->moving   = false;
    h->repeated = false;
    h->t0_ms    = now_ms;
    h->last_ms  = now_ms;
    return o;
}

m3h_out_t m3h_repeat(m3_hold_t *h, char key, uint32_t now_ms)
{
    if (!h->active || key != h->key) {
        return nothing();
    }
    h->repeated = true;
    h->last_ms  = now_ms;
    return start_if_due(h, now_ms);
}

m3h_out_t m3h_release(m3_hold_t *h, char key, uint32_t now_ms)
{
    if (!h->active || key != h->key) {
        return nothing();
    }
    return end_press(h, now_ms);
}

m3h_out_t m3h_tick(m3_hold_t *h, uint32_t now_ms)
{
    if (!h->active) {
        return nothing();
    }
    if (lost(h, now_ms)) {
        return end_press(h, h->last_ms);
    }
    return start_if_due(h, now_ms);
}
