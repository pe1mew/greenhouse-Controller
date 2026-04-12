/**
 * @file mock_gpio.h
 * @brief gpio_set_rs485_direction() stub for the native unit-test build of LIB-6.
 *
 * In the native (NATIVE_TEST) build, the gpio/ library is not linked.  This
 * header provides the gpio_set_rs485_direction() stub that modbus_rtu.cpp
 * calls, and records each DE/RE transition into the shared event log defined
 * in mock_uart.h (MOCK_EVT_GPIO_HIGH / MOCK_EVT_GPIO_LOW).
 *
 * The current direction state can be queried by mock_gpio_get_direction() for
 * direct-state assertions, and the event log order is used by UT-MB-009 and
 * UT-MB-010 to verify that the DE/RE transitions occur before the first UART
 * write and before the first UART read respectively.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdbool.h>
#include "mock_uart.h"   /* for mock_log_event / mock_event_t */

/**
 * @brief Stub for gpio_rs485_init() from LIB-1 gpio/.
 *
 * No-op in the native test build — pin mode configuration is irrelevant
 * when there is no real hardware.
 */
void gpio_rs485_init(void);

/**
 * @brief Stub for gpio_set_rs485_direction() from LIB-1 gpio/.
 *
 * Records MOCK_EVT_GPIO_HIGH when @p transmit is @c true, or
 * MOCK_EVT_GPIO_LOW when @p transmit is @c false.
 * Updates the internal direction state returned by mock_gpio_get_direction().
 *
 * @param transmit @c true  — DE/RE HIGH (driver enable / TX mode). \n
 *                 @c false — DE/RE LOW  (receiver enable / RX mode).
 */
void gpio_set_rs485_direction(bool transmit);

/**
 * @brief Return the current mocked DE/RE direction.
 *
 * @return @c true  if the last call was gpio_set_rs485_direction(true). \n
 *         @c false if the last call was gpio_set_rs485_direction(false)
 *                  or if mock_gpio_reset() was called (power-on default).
 */
bool mock_gpio_get_direction(void);

/**
 * @brief Reset the GPIO mock to its power-on default (direction = false / RX).
 *
 * Call from setUp() together with mock_uart_reset() to ensure test isolation.
 * Note: this does NOT reset the event log — that is handled by mock_uart_reset().
 */
void mock_gpio_reset(void);
