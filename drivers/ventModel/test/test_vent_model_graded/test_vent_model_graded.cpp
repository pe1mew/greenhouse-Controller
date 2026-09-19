/**
 * @file test_vent_model_graded.cpp
 * @brief Host tests for mode 2's first candidate, the graded law. No hardware.
 *
 * Run: cd drivers/ventModel && pio test -e native
 *
 * The contract's §5 list first, then what is particular to this candidate:
 * the rate limit, the hold time, the smallest correction, and the lockstep
 * check that M1 and M2 are the stepped law's, window for window.
 *
 * Defaults: t_max 28 °C, hyst_t 5, so the stepped law opens M1 at 29, M2 at
 * 30 and M3 at 31 °C (whole degrees). This candidate's M3 is shut up to
 * 29.5 °C and fully open from 31.5 °C: 250 at 30.0, 500 at 30.5, 750 at 31.0.
 * M3 is linear, at rest and shut, with its deadband at 1.3 % (13).
 */

#include <unity.h>

#include <string.h>

#include "vent_model.h"

static const vent_model_t *G;
static vent_state_t        S;

#define LONG_AGO 0xFFFFFFFFu

void setUp(void)
{
    G = vent_model_graded();
    G->reset(&S);
}

void tearDown(void) {}

static void set_t(vent_in_t *in, int c10)
{
    in->t_c10 = (int16_t)c10;
    in->t_avg_c10 = (int16_t)c10;
    in->t_avg_c = (int16_t)((c10 + 5) / 10);    /* the sensor layer rounds */
}

/** Cool, humidity abstaining, every window shut; M3 linear at rest. */
static vent_in_t base_in(void)
{
    vent_in_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms      = 1000u;
    in.unix_time   = 1758000000u;
    in.daytime     = true;
    in.t_valid     = true;
    in.rh_valid    = true;
    in.wind_valid  = true;
    set_t(&in, 200);
    in.rh_avg_pct  = 70;
    in.rh_pct      = 70;
    in.t_max_c10   = 280;
    in.rh_max_pct  = 85;
    in.rh_min_pct  = 60;
    in.hyst_t_c    = 5;
    in.hyst_rh_pct = 5;
    in.cr_priority = 0;
    in.rh_ctrl_en  = false;
    in.m3_deadzone_x10 = 13;
    for (int i = 0; i < VENT_WINDOWS; i++) {
        in.win[i].state           = VENT_WIN_CLOSED;
        in.win[i].cap             = VENT_CAP_DIGITAL;
        in.win[i].pos_x10         = -1;
        in.win[i].last_target_x10 = -1;
        in.win[i].last_result     = VENT_RES_NONE;
        in.win[i].ms_since_move   = LONG_AGO;
    }
    in.win[2].cap = VENT_CAP_LINEAR;
    in.win[2].pos_x10 = 0;
    in.win[2].pos_age_ms = 1000u;
    return in;
}

static vent_out_t run(const vent_in_t *in)
{
    vent_out_t out;
    memset(&out, 0, sizeof(out));
    G->step(in, &S, &out);
    return out;
}

/** M3 at rest where it stopped, the drive done `ago_ms` ago. */
static void m3_at(vent_in_t *in, int pos, uint32_t ago_ms)
{
    in->win[2].state = (pos == 0) ? VENT_WIN_CLOSED : VENT_WIN_OPEN;
    in->win[2].pos_x10 = (int16_t)pos;
    in->win[2].last_result = VENT_RES_DONE;
    in->win[2].ms_since_move = ago_ms;
}

#define M3_TARGET(out, t) do { \
    TEST_ASSERT_EQUAL_INT(VENT_ACT_TARGET, (out).win[2].action); \
    TEST_ASSERT_EQUAL_INT((t), (out).win[2].target_x10); } while (0)

/* ------------------------------------------------------ the contract's list */

static void test_below_threshold_closes_nothing(void)
{
    vent_in_t in = base_in();
    vent_out_t out = run(&in);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[i].action);
    }
    TEST_ASSERT_EQUAL_INT(0, out.step);
    TEST_ASSERT_EQUAL_INT(0, out.demand_t_x10);
}

static void test_rising_ramp(void)
{
    vent_in_t in = base_in();

    set_t(&in, 290);                                /* step 1: M1 only */
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[1].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);

    set_t(&in, 300);                                /* step 2: M2, and M3 starts */
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[1].action);
    TEST_ASSERT_EQUAL_INT(250, out.demand_t_x10);
    M3_TARGET(out, 250);

    in.win[2].state = VENT_WIN_MOVING_OPEN;         /* on its way: never chase */
    set_t(&in, 310);
    out = run(&in);
    M3_TARGET(out, 250);

    m3_at(&in, 240, 60000u);                        /* arrived; 750 wanted, held */
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    TEST_ASSERT_TRUE(out.reason & 0x40u);

    in.win[2].ms_since_move = 600001u;              /* hold over: one rate-limited step */
    out = run(&in);
    M3_TARGET(out, 500);
}

