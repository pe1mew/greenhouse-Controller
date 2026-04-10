/**
 * @file mock_wire.h
 * @brief Arduino Wire stubs for the native (host) unit-test build of LIB-2.
 *
 * Replaces the Arduino Wire library with an in-memory FIFO and TX log so that
 * @ref i2c_bus.cpp can be compiled and tested without target hardware.
 * This header is included automatically by @ref i2c_bus.cpp when
 * @c UNIT_TEST is defined.
 *
 * ### Design
 * - **TX log** — bytes passed to Wire.write() are appended to @ref mock_tx_buf.
 *   @ref mock_last_addr records the last address used in beginTransmission or
 *   requestFrom.
 * - **RX FIFO** — call mock_wire_preload_rx() to stage response bytes before
 *   the production code calls Wire.requestFrom() and Wire.read().
 * - **NACK injection** — set @ref mock_nack_next to true; the next
 *   Wire.endTransmission() call returns 2 (NACK on address) and clears the flag.
 * - **Scan ACK list** — populate @ref mock_ack_addrs / @ref mock_ack_count to
 *   make only specific addresses ACK during i2c_scan(). When the list is empty
 *   (default after reset) every address ACKs.
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
#define MOCK_WIRE_TX_BUF   64  /**< Capacity of the TX log buffer. */
#define MOCK_WIRE_RX_BUF   64  /**< Capacity of the RX FIFO. */
#define MOCK_WIRE_ACK_MAX   8  /**< Max entries in the scan ACK address list. */

/* ---------------------------------------------------------------------------
 * Arduino GPIO level constants (needed by tests that also use pin_config.h)
 * --------------------------------------------------------------------------- */
#define INPUT         0x0
#define OUTPUT        0x1
#define INPUT_PULLUP  0x2
#define HIGH          0x1
#define LOW           0x0

/* ---------------------------------------------------------------------------
 * Observable state (read by test assertions)
 * --------------------------------------------------------------------------- */

/** @brief Bytes sent by the most recent write sequence (in order). */
extern uint8_t mock_tx_buf[MOCK_WIRE_TX_BUF];

/** @brief Number of bytes currently in @ref mock_tx_buf. */
extern uint8_t mock_tx_len;

/** @brief Last 7-bit address used in beginTransmission() or requestFrom(). */
extern uint8_t mock_last_addr;

/* ---------------------------------------------------------------------------
 * Test-control knobs (write before calling production code)
 * --------------------------------------------------------------------------- */

/** @brief When true the next endTransmission() returns 2 (NACK) and resets. */
extern bool mock_nack_next;

/** @brief Bytes staged as the RX response (populated by mock_wire_preload_rx). */
extern uint8_t mock_rx_buf[MOCK_WIRE_RX_BUF];

/** @brief Number of staged RX bytes. */
extern uint8_t mock_rx_len;

/**
 * @brief Addresses that ACK during i2c_scan().
 *
 * When @ref mock_ack_count > 0 only listed addresses return ACK from
 * endTransmission(); all others return NACK (2). When @ref mock_ack_count == 0
 * every address ACKs (default after reset).
 */
extern uint8_t mock_ack_addrs[MOCK_WIRE_ACK_MAX];

/** @brief Number of entries in @ref mock_ack_addrs. */
extern uint8_t mock_ack_count;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */

/** @brief Reset all mock state to power-on defaults. Call in setUp(). */
void mock_wire_reset(void);

/**
 * @brief Stage bytes to be returned by the next Wire.read() sequence.
 *
 * @param data  Bytes to stage (copied into @ref mock_rx_buf).
 * @param len   Number of bytes (capped at @ref MOCK_WIRE_RX_BUF).
 */
void mock_wire_preload_rx(const uint8_t *data, uint8_t len);

/* ---------------------------------------------------------------------------
 * Fake TwoWire class
 * --------------------------------------------------------------------------- */

/**
 * @brief Minimal stand-in for the Arduino TwoWire class.
 *
 * Only the methods used by i2c_bus.cpp are implemented.
 */
class TwoWire {
public:
    bool    begin(int sda, int scl, uint32_t freq = 100000);
    void    beginTransmission(uint8_t addr);
    size_t  write(const uint8_t *data, size_t len);
    size_t  write(uint8_t byte);
    uint8_t endTransmission(bool stop = true);
    uint8_t requestFrom(uint8_t addr, uint8_t count, bool stop = true);
    int     available(void);
    int     read(void);
};

/** @brief Singleton Wire object, matches the Arduino global. */
extern TwoWire Wire;
