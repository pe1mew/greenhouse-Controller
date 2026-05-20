/**
 * @file i2c_bus.h
 * @brief Shared I2C bus driver — types and API for LIB-2.
 *
 * Mutex-aware wrapper around the Arduino @c Wire library for the shared
 * I2C bus.  All I2C peripherals use this driver; no higher-level driver
 * calls @c Wire directly.
 *
 * Pin assignments are centralised in @c firmware/config/pin_config.h
 * (@c PIN_I2C_SDA, @c PIN_I2C_SCL) and exposed here by inclusion so
 * callers need only include this header.
 *
 * ## Hardware
 *   - Bus speed   : 400 kHz Fast-mode (see @ref I2C_FREQ_HZ).
 *   - Pull-ups    : External 4.7 kΩ to 3V3 on SDA and SCL (board-level).
 *   - Pins        : @c PIN_I2C_SDA, @c PIN_I2C_SCL (see @c pin_config.h).
 *
 * ## Peripherals on this bus
 *   - 0x3E — AiP31068L (LCD1602 character controller, LIB-4)
 *   - 0x60 — PCA9633DP2 (LCD1602RGB backlight driver, LIB-4, optional)
 *   - 0x68 — DS1307 (RTC, LIB-3)
 *
 * ## API summary
 *   - @ref i2c_init       Bus configuration (once at boot).
 *   - @ref i2c_write / @ref i2c_read / @ref i2c_write_read
 *                         Bus transactions; each acquires MX1 internally.
 *   - @ref i2c_scan       Bus discovery helper.
 *   - @ref i2c_lock / @ref i2c_unlock
 *                         Coarse-grained lock for multi-transaction sequences
 *                         (e.g. the LCD's write-row-then-set-cursor pattern).
 *
 * ## Thread safety
 *   The bus mutex (MX1) is created on @ref i2c_init.  Single transactions
 *   acquire and release it internally; sequences that must remain atomic
 *   (such as multiple LCD writes) MUST be wrapped in
 *   @ref i2c_lock / @ref i2c_unlock by the caller.  Native-host builds
 *   compile these to no-ops.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include "pin_config.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Bus configuration
 * --------------------------------------------------------------------------- */

/** @brief I2C clock frequency (Fast-mode, 400 kHz). */
#define I2C_FREQ_HZ  400000UL

/* ---------------------------------------------------------------------------
 * @defgroup i2c_types I2C types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Return status for all I2C operations.
 */
typedef enum {
    I2C_OK          = 0, /**< Operation completed successfully. */
    I2C_ERR_TIMEOUT = 1, /**< Bus did not respond within the timeout period. */
    I2C_ERR_NACK    = 2, /**< Device returned NACK (address or data). */
    I2C_ERR_BUS_BUSY = 3 /**< Bus was held by another master or task. */
} i2c_status_t;

/** @} */ /* end i2c_types */

/* ---------------------------------------------------------------------------
 * @defgroup i2c_api I2C API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the I2C bus.
 *
 * Calls @c Wire.begin() on @c PIN_I2C_SDA / @c PIN_I2C_SCL at
 * @ref I2C_FREQ_HZ and creates the bus mutex (MX1) used by all other
 * functions in this header.
 *
 * Must be called once at boot before any other @c i2c_* function or any
 * LIB-3 (RTC) / LIB-4 (LCD) call.
 *
 * @return @ref I2C_OK on success.
 * @warning Calling any I2C-using driver function before @ref i2c_init has
 *          run is undefined behaviour (the mutex handle is uninitialised).
 */
i2c_status_t i2c_init(void);

/**
 * @brief Write bytes to an I2C device.
 *
 * Acquires the bus mutex internally. Zero-length writes are accepted and
 * serve as an address-only probe (the device will ACK or NACK its address).
 *
 * @param addr  7-bit I2C device address.
 * @param data  Pointer to bytes to transmit (may be NULL when len == 0).
 * @param len   Number of bytes to transmit.
 * @return @ref i2c_status_t.
 */
i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Read bytes from an I2C device.
 *
 * @param addr  7-bit I2C device address.
 * @param buf   Buffer to receive bytes. Must be at least @p len bytes.
 * @param len   Number of bytes to read.
 * @return @ref i2c_status_t.
 */
i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len);

/**
 * @brief Write-then-read in a single bus transaction (repeated start).
 *
 * Sends at most @p tx_len bytes to @p addr with no stop condition, then
 * reads @p rx_len bytes. Used for register-addressed reads (e.g. RTC).
 *
 * @param addr    7-bit I2C device address.
 * @param tx      Bytes to transmit (e.g. register address).
 * @param tx_len  Number of bytes to transmit.
 * @param rx      Buffer to receive the response.
 * @param rx_len  Number of bytes to read.
 * @return @ref i2c_status_t.
 */
i2c_status_t i2c_write_read(uint8_t addr,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t       *rx, size_t rx_len);

/**
 * @brief Scan the I2C bus and collect the addresses of responding devices.
 *
 * Probes addresses 1–126 with a zero-byte write and records any that ACK.
 *
 * @param found_addrs  Buffer to store found addresses.
 * @param max_count    Capacity of @p found_addrs.
 * @return Number of devices found.
 */
uint8_t i2c_scan(uint8_t *found_addrs, uint8_t max_count);

/**
 * @brief Acquire the shared I2C bus mutex.
 *
 * Blocks until the mutex is available. In the native (host) build this is a
 * no-op. In the FreeRTOS target build this will acquire the mutex, preventing
 * interleaved access from another task.
 *
 * Use i2c_lock() / i2c_unlock() only when a sequence of operations must not
 * be interrupted — individual i2c_write() / i2c_read() calls already acquire
 * and release the mutex internally.
 */
void i2c_lock(void);

/**
 * @brief Release the shared I2C bus mutex.
 *
 * Must be called after every i2c_lock() call. In the native build this is a
 * no-op.
 */
void i2c_unlock(void);

/** @} */ /* end i2c_api */
