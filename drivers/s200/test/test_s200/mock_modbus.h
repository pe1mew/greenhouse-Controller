/**
 * @file mock_modbus.h
 * @brief Modbus RTU stubs for the native unit-test build of the S200 driver.
 *
 * Provides controllable implementations of:
 *   - modbus_read_input_registers()    (FC04 — used by s200.cpp for all reads)
 *   - modbus_read_holding_registers()  (FC03 — stubbed for completeness)
 *   - modbus_write_multiple_registers()(FC16 — stubbed for completeness)
 *
 * Both FC03 and FC04 reads draw from a single shared 512-entry register bank,
 * addressed directly by register number.  The bank is large enough to cover
 * all S200 registers (highest address = 0x001D = 29).
 *
 * Usage pattern:
 *   1. Call mock_modbus_reset() in setUp().
 *   2. Pre-load register values with mock_modbus_set_registers().
 *   3. Set a forced status (default MODBUS_OK) with mock_modbus_set_read_status()
 *      when testing error paths.
 *   4. After the function under test, inspect the last FC04 call via
 *      mock_modbus_get_last_read_*() to verify the correct register range.
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
 * - Clears the last-read and last-write records.
 *
 * Call from setUp() at the start of each test.
 */
void mock_modbus_reset(void);

/**
 * @brief Pre-load a contiguous block of register values into the mock bank.
 *
 * When modbus_read_input_registers() or modbus_read_holding_registers() is
 * called, values are returned from this bank starting at the requested address.
 *
 * @param start_reg  First register address to write (0x0000–0x01FF).
 * @param values     Array of register values.
 * @param count      Number of registers.
 */
void mock_modbus_set_registers(uint16_t start_reg,
                                const uint16_t *values, uint8_t count);

/**
 * @brief Force the status returned by the next read call (FC03 or FC04).
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

/* --- Last FC04 (input register) read getters --- */

/** @brief Slave address used in the last modbus_read_input_registers() call. */
uint8_t  mock_modbus_get_last_read_addr(void);

/** @brief Start register used in the last modbus_read_input_registers() call. */
uint16_t mock_modbus_get_last_read_reg(void);

/** @brief Count used in the last modbus_read_input_registers() call. */
uint8_t  mock_modbus_get_last_read_count(void);

/* --- Last FC16 write getters --- */

/** @brief Slave address used in the last modbus_write_multiple_registers() call. */
uint8_t  mock_modbus_get_last_write_addr(void);

/** @brief Start register used in the last modbus_write_multiple_registers() call. */
uint16_t mock_modbus_get_last_write_reg(void);

/** @brief Count used in the last modbus_write_multiple_registers() call. */
uint8_t  mock_modbus_get_last_write_count(void);

/**
 * @brief Pointer to the values supplied to the last modbus_write_multiple_registers() call.
 *
 * Valid until the next mock_modbus_reset() or another write call.
 */
const uint16_t *mock_modbus_get_last_written_values(void);
