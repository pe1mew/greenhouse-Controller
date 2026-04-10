/**
 * @file mock_i2c_bus.h
 * @brief I2C bus stubs for the native (host) unit-test build of LIB-4.
 *
 * Replaces the LIB-2 i2c_bus library with a TX transmission log so that
 * @ref lcd1602.cpp can be compiled and tested without target hardware or
 * the Arduino Wire library.  This header is included automatically by
 * @ref lcd1602.cpp when @c UNIT_TEST is defined.
 *
 * ### Design
 * The LCD is write-only from the bus perspective.  Every call to
 * @c i2c_write(LCD_I2C_ADDR, data, len) with @c len > 0 appends @c len
 * bytes to @ref mock_lcd_tx_buf and increments @ref mock_lcd_tx_len.
 *
 * A zero-length write (@c len == 0 or @c data == NULL) is treated as a
 * device-presence probe and adds no bytes to the log.
 *
 * ### NACK injection
 * Set @ref mock_nack_next to @c true before calling production code; the
 * next @c i2c_write call returns @c I2C_ERR_NACK and clears the flag.
 * This is used by UT-LCD-011 to test the @c LCD_ERR_NO_DEVICE path.
 *
 * ### Decoding helper
 * PCF8574A byte layout (each nibble transfer = 2 bytes in the log):
 * @code
 *   bits 7–4 : DB7–DB4 (4-bit nibble value)
 *   bit  3   : backlight (1 = on)
 *   bit  2   : En  (1 = strobe high, 0 = strobe low / data latched)
 *   bit  1   : RW  (always 0)
 *   bit  0   : RS  (0 = command, 1 = data)
 * @endcode
 * A full HD44780 byte operation produces 4 bytes in the log (high nibble
 * pair, then low nibble pair).
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
 * Buffer sizes
 * --------------------------------------------------------------------------- */

/** @brief Capacity of the TX log (sufficient for init + several rows of text). */
#define MOCK_LCD_TX_BUF  512u

/* ---------------------------------------------------------------------------
 * i2c_status_t — mirrors i2c_bus.h (no dependency on the actual header)
 * --------------------------------------------------------------------------- */
typedef enum {
    I2C_OK           = 0,
    I2C_ERR_TIMEOUT  = 1,
    I2C_ERR_NACK     = 2,
    I2C_ERR_BUS_BUSY = 3
} i2c_status_t;

/* ---------------------------------------------------------------------------
 * Observable state (read by test assertions)
 * --------------------------------------------------------------------------- */

/** @brief Bytes sent to LCD_I2C_ADDR in order of transmission. */
extern uint8_t  mock_lcd_tx_buf[MOCK_LCD_TX_BUF];

/** @brief Number of bytes currently in @ref mock_lcd_tx_buf. */
extern uint16_t mock_lcd_tx_len;

/* ---------------------------------------------------------------------------
 * Test-control knobs
 * --------------------------------------------------------------------------- */

/** @brief When true the next i2c_write returns I2C_ERR_NACK then resets. */
extern bool mock_nack_next;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */

/** @brief Reset all mock state to power-on defaults.  Call in setUp(). */
void mock_lcd_reset(void);

/**
 * @brief Copy the current TX log into @p buf.
 *
 * @param buf     Destination buffer.
 * @param maxlen  Maximum bytes to copy.
 * @return Number of bytes copied (min of @ref mock_lcd_tx_len and @p maxlen).
 */
uint16_t mock_lcd_get_transmitted_bytes(uint8_t *buf, uint16_t maxlen);

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
