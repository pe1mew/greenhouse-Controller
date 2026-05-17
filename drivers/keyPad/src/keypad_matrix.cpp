/**
 * @file keypad_matrix.cpp
 * @brief Keypad matrix driver implementation — LIB-5.
 *
 * Migrated from arduino-esp32 to ESP-IDF in 2.0.0-alpha.2.2 (Phase 2.2).
 *
 * Implementation policy: this driver does not call ESP-IDF GPIO primitives
 * directly. Instead it goes through gpio_util (LIB-1), which already
 * encapsulates the arduino→ESP-IDF abstraction. The benefits:
 *   - One place to handle the `GPIO_MODE_INPUT_OUTPUT` vs `GPIO_MODE_OUTPUT`
 *     trap (documented in gpio_util.cpp) — keypad inherits the fix.
 *   - The keypad driver remains framework-independent. The public API in
 *     keypad_matrix.h doesn't change; callers are unaffected by the
 *     migration.
 *   - Future improvements in gpio_util (e.g. ISR support) become
 *     automatically available to this driver.
 *
 * Behavioural notes:
 *   - Rows are driven LOW one at a time during a scan. Active LOW on the
 *     selected row + INPUT_PULLUP on the columns means a pressed key
 *     pulls the corresponding column input LOW. After each row's scan,
 *     the row is restored to HIGH idle.
 *   - 2-scan debounce: a key is reported only when the same single key
 *     was detected on the previous call.
 *   - Multi-press detection (>1 column LOW on one row, or any keys on
 *     more than one row in the same scan) discards the input and resets
 *     debounce.
 */

#ifdef UNIT_TEST
  #include "../test/mock_keypad.h"
#endif

#include "keypad_matrix.h"
#include "gpio_util.h"

/* ---------------------------------------------------------------------------
 * Key character map  [row 0-3][col 0-3]
 * --------------------------------------------------------------------------- */
static const char key_map[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

static const uint8_t row_pins[4] = {KP_ROW1, KP_ROW2, KP_ROW3, KP_ROW4};
static const uint8_t col_pins[4] = {KP_COL1, KP_COL2, KP_COL3, KP_COL4};

/** Key detected in the previous scan — used for 2-scan debounce. */
static char prev_key = KP_NO_KEY;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void keypad_init(void)
{
    for (int r = 0; r < 4; r++) {
        gpio_set_pin_mode(row_pins[r], GPIO_OUTPUT);
        gpio_write(row_pins[r], GPIO_HIGH);   /* idle: all rows HIGH */
    }
    for (int c = 0; c < 4; c++) {
        gpio_set_pin_mode(col_pins[c], GPIO_INPUT_PULLUP);
    }
}

char keypad_scan(void)
{
    char detected       = KP_NO_KEY;
    bool multipress     = false;
    int  rows_with_keys = 0;

    for (int r = 0; r < 4 && !multipress; r++) {
        /* Drive exactly this row LOW — all other rows remain HIGH */
        gpio_write(row_pins[r], GPIO_LOW);

        int col_low_count = 0;
        int col_hit       = -1;
        for (int c = 0; c < 4; c++) {
            if (gpio_read(col_pins[c]) == GPIO_LOW) {
                col_low_count++;
                col_hit = c;
            }
        }

        /* Restore row to idle HIGH before moving on */
        gpio_write(row_pins[r], GPIO_HIGH);

        if (col_low_count > 1) {
            /* Two or more columns LOW in one row → multi-press */
            multipress = true;
        } else if (col_low_count == 1) {
            rows_with_keys++;
            if (rows_with_keys > 1) {
                /* Keys detected in more than one row → multi-press */
                multipress = true;
            } else {
                detected = key_map[r][col_hit];
            }
        }
    }

    if (multipress) {
        /* Discard: reset debounce so a clean press needs two fresh scans */
        prev_key = KP_NO_KEY;
        return KP_NO_KEY;
    }

    /* 2-scan debounce: report only on second consecutive identical detection */
    char result = KP_NO_KEY;
    if (detected != KP_NO_KEY && detected == prev_key) {
        result = detected;
    }
    prev_key = detected;
    return result;
}

int keypad_count_pressed(void)
{
    int total = 0;
    for (int r = 0; r < 4; r++) {
        gpio_write(row_pins[r], GPIO_LOW);
        for (int c = 0; c < 4; c++) {
            if (gpio_read(col_pins[c]) == GPIO_LOW) {
                total++;
            }
        }
        gpio_write(row_pins[r], GPIO_HIGH);
    }
    return total;
}

#ifdef UNIT_TEST
void keypad_test_reset_state(void)
{
    prev_key = KP_NO_KEY;
}
#endif