static void test_falling_ramp_with_hysteresis(void)
{
    vent_in_t in = base_in();
    set_t(&in, 315);                                /* all the way up */
    for (int pos = 0; pos < 1000; ) {
        vent_out_t out = run(&in);
        TEST_ASSERT_EQUAL_INT(VENT_ACT_TARGET, out.win[2].action);
        pos = out.win[2].target_x10;
        m3_at(&in, pos, LONG_AGO);
    }
    in.win[0].state = VENT_WIN_OPEN;
    in.win[1].state = VENT_WIN_OPEN;

    set_t(&in, 300);                                /* back to 250: 25 % per move */
    vent_out_t out = run(&in);
    M3_TARGET(out, 750);
    m3_at(&in, 750, LONG_AGO);
    out = run(&in);
    M3_TARGET(out, 500);
    m3_at(&in, 500, LONG_AGO);
    out = run(&in);
    M3_TARGET(out, 250);

    m3_at(&in, 250, LONG_AGO);
    set_t(&in, 260);                                /* step 1 under the close guard */
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(1, out.step);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[0].action);      /* M1 stays open */
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, out.win[1].action);
    M3_TARGET(out, 0);                              /* an end: no smallest step applies */

    set_t(&in, 230);                                /* -5 °C: the guard lets go */
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, out.win[0].action);
}

static void test_humidity_alone_opens_m3_fully(void)
{
    vent_in_t in = base_in();
    in.rh_ctrl_en = true;
    in.rh_avg_pct = 95;                             /* step 3 on humidity */
    in.cr_priority = 2;
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(3, out.step_rh);
    TEST_ASSERT_EQUAL_INT(1000, out.demand_rh_x10);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[1].action);
    M3_TARGET(out, 250);                            /* towards 1000, rate-limited */
}

static void test_conflict_follows_cr_priority(void)
{
    const int want[3] = { 250, -1, 250 };           /* -1: M3 stays shut */
    for (int cr = 0; cr < 3; cr++) {
        setUp();
        vent_in_t in = base_in();
        set_t(&in, 310);                            /* temperature: step 3 */
        in.rh_ctrl_en = true;
        in.rh_avg_pct = 40;                         /* humidity: too dry, close */
        in.cr_priority = (uint8_t)cr;
        vent_out_t out = run(&in);
        TEST_ASSERT_EQUAL_INT(0, out.step_rh);
        if (want[cr] < 0) {
            TEST_ASSERT_EQUAL_INT(0, out.step);
            TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
        } else {
            TEST_ASSERT_EQUAL_INT(3, out.step);
            M3_TARGET(out, want[cr]);
        }
    }
}

static void test_invalid_temperature_holds_everything(void)
{
    vent_in_t in = base_in();
    set_t(&in, 330);
    in.t_valid = false;
    vent_out_t out = run(&in);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[i].action);
    }
    TEST_ASSERT_EQUAL_INT(0, out.reason);           /* NO_DATA */
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, out.step);
    G->step(NULL, &S, &out);                        /* and no crash on NULL */
    G->step(&in, NULL, &out);
    G->step(&in, &S, NULL);
}

static void test_unknown_or_stale_position_holds_m3(void)
{
    vent_in_t in = base_in();
    set_t(&in, 310);
    in.win[2].pos_x10 = -1;
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    TEST_ASSERT_TRUE(out.reason & 0x20u);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[0].action);      /* M1, M2 still decide */

    in = base_in();
    set_t(&in, 310);
    in.win[2].pos_age_ms = 120000u;
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    TEST_ASSERT_TRUE(out.reason & 0x20u);
}

static void test_digital_m3_is_mode_one(void)
{
    vent_in_t in = base_in();
    in.win[2].cap = VENT_CAP_DIGITAL;
    in.win[2].pos_x10 = -1;
    set_t(&in, 310);
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, out.win[2].action);      /* stepped's M3 */
    TEST_ASSERT_TRUE(out.reason & 0x10u);
    set_t(&in, 300);
    out = run(&in);
    TEST_ASSERT_NOT_EQUAL(VENT_ACT_TARGET, out.win[2].action);
}

static void test_fail_timeout_rebases_on_the_position(void)
{
    vent_in_t in = base_in();
    set_t(&in, 310);                                /* 750 wanted */
    in.win[2].state = VENT_WIN_OPEN;
    in.win[2].pos_x10 = 300;
    in.win[2].last_target_x10 = 600;
    in.win[2].last_result = VENT_RES_FAIL_TIMEOUT;
    in.win[2].ms_since_move = 10000u;
    vent_out_t out = run(&in);                      /* held: from 300, not re-sent 600 */
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    TEST_ASSERT_TRUE(out.reason & 0x80u);
    in.win[2].ms_since_move = 600001u;
    out = run(&in);
    M3_TARGET(out, 550);
}

/* ------------------------------------------------------ this candidate's rules */

static void test_rate_limit_steps_to_fully_open(void)
{
    vent_in_t in = base_in();
    set_t(&in, 320);                                /* 1000 wanted */
    const int expect[4] = { 250, 500, 750, 1000 };
    for (int k = 0; k < 4; k++) {
        vent_out_t out = run(&in);
        M3_TARGET(out, expect[k]);
        m3_at(&in, expect[k], LONG_AGO);
    }
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);      /* there */
}

