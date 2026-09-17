/**
 * @file test_vent_model_stepped.cpp
 * @brief Host tests for mode 1's stepped model. No hardware, no ESP-IDF.
 *
 * Run: cd drivers/ventModel && pio test -e native
 *
 * The cases follow the contract's §5 list, and each one states the firmware
 * behaviour it pins down. Defaults used throughout: t_max 28 °C, hyst_t 5, so
 * the step width is max(5 / 3, 1) = 1 °C — M1 at +1, M1+M2 at +2, all three at
 * +3, which is what the greenhouse runs today.
 */

#include <unity.h>

#include <string.h>

#include "vent_model.h"

static const vent_model_t *M;
static vent_state_t        S;

void setUp(void)
{
    M = vent_model_stepped();
    M->reset(&S);
}

void tearDown(void) {}

/** A calm, cool, humidity-abstaining starting point with every window closed. */
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
    in.t_avg_c     = 20;
    in.t_avg_c10   = 200;
    in.t_c10       = 200;
    in.rh_avg_pct  = 70;
    in.rh_pct      = 70;
    in.t_max_c10   = 280;      /* 28.0 °C */
    in.rh_max_pct  = 85;
    in.rh_min_pct  = 60;
    in.hyst_t_c    = 5;
    in.hyst_rh_pct = 5;
    in.cr_priority = 0;
    in.rh_ctrl_en  = false;
    for (int i = 0; i < VENT_WINDOWS; i++) {
        in.win[i].state            = VENT_WIN_CLOSED;
        in.win[i].cap              = VENT_CAP_DIGITAL;
        in.win[i].pos_x10          = -1;
        in.win[i].last_target_x10  = -1;
        in.win[i].last_result      = VENT_RES_NONE;
    }
    return in;
}

static void set_states(vent_in_t *in, vent_win_state_t s)
{
    for (int i = 0; i < VENT_WINDOWS; i++) { in->win[i].state = s; }
}

static vent_out_t run(vent_in_t *in)
{
    vent_out_t out;
    memset(&out, 0, sizeof(out));
    M->step(in, &S, &out);
    return out;
}

/* ---------------------------------------------------------------- steps */

static void test_below_threshold_closes_nothing(void)
{
    vent_in_t in = base_in();
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(0, o.step);
    TEST_ASSERT_EQUAL_INT(0, o.step_t);
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, o.step_rh);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[i].action);
    }
}

static void test_one_degree_over_opens_m1_only(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 29;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(1, o.step);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[1].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[2].action);
}

static void test_two_degrees_over_opens_m1_and_m2(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 30;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(2, o.step);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[1].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[2].action);
}

static void test_three_degrees_over_opens_all(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(3, o.step);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[i].action);
    }
}

static void test_step_never_exceeds_the_table(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 60;                       /* far over */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_STEPS_MAX, o.step);
}

static void test_hysteresis_six_makes_two_degree_steps(void)
{
    vent_in_t in = base_in();
    in.hyst_t_c = 6;                        /* width = 6 / 3 = 2 °C */
    in.t_avg_c  = 30;                       /* +2 → ceil(2/2) = 1 */
    TEST_ASSERT_EQUAL_INT(1, run(&in).step);
    M->reset(&S);
    in.t_avg_c = 31;                        /* +3 → ceil(3/2) = 2 */
    TEST_ASSERT_EQUAL_INT(2, run(&in).step);
}

/* ------------------------------------------------------------ close guard */

static void test_close_guard_holds_one_step_open(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;
    (void)run(&in);                         /* step 3 */
    set_states(&in, VENT_WIN_OPEN);
    in.t_avg_c = 28;                        /* deviation 0: not yet -hyst */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(1, o.step);       /* stays slightly open */
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD,  o.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, o.win[1].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, o.win[2].action);
}

static void test_close_guard_releases_at_minus_hysteresis(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;
    (void)run(&in);
    set_states(&in, VENT_WIN_OPEN);
    in.t_avg_c = 23;                        /* 28 - 5 → deviation == -hyst */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(0, o.step);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, o.win[i].action);
    }
}

static void test_reset_clears_the_close_guard(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;
    (void)run(&in);                         /* step 3 */
    M->reset(&S);                           /* as at boot, or an inhibit onset */
    in.t_avg_c = 28;                        /* deviation 0 */
    TEST_ASSERT_EQUAL_INT(0, run(&in).step); /* no guard to hold it open */
}

/* -------------------------------------------------------------- humidity */

static void test_humidity_disabled_casts_no_vote(void)
{
    vent_in_t in = base_in();
    in.rh_avg_pct = 99;                     /* soaking wet */
    in.rh_ctrl_en = false;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, o.step_rh);
    TEST_ASSERT_EQUAL_INT(0, o.step);
}

static void test_humidity_invalid_casts_no_vote(void)
{
    vent_in_t in = base_in();
    in.rh_ctrl_en = true;
    in.rh_valid   = false;
    in.rh_avg_pct = 99;
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, run(&in).step_rh);
}

static void test_both_want_open_higher_step_wins(void)
{
    vent_in_t in = base_in();
    in.rh_ctrl_en = true;
    in.t_avg_c    = 29;                     /* step 1 */
    in.rh_avg_pct = 88;                     /* +3 over 85, width 1 → step 3 */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(1, o.step_t);
    TEST_ASSERT_EQUAL_INT(3, o.step_rh);
    TEST_ASSERT_EQUAL_INT(3, o.step);
}

