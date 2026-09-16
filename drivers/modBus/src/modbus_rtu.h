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
 * ## Half-duplex DE/RE timing (gh#70)
 *   The UART drives DE/RE: UART1 runs in UART_MODE_RS485_HALF_DUPLEX with its
 *   RTS output on @c PIN_RS485_DE_RE, and ESP-IDF raises it when a request
 *   enters the TX FIFO and drops it in the TX-done interrupt, right after the
 *   last stop bit. With CONFIG_UART_ISR_IN_IRAM that interrupt also runs while
 *   the flash is written. Each transaction:
 *     1. Write the request bytes (DE rises); wait for TX done (DE has fallen).
 *     2. Wait for the response within @ref MODBUS_TIMEOUT_MS (200 ms is
 *        comfortable for FG6485A and SenseCAP S200 at 9600 baud). The deadline
 *        is judged only when no byte is waiting, so a stalled task still
 *        collects a reply the UART buffered meanwhile.
 *     3. Validate length, function code, and CRC.
 *   Until 2026-09-16 the task dropped DE itself, 2 ms after TX; a flash write
 *   stalled it up to 665 ms and the wire encoder's ~4.5 ms reply was lost.
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
 *     1. the DE/RE direction — the UART's RTS since gh#70; a second
 *        transmission mid-response would raise it, garbling the wire AND
 *        blinding the in-flight caller;
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
 *          reinstalls the driver, and a transaction in flight during that is
 *          a use-after-delete.  **A re-init therefore takes the bus mutex**
 *          (waiting up to 2 s) and, if it cannot, leaves the installed driver
 *          alone and logs an error.  Until 2026-09-16 it took nothing, which
 *          was safe only while T5 was the sole caller: with T17 polling on
 *          `ropeSensor`, T5's entry re-init deleted the driver under a T17
 *          read and the board panicked (LoadProhibited in
 *          `uart_get_buffered_data_len`), twice in a row.
 * @see    modbus_read_holding_registers(), modbus_write_multiple_registers().
 */
void modbus_init(void);

/**
 * @brief Does this build hold the bus lock across a re-init? (gh#69)
 *
 * Always true on target, except in a deliberately built fail-first variant
 * (`MODBUS_FAILFIRST_UNLOCKED_REINIT`, see modbus_rtu.cpp) whose only purpose
 * is to show that `bin/at_modbus_reinit.py` catches the unlocked re-init. The
 * test reports this value so a pass can never be read off the wrong build.
 * False in a host (NATIVE_TEST) build, which has no lock at all.
 */
bool modbus_reinit_is_locked(void);

/**
 * @brief Who drives the RS-485 DE/RE line in this build (gh#70).
 *
 * @return "uart+iram" -- the UART, from an interrupt that runs during flash
 *         writes (the fix); "uart" -- the UART, but its interrupt is not in
 *         IRAM, so a flash write still holds the release back; "task" -- this
 *         driver, from task context (host tests, and the fail-first build
 *         `MODBUS_FAILFIRST_TASK_DE`). Reported so an OTA test result can
 *         never be read off the wrong build.
 */
const char *modbus_de_control(void);

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
/**
 * @brief Per-status transaction tallies, for diagnosing a shared bus.
 *
 * Added because a wind alarm was chased through three wrong root causes
 * without anyone being able to say which error the failing read actually
 * returned. On a single-caller bus the distinction rarely matters; with two
 * callers it is the whole question.
 *
 * Counts are monotonic since boot and are not locked: they are written inside
 * the bus mutex and read for diagnostics only, so a torn 32-bit read is not
 * worth a critical section.
 */
/** @brief How many distinct slave addresses get their own counter row. */
#define MODBUS_MAX_TRACKED_SLAVES 6u

/**
 * @brief Per-slave transaction tallies.
 *
 * Bus-wide totals cannot answer the question that matters on a shared bus:
 * *which device* is failing. Keying by address here, inside the driver, gives
 * every caller its own row with no per-task bookkeeping -- one table each task
 * reads its own row from, rather than a struct each task reimplements.
 *
 * The need is not theoretical. AT-WP05 arm A runs with the encoder deliberately
 * unplugged, so the presence gate's 30 s re-probes against addr 40 pile up
 * timeouts that are indistinguishable, in the totals, from the T5 read failures
 * the test is actually measuring.
 */
