/**
 * @file test_m3_char.cpp
 * @brief Host tests for the characterisation run's arithmetic. No hardware.
 *
 * Run: cd drivers/m3Char && pio test -e native
 *
 * Two kinds of test:
 *  - **Replays.** The rig's archived raw data (test/data/, via the generated
 *    fixtures.h) goes through the library, and the result must equal what
 *    bin/at_wp_minmove.py and bin/at_wp02.py PRINTED for the same runs, to
 *    the last digit they printed (one unit either way: they rounded Python
 *    doubles, this rounds its own). That is the plan's condition for step 1
 *    (§5e, Build order): the on-unit run may differ from the bench
 *    measurement only by what the mechanism did, never by the arithmetic.
 *  - **Rules.** The parts with no bench run behind them yet -- phase 1's
 *    speed, the width search, the band candidate and its check -- are tested
 *    against the plan's rules, case by case.
 */

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "m3_char.h"
#include "fixtures.h"

void setUp(void) {}
void tearDown(void) {}

static char s_msg[200];
#define MSG(...) (snprintf(s_msg, sizeof(s_msg), __VA_ARGS__), s_msg)

/** One unit of the printed last digit either way. */
#define NEAR1(want, got, ...) \
    TEST_ASSERT_INT32_WITHIN_MESSAGE(1, (int32_t)(want), (int32_t)(got), MSG(__VA_ARGS__))

/** 0.01 -> 0.1, rounded half away from zero, as a printed "%.1f" reads. */
static int32_t x100_to_x10(int32_t v)
{
    return (v >= 0) ? (v + 5) / 10 : -((-v + 5) / 10);
}

/* A run's pulses, copied so m3c_mark_reversals() can tag them. */
static m3c_pulse_t s_buf[400];

static size_t load_run(const fx_minmove_run_t *r)
{
    TEST_ASSERT_TRUE_MESSAGE(r->n_pulses <= sizeof(s_buf) / sizeof(s_buf[0]),
                             MSG("%s: more pulses than the test buffer", r->name));
    memcpy(s_buf, r->pulses, r->n_pulses * sizeof(s_buf[0]));
    /* Each run started by moving M3 OPEN to its start position, hence `true`:
     * the harness's mark_reversals(rows) default. */
    m3c_mark_reversals(s_buf, r->n_pulses, true);
    return r->n_pulses;
}

static uint32_t run_threshold(const fx_minmove_run_t *r)
{
    const uint32_t nz = m3c_noise_x1000(r->rest_x10 + r->rest_drop, r->n_rest - r->rest_drop);
    return m3c_moved_threshold_x1000(nz);
}

/* ---- replays -------------------------------------------------------------------- */

void test_noise_and_threshold_reproduce_the_harness(void)
{
    for (size_t i = 0; i < sizeof(MM_RUNS) / sizeof(MM_RUNS[0]); i++) {
        const fx_minmove_run_t *r = &MM_RUNS[i];
        const uint32_t nz = m3c_noise_x1000(r->rest_x10 + r->rest_drop,
                                            r->n_rest - r->rest_drop);
        NEAR1(r->noise_x100, (nz + 5u) / 10u, "%s: sigma at rest (0.01 mm)", r->name);
        const uint32_t th = m3c_moved_threshold_x1000(nz);
        NEAR1(r->thresh_x10, (th + 50u) / 100u, "%s: moved threshold (0.1 mm)", r->name);
    }
}

