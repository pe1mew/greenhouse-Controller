/**
 * @file mock_keypad.h
 * @brief Simulated 4×4 membrane keypad behind the LIB-1 API (LIB-5 native tests).
 *
 * keypad_matrix.cpp drives the matrix through gpio_util (LIB-1):
 * gpio_set_pin_mode(), gpio_write() and gpio_read(). mock_keypad.cpp
 * implements those three — against the real gpio_util.h, found through
 * `-I ../gpio/src` — with an electrical model of a diode-less membrane
 * matrix wired as pin_config.h describes:
 *
 * - **Rows drive only when configured.** A pin drives its line only while it
 *   is GPIO_OUTPUT, and then at its output latch. The latch starts at 0
 *   (the ESP32-S3 GPIO_OUT reset value) and gpio_write() sets it whatever
 *   the mode, so a row written LOW but never configured drives nothing.
 * - **A pressed key joins its row line to its column line.** A column reads
 *   what a driving row puts on it through a pressed key. With no such row
 *   it reads HIGH only if its pull-up is on (GPIO_INPUT_PULLUP). Otherwise
 *   it FLOATS: counted in @ref mock_keypad_floating_reads, read as LOW.
 *   LOW is the pessimistic answer; on the board noise can make a floating
 *   column look like a pressed key.
 * - **Two pressed keys in one column join two rows.** If those rows drive
 *   opposite levels, two SoC outputs fight through the membrane: counted in
 *   @ref mock_keypad_row_contention; the column reads LOW.
 * - **Outputs read back their latch.** gpio_util configures GPIO_OUTPUT as
 *   input+output precisely so gpio_read() sees it.
 *
 * Idealised: contact resistance, bounce and the diode-less ghost-key path
 * are not modelled (driverDevelopmentPlan.md, HW-KP-005, covers the latter).
 *
 * This header is included automatically by keypad_matrix.cpp when UNIT_TEST
 * is defined.
 *
 * @note Do NOT include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#pragma once

#include <stdint.h>
#include "gpio_util.h"   /* the real LIB-1 header: this mock implements its API */

/* ---------------------------------------------------------------------------
 * Mock configuration
 * --------------------------------------------------------------------------- */

/** Maximum number of simultaneously pressed keys the mock tracks. */
#define MOCK_MAX_KEYS   4

/** Pins in the model (covers all ESP32-S3 GPIOs). */
#define MOCK_PIN_COUNT  49

/** mock_keypad_pin_mode() result for a pin gpio_set_pin_mode() never touched. */
#define MOCK_PIN_UNCONFIGURED  (-1)

/* ---------------------------------------------------------------------------
 * Observable mock state (inspected directly by unit tests)
 * --------------------------------------------------------------------------- */

/**
 * @brief Peak number of row pins simultaneously DRIVEN LOW since the last reset.
 *
 * Re-evaluated after every gpio_set_pin_mode() and gpio_write(). A correct
 * driver drives exactly one row LOW at a time, so this must never exceed 1.
 * Inspected by UT-KP-009.
 */
extern int mock_max_rows_low;

/** @brief gpio_read() calls on a column that nothing drives and nothing pulls up. */
extern int mock_keypad_floating_reads;

/** @brief gpio_read() calls on a column tied to one row driven LOW and one driven HIGH. */
extern int mock_keypad_row_contention;

/* ---------------------------------------------------------------------------
 * Mock control functions
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset all mock state to power-on defaults.
 *
 * Releases all keys, returns every pin to unconfigured with latch 0, and
 * zeroes the counters. Call from setUp() to guarantee test isolation.
 */
void mock_keypad_reset(void);

/**
 * @brief Simulate a single key press (clears any previously set keys).
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

/**
 * @brief Mode last given to a pin by gpio_set_pin_mode().
 * @return A gpio_util_mode_t value, or MOCK_PIN_UNCONFIGURED.
 */
int mock_keypad_pin_mode(uint8_t pin);

/**
 * @brief Output latch of a pin (what it drives while it is an output).
 * @return GPIO_LOW or GPIO_HIGH.
 */
gpio_util_level_t mock_keypad_pin_latch(uint8_t pin);
