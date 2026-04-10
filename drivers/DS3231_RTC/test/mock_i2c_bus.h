/**
 * @file mock_i2c_bus.h
 * @brief I2C bus stubs for the native (host) unit-test build of LIB-3.
 *
 * Replaces the LIB-2 i2c_bus library with an in-memory register array so
 * that @ref ds1307_rtc.cpp can be compiled and tested without target hardware
 * or the Arduino Wire library. This header is included automatically by
 * @ref ds3231_rtc.cpp when @c UNIT_TEST is defined.
 *
 * ### Design
 * - @ref mock_rtc_regs — byte array representing DS3231 registers 0x00–0x12.
 *   Pre-populate using @ref mock_rtc_set_register before calling production code.
 * - @c i2c_write_read — reads from the register array at the offset given by
 *   @c tx[0] and returns @c rx_len bytes.
 * - @c i2c_write — interprets @c data[0] as the register pointer and writes
 *   @c data[1..] into the array. A zero-length write (device probe) is a no-op.
 * - @ref mock_nack_next — set to @c true to make the next @c i2c_write or
 *   @c i2c_write_read return @c I2C_ERR_NACK (then auto-clears).
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * i2c_status_t — mirrors i2c_bus.h (no dependency on actual header)
 * --------------------------------------------------------------------------- */
typedef enum {
    I2C_OK           = 0,
    I2C_ERR_TIMEOUT  = 1,
    I2C_ERR_NACK     = 2,
    I2C_ERR_BUS_BUSY = 3
} i2c_status_t;

/* ---------------------------------------------------------------------------
 * Mock register array (DS3231 address space 0x00–0x12)
 * --------------------------------------------------------------------------- */
#define MOCK_RTC_REG_COUNT  0x08u   /**< Registers 0x00–0x07 inclusive (DS1307 time + control). */

/** @brief In-memory DS1307 register file. Populate via @ref mock_rtc_set_register. */
extern uint8_t mock_rtc_regs[MOCK_RTC_REG_COUNT];

/** @brief When true the next i2c call returns I2C_ERR_NACK then resets. */
extern bool mock_nack_next;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */

/** @brief Reset all mock state to power-on defaults. Call in setUp(). */
void mock_rtc_reset(void);

/**
 * @brief Preset a single DS3231 register value.
 *
 * @param reg  Register address (0x00–0x12).
 * @param val  Value to store.
 */
void mock_rtc_set_register(uint8_t reg, uint8_t val);

/* ---------------------------------------------------------------------------
 * Stubbed i2c_bus API — implemented in mock_i2c_bus.cpp
 * --------------------------------------------------------------------------- */
i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len);
i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len);
i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len);
void         i2c_lock(void);
void         i2c_unlock(void);