void test_minmove_reproduces_the_harness(void)
{
    for (size_t i = 0; i < sizeof(MM_RUNS) / sizeof(MM_RUNS[0]); i++) {
        const fx_minmove_run_t *r = &MM_RUNS[i];
        const size_t n = load_run(r);
        const uint32_t th = run_threshold(r);
        for (int d = 0; d < 2; d++) {
            const fx_minmove_expect_t *e = &r->expect[d];
            const char *dir = e->opening ? "OPEN" : "CLOSE";
            m3c_minmove_t out;
            TEST_ASSERT_TRUE_MESSAGE(m3c_minmove(s_buf, n, e->opening, th, &out),
                                     MSG("%s %s: no result", r->name, dir));
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(e->n_rows, out.n_rows,
                                            MSG("%s %s: widths", r->name, dir));
            for (uint8_t k = 0; k < e->n_rows; k++) {
                const fx_row_t *w = &e->rows[k];
                const m3c_width_row_t *g = &out.row[k];
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(w->req_ms, g->req_ms,
                                                 MSG("%s %s: row %u width", r->name, dir, k));
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(w->n, g->n,
                                                MSG("%s %s %u ms: n", r->name, dir, w->req_ms));
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(w->moved, g->moved,
                                                MSG("%s %s %u ms: moved", r->name, dir, w->req_ms));
                NEAR1(w->real_x10, (g->real_us_mean + 50u) / 100u,
                      "%s %s %u ms: real width (0.1 ms)", r->name, dir, w->req_ms);
                NEAR1(w->mean_x100, g->disp_mean_x100,
                      "%s %s %u ms: mean displacement (0.01 mm)", r->name, dir, w->req_ms);
            }
            TEST_ASSERT_EQUAL_MESSAGE(e->has_fit, out.fit_ok, MSG("%s %s: fit", r->name, dir));
            if (e->has_fit) {
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(e->fit_n, out.fit_n,
                                                 MSG("%s %s: pulses fitted", r->name, dir));
                NEAR1(e->speed_x10, out.fit_speed_x10, "%s %s: speed (0.1 mm/s)", r->name, dir);
                NEAR1(e->dead_ms, out.dead_ms, "%s %s: dead time (ms)", r->name, dir);
            }
            TEST_ASSERT_EQUAL_MESSAGE(e->has_floor2, out.floor2_ms != 0u,
                                      MSG("%s %s: floor 2 found", r->name, dir));
            if (e->has_floor2) {
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(e->floor2_ms, out.floor2_ms,
                                                 MSG("%s %s: floor 2 (ms)", r->name, dir));
                NEAR1(e->floor2_real_x10, (out.floor2_real_us + 50u) / 100u,
                      "%s %s: floor 2 real width (0.1 ms)", r->name, dir);
                NEAR1(e->floor2_disp_x100, out.floor2_disp_x100,
                      "%s %s: floor 2 displacement (0.01 mm)", r->name, dir);
            }
        }
    }
}

