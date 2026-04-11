/**
 * @file mock_uart.h
 * @brief Arduino Serial1 / millis stubs for the native unit-test build of LIB-6.
 *
 * Provides:
 *  - MockSerial class instantiated as Serial1 — records transmitted bytes and
 *    serves pre-queued response bytes.
 *  - millis() — controllable time source for timeout tests.
 *  - A shared event log (mock_event_log) used by both this UART mock and
 *    mock_gpio to record the order of DE/RE toggles vs. UART reads/writes,
 *    enabling ordering assertions in UT-MB-009 and UT-MB-010.
 *
 * This header is included automatically by modbus_rtu.cpp when UNIT_TEST is
 * defined. Do NOT include it in production (target) builds.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Arduino constant stubs
 * --------------------------------------------------------------------------- */
#define SERIAL_8N1  0   /**< Stub for Arduino serial config constant. */

/* ---------------------------------------------------------------------------
 * @defgroup mock_event_log Shared event log
 *
 * Records the sequence of GPIO and UART events so that ordering assertions
 * (UT-MB-009, UT-MB-010) can verify DE/RE transitions happen before the
 * first TX write and before the first RX read respectively.
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Event types recorded in the shared event log.
 */
typedef enum {
    MOCK_EVT_GPIO_HIGH  = 0, /**< gpio_set_rs485_direction(true)  called. */
    MOCK_EVT_GPIO_LOW   = 1, /**< gpio_set_rs485_direction(false) called. */
    MOCK_EVT_UART_WRITE = 2, /**< Serial1.write() called. */
    MOCK_EVT_UART_READ  = 3, /**< Serial1.read()  called. */
} mock_event_t;

/** @brief Maximum entries in the event log. */
#define MOCK_LOG_MAX 64

/** @brief Chronological log of GPIO and UART events. */
extern mock_event_t mock_event_log[MOCK_LOG_MAX];

/** @brief Number of events currently in mock_event_log. */
extern int mock_event_count;

/**
 * @brief Append an event to mock_event_log (silently drops if full).
 *
 * Called internally by the UART mock and by mock_gpio.cpp.
 *
 * @param evt Event type to record.
 */
void mock_log_event(mock_event_t evt);

/** @} */ /* end mock_event_log */

/* ---------------------------------------------------------------------------
 * @defgroup mock_millis millis() stub
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Controllable replacement for Arduino millis().
 *
 * Returns the current mock time, then advances it by the configured step.
 * - step = 0  → time never advances (all RX bytes arrive "instantly")
 * - step = MODBUS_TIMEOUT_MS + 1 → triggers timeout after the first call
 *
 * @return Current mock time in milliseconds.
 */
uint32_t millis(void);

/**
 * @brief Configure the mock millis() behaviour.
 *
 * @param initial Starting value returned by the first millis() call.
 * @param step    Amount added to the internal counter after each call.
 */
void mock_set_millis(uint32_t initial, uint32_t step);

/** @} */ /* end mock_millis */

/* ---------------------------------------------------------------------------
 * @defgroup mock_serial MockSerial class (Serial1 stub)
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief In-process replacement for the Arduino HardwareSerial API.
 *
 * - begin()     — no-op (parameters are ignored).
 * - write()     — stores bytes in an internal TX buffer and logs MOCK_EVT_UART_WRITE.
 * - flush()     — no-op.
 * - available() — returns the number of pre-queued RX bytes remaining.
 * - read()      — returns the next pre-queued RX byte and logs MOCK_EVT_UART_READ;
 *                 returns -1 if the queue is empty.
 */
class MockSerial {
public:
    /** @brief No-op initialisation (parameters ignored). */
    void begin(uint32_t baud, uint32_t config = 0,
               int8_t rxPin = -1, int8_t txPin = -1);

    /**
     * @brief Record @p len bytes in the TX buffer and log MOCK_EVT_UART_WRITE.
     * @return Number of bytes written (always @p len).
     */
    size_t write(const uint8_t *buf, size_t len);

    /** @brief No-op flush (TX is assumed instantaneous). */
    void flush(void);

    /**
     * @brief Return the number of pre-queued RX bytes available.
     * @return Bytes remaining in the RX queue.
     */
    int available(void);

    /**
     * @brief Read and return the next byte from the RX queue.
     *
     * Logs MOCK_EVT_UART_READ.
     *
     * @return Next queued byte (0–255), or -1 if the queue is empty.
     */
    int read(void);

    /* ------------------------------------------------------------------
     * Test control helpers
     * ------------------------------------------------------------------ */

    /**
     * @brief Reset all internal state (TX buffer, RX queue).
     *
     * Call from mock_uart_reset() at the start of each test.
     */
    void reset(void);

    /**
     * @brief Pre-load @p len bytes into the RX queue for the driver to read.
     *
     * @param bytes Response bytes (as a Modbus slave would transmit them).
     * @param len   Number of bytes.
     */
    void queue_response(const uint8_t *bytes, uint8_t len);

    /**
     * @brief Copy up to @p max_len transmitted bytes into @p buf.
     *
     * @param buf     Destination buffer.
     * @param max_len Maximum bytes to copy.
     * @return Actual number of bytes copied.
     */
    int get_transmitted(uint8_t *buf, int max_len) const;

    /** @brief Return the total number of bytes transmitted so far. */
    int get_tx_count(void) const;

private:
    static const int BUF_SIZE = 256;

    uint8_t  rx_buf[BUF_SIZE]; /**< Pre-queued RX bytes. */
    int      rx_head;          /**< Read index. */
    int      rx_tail;          /**< Write index (one past last queued byte). */

    uint8_t  tx_buf[BUF_SIZE]; /**< Bytes transmitted by the driver. */
    int      tx_count;         /**< Number of bytes in tx_buf. */
};

/** @brief Global Serial1 mock instance (replaces Arduino's HardwareSerial). */
extern MockSerial Serial1;

/** @} */ /* end mock_serial */

/* ---------------------------------------------------------------------------
 * Convenience wrappers (match the API described in the LIB-6 spec)
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset all UART mock state, the event log, and the millis counter.
 *
 * Call this from setUp() at the start of each Unity test case.
 */
void mock_uart_reset(void);

/**
 * @brief Pre-load @p len response bytes for Serial1.read() to return.
 *
 * @param bytes Modbus response bytes.
 * @param len   Number of bytes.
 */
void mock_uart_queue_response(const uint8_t *bytes, uint8_t len);

/**
 * @brief Copy the bytes that the driver wrote via Serial1.write() into @p buf.
 *
 * @param buf     Destination buffer (caller must provide at least @p max_len bytes).
 * @param max_len Maximum bytes to copy.
 * @return Number of bytes actually copied.
 */
int mock_uart_get_transmitted(uint8_t *buf, int max_len);

/* ---------------------------------------------------------------------------
 * delay() stub (unused by the driver but required to link in some toolchains)
 * --------------------------------------------------------------------------- */
/** @brief No-op delay stub for the native build. */
void delay(uint32_t ms);
