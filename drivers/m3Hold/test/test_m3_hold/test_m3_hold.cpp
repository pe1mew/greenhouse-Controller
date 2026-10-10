/**
 * @file test_m3_hold.cpp
 * @brief Host tests for the LCD's tap-or-hold timing of M3 (gh#93). No hardware.
 *
 * Run: cd drivers/m3Hold && pio test -e native
 *
 * Each test plays a press the way T7 delivers it -- the first press, a repeat
 * every 100 ms once the key has been held 500 ms, and a release -- and checks
 * the one action T8 must take, against the operator's rule (2026-10-10):
 * released within 1 s, a full stroke; between 1 and 2 s, nothing; held 2 s,
 * a move until the release.
 *
 *   UT-M3H-001  a tap is a full stroke, in the press's direction
 *   UT-M3H-002  the 1 s edge: 999 ms is a tap, 1000 ms is not
 *   UT-M3H-003  a 1.5 s press does nothing (MID)
 *   UT-M3H-004  a hold starts the move on the repeat at 2 s, and the release stops it
 *   UT-M3H-005  the tick starts the move when the 2 s repeat has not arrived yet
 *   UT-M3H-006  the move starts once, however many repeats and ticks follow
 *   UT-M3H-007  a release at the 2 s mark before the move started does nothing
 *   UT-M3H-008  a tap whose release is lost is ended 900 ms after the press
 *   UT-M3H-009  a held press whose release is lost is ended 400 ms after its last repeat
 *   UT-M3H-010  a moving hold whose release is lost is stopped
 *   UT-M3H-011  a second key ends the first press, and is timed itself
 *   UT-M3H-012  another key's repeat or release changes nothing
 *   UT-M3H-013  nothing without a press
 *   UT-M3H-014  the millisecond counter wrapping mid-press changes nothing
 *   UT-M3H-015  a close press carries opening = false through every action
 */

#include <unity.h>

#include "m3_hold.h"

void setUp(void) {}
void tearDown(void) {}

static m3_hold_t h;

/* T7's cadence, as T8 receives it. */
static void repeats(char key, uint32_t from_ms, uint32_t to_ms)
{
    for (uint32_t t = from_ms; t <= to_ms; t += 100u) {
        const m3h_out_t o = m3h_repeat(&h, key, t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(M3H_NOTHING, o.action, "a repeat before 2 s acts");
    }
}

static void expect(m3h_out_t o, m3h_action_t action, bool opening, uint32_t held_ms)
{
    TEST_ASSERT_EQUAL_INT(action, o.action);
    TEST_ASSERT_EQUAL(opening, o.opening);
    TEST_ASSERT_EQUAL_UINT32(held_ms, o.held_ms);
}

/* UT-M3H-001 */
static void test_a_tap_is_a_full_stroke(void)
{
    m3h_reset(&h);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_press(&h, '1', true, 0u).action);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 100u).action);
    expect(m3h_release(&h, '1', 300u), M3H_FULL, true, 300u);
    TEST_ASSERT_FALSE(h.active);
}

/* UT-M3H-002 */
static void test_the_one_second_edge(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 900u);
    expect(m3h_release(&h, '1', 999u), M3H_FULL, true, 999u);

    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1000u);
    expect(m3h_release(&h, '1', 1000u), M3H_MID, true, 1000u);
}

/* UT-M3H-003 */
static void test_a_mid_press_does_nothing(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1400u);
    expect(m3h_release(&h, '1', 1500u), M3H_MID, true, 1500u);
}

/* UT-M3H-004 */
static void test_a_hold_moves_until_the_release(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1900u);
    expect(m3h_repeat(&h, '1', 2000u), M3H_MOVE_START, true, 2000u);
    TEST_ASSERT_TRUE(h.moving);
    for (uint32_t t = 2100u; t <= 2900u; t += 100u) {
        TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '1', t).action);
        TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, t + 50u).action);
    }
    expect(m3h_release(&h, '1', 3000u), M3H_MOVE_STOP, true, 3000u);
    TEST_ASSERT_FALSE(h.active);
}

/* UT-M3H-005 */
static void test_the_tick_starts_the_move(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1900u);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 1999u).action);
    expect(m3h_tick(&h, 2000u), M3H_MOVE_START, true, 2000u);
}

/* UT-M3H-006 */
static void test_the_move_starts_once(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '2', false, 0u);
    repeats('2', 500u, 1900u);
    expect(m3h_tick(&h, 2010u), M3H_MOVE_START, false, 2010u);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '2', 2100u).action);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 2150u).action);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '2', 2200u).action);
}

/* UT-M3H-007 */
static void test_a_release_at_two_seconds_before_the_start_does_nothing(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1900u);
    expect(m3h_release(&h, '1', 2020u), M3H_MID, true, 2020u);
}