void test_reversal_loss_reproduces_the_harness(void)
{
    size_t checked = 0u;
    for (size_t i = 0; i < sizeof(MM_RUNS) / sizeof(MM_RUNS[0]); i++) {
        const fx_minmove_run_t *r = &MM_RUNS[i];
        if (r->rev == NULL) { continue; }
        const size_t n = load_run(r);
        for (size_t j = 0; j < r->n_rev; j++) {
            const fx_rev_t *v = &r->rev[j];
            const char *dir = v->opening ? "open" : "close";
            m3c_reversal_t out;
            const bool ok = m3c_reversal_loss(s_buf, n, v->opening, v->req_ms, &out);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(v->n_rev, out.n_rev,
                                            MSG("%s %s %u ms: after a reversal, n",
                                                r->name, dir, v->req_ms));
            NEAR1(v->rev_x100, out.rev_mean_x100, "%s %s %u ms: after a reversal (0.01 mm)",
                  r->name, dir, v->req_ms);
            TEST_ASSERT_EQUAL_MESSAGE(v->has_same, ok,
                                      MSG("%s %s %u ms: both kinds", r->name, dir, v->req_ms));
            if (v->has_same) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(v->n_same, out.n_same,
                                                MSG("%s %s %u ms: same direction, n",
                                                    r->name, dir, v->req_ms));
                NEAR1(v->same_x100, out.same_mean_x100, "%s %s %u ms: same direction (0.01 mm)",
                      r->name, dir, v->req_ms);
                /* The harness printed after-a-reversal minus same-direction. */
                NEAR1(v->diff_x100, -out.loss_x100, "%s %s %u ms: the difference (0.01 mm)",
                      r->name, dir, v->req_ms);
            } else {
                TEST_ASSERT_EQUAL_UINT8(0u, out.n_same);
            }
            checked++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(checked >= 20u, "the reversal lines were not all replayed");
}

void test_the_recorded_run_gives_plan_3_6s_figures(void)
{
    /* The same replay, stated as the plan records it (§3.6, 2026-10-02):
     * 117 / 122 mm/s, dead time 15 / 21 ms, floor 2 30 ms (2.3 mm) opening and
     * 35 ms (2.1 mm) closing; and the first sweep's "10-12 mm" of slack, which
     * at 200 ms is 11.9 mm opening and 9.9 mm closing. */
    const fx_minmove_run_t *d = NULL, *first = NULL;
    for (size_t i = 0; i < sizeof(MM_RUNS) / sizeof(MM_RUNS[0]); i++) {
        if (strcmp(MM_RUNS[i].name, "1002d") == 0) { d = &MM_RUNS[i]; }
        if (strcmp(MM_RUNS[i].name, "1002") == 0)  { first = &MM_RUNS[i]; }
    }
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(first);

    size_t n = load_run(d);
    const uint32_t th = run_threshold(d);
    m3c_minmove_t o, c;
    TEST_ASSERT_TRUE(m3c_minmove(s_buf, n, true, th, &o));
    TEST_ASSERT_TRUE(m3c_minmove(s_buf, n, false, th, &c));
    TEST_ASSERT_UINT16_WITHIN(5, 1170, o.fit_speed_x10);
    TEST_ASSERT_UINT16_WITHIN(5, 1220, c.fit_speed_x10);
    TEST_ASSERT_EQUAL_INT16(15, o.dead_ms);
    TEST_ASSERT_EQUAL_INT16(21, c.dead_ms);
    TEST_ASSERT_EQUAL_UINT16(30, o.floor2_ms);
    TEST_ASSERT_EQUAL_UINT16(35, c.floor2_ms);
    TEST_ASSERT_INT32_WITHIN(5, 230, o.floor2_disp_x100);
    TEST_ASSERT_INT32_WITHIN(5, 210, c.floor2_disp_x100);

    n = load_run(first);
    m3c_reversal_t ro, rc;
    TEST_ASSERT_TRUE(m3c_reversal_loss(s_buf, n, true, 200, &ro));
    TEST_ASSERT_TRUE(m3c_reversal_loss(s_buf, n, false, 200, &rc));
    TEST_ASSERT_INT32_WITHIN(5, 1192, ro.loss_x100);
    TEST_ASSERT_INT32_WITHIN(5, 992, rc.loss_x100);
}

void test_wp02_reproduces_the_harness(void)
{
    for (size_t i = 0; i < sizeof(WP02_RUNS) / sizeof(WP02_RUNS[0]); i++) {
        const fx_wp02_run_t *r = &WP02_RUNS[i];
        m3c_wp02_t out;
        TEST_ASSERT_TRUE(m3c_wp02(r->rest_x10, r->below, 10u, r->target_x10, &out));
        TEST_ASSERT_EQUAL_UINT8(10u, out.n);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(5u, out.n_below, MSG("%s: from below", r->name));
        NEAR1(r->mean_x10, x100_to_x10(out.mean_x100), "%s: mean (0.1 %%)", r->name);
        NEAR1(r->err_x10, x100_to_x10(out.err_x100), "%s: error (0.1 %%)", r->name);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(r->spread_x10 * 10, out.spread_x100,
                                        MSG("%s: spread", r->name));
        NEAR1(r->hyst_x10, x100_to_x10(out.hyst_x100), "%s: hysteresis (0.1 %%)", r->name);
        TEST_ASSERT_EQUAL_MESSAGE(r->pass, out.pass, MSG("%s: verdict", r->name));
    }
}

/* ---- rules ----------------------------------------------------------------------- */

void test_mark_reversals_follows_the_last_measured_direction(void)
{
    m3c_pulse_t p[6];
    memset(p, 0, sizeof(p));
    const bool dir[6]   = { true, true, false, true, false, false };
    const bool valid[6] = { true, true, true, false, true, true };
    for (int i = 0; i < 6; i++) { p[i].opening = dir[i]; p[i].valid = valid[i]; }
    m3c_mark_reversals(p, 6, false);
    TEST_ASSERT_TRUE(p[0].reversal);     /* the drive before closed */
    TEST_ASSERT_FALSE(p[1].reversal);
    TEST_ASSERT_TRUE(p[2].reversal);
    TEST_ASSERT_TRUE(p[3].reversal);     /* tagged, but it measured nothing ... */
    TEST_ASSERT_FALSE(p[4].reversal);    /* ... so this close follows a close */
    TEST_ASSERT_FALSE(p[5].reversal);
}

/* Two of the harness's rules that the archived runs never exercise -- found by
 * breaking each rule in a copy of the library and seeing the replays still
 * pass (2026-10-03). No displacement there sits exactly on a threshold, and no
 * shorter width succeeds after a longer one failed. */

static m3c_pulse_t pulse(uint16_t ms, int32_t disp_x10)
{
    m3c_pulse_t p;
    memset(&p, 0, sizeof(p));
    p.req_ms = ms;
    p.width_us = (uint32_t)ms * 1000u;
    p.disp_x10 = disp_x10;
    p.opening = true;
    p.valid = true;
    return p;
}

void test_moved_means_more_than_the_threshold(void)
{
    /* A 1.0 mm threshold: exactly 1.0 mm did NOT move (`x > thresh`). */
    m3c_pulse_t p[4] = { pulse(50, 30), pulse(50, 30), pulse(40, 10), pulse(40, 30) };
    m3c_minmove_t out;
    TEST_ASSERT_TRUE(m3c_minmove(p, 4, true, 1000u, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.row[1].moved);
    TEST_ASSERT_EQUAL_UINT16(50, out.floor2_ms);
}

void test_floor2_is_an_unbroken_run_from_the_longest(void)
{
    /* 100 ms always moved, 80 ms once did not, 60 ms always moved: floor 2 is
     * 100 ms, and the fit takes the 100 ms pulses only. */
    m3c_pulse_t p[9] = { pulse(100, 100), pulse(100, 104), pulse(100, 98),
                         pulse(80, 75),   pulse(80, 0),    pulse(80, 78),
                         pulse(60, 52),   pulse(60, 55),   pulse(60, 50) };
    m3c_minmove_t out;
    TEST_ASSERT_TRUE(m3c_minmove(p, 9, true, 1000u, &out));
    TEST_ASSERT_EQUAL_UINT16(100, out.floor2_ms);
    TEST_ASSERT_EQUAL_UINT16(3, out.fit_n);
    TEST_ASSERT_FALSE(out.fit_ok);   /* one width: no slope to fit */
}

/** A move from @p from to @p to (0.1 mm) at @p rate (0.1 mm/s), read every
 *  180 ms, with a slow start and a coast that must not count. */
static size_t synth_move(m3c_sample_t *s, int32_t from, int32_t to, int32_t rate)
{
    const int32_t sign = (to > from) ? 1 : -1;
    size_t n = 0u;
    uint32_t t = 5000u;
    int32_t pos = from;
    const int32_t start_rates[3] = { 300, 800, 1100 };
    /* 0.1 mm/s x 180 ms / 1000 = 0.1 mm travelled per read */
    for (int i = 0; i < 3; i++) {                   /* accelerating */
        pos += sign * start_rates[i] * 180 / 1000;
        s[n++] = { t, pos, sign * start_rates[i] };
        t += 180u;
    }
    while ((sign > 0) ? (pos < to - 40) : (pos > to + 40)) {
        pos += sign * rate * 180 / 1000;
        s[n++] = { t, pos, sign * rate };
        t += (n == 12u) ? 400u : 180u;              /* one late read */
    }
    s[n++] = { t, to - sign * 10, sign * 400 };     /* coasting, near the end */
    return n;
}

void test_speed_uses_the_middle_of_the_move(void)
{
    static m3c_sample_t s[M3C_MAX_SAMPLES];
    m3c_speed_t out;

    size_t n = synth_move(s, 2000, 12000, 1170);    /* 200 -> 1200 mm, opening */
    TEST_ASSERT_TRUE(m3c_speed(s, n, 2000, 12000, &out));
    TEST_ASSERT_EQUAL_UINT16(1170, out.speed_x10);
    TEST_ASSERT_EQUAL_UINT16(180, out.read_ms);      /* the late read is one gap of many */
    TEST_ASSERT_TRUE(out.n_mid >= 3u);

    n = synth_move(s, 12000, 2000, 1220);           /* closing: negative rates */
    TEST_ASSERT_TRUE(m3c_speed(s, n, 12000, 2000, &out));
    TEST_ASSERT_EQUAL_UINT16(1220, out.speed_x10);

    /* A move that spans nothing, and one with too few readings in the middle. */
    TEST_ASSERT_FALSE(m3c_speed(s, n, 5000, 5000, &out));
    TEST_ASSERT_FALSE(m3c_speed(s, 2, 12000, 2000, &out));
    TEST_ASSERT_EQUAL_UINT16(0, out.speed_x10);
}

void test_speed_ignores_a_slow_start_and_finish(void)
{
    /* The first and last 20 % of the distance at a crawl, so most READINGS are
     * slow even though most of the distance is not: a median over the whole
     * move would report the crawl. */
    static m3c_sample_t s[M3C_MAX_SAMPLES];
    size_t n = 0u;
    uint32_t t = 0u;
    int32_t pos = 2000;
    const struct { int32_t until, rate; } leg[3] = { { 4000, 300 }, { 10000, 1170 },
                                                    { 12000, 400 } };
    for (int k = 0; k < 3; k++) {
        while (pos < leg[k].until && n < M3C_MAX_SAMPLES) {
            pos += leg[k].rate * 180 / 1000;
            s[n++] = { t, pos, leg[k].rate };
            t += 180u;
        }
    }
    m3c_speed_t out;
    TEST_ASSERT_TRUE(m3c_speed(s, n, 2000, 12000, &out));
    TEST_ASSERT_EQUAL_UINT16(1170, out.speed_x10);
}

static void search_expect(const uint16_t *want, size_t n_want, const bool *moved,
                          uint16_t w0, uint16_t min_ms)
{
    m3c_search_t s;
    m3c_search_start(&s, w0);
    for (size_t i = 0; i < n_want; i++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(want[i], s.next_ms, MSG("step %u", (unsigned)i));
        m3c_search_result(&s, moved[i], min_ms);
    }
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, s.next_ms, "the search should be over");
}

void test_search_halves_then_refines_twice(void)
{
    /* The rig at 1 % of 1500 mm = 128 ms: 16 ms is the first width with a pulse
     * that did not move; then 24 (all moved) and 20 (one did not). */
    const uint16_t w[] = { 128, 64, 32, 16, 24, 20 };
    const bool     m[] = { true, true, true, false, true, false };
    search_expect(w, 6, m, 128, 10);

    /* The first midpoint fails, so the second refines above it. */
    const uint16_t w2[] = { 128, 64, 96, 112 };
    const bool     m2[] = { true, false, false, true };
    search_expect(w2, 4, m2, 128, 10);
}

void test_search_stops_at_the_floor_or_a_failed_start(void)
{
    const uint16_t w[] = { 40, 20, 10 };            /* all moved; 5 is below the minimum */
    const bool     m[] = { true, true, true };
    search_expect(w, 3, m, 40, 10);

    const uint16_t w2[] = { 128 };                  /* the start width failed */
    const bool     m2[] = { false };
    search_expect(w2, 1, m2, 128, 10);
}

void test_band_candidate_on_the_rigs_figures(void)
{
    /* Plan §5e's estimate for the rig: T17 reads every ~180 ms; 117 / 122 mm/s;
     * floor 2 2.25 / 2.09 mm; AT-WP02 landing error 0.30 %; leads 1.98 /
     * 1.69 % (wp02_final's "after"); a 1500 mm window. */
    m3c_band_in_t in;
    memset(&in, 0, sizeof(in));
    in.read_ms = 180;
    in.speed_x10[0] = 1174;  in.speed_x10[1] = 1220;
    in.floor2_x100[0] = 225; in.floor2_x100[1] = 209;
    in.rms_x100 = 30;
    in.lead_x100[0] = 198;   in.lead_x100[1] = 169;
    in.window_mm = 1500;

    m3c_band_cand_t c;
    TEST_ASSERT_TRUE(m3c_band_candidate(&in, 1, 200, &c));
    TEST_ASSERT_EQUAL_INT32(2196, c.floor1_x100);    /* 180 ms x 122.0 mm/s = 21.96 mm */
    TEST_ASSERT_EQUAL_INT32(225, c.floor2_x100);
    TEST_ASSERT_EQUAL_INT32(1350, c.landing_x100);   /* 3 x 0.30 % of 1500 mm */
    TEST_ASSERT_EQUAL_INT32(2970, c.lead_x100);      /* 1.98 % of 1500 mm */
    TEST_ASSERT_EQUAL(M3C_TERM_LEAD, c.which);
    TEST_ASSERT_EQUAL_UINT16(30, c.b0_mm);           /* 29.70 mm, rounded UP */

    /* Without the lead, floor 1 decides: the "floors only" band of §5e. */
    in.lead_x100[0] = 0; in.lead_x100[1] = 0;
    TEST_ASSERT_TRUE(m3c_band_candidate(&in, 1, 200, &c));
    TEST_ASSERT_EQUAL(M3C_TERM_FLOOR1, c.which);
    TEST_ASSERT_EQUAL_UINT16(22, c.b0_mm);
}

void test_band_candidate_limits(void)
{
    m3c_band_in_t in;
    memset(&in, 0, sizeof(in));
    m3c_band_cand_t c;

    in.window_mm = 0u;                               /* not taught: no band */
    in.read_ms = 180; in.speed_x10[0] = 1200;
    TEST_ASSERT_FALSE(m3c_band_candidate(&in, 1, 200, &c));

    in.window_mm = 1500;
    in.lead_x100[0] = 300;                           /* 45 mm */
    TEST_ASSERT_FALSE(m3c_band_candidate(&in, 1, 40, &c));   /* above the bound */
    TEST_ASSERT_EQUAL_UINT16(45, c.b0_mm);                   /* ... and it says what */

    memset(&in, 0, sizeof(in));
    in.window_mm = 1500;                             /* nothing measured at all */
    TEST_ASSERT_TRUE(m3c_band_candidate(&in, 1, 200, &c));
    TEST_ASSERT_EQUAL_UINT16(1, c.b0_mm);            /* never 0: the chattering case */
}

void test_check_step_exceeds_the_band(void)
{
    TEST_ASSERT_EQUAL_UINT16(25, m3c_check_step_x10(30, 1500));   /* 37.5 mm = 2.5 % */
    TEST_ASSERT_EQUAL_UINT16(19, m3c_check_step_x10(22, 1500));   /* 27.5 mm, rounded up */
    TEST_ASSERT_EQUAL_UINT16(0, m3c_check_step_x10(30, 0));
    /* Whatever the window, the step in mm (step_x10 x window / 1000) is at
     * least 1.25 x the band, so T2 never answers it with "already there". */
    for (uint16_t win = 500; win <= 5000; win += 250) {
        for (uint16_t b = 1; b <= 200; b++) {
            const uint32_t step_x10 = m3c_check_step_x10(b, win);
            TEST_ASSERT_TRUE_MESSAGE(step_x10 * win >= (uint32_t)b * 1250u,
                                     MSG("band %u mm, window %u mm", b, win));
        }
    }
}

void test_band_check_passes_raises_and_gives_up(void)
{
    uint16_t next = 0u;
    int32_t worst = 0;

    /* Every landing within 30 mm (0.1 mm units): pass. */
    const int32_t ok[] = { 50, -120, 290, -300 };
    TEST_ASSERT_EQUAL(M3C_CHECK_PASS, m3c_band_check(30, ok, 4, 0, 200, &next, &worst));
    TEST_ASSERT_EQUAL_INT32(300, worst);

    /* The latency case: a floors-only 22 mm band on the rig, one correction
     * landing 40 mm off. Raise by 25 %, rounded up: 27.5 -> 28. */
    const int32_t bad[] = { 30, -60, 400, 10 };
    TEST_ASSERT_EQUAL(M3C_CHECK_RAISE, m3c_band_check(22, bad, 4, 0, 200, &next, &worst));
    TEST_ASSERT_EQUAL_UINT16(28, next);
    TEST_ASSERT_EQUAL_INT32(400, worst);

    /* At most four raises. */
    TEST_ASSERT_EQUAL(M3C_CHECK_GIVE_UP, m3c_band_check(22, bad, 4, 4, 200, &next, &worst));
    /* Never past the setting's bound. */
    const int32_t far[] = { 2000 };
    TEST_ASSERT_EQUAL(M3C_CHECK_GIVE_UP, m3c_band_check(180, far, 1, 0, 200, &next, &worst));
    /* A raise always moves, even for a 1 mm band. */
    TEST_ASSERT_EQUAL(M3C_CHECK_RAISE, m3c_band_check(1, far, 1, 0, 200, &next, &worst));
    TEST_ASSERT_EQUAL_UINT16(2, next);
    /* A round with nothing in it proves nothing. */
    TEST_ASSERT_EQUAL(M3C_CHECK_GIVE_UP, m3c_band_check(30, ok, 0, 0, 200, &next, &worst));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_noise_and_threshold_reproduce_the_harness);
    RUN_TEST(test_minmove_reproduces_the_harness);
    RUN_TEST(test_reversal_loss_reproduces_the_harness);
    RUN_TEST(test_the_recorded_run_gives_plan_3_6s_figures);
    RUN_TEST(test_wp02_reproduces_the_harness);
    RUN_TEST(test_mark_reversals_follows_the_last_measured_direction);
    RUN_TEST(test_moved_means_more_than_the_threshold);
    RUN_TEST(test_floor2_is_an_unbroken_run_from_the_longest);
    RUN_TEST(test_speed_uses_the_middle_of_the_move);
    RUN_TEST(test_speed_ignores_a_slow_start_and_finish);
    RUN_TEST(test_search_halves_then_refines_twice);
    RUN_TEST(test_search_stops_at_the_floor_or_a_failed_start);
    RUN_TEST(test_band_candidate_on_the_rigs_figures);
    RUN_TEST(test_band_candidate_limits);
    RUN_TEST(test_check_step_exceeds_the_band);
    RUN_TEST(test_band_check_passes_raises_and_gives_up);
    return UNITY_END();
}
