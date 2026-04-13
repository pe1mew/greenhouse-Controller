/**
 * @file modbus_rtu.h
 * @brief Modbus RTU master driver — types and API for LIB-6.
 *
 * Modbus RTU master over UART1 and the SIT65HVD08P RS485 transceiver.
 * Reads sensor data from the SenseCAP S200 (wind) and FG6485A
 * (temperature/humidity). Used by T5 (Sensor Poll).
 *
 * Depends on LIB-1 (gpio/) for RS485 direction control via
 * gpio_set_rs485_direction().
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pin_config.h"

/* ---------------------------------------------------------------------------
 * @defgroup modbus_pins UART / transceiver pin assignments
 * @{
 * --------------------------------------------------------------------------- */

/** @brief UART1 TX pin — connected to SIT65HVD08P DI. */
#define MODBUS_UART_TX      PIN_RS485_TX

/** @brief UART1 RX pin — connected to SIT65HVD08P RO. */
#define MODBUS_UART_RX      PIN_RS485_RX

/** @brief Baud rate for Modbus RTU (SenseCAP S200, FG6485A). */
#define MODBUS_BAUD         9600

/** @brief Maximum time to wait for a complete response frame (ms). */
#define MODBUS_TIMEOUT_MS   200

/** @} */ /* end modbus_pins */

/* ---------------------------------------------------------------------------
 * @defgroup modbus_status Status codes
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Return codes for all Modbus API functions.
 */
typedef enum {
    MODBUS_OK = 0,          /**< Transaction completed successfully. */
    MODBUS_ERR_TIMEOUT,     /**< No (complete) response within MODBUS_TIMEOUT_MS. */
    MODBUS_ERR_CRC,         /**< Response CRC does not match computed value. */
    MODBUS_ERR_EXCEPTION,   /**< Device returned a Modbus exception response. */
    MODBUS_ERR_FRAMING,     /**< Invalid response length or function code mismatch. */
    MODBUS_ERR_PARAM        /**< Caller supplied an invalid parameter. */
} modbus_status_t;

/** @} */ /* end modbus_status */

/* ---------------------------------------------------------------------------
 * @defgroup modbus_api Modbus RTU API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the Modbus RTU driver.
 *
 * Configures UART1 at @ref MODBUS_BAUD (8N1) on GPIO @ref MODBUS_UART_TX /
 * @ref MODBUS_UART_RX and sets the RS485 transceiver to receive mode.
 * Must be called once before any transaction function.
 */
void modbus_init(void);

/**
 * @brief Read holding registers (FC03).
 *
 * Sends a Modbus FC03 request to @p device_addr and returns @p count
 * register values starting at @p start_reg into @p out.
 *
 * @param device_addr Slave address (1–247; 0 = broadcast, rejected).
 * @param start_reg   First register address.
 * @param count       Number of registers to read (1–125).
 * @param out         Caller-supplied buffer of at least @p count uint16_t.
 * @return @ref MODBUS_OK on success, or a @ref modbus_status_t error code.
 */
modbus_status_t modbus_read_holding_registers(uint8_t  device_addr,
                                               uint16_t start_reg,
                                               uint8_t  count,
                                               uint16_t *out);

/**
 * @brief Read input registers (FC04).
 *
 * Sends a Modbus FC04 request to @p device_addr and returns @p count
 * register values starting at @p start_reg into @p out.
 *
 * @param device_addr Slave address (1–247; 0 = broadcast, rejected).
 * @param start_reg   First register address.
 * @param count       Number of registers to read (1–125).
 * @param out         Caller-supplied buffer of at least @p count uint16_t.
 * @return @ref MODBUS_OK on success, or a @ref modbus_status_t error code.
 */
modbus_status_t modbus_read_input_registers(uint8_t  device_addr,
                                             uint16_t start_reg,
                                             uint8_t  count,
                                             uint16_t *out);

/**
 * @brief Write multiple holding registers (FC16 / 0x10).
 *
 * Sends a Modbus FC16 request to @p device_addr, writing @p count register
 * values starting at @p start_reg from @p values.
 *
 * Request frame (header + data):
 *   [addr][0x10][reg_hi][reg_lo][cnt_hi][cnt_lo][byte_cnt][data...][crc_lo][crc_hi]
 * Response frame (8 bytes):
 *   [addr][0x10][reg_hi][reg_lo][cnt_hi][cnt_lo][crc_lo][crc_hi]
 *
 * @param device_addr Slave address (1–247; 0 = broadcast, rejected).
 * @param start_reg   First register address.
 * @param count       Number of registers to write (1–123).
 * @param values      Caller-supplied buffer of @p count uint16_t values.
 * @return @ref MODBUS_OK on success, or a @ref modbus_status_t error code.
 */
modbus_status_t modbus_write_multiple_registers(uint8_t         device_addr,
                                                 uint16_t        start_reg,
                                                 uint8_t         count,
                                                 const uint16_t *values);

/** @} */ /* end modbus_api */

/* ---------------------------------------------------------------------------
 * CRC helper — exposed only in unit-test builds for direct verification.
 * --------------------------------------------------------------------------- */
#ifdef NATIVE_TEST
/**
 * @brief Compute the Modbus CRC16 of a byte buffer.
 *
 * Polynomial 0xA001 (reflected 0x8005), initial value 0xFFFF.
 * Exposed for native unit testing (UT-MB-001, UT-MB-002) only.
 *
 * @param buf Pointer to data bytes.
 * @param len Number of bytes.
 * @return 16-bit CRC.
 */
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len);
#endif