static void test_small_correction_is_not_made(void)
{
    vent_in_t in = base_in();
    m3_at(&in, 500, LONG_AGO);                      /* after reset: 500 is the target */
    set_t(&in, 306);                                /* 550 wanted: only 5 % off */
    in.win[0].state = VENT_WIN_OPEN;
    in.win[1].state = VENT_WIN_OPEN;
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    TEST_ASSERT_EQUAL_INT(550, out.demand_t_x10);
}

static void test_within_the_deadband_is_held(void)
{
    vent_in_t in = base_in();
    set_t(&in, 300);
    vent_out_t out = run(&in);
    M3_TARGET(out, 250);
    m3_at(&in, 238, 1000u);                         /* stopped 12 short: in the band */
    out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
    m3_at(&in, 230, 1000u);                         /* 20 short: ask again */
    out = run(&in);
    M3_TARGET(out, 250);
}

static void test_reset_takes_the_position_as_target(void)
{
    vent_in_t in = base_in();
    set_t(&in, 320);
    run(&in);                                       /* holds a target of 250 */
    G->reset(&S);
    m3_at(&in, 700, LONG_AGO);
    set_t(&in, 310);                                /* 750 wanted, 700 is close */
    in.win[0].state = VENT_WIN_OPEN;
    in.win[1].state = VENT_WIN_OPEN;
    vent_out_t out = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, out.win[2].action);
}

/** M1 and M2 are the stepped law's, and so are the steps: in lockstep over a
 *  long pseudo-random run, which also shows the stepped model still keeps its
 *  memory within the slots this one carries for it. Today its decisions read
 *  slot 0 only (the temperature step): its humidity branch calls the close
 *  guard only above rh_max, where it never engages. Carrying none of its
 *  memory made 2 033 of 20 000 steps differ (2026-09-19); carrying slot 0 alone
 *  made none. */
static void test_m1_m2_and_steps_are_stepped_in_lockstep(void)
{
    const vent_model_t *st = vent_model_stepped();
    vent_state_t s_st;
    st->reset(&s_st);
    uint32_t r = 12345u;
    const vent_win_state_t states[5] = { VENT_WIN_UNKNOWN, VENT_WIN_CLOSED,
                                         VENT_WIN_MOVING_OPEN, VENT_WIN_OPEN,
                                         VENT_WIN_MOVING_CLOSE };
    for (int k = 0; k < 20000; k++) {
        vent_in_t in = base_in();
        r = r * 1103515245u + 12345u;
        set_t(&in, 150 + (int)((r >> 8) % 240u));
        in.rh_ctrl_en = ((r >> 3) & 1u) != 0u;
        in.rh_avg_pct = (uint8_t)(30 + (r >> 12) % 70u);
        in.cr_priority = (uint8_t)((r >> 20) % 3u);
        in.hyst_t_c = (uint8_t)(2 + (r >> 24) % 8u);
        in.t_valid = ((r >> 28) % 50u) != 0u;
        for (int i = 0; i < 2; i++) {
            in.win[i].state = states[(r >> (4 + 3 * i)) % 5u];
        }
        in.win[2].pos_x10 = (int16_t)((r >> 16) % 1001u);
        in.win[2].state = VENT_WIN_OPEN;
        vent_out_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        st->step(&in, &s_st, &a);
        G->step(&in, &S, &b);
        TEST_ASSERT_EQUAL_INT(a.win[0].action, b.win[0].action);
        TEST_ASSERT_EQUAL_INT(a.win[1].action, b.win[1].action);
        TEST_ASSERT_EQUAL_INT(a.step, b.step);
        TEST_ASSERT_EQUAL_INT(a.step_t, b.step_t);
        TEST_ASSERT_EQUAL_INT(a.step_rh, b.step_rh);
        TEST_ASSERT_EQUAL_INT(a.reason, b.reason & 0x0F);
    }
}

static void test_identity(void)
{
    TEST_ASSERT_EQUAL_STRING("graded", G->name);
    TEST_ASSERT_EQUAL_INT(1, G->version);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_below_threshold_closes_nothing);
    RUN_TEST(test_rising_ramp);
    RUN_TEST(test_falling_ramp_with_hysteresis);
    RUN_TEST(test_humidity_alone_opens_m3_fully);
    RUN_TEST(test_conflict_follows_cr_priority);
    RUN_TEST(test_invalid_temperature_holds_everything);
    RUN_TEST(test_unknown_or_stale_position_holds_m3);
    RUN_TEST(test_digital_m3_is_mode_one);
    RUN_TEST(test_fail_timeout_rebases_on_the_position);
    RUN_TEST(test_rate_limit_steps_to_fully_open);
    RUN_TEST(test_small_correction_is_not_made);
    RUN_TEST(test_within_the_deadband_is_held);
    RUN_TEST(test_reset_takes_the_position_as_target);
    RUN_TEST(test_m1_m2_and_steps_are_stepped_in_lockstep);
    RUN_TEST(test_identity);
    return UNITY_END();
}
