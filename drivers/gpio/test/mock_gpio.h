/**
 * @file mock_gpio.h
 * @brief Simulated ESP32-S3 GPIO pads for the native (host) unit-test build of LIB-1.
 *
 * gpio_util.cpp is compiled unchanged. Its ESP-IDF calls — gpio_config(),
 * gpio_set_level(), gpio_get_level() — land in mock_gpio.cpp, which answers
 * them from a per-pad model of the silicon, so a test can assert what the
 * pin would really do, not just which function was called:
 *
 * - **Output latch vs pad.** gpio_set_level() always writes the latch (the
 *   GPIO_OUT register bit, 0 after reset). The pad follows the latch only
 *   while the output driver is enabled.
 * - **Input buffer.** gpio_get_level() returns 0 when the pad's input buffer
 *   is off, whatever the pad carries. ESP-IDF 5.5 documents this, and it is
 *   why gpio_util maps GPIO_OUTPUT to GPIO_MODE_INPUT_OUTPUT: with
 *   GPIO_MODE_OUTPUT, gpio_read() and gpio_toggle() cannot see the latch.
 *   A regression there fails UT-GPIO-005 here, as it would on the board.
 * - **Pad level.** Driven by the SoC while output is enabled. Otherwise by
 *   the outside world (mock_gpio_drive_external()), else by an enabled pull,
 *   else nothing: the pad floats. A floating read is counted in
 *   @ref mock_gpio_floating_reads and returns 0.
 * - **Contention.** The SoC and the outside world driving opposite levels is
 *   counted in @ref mock_gpio_contention. The SoC's level is reported.
 * - **Validation.** gpio_config() and gpio_set_level() refuse a pin the S3
 *   does not have with ESP_ERR_INVALID_ARG and change nothing, as ESP-IDF
 *   5.5 does. gpio_util ignores those return codes (its API is void), so the
 *   refusals are counted for the tests to see.
 *
 * Not modelled: interrupts (intr_type is only recorded), drive strength,
 * hold, RTC IO, sleep. gpio_util uses none of them.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"   /* test/mock_idf/driver/gpio.h */

/** @brief Pads in the model: GPIO 0-48 (22-25 exist in the array but are never valid). */
#define MOCK_GPIO_PIN_COUNT  SOC_GPIO_PIN_COUNT

/** @brief What the circuit outside the SoC does to a pad. */
typedef enum {
    MOCK_EXT_NONE = 0, /**< Nothing drives the pad: it floats unless a pull is on. */
    MOCK_EXT_LOW,      /**< The outside world holds the pad LOW. */
    MOCK_EXT_HIGH      /**< The outside world holds the pad HIGH. */
} mock_gpio_ext_t;

/** @brief Snapshot of one simulated pad. */
typedef struct {
    bool            configured;  /**< gpio_config() has applied a config to this pad. */
    gpio_config_t   last_config; /**< The config struct exactly as the driver passed it. */
    bool            input_en;    /**< Input buffer on. */
    bool            output_en;   /**< Output driver on. */
    bool            open_drain;  /**< Output releases the pad instead of driving HIGH. */
    bool            pull_up;     /**< Internal pull-up on. */
    bool            pull_down;   /**< Internal pull-down on. */
    uint8_t         out_latch;   /**< GPIO_OUT bit: 0 or 1. */
    mock_gpio_ext_t external;    /**< What the outside world drives. */
} mock_gpio_pad_t;

/* ---------------------------------------------------------------------------
 * Counters (cleared by mock_gpio_reset())
 * --------------------------------------------------------------------------- */

/** @brief gpio_config() calls, accepted or refused. */
extern int mock_gpio_config_calls;

/** @brief gpio_config() calls refused with ESP_ERR_INVALID_ARG. */
extern int mock_gpio_config_rejected;

/** @brief gpio_set_level() calls refused with ESP_ERR_INVALID_ARG. */
extern int mock_gpio_set_level_rejected;

/** @brief gpio_get_level() calls on a pad that nothing drives or pulls. */
extern int mock_gpio_floating_reads;

/** @brief Times the SoC and the outside world drove a pad to opposite levels. */
extern int mock_gpio_contention;

/* ---------------------------------------------------------------------------
 * Control and inspection
 * --------------------------------------------------------------------------- */

/**
 * @brief Return every pad to its reset state and clear the counters.
 *
 * Reset state: never configured, input/output/pulls off, latch 0, nothing
 * driving from outside. Call from setUp().
 */
void mock_gpio_reset(void);

/**
 * @brief Read-only view of one pad.
 * @param pin GPIO number, 0 to MOCK_GPIO_PIN_COUNT - 1.
 * @return The pad, or NULL for an out-of-range number.
 */
const mock_gpio_pad_t *mock_gpio_pad(uint8_t pin);

/**
 * @brief The level the pad physically carries, as a meter on the pin would show.
 * @param pin GPIO number.
 * @return 0 or 1, or -1 when the pad floats (or the number is out of range).
 */
int mock_gpio_pad_level(uint8_t pin);

/**
 * @brief Set what the external circuit drives onto a pad.
 * @param pin   GPIO number.
 * @param drive @ref MOCK_EXT_NONE to release the pad.
 */
void mock_gpio_drive_external(uint8_t pin, mock_gpio_ext_t drive);
