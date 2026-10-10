/**
 * @file mock_i2c_master.h
 * @brief Simulated ESP-IDF I2C master and bus devices for the native (host) unit-test build of LIB-2.
 *
 * The native build compiles the ESP-IDF branch of i2c_bus.cpp unchanged
 * (its UNIT_TEST branch is do-nothing stubs; test/include_lib.cpp explains).
 * Its driver/i2c_master.h calls land in mock_i2c_master.cpp, which answers
 * the way ESP-IDF 5.5.0 does. Each result below was read from
 * components/esp_driver_i2c/i2c_master.c and i2c_common.c in that release:
 *
 * | Call                                    | Result                                  |
 * |-----------------------------------------|-----------------------------------------|
 * | i2c_new_master_bus, port already taken  | ESP_ERR_INVALID_STATE                   |
 * | i2c_master_probe, device ACKs           | ESP_OK                                  |
 * | i2c_master_probe, address NACKed        | ESP_ERR_NOT_FOUND                       |
 * | i2c_master_probe, SCL held low          | ESP_ERR_TIMEOUT                         |
 * | transmit/receive, buffer NULL or len 0  | ESP_ERR_INVALID_ARG, nothing on the bus |
 * | transmit/receive, address NACKed        | ESP_ERR_INVALID_STATE — not ESP_FAIL    |
 * | transmit/receive, SCL held low          | ESP_ERR_INVALID_STATE                   |
 * | any call on a removed device handle     | ESP_ERR_INVALID_ARG (counted)           |
 *
 * Why INVALID_STATE for both a NACK and a stall: the synchronous transfer
 * path ends in s_i2c_transaction_start(), which returns
 * ESP_ERR_INVALID_STATE whenever the bus status is not DONE, and a NACK
 * (I2C_STATUS_ACK_ERROR) and a timeout (I2C_STATUS_TIMEOUT) are both "not
 * DONE". ESP_ERR_TIMEOUT from a transfer only means the bus lock could not
 * be taken, which a single-threaded test never provokes. The header's own
 * @return lists do not mention INVALID_STATE; the source is what counts.
 *
 * **Devices are register-addressed**, like the DS1307 and the PCA9633. The
 * first byte of a write sets the register pointer; further bytes are stored
 * from it; a read returns bytes from the pointer on, advancing it. A read
 * therefore returns the register the preceding write asked for, and nothing
 * else: the I2C form of "the reply comes after the request".
 *
 * Every transaction that reaches the bus is logged (mock_i2c_xfer()), and
 * device handles are tracked, so a test can check that i2c_bus releases the
 * transient handle it creates for each call.
 *
 * @note Do **not** include this header in production (target) builds.
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "driver/i2c_master.h"   /* test/mock_idf/driver/i2c_master.h */

/* ---------------------------------------------------------------------------
 * Capacities
 * --------------------------------------------------------------------------- */
#define MOCK_I2C_XFER_MAX_BYTES  32   /**< Write bytes kept per logged transaction. */
#define MOCK_I2C_LOG_MAX        160   /**< Logged transactions (a full scan is 126). */
#define MOCK_I2C_HANDLE_MAX       4   /**< Device handles alive at once. */

/* ---------------------------------------------------------------------------
 * Transaction log
 * --------------------------------------------------------------------------- */

/** @brief Shape of a bus transaction. */
typedef enum {
    MOCK_I2C_PROBE = 0,  /**< START, address+W, STOP (i2c_master_probe). */
    MOCK_I2C_WRITE,      /**< START, address+W, data, STOP. */
    MOCK_I2C_READ,       /**< START, address+R, data, STOP. */
    MOCK_I2C_WRITE_READ  /**< START, address+W, data, repeated START, address+R, data, STOP. */
} mock_i2c_kind_t;

/** @brief One transaction as it appeared on the bus. */
typedef struct {
    mock_i2c_kind_t kind;
    uint16_t        addr;          /**< 7-bit address. */
    uint8_t         tx[MOCK_I2C_XFER_MAX_BYTES];
    size_t          tx_len;        /**< Bytes the master wrote (true count, even if > the copy). */
    size_t          rx_len;        /**< Bytes the master asked to read. */
    uint32_t        scl_speed_hz;  /**< The device handle's speed; 100 kHz for a probe, as ESP-IDF sets. */
    int             timeout_ms;    /**< As passed by the caller; -1 would mean "wait forever". */
    esp_err_t       result;        /**< What the ESP-IDF call returned. */
} mock_i2c_xfer_t;

/** @brief Transactions logged since the last mock_i2c_reset(). */
int mock_i2c_xfer_count(void);

/** @brief Logged transaction @p index (0 = first), or NULL when out of range. */
const mock_i2c_xfer_t *mock_i2c_xfer(int index);

/* ---------------------------------------------------------------------------
 * Bus
 * --------------------------------------------------------------------------- */

/**
 * @brief Detach all devices, clear the log, the handles and the counters.
 *
 * The bus itself is NOT torn down: i2c_bus.cpp keeps its bus handle in a
 * static for the life of the process, as the real port stays acquired.
 * Call from setUp().
 */
void mock_i2c_reset(void);

/** @brief The config i2c_new_master_bus() accepted, or NULL before that. */
const i2c_master_bus_config_t *mock_i2c_bus_config(void);

/** @brief i2c_new_master_bus() calls in this process. Never reset. */
int mock_i2c_bus_create_calls(void);

/* ---------------------------------------------------------------------------
 * Devices
 * --------------------------------------------------------------------------- */

/** @brief Put a device on the bus at @p addr: it ACKs; registers 0, pointer 0. */
void mock_i2c_attach(uint8_t addr);

/** @brief Preset @p count registers of the device at @p addr, from @p first_reg. */
void mock_i2c_set_registers(uint8_t addr, uint8_t first_reg,
                            const uint8_t *values, size_t count);

/** @brief Move the device's register pointer, as an earlier transfer would have. */
void mock_i2c_set_pointer(uint8_t addr, uint8_t reg);

/** @brief Current content of one register. */
uint8_t mock_i2c_register(uint8_t addr, uint8_t reg);

/** @brief Make the device stretch SCL past any timeout (true) or release it. */
void mock_i2c_hold_scl(uint8_t addr, bool hold);

/* ---------------------------------------------------------------------------
 * Handle bookkeeping (cleared by mock_i2c_reset())
 * --------------------------------------------------------------------------- */

/** @brief Device handles added and not yet removed. */
extern int mock_i2c_live_handles;

/** @brief i2c_master_bus_add_device() successes. */
extern int mock_i2c_handles_added;

/** @brief The config of the most recent successful i2c_master_bus_add_device(). */
extern i2c_device_config_t mock_i2c_last_device_config;

/** @brief Calls made with a handle that was never added or already removed. */
extern int mock_i2c_stale_handle_calls;

/** @brief Calls refused with ESP_ERR_INVALID_ARG for a NULL/empty buffer or bad config. */
extern int mock_i2c_invalid_arg_calls;
