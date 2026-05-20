/**
 * @file ds1307_rtc.h
 * @brief DS1307 real-time clock driver — types and API for LIB-3.
 *
 * Thin driver over the LIB-2 I2C bus for the DS1307 battery-backed RTC.
 * All time fields are stored and retrieved in human-readable decimal; BCD
 * encoding/decoding is handled internally by the driver.
 *
 * The DS1307 keeps wall-clock time across VCC loss using a CR2032 backup
 * battery wired to its VBAT pin.  When the backup battery is also lost, the
 * oscillator stops and the Clock Halt (CH) bit in register 0x00 latches at 1;
 * callers can detect this via @ref rtc_oscillator_stopped() and prompt the
 * user to re-set the time.
 *
 * ## Hardware
 *   - Chip       : Maxim/Analog Devices DS1307 (8-pin DIP / SOIC)
 *   - Bus        : I2C, Standard-mode (100 kHz) — also tolerates 400 kHz
 *                  Fast-mode used by this project (see @ref I2C_FREQ_HZ).
 *   - Address    : 0x68 (7-bit, factory fixed — see @ref DS1307_I2C_ADDR).
 *   - Registers  : 0x00..0x06 timekeeping (BCD); 0x07 control; 0x08..0x3F
 *                  56 bytes of battery-backed general-purpose SRAM (unused
 *                  by this driver).
 *   - Backup     : VBAT pin needs a 3 V CR2032 to retain time without VCC.
 *
 * ## API summary
 *   - @ref rtc_init                Probe device on the bus.
 *   - @ref rtc_get_time            Read wall-clock time.
 *   - @ref rtc_set_time            Write wall-clock time (also clears CH bit).
 *   - @ref rtc_oscillator_stopped  Detect time-loss after battery failure.
 *
 * ## Thread safety
 *   No internal mutex.  The driver acquires @ref i2c_lock() inside each call
 *   so a single transaction is atomic with respect to other I2C peripherals,
 *   but the caller is responsible for serialising calls to this driver itself
 *   if multiple tasks may touch the RTC.
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
 * Sends a zero-byte write to @ref DS1307_I2C_ADDR.  Returns
 * @ref RTC_ERR_NO_DEVICE if the device does not ACK.
 *
 * @return @ref RTC_OK if the device acknowledges, @ref RTC_ERR_NO_DEVICE
 *         if no ACK was received, @ref RTC_ERR_COMM on bus error.
 * @warning @c i2c_init() (LIB-2) MUST have been called first; this driver
 *          does not initialise the bus.
 * @see rtc_oscillator_stopped() — check after init whether the time is valid.
 * @see DS1307 datasheet §8 (Slave Address Byte).
 */
rtc_status_t rtc_init(void);

/**
 * @brief Read the current date and time from the DS1307.
 *
 * Reads registers 0x00–0x06 with a single repeated-start transaction,
 * BCD-decodes all fields, and validates each field's range.
 *
 * @param dt  Pointer to caller-allocated @ref rtc_datetime_t to fill (must
 *            not be NULL).
 * @return @ref RTC_OK on success, @ref RTC_ERR_COMM on bus error,
 *         @ref RTC_ERR_INVALID if any register holds an out-of-range value
 *         (e.g. corrupted RAM after total power loss).
 * @note   Year is reported as a full four-digit value (2000 added to the
 *         DS1307's 2-digit year register).
 * @warning Always check @ref rtc_oscillator_stopped() after power-up — if
 *          the CH bit is set, the data returned by this call is stale.
 * @see    rtc_set_time().
 */
rtc_status_t rtc_get_time(rtc_datetime_t *dt);

/**
 * @brief Write a new date and time to the DS1307.
 *
 * BCD-encodes all fields and writes registers 0x00–0x06 in a single
 * transaction.  Writing the seconds register implicitly clears the Clock
 * Halt (CH) bit, restarting the oscillator if it was stopped.
 *
 * @param dt  Pointer to @ref rtc_datetime_t containing the time to set
 *            (must not be NULL).
 * @return @ref RTC_OK on success, @ref RTC_ERR_COMM on bus error.
 * @warning No input validation is performed — the caller must supply a
 *          consistent datetime (e.g. day ≤ 31, hour ≤ 23).  Invalid BCD
 *          patterns will be accepted and read back as @ref RTC_ERR_INVALID.
 * @see    rtc_get_time(), rtc_oscillator_stopped().
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
 * @return @c true if CH is set (oscillator halted, time may be invalid),
 *         @c false otherwise.
 * @note   Also returns @c false (oscillator nominally running) on any I2C
 *         error; combine with @ref rtc_init() for full diagnostics.
 * @see    DS1307 datasheet §9.1 (Clock Halt Bit).
 */
bool rtc_oscillator_stopped(void);

/** @} */ /* end rtc_api */
