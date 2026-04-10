/**
 * @file pin_config.h
 * @brief GPIO pin assignments — single authoritative source for all project GPIOs.
 *
 * Include this header wherever a project GPIO number is needed.
 * Do NOT hard-code GPIO numbers anywhere else in firmware or drivers.
 *
 * @par Reserved / inaccessible ESP32-S3 pins (must not be used)
 *   0, 19, 20, 22–25 (not on LOLIN S3 header), 26–37, 43, 44, 45, 46
 *
 * @author Greenhouse Controller project
 */

#pragma once

/* ---------------------------------------------------------------------------
 * Relay outputs (active-low relay driver inputs)
 * --------------------------------------------------------------------------- */

/** @brief Relay output — motor 1 OPEN direction. */
#define PIN_RELAY_M1_OPEN   12

/** @brief Relay output — motor 1 CLOSE direction. */
#define PIN_RELAY_M1_CLOSE  13

/** @brief Relay output — motor 2 OPEN direction. */
#define PIN_RELAY_M2_OPEN   14

/** @brief Relay output — motor 2 CLOSE direction. */
#define PIN_RELAY_M2_CLOSE  15

/** @brief Relay output — motor 3 OPEN direction. */
#define PIN_RELAY_M3_OPEN   16

/** @brief Relay output — motor 3 CLOSE direction. */
#define PIN_RELAY_M3_CLOSE  21

/* ---------------------------------------------------------------------------
 * Feedback input
 * --------------------------------------------------------------------------- */

/** @brief Digital input — opto-coupler feedback from RRK-3 motor controller. */
#define PIN_OPTO_INPUT      42

/* ---------------------------------------------------------------------------
 * Indicators
 * --------------------------------------------------------------------------- */

/** @brief Digital output — heartbeat / status LED (amber). */
#define PIN_HB_LED          41

/* ---------------------------------------------------------------------------
 * RS-485 direction control
 * --------------------------------------------------------------------------- */

/** @brief Digital output — RS-485 transceiver DE/RE direction control. */
#define PIN_RS485_DE_RE      8

/* ---------------------------------------------------------------------------
 * I2C bus (shared — LCD at 0x27, RTC at 0x68)
 * --------------------------------------------------------------------------- */

/** @brief I2C data line (SDA). */
#define PIN_I2C_SDA          1

/** @brief I2C clock line (SCL). */
#define PIN_I2C_SCL          2
