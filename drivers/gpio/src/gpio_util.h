/**
 * @file gpio_util.h
 * @brief GPIO utility driver — pin constants, types and API for LIB-1.
 *
 * Single authoritative source for all project GPIO numbers and the thin
 * abstraction layer over the Arduino GPIO API used by every other driver.
 *
 * @par Reserved ESP32-S3 pins (must not be used)
 *   0, 19, 20, 26–37, 43, 44, 45, 46
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * @defgroup gpio_pins GPIO pin assignments
 * @{
 * --------------------------------------------------------------------------- */

/** @brief Relay output — motor 1 OPEN direction. */
#define PIN_RELAY_M1_OPEN   12

/** @brief Relay output — motor 1 CLOSE direction. */
#define PIN_RELAY_M1_CLOSE  13

/** @brief Relay output — motor 2 OPEN direction. */
#define PIN_RELAY_M2_OPEN   14

/** @brief Relay output — motor 2 CLOSE direction. */
#define PIN_RELAY_M2_CLOSE  15

/** @brief Relay output — motor 3 OPEN direction. */
#define PIN_RELAY_M3_OPEN   16

/** @brief Relay output — motor 3 CLOSE direction. */
#define PIN_RELAY_M3_CLOSE  21

/** @brief Digital input — opto-coupler feedback from RRK-3 motor controller. */
#define PIN_OPTO_INPUT      42

/** @brief Digital output — heartbeat / status LED (amber). */
#define PIN_HB_LED          41

/** @brief Digital output — RS-485 transceiver DE/RE direction control. */
#define PIN_RS485_DE_RE      8

/** @brief Digital output — SD card status LED. */
#define PIN_SD_STATUS_LED   39

/** @brief Digital input — SD card safe-unmount button. */
#define PIN_SD_MOUNT_BTN    40

/** @} */ /* end gpio_pins */

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
