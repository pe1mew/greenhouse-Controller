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
 *
 * base_in() leaves t_min_c10 at 0, so the humidity floor (t_min + 2 °C, v2)
 * is 2 °C and never in the way; the floor's own tests set t_min.
 *
 * Version 2 (gh#84): two tests pinned v1's behaviour as correct and were
 * changed deliberately -- test_conflict_follows_cr_priority (humidity alone
 * now opens M1 at most) and the dry-close test, renamed
 * test_too_dry_never_closes_against_heat. Every test in the "v2" section
 * fails on v1 and passes on v2.
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

/* CHANGED DELIBERATELY for v2 (gh#84): v1 returned 3 under priorities 1 and 2.
 * Humidity alone now opens M1 at most, and 2 is 1. */
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
        TEST_ASSERT_EQUAL_INT(3, o.step_rh);     /* the vote itself is unchanged */
        const int expect = (pri == 0) ? 0 : 1;   /* 0 = temperature first; else M1 */
        TEST_ASSERT_EQUAL_INT(expect, o.step);
    }
}

/* CHANGED DELIBERATELY for v2 (gh#84), and renamed: this was
 * test_too_dry_demands_close_and_priority_decides, which asserted that
 * priority 1 closes a 30 °C house for dryness. Rule 1: dryness never closes
 * against heat, under any priority. */
static void test_too_dry_never_closes_against_heat(void)
{
    for (int pri = 0; pri <= 2; pri++) {
        M->reset(&S);
        vent_in_t in = base_in();
        in.rh_ctrl_en  = true;
        in.rh_avg_pct  = 50;                /* below rh_min 60 → a dry vote, 0 */
        in.t_avg_c     = 30;                /* step 2 */
        in.cr_priority = (uint8_t)pri;
        vent_out_t o = run(&in);
        TEST_ASSERT_EQUAL_INT(0, o.step_rh);
        TEST_ASSERT_EQUAL_INT(2, o.step);
        TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[0].action);
        TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[1].action);
    }
}

/* ------------------------------------------------------------------ v2 */

/** The prototype's humidity setting (humidity_prototype.py, vin()): rh_max
 *  75 by day and 80 by night, rh_min 50/55, hyst_rh 12, so the humidity step
 *  width is 4 % and the close band at rh_max is max(12 / 3, 1) = 4 %. */
static vent_in_t humid_in(int t_c, int rh, int pri, bool day, int t_min_c)
{
    vent_in_t in = base_in();
    in.daytime     = day;
    in.t_avg_c     = (int16_t)t_c;
    in.t_avg_c10   = (int16_t)(t_c * 10);
    in.t_c10       = in.t_avg_c10;
    in.t_max_c10   = (int16_t)((day ? 28 : 20) * 10);
    in.t_min_c10   = (int16_t)(t_min_c * 10);
    in.rh_ctrl_en  = true;
    in.rh_avg_pct  = (uint8_t)rh;
    in.rh_pct      = (uint8_t)rh;
    in.rh_max_pct  = (uint8_t)(day ? 75 : 80);
    in.rh_min_pct  = (uint8_t)(day ? 50 : 55);
    in.hyst_rh_pct = 12;
    in.cr_priority = (uint8_t)pri;
    return in;
}

/* Rule 2: below t_min + 2 °C humidity may not open the house on its own. */
static void test_humidity_floor_keeps_a_cold_house_shut(void)
{
    vent_in_t in = humid_in(12, 90, 1, false, 14);   /* night: floor 16 */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(3, o.step_rh);
    TEST_ASSERT_EQUAL_INT(0, o.step);

    M->reset(&S);
    in = humid_in(15, 90, 1, false, 14);             /* one under the floor */
    TEST_ASSERT_EQUAL_INT(0, run(&in).step);

    M->reset(&S);
    in = humid_in(16, 90, 1, false, 14);             /* at the floor: M1 */
    TEST_ASSERT_EQUAL_INT(1, run(&in).step);
}

/* Rule 2's hysteresis: an opening already live holds one degree under the
 * floor, and a house that was not open needs the floor itself. */
static void test_humidity_floor_holds_a_live_opening_one_degree(void)
{
    const int t_seq[5]  = { 18, 17, 16, 17, 18 };    /* day, t_min 16: floor 18 */
    const int expect[5] = {  1,  1,  0,  0,  1 };
    for (int k = 0; k < 5; k++) {
        vent_in_t in = humid_in(t_seq[k], 90, 1, true, 16);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect[k], run(&in).step, "floor sequence");
    }
}