typedef struct {
    uint8_t  addr;       /**< Slave address; **0 means the slot is unused**. */
    uint32_t ok;
    uint32_t timeout;
    uint32_t crc;
    uint32_t exception;
    uint32_t framing;
    uint32_t param;
    uint32_t busy;

    /** Current run of consecutive failures, and the high-water mark since
     *  boot. **This is the number that predicts a fault**, because T5 faults on
     *  consecutive failures rather than on a rate -- a slave can sit at 0.1 %
     *  forever without ever faulting, or fault at the same rate if the failures
     *  arrive together.
     *
     *  @c MODBUS_ERR_BUSY does **not** break or extend the run: losing the bus
     *  lock says the bus was busy, not that this slave failed. Counting it would
     *  let ordinary contention predict a fault, which is the same mistake that
     *  collapsing BUSY into COMM made in the sensor drivers. */
    uint16_t consec_fail;
    uint16_t consec_fail_max;
} modbus_slave_counters_t;

typedef struct {
    uint32_t ok;         /**< Completed transactions. */
    uint32_t timeout;    /**< @ref MODBUS_ERR_TIMEOUT. */
    uint32_t crc;        /**< @ref MODBUS_ERR_CRC. */
    uint32_t exception;  /**< @ref MODBUS_ERR_EXCEPTION. */
    uint32_t framing;    /**< @ref MODBUS_ERR_FRAMING. */
    uint32_t param;      /**< @ref MODBUS_ERR_PARAM. */
    uint32_t busy;       /**< @ref MODBUS_ERR_BUSY -- lock not acquired. */
    uint8_t  last_status;/**< Status of the most recent transaction. */
    uint8_t  last_addr;  /**< Slave address it was addressed to. */
    uint8_t  last_fail_status; /**< Status of the most recent FAILING one. */
    uint8_t  last_fail_addr;   /**< Slave address of that failure. */

    /* Detail of the most recent TIMEOUT. The distinction these two carry is
     * the whole diagnosis: 0 bytes means the slave never answered (it never
     * accepted our request as a frame, or dropped it on CRC), while a
     * partial count means the response started and was cut short. */
    uint8_t  last_to_received; /**< Bytes in hand when the deadline passed. */
    uint8_t  last_to_expected; /**< Bytes the frame should have had. */
    uint32_t last_lock_wait_ms;/**< How long the last caller waited for the
                                *   bus lock. busy==0 says a caller always
                                *   got it, not that it got it promptly. */

    /* Re-inits (gh#69). The first modbus_init() is not counted: only a call
     * that deletes and reinstalls a live driver is. */
    uint32_t reinit;           /**< Re-inits that reinstalled the driver. */
    uint32_t reinit_skipped;   /**< Re-inits refused: the bus lock was not
                                *   free within 2 s, so the installed driver
                                *   was left alone. Anything but 0 means a
                                *   caller held the bus far too long. */

    /* When the driver started listening for the reply, measured from the
     * request's last bit ON THE WIRE (transmit start plus one character time
     * per byte) -- gh#70. See modbus_de_control() for what it means:
     *   "task": this IS the DE/RE release; later than the slave's answer (the
     *           wire encoder: ~4.5 ms) and the reply is lost. Nominal ~2 ms.
     *   "uart"/"uart+iram": the UART released DE/RE in its TX-done interrupt;
     *           this is only when the task resumed. Late costs nothing -- the
     *           reply is buffered -- but a stall stays visible. Nominal ~0.1 ms. */
    uint32_t listen_late;             /**< Listening started > 4 ms after the last bit. */
    uint32_t listen_late_failed;      /**< ...after which the transaction timed out
                                       *   or failed CRC/framing. */
    uint32_t listen_lat_max_us;       /**< Worst listen latency seen, microseconds. */
    uint32_t last_fail_listen_lat_us; /**< Listen latency of the most recent
                                       *   transaction that timed out or failed
                                       *   CRC/framing. */

    /** Per-slave breakdown. Rows with @c addr == 0 are unused. A slave beyond
     *  @ref MODBUS_MAX_TRACKED_SLAVES still counts in the totals above, just
     *  without a row of its own. */
    modbus_slave_counters_t slave[MODBUS_MAX_TRACKED_SLAVES];
} modbus_counters_t;

/**
 * @brief Copy the transaction tallies out.
 * @param out Destination; ignored if NULL.
 */
void modbus_get_counters(modbus_counters_t *out);

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
