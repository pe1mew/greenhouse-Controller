/**
 * @file mock_keypad.h
 * @brief Arduino GPIO stubs for the native (host) unit-test build of LIB-5.
 *
 * Simulates a 4×4 membrane keypad matrix. A pressed key is identified by its
 * row GPIO and column GPIO. digitalRead on a column pin returns LOW only when
 * the row that holds the pressed key is currently driven LOW — exactly as real
 * membrane-keypad hardware behaves.
 *
 * Multiple simultaneous presses (ghost keys, accidental multi-touch) are
 * supported via mock_keypad_add_key() so that the driver's multi-press
 * rejection can be exercised in unit tests.
 *
 * @note Do NOT include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Arduino constant stubs
 * --------------------------------------------------------------------------- */
#define INPUT         0  /**< Floating input mode. */
#define OUTPUT        1  /**< Push-pull output mode. */
#define INPUT_PULLUP  2  /**< Input with internal pull-up mode. */
#define HIGH          1  /**< Logic high. */
#define LOW           0  /**< Logic low. */

/* ---------------------------------------------------------------------------
 * Mock configuration
 * --------------------------------------------------------------------------- */

/** Maximum number of simultaneously pressed keys the mock tracks. */
#define MOCK_MAX_KEYS   4

/** Size of the pin-state and pin-mode arrays (covers all ESP32-S3 GPIOs). */
#define MOCK_PIN_COUNT  48

/* ---------------------------------------------------------------------------
 * Observable mock state (inspected directly by unit tests)
 * --------------------------------------------------------------------------- */

/**
 * @brief Logic level last written to each GPIO pin by digitalWrite().
 *
 * Index equals the GPIO pin number. Row pins are written by the driver during
 * scanning; column pins are not written (they are inputs).
 */
extern uint8_t pin_state[MOCK_PIN_COUNT];

/**
 * @brief Mode last set for each GPIO pin by pinMode().
 */
extern uint8_t pin_mode_arr[MOCK_PIN_COUNT];

/**
 * @brief Maximum number of row pins that were simultaneously LOW during any
 *        single call to keypad_scan().
 *
 * A correctly implemented driver drives exactly one row LOW at a time, so
 * this value must never exceed 1. Inspected by UT-KP-009.
 */
extern int mock_max_rows_low;

/* ---------------------------------------------------------------------------
 * Mock control functions
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset all mock state to power-on defaults.
 *
 * Clears all pressed keys, resets pin_state and pin_mode_arr, and zeroes
 * mock_max_rows_low. Call from setUp() to guarantee test isolation.
 */
void mock_keypad_reset(void);

/**
 * @brief Simulate a single key press (clears any previously set keys).
 *
 * digitalRead on @p col_gpio will return LOW when @p row_gpio is driven LOW.
 *
 * @param row_gpio  Row GPIO of the pressed key (KP_ROW1 … KP_ROW4).
 * @param col_gpio  Column GPIO of the pressed key (KP_COL1 … KP_COL4).
 */
void mock_keypad_set_key(uint8_t row_gpio, uint8_t col_gpio);

/**
 * @brief Add an additional pressed key without clearing existing ones.
 *
 * Use this to simulate multi-press / ghost-key scenarios. Up to
 * MOCK_MAX_KEYS keys may be active at the same time.
 *
 * @param row_gpio  Row GPIO of the additional key.
 * @param col_gpio  Column GPIO of the additional key.
 */
void mock_keypad_add_key(uint8_t row_gpio, uint8_t col_gpio);

/**
 * @brief Release all pressed keys (simulate idle / open keypad).
 */
void mock_keypad_clear_keys(void);

/* ---------------------------------------------------------------------------
 * Arduino stub declarations
 * --------------------------------------------------------------------------- */

/** @brief Record the requested mode in pin_mode_arr. */
void pinMode(uint8_t pin, uint8_t mode);

/** @brief Write a logic level; also updates mock_max_rows_low tracking. */
void digitalWrite(uint8_t pin, uint8_t val);

/**
 * @brief Read the logic level of a pin.
 *
 * For column pins: returns LOW when a pressed key's row is currently driven
 * LOW, HIGH otherwise (simulating the pull-up resistor).
 * For any other pin: returns pin_state[pin].
 */
int digitalRead(uint8_t pin);

/** @brief No-op delay stub (timing is irrelevant in unit tests). */
void delay(unsigned long ms);
