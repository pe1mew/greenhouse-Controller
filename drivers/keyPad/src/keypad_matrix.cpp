/**
 * @file keypad_matrix.cpp
 * @brief Keypad matrix driver implementation — LIB-5.
 */

#ifndef UNIT_TEST
  #include <Arduino.h>
#else
  #include "../test/mock_keypad.h"
#endif

#include "keypad_matrix.h"

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
        pinMode(row_pins[r], OUTPUT);
        digitalWrite(row_pins[r], HIGH);   /* idle: all rows HIGH */
    }
    for (int c = 0; c < 4; c++) {
        pinMode(col_pins[c], INPUT_PULLUP);
    }
}

char keypad_scan(void)
{
    char detected       = KP_NO_KEY;
    bool multipress     = false;
    int  rows_with_keys = 0;

    for (int r = 0; r < 4 && !multipress; r++) {
        /* Drive exactly this row LOW — all other rows remain HIGH */
        digitalWrite(row_pins[r], LOW);

        int col_low_count = 0;
        int col_hit       = -1;
        for (int c = 0; c < 4; c++) {
            if (digitalRead(col_pins[c]) == LOW) {
                col_low_count++;
                col_hit = c;
            }
        }

        /* Restore row to idle HIGH before moving on */
        digitalWrite(row_pins[r], HIGH);

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
        digitalWrite(row_pins[r], LOW);
        for (int c = 0; c < 4; c++) {
            if (digitalRead(col_pins[c]) == LOW) {
                total++;
            }
        }
        digitalWrite(row_pins[r], HIGH);
    }
    return total;
}

#ifdef UNIT_TEST
void keypad_test_reset_state(void)
{
    prev_key = KP_NO_KEY;
}
#endif