/* Rule 3: humidity alone never reaches M2 or M3. */
static void test_humidity_alone_is_capped_at_m1(void)
{
    vent_in_t in = humid_in(20, 88, 1, true, 16);    /* T idle, humidity step 3 */
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(3, o.step_rh);
    TEST_ASSERT_EQUAL_INT(1, o.step);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[0].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[1].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o.win[2].action);
}

/* Rule 4: a live humidity vote holds at 1 down to rh_max - 4, then lapses. */
static void test_humidity_close_guard_at_rh_max(void)
{
    const int rh_seq[3]    = { 80, 73, 71 };         /* rh_max 75, band 4 */
    const int expect[3]    = {  1,  1,  0 };
    const int expect_rh[3] = {  2,  1, VENT_STEP_NONE };
    for (int k = 0; k < 3; k++) {
        vent_in_t in = humid_in(20, rh_seq[k], 1, true, 16);
        vent_out_t o = run(&in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect_rh[k], o.step_rh, "humidity vote");
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect[k], o.step, "resolved step");
    }
}

/* Rule 5: priority 2 decides exactly as priority 1, over every temperature,
 * humidity and time of day in a sweep that includes the dry-against-heat
 * conflict, the only case in which v1 told them apart. */
static void test_priority_two_is_priority_one(void)
{
    const vent_model_t *m = vent_model_stepped();
    vent_state_t s1, s2;
    m->reset(&s1);
    m->reset(&s2);
    int compared = 0;
    for (int day = 0; day <= 1; day++) {
        for (int t = 10; t <= 35; t++) {
            for (int rh = 30; rh <= 99; rh += 3) {
                vent_in_t a = humid_in(t, rh, 1, day != 0, 16);
                vent_in_t b = humid_in(t, rh, 2, day != 0, 16);
                vent_out_t oa, ob;
                memset(&oa, 0, sizeof(oa));
                memset(&ob, 0, sizeof(ob));
                m->step(&a, &s1, &oa);
                m->step(&b, &s2, &ob);
                TEST_ASSERT_EQUAL_INT(oa.step, ob.step);
                for (int i = 0; i < VENT_WINDOWS; i++) {
                    TEST_ASSERT_EQUAL_INT(oa.win[i].action, ob.win[i].action);
                }
                compared++;
            }
        }
    }
    TEST_ASSERT_TRUE(compared > 1000);
}

/* Guard, not new behaviour: priority 0 never lets humidity open the house on
 * its own, and at priority 0 v2 decides as v1 did -- at 30 °C and too dry the
 * temperature wins, as it always did. Passes on v1 and v2 alike. */
static void test_priority_zero_is_unchanged(void)
{
    vent_in_t in = humid_in(20, 88, 0, true, 16);
    TEST_ASSERT_EQUAL_INT(0, run(&in).step);
    M->reset(&S);
    in = humid_in(30, 40, 0, true, 16);
    TEST_ASSERT_EQUAL_INT(2, run(&in).step);
}

/* ------------------------------------------------------------ versioning */

