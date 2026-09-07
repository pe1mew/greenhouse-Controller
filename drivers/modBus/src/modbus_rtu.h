/**
 * @file modbus_rtu.h
 * @brief Modbus RTU master driver — types and API for LIB-6.
 *
 * Modbus RTU master over UART1 and the SIT65HVD08P RS-485 transceiver.
 * Reads sensor data from the SenseCAP S200 (LIB-S200, wind) and FG6485A
 * (LIB-FG, temperature/humidity).  Consumed by T5 (Sensor Poll).
 *
 * ## Hardware
 *   - Transceiver : Sipex SIT65HVD08P (3.3 V, half-duplex RS-485).
 *   - UART        : ESP32-S3 UART1 — TX on @c PIN_RS485_TX, RX on
 *                   @c PIN_RS485_RX, line direction on @c PIN_RS485_DE_RE.
 *   - Wiring      : Differential A/B pair, 120 Ω bus terminator at the
 *                   far end of the daisy chain (sensor end).  Bias network
 *                   is on the controller PCB.
 *   - Frame       : 9600 baud, 8N1 (see @ref MODBUS_BAUD).
 *
 * ## Half-duplex DE/RE timing
 *   Each transaction:
 *     1. Assert DE HIGH (TX) via LIB-1 @ref gpio_set_rs485_direction.
 *     2. Write the request bytes; wait for TX FIFO + shift register drain
 *        before dropping DE — premature drop truncates the final byte.
 *     3. Drop DE LOW (RX) and wait for the response within
 *        @ref MODBUS_TIMEOUT_MS (200 ms is comfortable for FG6485A and
 *        SenseCAP S200 at 9600 baud).
 *     4. Validate length, function code, and CRC.
 *
 * ## API summary
 *   - @ref modbus_init                       UART + DE/RE setup.
 *   - @ref modbus_read_holding_registers     FC03 read.
 *   - @ref modbus_read_input_registers       FC04 read.
 *   - @ref modbus_write_multiple_registers   FC16 write.
 *
 * ## Thread safety — locked, but still one caller by policy (gh#49)
 *   Since 2026-09-07 the driver **does** serialise whole transactions with a
 *   FreeRTOS mutex created in @ref modbus_init.  Concurrent calls are now
 *   safe for **correctness**: the loser waits up to
 *   @ref MODBUS_LOCK_TIMEOUT_MS and then gets @ref MODBUS_ERR_BUSY with
 *   nothing put on the wire.
 *
 *   **That is not permission to call it from anywhere.**  A transaction can
 *   hold the bus for ~215 ms (@ref MODBUS_TIMEOUT_MS dominates), and blocking
 *   that long inside a High-priority, WDT-subscribed task — T2
 *   (relay_controller) or T3 (safety_monitor) — could delay a wind-override
 *   response.  **All bus I/O stays in T5 (sensor_poll)**, or in the dedicated
 *   bus task of design/refactorSensorConfiguration.md §2.2.  The lock is a
 *   safety net beneath that rule, not a replacement for it.
 *
 *   (Historical: before 2026-09-07 this block claimed a UART mutex that had
 *   never been implemented.  gh#49 removed the false claim, then made it
 *   true.  Plan and rationale: design/addModbusMutex.md.)
 *
 *   What the lock protects — three shared resources, not one:
 *     1. the DE/RE direction GPIO — asserting DE mid-response garbles the
 *        wire AND blinds the in-flight caller;
 *     2. `s_frame_end_us`, the inter-frame-gap timestamp, which is
 *        read-modify-written per transaction to enforce RTU t3.5 silence;
 *     3. the single UART RX FIFO — two readers steal each other's response
 *        bytes, and the receive path drains *exactly* 8 half-duplex echo
 *        bytes on the assumption that the FIFO holds only its own echo.
 *   The visible symptom of (3) is a CRC or framing error blamed on the
 *   sensor, which is why this is worth stating rather than assuming.
 *
 *   Per-call output buffers are owned by the calling task only.
 *
 * Depends on LIB-1 (gpio/) for RS-485 direction control via
 * @ref gpio_set_rs485_direction.
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

/**
 * @brief Maximum time to wait for the bus lock before giving up (ms).
 *
 * A transaction holds the lock for at most ~215 ms (inter-frame gap 4 ms +
 * 8-byte TX ~8.3 ms + 2 ms DE guard + 1.5 ms settle + @ref MODBUS_TIMEOUT_MS).
 * 500 ms is roughly twice that, so a caller that gives up has hit genuine
 * contention rather than one slow-but-legitimate transaction ahead of it.
 * Exceeding it yields @ref MODBUS_ERR_BUSY and puts nothing on the wire.
 */
#define MODBUS_LOCK_TIMEOUT_MS  500u

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
    MODBUS_ERR_PARAM,       /**< Caller supplied an invalid parameter. */
    /**
     * @brief Another task held the bus for longer than
     *        @ref MODBUS_LOCK_TIMEOUT_MS; nothing was put on the wire.
     *
     * Deliberately distinct from @ref MODBUS_ERR_TIMEOUT (gh#49). Folding the
     * two together would make bus contention indistinguishable from "the slave
     * did not answer" — and T5 raises a **sensor fault** after two consecutive
     * read failures, so contention would send an operator hunting a sensor that
     * is perfectly healthy. Treat BUSY as a scheduling problem, TIMEOUT as a
     * device problem.
     *
     * Should not occur while the single-caller policy below holds.
     */
    MODBUS_ERR_BUSY
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
 * @ref MODBUS_UART_RX, creates the bus mutex, and sets the RS-485 transceiver
 * to receive mode via @ref gpio_set_rs485_direction(@c false).
 *
 * The mutex is created **only if it does not already exist** — see the @note
 * below on the deliberate double call.  If creation fails (heap exhaustion at
 * boot) the driver falls back to running unlocked, which is exactly the
 * pre-gh#49 behaviour and is safe under the single-caller policy.
 *
 * @warning Must be called before any transaction function.  The internal
 *          DE/RE init also runs here; do NOT call @ref gpio_rs485_init
 *          separately.
 * @note    Deliberately called **twice** in this firmware — once at boot
 *          (`main.cpp`) and again by T5 at task entry, which reconfirms the
 *          driver state.  It is idempotent for the UART: it deletes and
 *          reinstalls the driver.  That delete is why a second concurrent
 *          caller would be unsafe even beyond the locking question — a
 *          transaction in flight during a re-init is a use-after-delete.
 * @see    modbus_read_holding_registers(), modbus_write_multiple_registers().
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
