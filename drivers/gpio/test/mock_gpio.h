/**
 * @file mock_gpio.h
 * @brief Arduino GPIO stubs for the native (host) unit-test build of LIB-1.
 *
 * Replaces the Arduino HAL with an in-memory state array so that
 * @ref gpio_util.cpp can be compiled and tested without target hardware.
 * This header is included automatically by @ref gpio_util.cpp when
 * @c UNIT_TEST is defined.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @defgroup mock_gpio_constants Arduino constant stubs
 * @brief Numeric equivalents of the Arduino pin-mode and logic-level macros.
 * @{
 */
#define INPUT         0 /**< Floating input mode. */
#define OUTPUT        1 /**< Push-pull output mode. */
#define INPUT_PULLUP  2 /**< Input with internal pull-up mode. */
#define HIGH          1 /**< Logic high level. */
#define LOW           0 /**< Logic low level. */
/** @} */

/**
 * @brief Recorded logic level for each GPIO pin.
 *
 * Index equals the GPIO pin number. Written by @ref digitalWrite,
 * read by @ref digitalRead.
 */
extern uint8_t pin_state[48];

/**
 * @brief Recorded mode for each GPIO pin.
 *
 * Index equals the GPIO pin number. Written by @ref pinMode.
 */
extern uint8_t pin_mode_arr[48];

/**
 * @defgroup mock_gpio_stubs Arduino stub functions
 * @brief In-process replacements for the Arduino GPIO API.
 * @{
 */

/**
 * @brief Record the requested mode in @ref pin_mode_arr.
 *
 * @param pin  GPIO pin number (0–47).
 * @param mode One of @c INPUT, @c OUTPUT, or @c INPUT_PULLUP.
 */
void pinMode(uint8_t pin, uint8_t mode);

/**
 * @brief Write a logic level to @ref pin_state.
 *
 * @param pin GPIO pin number (0–47).
 * @param val @c HIGH or @c LOW.
 */
void digitalWrite(uint8_t pin, uint8_t val);

/**
 * @brief Read the logic level from @ref pin_state.
 *
 * @param pin GPIO pin number (0–47).
 * @return @c HIGH or @c LOW.
 */
int digitalRead(uint8_t pin);

/** @} */ /* end mock_gpio_stubs */

/**
 * @brief Reset all mock state to its power-on default.
 *
 * Sets every entry in @ref pin_state to @c LOW and every entry in
 * @ref pin_mode_arr to @c INPUT.  Call this from @c setUp() at the
 * start of each Unity test case to guarantee test isolation.
 */
void mock_gpio_reset(void);
