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

/* ---------------------------------------------------------------------------
 * 4×4 membrane keypad (LIB-5)
 *
 * Row pins are driven LOW one at a time during scanning (OUTPUT, idle HIGH).
 * Column pins are read with internal pull-ups (INPUT_PULLUP); LOW = pressed.
 *
 * Key layout:
 *   Col:     1    2    3    4
 *   Row 1:  '1'  '2'  '3'  'A'
 *   Row 2:  '4'  '5'  '6'  'B'
 *   Row 3:  '7'  '8'  '9'  'C'
 *   Row 4:  '*'  '0'  '#'  'D'
 * --------------------------------------------------------------------------- */

/** @brief Keypad row 1 — OUTPUT, driven LOW to scan row 1. */
#define KP_ROW1              3

/** @brief Keypad row 2 — OUTPUT, driven LOW to scan row 2. */
#define KP_ROW2              4

/** @brief Keypad row 3 — OUTPUT, driven LOW to scan row 3. */
#define KP_ROW3              5

/** @brief Keypad row 4 — OUTPUT, driven LOW to scan row 4. */
#define KP_ROW4              6

/** @brief Keypad column 1 — INPUT_PULLUP; LOW when key in column 1 is pressed. */
#define KP_COL1              7

/** @brief Keypad column 2 — INPUT_PULLUP; LOW when key in column 2 is pressed. */
#define KP_COL2              9

/** @brief Keypad column 3 — INPUT_PULLUP; LOW when key in column 3 is pressed. */
#define KP_COL3             10

/** @brief Keypad column 4 — INPUT_PULLUP; LOW when key in column 4 is pressed. */
#define KP_COL4             11