/** FNV-1a over one output: the fields a caller acts on or logs. */
static uint32_t fnv_out(uint32_t h, const vent_out_t *o)
{
    const int32_t f[6] = { o->step, o->step_t, o->step_rh,
                           o->win[0].action, o->win[1].action, o->win[2].action };
    for (int k = 0; k < 6; k++) {
        uint32_t v = (uint32_t)f[k];
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

/* A behaviour change without a version bump fails here (contract §2, "A law's
 * name and version"; gh#83's change went unnoticed without this). The sweep
 * is a fixed pseudo-random run with the law's memory carried, so the close
 * guards and the floor's hysteresis are exercised. If this fails after a
 * deliberate change to the law: bump the version, then pin the new digest
 * printed by the failure, both in the same change. */
#define STEPPED_VERSION_PINNED  2
#define STEPPED_DIGEST_PINNED   0xAFEA256Fu

static void test_decision_digest_pins_the_version(void)
{
    const vent_model_t *m = vent_model_stepped();
    vent_state_t st;
    m->reset(&st);
    const vent_win_state_t states[6] = { VENT_WIN_UNKNOWN, VENT_WIN_CLOSED,
                                         VENT_WIN_MOVING_OPEN, VENT_WIN_OPEN,
                                         VENT_WIN_MOVING_CLOSE, VENT_WIN_PART_OPEN };
    uint32_t r = 20260927u, h = 2166136261u;
    for (int k = 0; k < 20000; k++) {
        r = r * 1103515245u + 12345u;
        vent_in_t in = humid_in(5 + (int)((r >> 8) % 36u), 20 + (int)((r >> 14) % 80u),
                                (int)((r >> 22) % 3u), ((r >> 25) & 1u) != 0u,
                                (int)((r >> 26) % 25u));
        in.rh_ctrl_en = ((r >> 3) & 7u) != 0u;
        in.t_valid    = ((r >> 5) % 64u) != 0u;
        in.hyst_t_c   = (uint8_t)(2 + (r >> 29) % 6u);
        for (int i = 0; i < VENT_WINDOWS; i++) {
            in.win[i].state = states[(r >> (6 + 3 * i)) % 6u];
        }
        if ((r % 997u) == 0u) { m->reset(&st); }
        vent_out_t o;
        memset(&o, 0, sizeof(o));
        m->step(&in, &st, &o);
        h = fnv_out(h, &o);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(STEPPED_VERSION_PINNED, m->version,
                                  "the version the digest was pinned with");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(STEPPED_DIGEST_PINNED, h,
                                    "decisions changed: bump the version, then pin this digest");
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

/* Interface 2: a part-open M3 is at neither end, so mode 1 sends it to the end
 * it wants, both ways. Holding it would leave M3 part-open while the step says
 * OPEN, the silent under-ventilation 2.10.0's verdicts exist to catch. */
static void test_part_open_is_driven_to_the_wanted_end(void)
{
    vent_in_t in = base_in();
    in.t_avg_c = 31;                        /* wants all open */
    in.win[2].state   = VENT_WIN_PART_OPEN;
    in.win[2].cap     = VENT_CAP_LINEAR;
    in.win[2].pos_x10 = 400;
    vent_out_t o = run(&in);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_OPEN, o.win[2].action);

    M->reset(&S);
    vent_in_t in2 = base_in();              /* wants all closed */
    in2.win[2].state   = VENT_WIN_PART_OPEN;
    in2.win[2].cap     = VENT_CAP_LINEAR;
    in2.win[2].pos_x10 = 400;
    vent_out_t o2 = run(&in2);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_CLOSE, o2.win[2].action);
    TEST_ASSERT_EQUAL_INT(VENT_ACT_HOLD, o2.win[0].action);   /* already closed */
}

/* ------------------------------------------------------------- contract */

/* Interface 2: the minimum interval stands in for dwells of 10 and 25 minutes.
 * Interface 1 carried it in 16 bits, at most 65.5 s. */
static void test_min_interval_holds_a_real_dwell(void)
{
    vent_in_t in = base_in();
    in.m3_min_interval_ms = 25u * 60u * 1000u;
    TEST_ASSERT_EQUAL_UINT32(1500000u, in.m3_min_interval_ms);
}

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
    TEST_ASSERT_EQUAL_INT(2, M->version);    /* v2: gh#84 */
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
    RUN_TEST(test_too_dry_never_closes_against_heat);
    RUN_TEST(test_humidity_floor_keeps_a_cold_house_shut);
    RUN_TEST(test_humidity_floor_holds_a_live_opening_one_degree);
    RUN_TEST(test_humidity_alone_is_capped_at_m1);
    RUN_TEST(test_humidity_close_guard_at_rh_max);
    RUN_TEST(test_priority_two_is_priority_one);
    RUN_TEST(test_priority_zero_is_unchanged);
    RUN_TEST(test_decision_digest_pins_the_version);
    RUN_TEST(test_unknown_position_is_left_alone);
    RUN_TEST(test_moving_windows_are_commanded_toward_the_target);
    RUN_TEST(test_a_satisfied_window_is_held);
    RUN_TEST(test_part_open_is_driven_to_the_wanted_end);
    RUN_TEST(test_min_interval_holds_a_real_dwell);
    RUN_TEST(test_no_measurement_holds_everything);
    RUN_TEST(test_stepped_never_asks_for_a_target);
    RUN_TEST(test_log_fields_are_filled_for_mode_one);
    RUN_TEST(test_identity);
    return UNITY_END();
}
