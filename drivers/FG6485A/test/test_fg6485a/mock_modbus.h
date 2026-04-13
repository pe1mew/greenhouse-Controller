/**
 * @file mock_modbus.h
 * @brief Modbus RTU stubs for the native unit-test build of the FG6485A driver.
 *
 * Provides controllable implementations of:
 *   - modbus_read_holding_registers()
 *   - modbus_write_multiple_registers()
 *
 * These replace the real LIB-6 functions so that fg6485a.cpp can be exercised
 * on the host PC without any hardware or Arduino framework.
 *
 * Usage pattern:
 *   1. Call mock_modbus_reset() in setUp().
 *   2. Pre-load register values with mock_modbus_set_registers().
 *   3. Set a forced status (default MODBUS_OK) with mock_modbus_set_read_status()
 *      or mock_modbus_set_write_status() when testing error paths.
 *   4. After the function under test, inspect writes via mock_modbus_get_last_write_*().
 *
 * @note Do NOT include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include "modbus_rtu.h"   /* for modbus_status_t */

/* ---------------------------------------------------------------------------
 * Test control API
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset all mock state to power-on defaults.
 *
 * - Clears the register bank (all zeros).
 * - Sets read and write status to MODBUS_OK.
 * - Clears the last-write record.
 *
 * Call from setUp() at the start of each test.
 */
void mock_modbus_reset(void);

/**
 * @brief Pre-load a contiguous block of register values into the mock bank.
 *
 * When modbus_read_holding_registers() is called, it returns values from this
 * bank starting at the requested register address.
 *
 * @param start_reg  First register address to write (0x0000–0x00FF).
 * @param values     Array of register values.
 * @param count      Number of registers.
 */
void mock_modbus_set_registers(uint16_t start_reg,
                                const uint16_t *values, uint8_t count);

/**
 * @brief Force the status returned by the next modbus_read_holding_registers() call.
 *
 * Default is MODBUS_OK.  Set to MODBUS_ERR_TIMEOUT, MODBUS_ERR_CRC, etc.
 * to exercise error-handling paths.
 *
 * @param status  Status code to return.
 */
void mock_modbus_set_read_status(modbus_status_t status);

/**
 * @brief Force the status returned by the next modbus_write_multiple_registers() call.
 *
 * Default is MODBUS_OK.
 *
 * @param status  Status code to return.
 */
void mock_modbus_set_write_status(modbus_status_t status);

/**
 * @brief Return the slave address used in the last modbus_write_multiple_registers() call.
 *
 * Returns 0 if no write has been performed since the last mock_modbus_reset().
 */
uint8_t  mock_modbus_get_last_write_addr(void);

/**
 * @brief Return the start register used in the last modbus_write_multiple_registers() call.
 */
uint16_t mock_modbus_get_last_write_reg(void);

/**
 * @brief Return the register count used in the last modbus_write_multiple_registers() call.
 */
uint8_t  mock_modbus_get_last_write_count(void);

/**
 * @brief Return a pointer to the values supplied to the last modbus_write_multiple_registers() call.
 *
 * Valid until the next mock_modbus_reset() or another write call.
 *
 * @return Pointer to the copied values array.
 */
const uint16_t *mock_modbus_get_last_written_values(void);
