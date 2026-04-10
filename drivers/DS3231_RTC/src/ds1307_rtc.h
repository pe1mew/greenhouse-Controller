/**
 * @file ds1307_rtc.h
 * @brief DS1307 real-time clock driver — types and API for LIB-3.
 *
 * Thin driver over the LIB-2 I2C bus for the DS1307 battery-backed RTC.
 * All time fields are stored and retrieved in human-readable decimal; BCD
 * encoding/decoding is handled internally.
 *
 * Callers must call i2c_init() (from i2c_bus.h) before rtc_init().
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Device address
 * --------------------------------------------------------------------------- */

/** @brief DS1307 7-bit I2C address (factory-fixed). */
#define DS1307_I2C_ADDR  0x68

/* ---------------------------------------------------------------------------
 * @defgroup rtc_types RTC types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Calendar / time structure (all fields in decimal, not BCD).
 */
typedef struct {
    uint8_t  second;      /**< Seconds,      0–59. */
    uint8_t  minute;      /**< Minutes,      0–59. */
    uint8_t  hour;        /**< Hours,        0–23 (24-hour format). */
    uint8_t  day_of_week; /**< Day of week,  1–7 (1 = Sunday, user-defined). */
    uint8_t  day;         /**< Day of month, 1–31. */
    uint8_t  month;       /**< Month,        1–12. */
    uint16_t year;        /**< Full year, e.g. 2026 (2000–2099 supported). */
} rtc_datetime_t;

/**
 * @brief Return status for all RTC operations.
 */
typedef enum {
    RTC_OK           = 0, /**< Operation succeeded. */
    RTC_ERR_NO_DEVICE = 1, /**< Device did not ACK — not connected or wrong address. */
    RTC_ERR_COMM     = 2, /**< I2C communication error during read/write. */
    RTC_ERR_INVALID  = 3  /**< Register data contains an out-of-range value. */
} rtc_status_t;

/** @} */ /* end rtc_types */

/* ---------------------------------------------------------------------------
 * @defgroup rtc_api RTC API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Probe the DS1307 on the I2C bus.
 *
 * Sends a zero-byte write to @ref DS1307_I2C_ADDR. Returns
 * @ref RTC_ERR_NO_DEVICE if the device does not ACK.
 * i2c_init() must have been called before this function.
 *
 * @return @ref rtc_status_t.
 */
rtc_status_t rtc_init(void);

/**
 * @brief Read the current date and time from the DS3231.
 *
 * Reads registers 0x00–0x06, BCD-decodes all fields, and validates ranges.
 *
 * @param dt  Pointer to caller-allocated @ref rtc_datetime_t to fill.
 * @return @ref RTC_OK, @ref RTC_ERR_COMM, or @ref RTC_ERR_INVALID.
 */
rtc_status_t rtc_get_time(rtc_datetime_t *dt);

/**
 * @brief Write a new date and time to the DS3231.
 *
 * BCD-encodes all fields and writes registers 0x00–0x06. No input validation
 * is performed — caller must supply a valid datetime.
 *
 * @param dt  Pointer to @ref rtc_datetime_t containing the time to set.
 * @return @ref RTC_OK or @ref RTC_ERR_COMM.
 */
rtc_status_t rtc_set_time(const rtc_datetime_t *dt);

/**
 * @brief Check whether the Clock Halt (CH) bit is set.
 *
 * The CH bit is bit 7 of the DS1307 seconds register (0x00). It is set to 1
 * at initial power-up (oscillator disabled) and cleared automatically to 0
 * whenever a valid time is written via @ref rtc_set_time (BCD seconds 0–59
 * never set bit 7). If the backup battery kept the DS1307 running across a
 * VCC loss the CH bit remains 0 and time is still valid.
 *
 * @return true if CH is set (oscillator halted, time may be invalid), false otherwise.
 */
bool rtc_oscillator_stopped(void);

/** @} */ /* end rtc_api */
