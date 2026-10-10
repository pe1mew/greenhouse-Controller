/**
 * @file mock_keypad.cpp
 * @brief LIB-1 GPIO API answered from a simulated membrane keypad (LIB-5 unit tests).
 *
 * keypad_matrix.cpp uses gpio_set_pin_mode(), gpio_write() and gpio_read();
 * only those three are implemented. See mock_keypad.h for the model.
 */

#include "mock_keypad.h"
#include "pin_config.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Observable state
 * --------------------------------------------------------------------------- */
int mock_max_rows_low;
int mock_keypad_floating_reads;
int mock_keypad_row_contention;

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */
static int     s_mode[MOCK_PIN_COUNT];     /* gpio_util_mode_t or MOCK_PIN_UNCONFIGURED */
static uint8_t s_latch[MOCK_PIN_COUNT];    /* 0 / 1 */

static uint8_t pressed_row[MOCK_MAX_KEYS];
static uint8_t pressed_col[MOCK_MAX_KEYS];
static int     pressed_count;

static const uint8_t row_gpios[4] = {KP_ROW1, KP_ROW2, KP_ROW3, KP_ROW4};

/* True when @p pin drives its line; *level receives what it drives. */
static bool pin_drives(uint8_t pin, gpio_util_level_t *level)
{
    if (s_mode[pin] != GPIO_OUTPUT) {
        return false;
    }
    *level = s_latch[pin] ? GPIO_HIGH : GPIO_LOW;
    return true;
}

static void track_rows_low(void)
{
    int low_rows = 0;
    for (int i = 0; i < 4; i++) {
        gpio_util_level_t level;
        if (pin_drives(row_gpios[i], &level) && level == GPIO_LOW) {
            low_rows++;
        }
    }
    if (low_rows > mock_max_rows_low) {
        mock_max_rows_low = low_rows;
    }
}

/* ---------------------------------------------------------------------------
 * Mock control functions
 * --------------------------------------------------------------------------- */

void mock_keypad_reset(void)
{
    for (int i = 0; i < MOCK_PIN_COUNT; i++) {
        s_mode[i] = MOCK_PIN_UNCONFIGURED;
    }
    memset(s_latch, 0, sizeof(s_latch));
    pressed_count              = 0;
    mock_max_rows_low          = 0;
    mock_keypad_floating_reads = 0;
    mock_keypad_row_contention = 0;
}

void mock_keypad_set_key(uint8_t row_gpio, uint8_t col_gpio)
{
    pressed_row[0] = row_gpio;
    pressed_col[0] = col_gpio;
    pressed_count  = 1;
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

int mock_keypad_pin_mode(uint8_t pin)
{
    return (pin < MOCK_PIN_COUNT) ? s_mode[pin] : MOCK_PIN_UNCONFIGURED;
}

gpio_util_level_t mock_keypad_pin_latch(uint8_t pin)
{
    return (pin < MOCK_PIN_COUNT && s_latch[pin]) ? GPIO_HIGH : GPIO_LOW;
}

/* ---------------------------------------------------------------------------
 * LIB-1 API (gpio_util.h)
 * --------------------------------------------------------------------------- */

void gpio_set_pin_mode(uint8_t pin, gpio_util_mode_t mode)
{
    if (pin >= MOCK_PIN_COUNT) return;
    s_mode[pin] = (int)mode;
    track_rows_low();          /* a newly enabled output drives its latch at once */
}

void gpio_write(uint8_t pin, gpio_util_level_t level)
{
    if (pin >= MOCK_PIN_COUNT) return;
    s_latch[pin] = (level == GPIO_HIGH) ? 1u : 0u;
    track_rows_low();
}

gpio_util_level_t gpio_read(uint8_t pin)
{
    if (pin >= MOCK_PIN_COUNT) return GPIO_LOW;

    gpio_util_level_t own;
    if (pin_drives(pin, &own)) {
        return own;
    }

    /* Column read: look through every pressed key on this column for a row
     * that is actually driving the line. */
    bool pulled_low  = false;
    bool pulled_high = false;
    for (int k = 0; k < pressed_count; k++) {
        gpio_util_level_t row_level;
        if (pressed_col[k] == pin && pin_drives(pressed_row[k], &row_level)) {
            if (row_level == GPIO_LOW) pulled_low  = true;
            else                       pulled_high = true;
        }
    }
    if (pulled_low && pulled_high) {
        mock_keypad_row_contention++;
        return GPIO_LOW;
    }
    if (pulled_low)  return GPIO_LOW;
    if (pulled_high) return GPIO_HIGH;

    if (s_mode[pin] == GPIO_INPUT_PULLUP) {
        return GPIO_HIGH;
    }
    mock_keypad_floating_reads++;
    return GPIO_LOW;
}
