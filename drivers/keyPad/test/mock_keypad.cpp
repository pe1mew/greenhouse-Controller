/**
 * @file mock_keypad.cpp
 * @brief Arduino GPIO stub implementation for LIB-5 unit tests.
 */

#include "mock_keypad.h"
#include "pin_config.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Observable state
 * --------------------------------------------------------------------------- */
uint8_t pin_state[MOCK_PIN_COUNT];
uint8_t pin_mode_arr[MOCK_PIN_COUNT];
int     mock_max_rows_low;

/* ---------------------------------------------------------------------------
 * Internal pressed-key table
 * --------------------------------------------------------------------------- */
static uint8_t pressed_row[MOCK_MAX_KEYS];
static uint8_t pressed_col[MOCK_MAX_KEYS];
static int     pressed_count;

/* Row GPIO numbers — used by digitalWrite to track simultaneous-LOW count. */
static const uint8_t row_gpios[4] = {KP_ROW1, KP_ROW2, KP_ROW3, KP_ROW4};

/* ---------------------------------------------------------------------------
 * Mock control functions
 * --------------------------------------------------------------------------- */

void mock_keypad_reset(void)
{
    memset(pin_state,    LOW,   sizeof(pin_state));
    memset(pin_mode_arr, INPUT, sizeof(pin_mode_arr));
    pressed_count     = 0;
    mock_max_rows_low = 0;
}

void mock_keypad_set_key(uint8_t row_gpio, uint8_t col_gpio)
{
    pressed_count    = 0;
    pressed_row[0]   = row_gpio;
    pressed_col[0]   = col_gpio;
    pressed_count    = 1;
}

void mock_keypad_add_key(uint8_t row_gpio, uint8_t col_gpio)
{
    if (pressed_count < MOCK_MAX_KEYS) {
        pressed_row[pressed_count] = row_gpio;
        pressed_col[pressed_count] = col_gpio;
        pressed_count++;
    }
}

void mock_keypad_clear_keys(void)
{
    pressed_count = 0;
}

/* ---------------------------------------------------------------------------
 * Arduino stub implementations
 * --------------------------------------------------------------------------- */

void pinMode(uint8_t pin, uint8_t mode)
{
    if (pin < MOCK_PIN_COUNT) {
        pin_mode_arr[pin] = mode;
    }
}

void digitalWrite(uint8_t pin, uint8_t val)
{
    if (pin >= MOCK_PIN_COUNT) return;
    pin_state[pin] = val;

    /* Track the maximum number of row pins simultaneously at LOW */
    if (val == LOW) {
        int low_rows = 0;
        for (int i = 0; i < 4; i++) {
            if (pin_state[row_gpios[i]] == LOW) {
                low_rows++;
            }
        }
        if (low_rows > mock_max_rows_low) {
            mock_max_rows_low = low_rows;
        }
    }
}

int digitalRead(uint8_t pin)
{
    /*
     * Column read: return LOW when a pressed key's row is currently driven LOW
     * and this pin is that key's column GPIO — exactly as real hardware behaves.
     */
    for (int k = 0; k < pressed_count; k++) {
        if (pin == pressed_col[k] && pin_state[pressed_row[k]] == LOW) {
            return LOW;
        }
    }
    return HIGH;   /* pull-up default */
}

void delay(unsigned long /*ms*/)
{
    /* No-op in unit-test builds */
}
