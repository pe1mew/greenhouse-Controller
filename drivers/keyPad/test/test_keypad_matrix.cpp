/**
 * LIB-5 Keypad Matrix — unit tests (native build)
 *
 * Test IDs: UT-KP-001 … UT-KP-012
 *
 * Run with:
 *   export PATH="/c/Program Files/CodeBlocks/MinGW/bin:$PATH"
 *   ~/.platformio/penv/Scripts/pio.exe test -e native
 */

#include <unity.h>
#include "../src/keypad_matrix.h"
#include "mock_keypad.h"

/* setUp / tearDown run before / after every test case */
void setUp(void)
{
    mock_keypad_reset();
    keypad_test_reset_state();
    keypad_init();              /* configures row/col pin modes */
}

void tearDown(void) {}

/* -------------------------------------------------------------------------
 * UT-KP-001 — No key pressed → KP_NO_KEY on every scan
 * ------------------------------------------------------------------------- */
void test_no_key_returns_no_key(void)
{
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-002 — Key pressed, first scan → KP_NO_KEY (debounce pending)
 * ------------------------------------------------------------------------- */
void test_first_scan_debounce(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);   /* key '1' */
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-003 — Key pressed, second consecutive scan → correct character
 * ------------------------------------------------------------------------- */
void test_second_scan_returns_key(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);   /* key '1' */
    keypad_scan();                             /* first scan: debounce */
    TEST_ASSERT_EQUAL_INT('1', keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-004 — Key 'A' (Row 1, Col 4) → 'A'
 * ------------------------------------------------------------------------- */
void test_key_A(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL4);
    keypad_scan();
    TEST_ASSERT_EQUAL_INT('A', keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-005 — Key '*' (Row 4, Col 1) → '*'
 * ------------------------------------------------------------------------- */
void test_key_star(void)
{
    mock_keypad_set_key(KP_ROW4, KP_COL1);
    keypad_scan();
    TEST_ASSERT_EQUAL_INT('*', keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-006 — Key '#' (Row 4, Col 3) → '#'
 * ------------------------------------------------------------------------- */
void test_key_hash(void)
{
    mock_keypad_set_key(KP_ROW4, KP_COL3);
    keypad_scan();
    TEST_ASSERT_EQUAL_INT('#', keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-007 — Key 'D' (Row 4, Col 4) → 'D'
 * ------------------------------------------------------------------------- */
void test_key_D(void)
{
    mock_keypad_set_key(KP_ROW4, KP_COL4);
    keypad_scan();
    TEST_ASSERT_EQUAL_INT('D', keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-008 — Key released → KP_NO_KEY on next scan (no phantom repeat)
 * ------------------------------------------------------------------------- */
void test_key_released_returns_no_key(void)
{
    mock_keypad_set_key(KP_ROW2, KP_COL2);    /* key '5' */
    keypad_scan();                              /* first scan */
    TEST_ASSERT_EQUAL_INT('5', keypad_scan()); /* debounce satisfied */
    mock_keypad_clear_keys();                   /* release */
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-009 — Only one row GPIO driven LOW at a time during scan
 *
 * mock_max_rows_low records the peak simultaneous-LOW count across all
 * digitalWrite calls. A correct driver drives exactly one row LOW at a time
 * and restores it HIGH before moving on, so the peak must be 1.
 * ------------------------------------------------------------------------- */
void test_only_one_row_low_at_a_time(void)
{
    mock_keypad_set_key(KP_ROW3, KP_COL2);   /* key '8' — exercises row 3 */
    keypad_scan();
    TEST_ASSERT_EQUAL_INT(1, mock_max_rows_low);
}

/* -------------------------------------------------------------------------
 * UT-KP-010 — All 16 keys map to distinct characters (no duplicate mapping)
 * ------------------------------------------------------------------------- */
void test_all_keys_distinct(void)
{
    static const uint8_t rows[4] = {KP_ROW1, KP_ROW2, KP_ROW3, KP_ROW4};
    static const uint8_t cols[4] = {KP_COL1, KP_COL2, KP_COL3, KP_COL4};

    char chars[16];
    int  idx = 0;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            /* Reset full state for each key to avoid prev_key cross-contamination */
            mock_keypad_reset();
            keypad_test_reset_state();
            keypad_init();

            mock_keypad_set_key(rows[r], cols[c]);
            keypad_scan();               /* first scan: debounce */
            chars[idx++] = keypad_scan();
        }
    }

    /* Every character must be unique */
    for (int i = 0; i < 16; i++) {
        for (int j = i + 1; j < 16; j++) {
            TEST_ASSERT_NOT_EQUAL(chars[i], chars[j]);
        }
    }
}

/* -------------------------------------------------------------------------
 * UT-KP-011 — Multi-press in same row → KP_NO_KEY (discarded)
 *
 * Two columns LOW in one row exceeds the single-key limit; the driver must
 * discard this input. Verified over two scans so debounce cannot mask it.
 * ------------------------------------------------------------------------- */
void test_multipress_same_row(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);
    mock_keypad_add_key(KP_ROW1, KP_COL2);  /* two columns in row 1 */
    keypad_scan();                            /* first scan */
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-012 — Multi-press across rows → KP_NO_KEY (discarded)
 *
 * One key in row 1 and one in row 2; after two scans a single key would have
 * been reported — returning KP_NO_KEY proves multi-press rejection is active.
 * ------------------------------------------------------------------------- */
void test_multipress_across_rows(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);
    mock_keypad_add_key(KP_ROW2, KP_COL2);  /* keys in two different rows */
    keypad_scan();                            /* first scan */
    TEST_ASSERT_EQUAL_INT(KP_NO_KEY, keypad_scan());
}

/* -------------------------------------------------------------------------
 * UT-KP-013 — keypad_count_pressed() returns 0 when no key is pressed
 * ------------------------------------------------------------------------- */
void test_count_pressed_none(void)
{
    TEST_ASSERT_EQUAL_INT(0, keypad_count_pressed());
}

/* -------------------------------------------------------------------------
 * UT-KP-014 — keypad_count_pressed() returns 1 for a single key
 * ------------------------------------------------------------------------- */
void test_count_pressed_single(void)
{
    mock_keypad_set_key(KP_ROW2, KP_COL3);   /* key '6' */
    TEST_ASSERT_EQUAL_INT(1, keypad_count_pressed());
}

/* -------------------------------------------------------------------------
 * UT-KP-015 — keypad_count_pressed() returns 2 for two keys in same row
 * ------------------------------------------------------------------------- */
void test_count_pressed_two_same_row(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);
    mock_keypad_add_key(KP_ROW1, KP_COL2);
    TEST_ASSERT_EQUAL_INT(2, keypad_count_pressed());
}

/* -------------------------------------------------------------------------
 * UT-KP-016 — keypad_count_pressed() returns 2 for two keys across rows
 * ------------------------------------------------------------------------- */
void test_count_pressed_two_across_rows(void)
{
    mock_keypad_set_key(KP_ROW1, KP_COL1);
    mock_keypad_add_key(KP_ROW3, KP_COL4);
    TEST_ASSERT_EQUAL_INT(2, keypad_count_pressed());
}

/* -------------------------------------------------------------------------
 * UT-KP-017 — keypad_count_pressed() does not affect debounce state
 *
 * Call count_pressed between two keypad_scan() calls; the debounce sequence
 * must still complete correctly (second scan returns the key).
 * ------------------------------------------------------------------------- */
void test_count_pressed_does_not_disturb_debounce(void)
{
    mock_keypad_set_key(KP_ROW3, KP_COL3);   /* key '9' */
    keypad_scan();                             /* first scan: debounce pending */
    keypad_count_pressed();                    /* should not reset prev_key */
    TEST_ASSERT_EQUAL_INT('9', keypad_scan()); /* second scan: must report '9' */
}

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_key_returns_no_key);
    RUN_TEST(test_first_scan_debounce);
    RUN_TEST(test_second_scan_returns_key);
    RUN_TEST(test_key_A);
    RUN_TEST(test_key_star);
    RUN_TEST(test_key_hash);
    RUN_TEST(test_key_D);
    RUN_TEST(test_key_released_returns_no_key);
    RUN_TEST(test_only_one_row_low_at_a_time);
    RUN_TEST(test_all_keys_distinct);
    RUN_TEST(test_multipress_same_row);
    RUN_TEST(test_multipress_across_rows);
    RUN_TEST(test_count_pressed_none);
    RUN_TEST(test_count_pressed_single);
    RUN_TEST(test_count_pressed_two_same_row);
    RUN_TEST(test_count_pressed_two_across_rows);
    RUN_TEST(test_count_pressed_does_not_disturb_debounce);
    return UNITY_END();
}