/* UT-M3H-008 */
static void test_a_lost_release_of_a_tap(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 1000u);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 1000u + 900u).action);
    expect(m3h_tick(&h, 1000u + 901u), M3H_FULL, true, 0u);
    TEST_ASSERT_FALSE(h.active);
}

/* UT-M3H-009 */
static void test_a_lost_release_of_a_held_press(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1200u);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 1600u).action);
    expect(m3h_tick(&h, 1601u), M3H_MID, true, 1200u);
}

/* UT-M3H-010 */
static void test_a_lost_release_of_a_moving_hold(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1900u);
    expect(m3h_repeat(&h, '1', 2000u), M3H_MOVE_START, true, 2000u);
    for (uint32_t t = 2100u; t <= 2500u; t += 100u) {
        (void)m3h_repeat(&h, '1', t);
    }
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 2900u).action);
    expect(m3h_tick(&h, 2901u), M3H_MOVE_STOP, true, 2500u);
}

/* UT-M3H-011 */
static void test_a_second_key_ends_the_first(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    expect(m3h_press(&h, '2', false, 300u), M3H_FULL, true, 0u);
    TEST_ASSERT_TRUE(h.active);
    expect(m3h_release(&h, '2', 500u), M3H_FULL, false, 200u);

    /* A moving hold is stopped by it. */
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    repeats('1', 500u, 1900u);
    (void)m3h_repeat(&h, '1', 2000u);
    expect(m3h_press(&h, '2', false, 2100u), M3H_MOVE_STOP, true, 2000u);
    TEST_ASSERT_FALSE(h.moving);
}

/* UT-M3H-012 */
static void test_another_keys_events_change_nothing(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, 0u);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_release(&h, '2', 200u).action);
    TEST_ASSERT_TRUE(h.active);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '2', 600u).action);
    TEST_ASSERT_EQUAL_UINT32(0u, h.last_ms);      /* not a sign of life of '1' */
    expect(m3h_release(&h, '1', 700u), M3H_FULL, true, 700u);
}

/* UT-M3H-013 */
static void test_nothing_without_a_press(void)
{
    m3h_reset(&h);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, 5000u).action);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '1', 5000u).action);
    TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_release(&h, '1', 5000u).action);
}

/* UT-M3H-014 */
static void test_the_counter_wrapping(void)
{
    const uint32_t t0 = 0xFFFFFC00u;               /* 1024 ms before the wrap */
    m3h_reset(&h);
    (void)m3h_press(&h, '1', true, t0);
    for (uint32_t d = 500u; d <= 1900u; d += 100u) {
        TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_repeat(&h, '1', t0 + d).action);
        TEST_ASSERT_EQUAL_INT(M3H_NOTHING, m3h_tick(&h, t0 + d + 1u).action);
    }
    expect(m3h_repeat(&h, '1', t0 + 2000u), M3H_MOVE_START, true, 2000u);
    expect(m3h_release(&h, '1', t0 + 2600u), M3H_MOVE_STOP, true, 2600u);
}

/* UT-M3H-015 */
static void test_a_close_press_keeps_its_direction(void)
{
    m3h_reset(&h);
    (void)m3h_press(&h, '2', false, 0u);
    expect(m3h_release(&h, '2', 100u), M3H_FULL, false, 100u);

    (void)m3h_press(&h, '2', false, 0u);
    repeats('2', 500u, 1500u);
    expect(m3h_release(&h, '2', 1550u), M3H_MID, false, 1550u);

    (void)m3h_press(&h, '2', false, 0u);
    repeats('2', 500u, 1900u);
    expect(m3h_repeat(&h, '2', 2000u), M3H_MOVE_START, false, 2000u);
    expect(m3h_release(&h, '2', 2300u), M3H_MOVE_STOP, false, 2300u);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_a_tap_is_a_full_stroke);
    RUN_TEST(test_the_one_second_edge);
    RUN_TEST(test_a_mid_press_does_nothing);
    RUN_TEST(test_a_hold_moves_until_the_release);
    RUN_TEST(test_the_tick_starts_the_move);
    RUN_TEST(test_the_move_starts_once);
    RUN_TEST(test_a_release_at_two_seconds_before_the_start_does_nothing);
    RUN_TEST(test_a_lost_release_of_a_tap);
    RUN_TEST(test_a_lost_release_of_a_held_press);
    RUN_TEST(test_a_lost_release_of_a_moving_hold);
    RUN_TEST(test_a_second_key_ends_the_first);
    RUN_TEST(test_another_keys_events_change_nothing);
    RUN_TEST(test_nothing_without_a_press);
    RUN_TEST(test_the_counter_wrapping);
    RUN_TEST(test_a_close_press_keeps_its_direction);
    return UNITY_END();
}
