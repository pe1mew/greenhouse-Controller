/**
 * @file gpio_util.h
 * @brief GPIO utility driver — types and API for LIB-1.
 *
 * Thin abstraction layer over the Arduino GPIO API used by every other driver.
 * Pin assignments are centralised in firmware/config/pin_config.h and exposed
 * here by inclusion so callers need only include this header.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include "pin_config.h"
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * @defgroup gpio_types GPIO types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Pin direction / mode selector.
 */
typedef enum {
    GPIO_INPUT        = 0, /**< Floating input. */
    GPIO_OUTPUT       = 1, /**< Push-pull output. */
    GPIO_INPUT_PULLUP = 2  /**< Input with internal pull-up resistor enabled. */
} gpio_util_mode_t;

/**
 * @brief Logic level on a GPIO pin.
 */
typedef enum {
    GPIO_LOW  = 0, /**< Logic low  (0 V). */
    GPIO_HIGH = 1  /**< Logic high (3.3 V). */
} gpio_util_level_t;

/** @} */ /* end gpio_types */

/* ---------------------------------------------------------------------------
 * @defgroup gpio_api GPIO API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Configure the direction / mode of a GPIO pin.
 *
 * @param pin  GPIO number (use a PIN_* constant from this header).
 * @param mode Desired pin mode (@ref gpio_util_mode_t).
 */
void gpio_set_pin_mode(uint8_t pin, gpio_util_mode_t mode);

/**
 * @brief Write a logic level to an output pin.
 *
 * @param pin   GPIO number.
 * @param level @ref GPIO_HIGH or @ref GPIO_LOW.
 */
void gpio_write(uint8_t pin, gpio_util_level_t level);

/**
 * @brief Read the current logic level of a pin.
 *
 * @param pin GPIO number.
 * @return @ref GPIO_HIGH or @ref GPIO_LOW.
 */
gpio_util_level_t gpio_read(uint8_t pin);

/**
 * @brief Toggle the output level of a pin.
 *
 * If the pin is currently @ref GPIO_HIGH it is driven @ref GPIO_LOW, and
 * vice-versa.
 *
 * @param pin GPIO number.
 */
void gpio_toggle(uint8_t pin);

/**
 * @brief Set the RS-485 transceiver direction.
 *
 * Controls the DE/RE line on the SIT65HVD08P (or equivalent) transceiver
 * via @ref PIN_RS485_DE_RE.
 *
 * @param transmit @c true  — assert HIGH (driver enable / TX mode). \n
 *                 @c false — assert LOW  (receiver enable / RX mode).
 */
void gpio_set_rs485_direction(bool transmit);

/** @} */ /* end gpio_api */