static void test_conflict_follows_cr_priority(void)
{
    /* Temperature is content (step 0), humidity wants step 3. */
    for (int pri = 0; pri <= 2; pri++) {
        M->reset(&S);
        vent_in_t in = base_in();
        in.rh_ctrl_en  = true;
        in.cr_priority = (uint8_t)pri;
        in.rh_avg_pct  = 88;
        vent_out_t o = run(&in);
        const int expect = (pri == 0) ? 0 : 3;   /* 1 = RH first, 2 = higher wins */
        TEST_ASSERT_EQUAL_INT(expect, o.step);
    }
}

static void test_too_dry_demands_close_and_priority_decides(void)
{
    vent_in_t in = base_in();
    in.rh_ctrl_en  = true;
    in.rh_avg_pct  = 50;                    /* below rh_min 60 → step 0 */
    in.t_avg_c     = 30;                    /* step 2 */

    in.cr_priority = 0;                     /* temperature first */
    TEST_ASSERT_EQUAL_INT(2, run(&in).step);

    M->reset(&S);
    in.cr_priority = 1;                     /* humidity first → close */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(0, o.step_rh);
    TEST_ASSERT_EQUAL_INT(0, o.step);
}

/* ------------------------------------------------- actuator state mapping */

static void test_unknown_position_is_left_alone(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;                        /* wants all open */
    set_states(&in, VENT_WIN_UNKNOWN);
    vent_out_t o = run(&in);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[i].action);
    }
}

static void test_moving_windows_are_commanded_toward_the_target(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;                        /* wants all open */
    set_states(&in, VENT_WIN_MOVING_CLOSE);
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[0].action);

    M->reset(&S);
    vent_in_t in2 = base_in();              /* wants all closed */
    set_states(&in2, VENT_WIN_MOVING_OPEN);
    vent_out_t o2 = run(&in2);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, o2.win[0].action);
}

static void test_a_satisfied_window_is_held(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;
    set_states(&in, VENT_WIN_OPEN);
    vent_out_t o = run(&in);
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[i].action);
    }
}

/* ------------------------------------------------------------- contract */

static void test_no_measurement_holds_everything(void)
{
    vent_in_t in = base_in();
    in.t_valid = false;
    in.t_avg_c = 40;                        /* would otherwise open everything */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, o.step);
    TEST_ASSERT_EQUAL_INT(VENT_STEP_NONE, o.step_t);
    TEST_ASSERT_EQUAL_INT(0, o.reason);     /* no data */
    for (int i = 0; i < VENT_WINDOWS; i++) {
        TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[i].action);
    }
}

static void test_stepped_never_asks_for_a_target(void)
{
    for (int t = 15; t <= 45; t++) {
        M->reset(&S);
        vent_in_t in = base_in();
        in.rh_ctrl_en = true;
        in.t_avg_c    = (int16_t)t;
        in.rh_avg_pct = (uint8_t)(50 + t);
        vent_out_t o  = run(&in);
        for (int i = 0; i < VENT_WINDOWS; i++) {
            TEST_ASSERT_NOT_EQUAL(VENT_ACT_TARGET, o.win[i].action);
        }
    }
}

static void test_log_fields_are_filled_for_mode_one(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 30;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(2, o.step);          /* value_a of LOG_MODE_CHANGE */
    TEST_ASSERT_EQUAL_INT(2, o.step_t);        /* value_b high byte */
    TEST_ASSERT_EQUAL_INT(-1, o.step_rh);      /* value_b low byte, no vote */
    TEST_ASSERT_EQUAL_INT(1, o.reason);        /* temperature alone */
}

static void test_identity(void)
{
    TEST_ASSERT_EQUAL_STRING("stepped", M->name);
    TEST_ASSERT_TRUE(M->version >= 1);
    TEST_ASSERT_NOT_NULL(M->reset);
    TEST_ASSERT_NOT_NULL(M->step);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_below_threshold_closes_nothing);
    RUN_TEST(test_one_degree_over_opens_m1_only);
    RUN_TEST(test_two_degrees_over_opens_m1_and_m2);
    RUN_TEST(test_three_degrees_over_opens_all);
    RUN_TEST(test_step_never_exceeds_the_table);
    RUN_TEST(test_hysteresis_six_makes_two_degree_steps);
    RUN_TEST(test_close_guard_holds_one_step_open);
    RUN_TEST(test_close_guard_releases_at_minus_hysteresis);
    RUN_TEST(test_reset_clears_the_close_guard);
    RUN_TEST(test_humidity_disabled_casts_no_vote);
    RUN_TEST(test_humidity_invalid_casts_no_vote);
    RUN_TEST(test_both_want_open_higher_step_wins);
    RUN_TEST(test_conflict_follows_cr_priority);
    RUN_TEST(test_too_dry_demands_close_and_priority_decides);
    RUN_TEST(test_unknown_position_is_left_alone);
    RUN_TEST(test_moving_windows_are_commanded_toward_the_target);
    RUN_TEST(test_a_satisfied_window_is_held);
    RUN_TEST(test_no_measurement_holds_everything);
    RUN_TEST(test_stepped_never_asks_for_a_target);
    RUN_TEST(test_log_fields_are_filled_for_mode_one);
    RUN_TEST(test_identity);
    return UNITY_END();
}
