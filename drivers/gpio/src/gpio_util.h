/**
 * @file gpio_util.h
 * @brief GPIO utility driver — types and API for LIB-1.
 *
 * Thin abstraction layer over the Arduino GPIO API used by every other
 * driver.  Pin assignments are centralised in @c firmware/config/pin_config.h
 * and exposed here by inclusion so callers need only include this header.
 *
 * ## Hardware
 *   - MCU         : Espressif ESP32-S3 (LOLIN S3).
 *   - Logic level : 3.3 V CMOS; pins are not 5 V tolerant.
 *   - Drive       : ~20 mA per pin sink/source; respect total package limit.
 *   - Pull-ups    : Internal weak pull-up (~45 kΩ) selectable via
 *                   @ref GPIO_INPUT_PULLUP.
 *   - RS-485 ctrl : One dedicated pin @c PIN_RS485_DE_RE drives the
 *                   SIT65HVD08P transceiver DE/RE jointly — HIGH = TX,
 *                   LOW = RX.
 *
 * ## API summary
 *   - @ref gpio_set_pin_mode         Configure direction / pull-up.
 *   - @ref gpio_write / @ref gpio_read / @ref gpio_toggle
 *                                    Generic pin I/O.
 *   - @ref gpio_rs485_init           One-shot RS-485 DE/RE pin init.
 *   - @ref gpio_set_rs485_direction  Toggle half-duplex transceiver direction.
 *
 * ## Thread safety
 *   The Arduino GPIO peripheral writes are not protected by a mutex.  Two
 *   tasks writing the same pin simultaneously will produce a last-writer-
 *   wins outcome (no register corruption, but logical races are possible).
 *   The RS-485 DE/RE pin is owned exclusively by LIB-6 (@c modbus_rtu) once
 *   @ref gpio_rs485_init has run.
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
 * @brief Initialise the RS-485 direction pin.
 *
 * Configures @c PIN_RS485_DE_RE as a push-pull output and drives it LOW
 * (receiver enabled — safe idle state on the differential bus).
 *
 * @warning Must be called once before any call to
 *          @ref gpio_set_rs485_direction or any LIB-6 transaction.
 * @see    gpio_set_rs485_direction(), modbus_init().
 */
void gpio_rs485_init(void);

/**
 * @brief Set the RS-485 transceiver direction.
 *
 * Controls the DE/RE line on the SIT65HVD08P (or equivalent) transceiver
 * via @c PIN_RS485_DE_RE.  DE and RE are tied together on the board so a
 * single GPIO toggles the half-duplex direction.
 *
 * @param transmit @c true  — assert HIGH (driver enable / TX mode). \n
 *                 @c false — assert LOW  (receiver enable / RX mode).
 * @warning The caller is responsible for inter-byte and turn-around timing.
 *          The transceiver needs a few µs to switch direction; LIB-6 holds
 *          DE HIGH until the final byte has clocked out of the UART FIFO
 *          before dropping it LOW again.
 * @see    gpio_rs485_init().
 */
void gpio_set_rs485_direction(bool transmit);

/** @} */ /* end gpio_api */
