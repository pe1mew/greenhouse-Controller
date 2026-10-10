/**
 * LIB-2 I2C Bus — unit tests (native build)
 *
 * Test IDs: UT-I2C-001 … UT-I2C-011
 *
 * i2c_bus.cpp's ESP-IDF code is compiled unchanged (test/include_lib.cpp)
 * against a simulated ESP-IDF I2C master (test/mock_i2c_master.h), which
 * returns what ESP-IDF 5.5.0 returns and models register-addressed devices.
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include "../src/i2c_bus.h"
#include "mock_i2c_master.h"

void setUp(void)
{
    mock_i2c_reset();
    (void)i2c_init();   /* idempotent; the bus outlives the reset */
}

void tearDown(void) {}

/* The only transaction the call under test put on the bus. */
static const mock_i2c_xfer_t *only_xfer(mock_i2c_kind_t kind, uint16_t addr)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_i2c_xfer_count(),
                                  "expected exactly one bus transaction");
    const mock_i2c_xfer_t *x = mock_i2c_xfer(0);
    TEST_ASSERT_EQUAL_INT(kind, x->kind);
    TEST_ASSERT_EQUAL_HEX16(addr, x->addr);
    /* A finite timeout: -1 would let a stuck bus block the calling task forever. */
    TEST_ASSERT_TRUE_MESSAGE(x->timeout_ms > 0, "transfer timeout is not bounded");
    return x;
}

/* i2c_bus creates a device handle per call; it must be gone afterwards. */
static void assert_handles_released(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_i2c_live_handles, "device handle leaked");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_i2c_stale_handle_calls, "call on a removed handle");
}

/* -------------------------------------------------------------------------
 * UT-I2C-001 — i2c_init creates the bus once, on the configured pins
 * ------------------------------------------------------------------------- */
void test_i2c_init_returns_ok(void)
{
    /* setUp() has initialised already; a repeat must succeed without asking
     * ESP-IDF for the port again — i2c_new_master_bus() on an acquired port
     * returns ESP_ERR_INVALID_STATE. */
    TEST_ASSERT_EQUAL(I2C_OK, i2c_init());
    TEST_ASSERT_EQUAL_INT(1, mock_i2c_bus_create_calls());

    const i2c_master_bus_config_t *cfg = mock_i2c_bus_config();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL_INT(I2C_NUM_0, cfg->i2c_port);
    TEST_ASSERT_EQUAL_INT(PIN_I2C_SDA, cfg->sda_io_num);
    TEST_ASSERT_EQUAL_INT(PIN_I2C_SCL, cfg->scl_io_num);
    TEST_ASSERT_EQUAL_UINT32(1, cfg->flags.enable_internal_pullup);
}

/* -------------------------------------------------------------------------
 * UT-I2C-002 — i2c_write sends correct address and bytes
 * ------------------------------------------------------------------------- */
void test_i2c_write_sends_address_and_bytes(void)
{
    mock_i2c_attach(0x3E);
    const uint8_t data[] = { 0xAB, 0xCD };

    TEST_ASSERT_EQUAL(I2C_OK, i2c_write(0x3E, data, 2));

    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_WRITE, 0x3E);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned)x->tx_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, x->tx, 2);
    TEST_ASSERT_EQUAL_INT(ESP_OK, x->result);

    /* The transient handle was a 7-bit device at the documented bus speed. */
    TEST_ASSERT_EQUAL_INT(1, mock_i2c_handles_added);
    TEST_ASSERT_EQUAL_INT(I2C_ADDR_BIT_LEN_7, mock_i2c_last_device_config.dev_addr_length);
    TEST_ASSERT_EQUAL_UINT32(I2C_FREQ_HZ, mock_i2c_last_device_config.scl_speed_hz);
    assert_handles_released();
}

/* -------------------------------------------------------------------------
 * UT-I2C-003 — i2c_read returns the bytes the device holds
 * ------------------------------------------------------------------------- */
void test_i2c_read_returns_preloaded_bytes(void)
{
    mock_i2c_attach(0x68);
    const uint8_t staged[] = { 0x12, 0x34, 0x56 };
    mock_i2c_set_registers(0x68, 0x00, staged, 3);   /* pointer is 0 */

    uint8_t buf[3] = { 0 };
    TEST_ASSERT_EQUAL(I2C_OK, i2c_read(0x68, buf, 3));

    TEST_ASSERT_EQUAL_HEX8_ARRAY(staged, buf, 3);
    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_READ, 0x68);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)x->tx_len);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)x->rx_len);
    assert_handles_released();
}

/* -------------------------------------------------------------------------
 * UT-I2C-004 — i2c_write_read writes the register address, then reads it
 *
 * The device's pointer starts at 7, where an earlier 7-byte read would have
 * left a DS1307. Only a write of the register address BEFORE the read, in
 * one transaction, returns register 0.
 * ------------------------------------------------------------------------- */
void test_i2c_write_read_sequence(void)
{
    mock_i2c_attach(0x68);
    const uint8_t regs[] = { 0x45, 0x59, 0x23, 0x04, 0x16, 0x09, 0x26, 0x10 };
    mock_i2c_set_registers(0x68, 0x00, regs, sizeof(regs));
    mock_i2c_set_pointer(0x68, 0x07);

    const uint8_t tx_reg = 0x00;
    uint8_t rx_byte = 0;
    TEST_ASSERT_EQUAL(I2C_OK, i2c_write_read(0x68, &tx_reg, 1, &rx_byte, 1));

    TEST_ASSERT_EQUAL_HEX8(0x45, rx_byte);
    /* One transaction with a repeated start: no STOP between the phases. */
    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_WRITE_READ, 0x68);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)x->tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x00, x->tx[0]);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)x->rx_len);
    assert_handles_released();
}

/* -------------------------------------------------------------------------
 * UT-I2C-005 — a NACKed write returns I2C_ERR_NACK
 *
 * i2c_bus.h: I2C_ERR_NACK = "Device returned NACK (address or data)".
 * Nothing is attached at 0x3E, so the address is NACKed. On ESP-IDF 5.5
 * i2c_master_transmit() reports that as ESP_ERR_INVALID_STATE (see
 * mock_i2c_master.h), not the ESP_FAIL that i2c_bus.cpp's to_status()
 * comment expects.
 * ------------------------------------------------------------------------- */
void test_i2c_write_nack_returns_error(void)
{
    const uint8_t data = 0x00;
    i2c_status_t st = i2c_write(0x3E, &data, 1);

    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_WRITE, 0x3E);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, x->result);   /* what ESP-IDF said */
    assert_handles_released();                                 /* error path too */
    TEST_ASSERT_EQUAL_MESSAGE(I2C_ERR_NACK, st,
                              "a NACKed write is not reported as I2C_ERR_NACK");
}

/* -------------------------------------------------------------------------
 * UT-I2C-006 — i2c_scan returns the addresses that ACK
 * ------------------------------------------------------------------------- */
void test_i2c_scan_finds_ack_addresses(void)
{
    /* The LCD's AiP31068L and the DS1307 RTC. */
    mock_i2c_attach(0x3E);
    mock_i2c_attach(0x68);

    uint8_t found[8] = { 0 };
    uint8_t n = i2c_scan(found, 8);

    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x3E, found[0]);   /* ascending scan order */
    TEST_ASSERT_EQUAL_HEX8(0x68, found[1]);

    /* i2c_bus.h: addresses 1-126, each an address-only probe. */
    TEST_ASSERT_EQUAL_INT(126, mock_i2c_xfer_count());
    TEST_ASSERT_EQUAL_HEX16(1,   mock_i2c_xfer(0)->addr);
    TEST_ASSERT_EQUAL_HEX16(126, mock_i2c_xfer(125)->addr);
    for (int i = 0; i < mock_i2c_xfer_count(); i++) {
        TEST_ASSERT_EQUAL_INT(MOCK_I2C_PROBE, mock_i2c_xfer(i)->kind);
        TEST_ASSERT_TRUE(mock_i2c_xfer(i)->timeout_ms > 0);
    }
    TEST_ASSERT_EQUAL_INT(0, mock_i2c_handles_added);
}

/* -------------------------------------------------------------------------
 * UT-I2C-007 — a zero-length i2c_write is an address-only probe
 *
 * i2c_master_transmit() refuses a zero-length buffer with
 * ESP_ERR_INVALID_ARG, so the driver has to use i2c_master_probe().
 * ------------------------------------------------------------------------- */
void test_i2c_write_zero_length_returns_ok(void)
{
    mock_i2c_attach(0x3E);

    TEST_ASSERT_EQUAL(I2C_OK, i2c_write(0x3E, NULL, 0));

    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_PROBE, 0x3E);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)x->tx_len);           /* no data on the bus */
    TEST_ASSERT_EQUAL_INT(0, mock_i2c_invalid_arg_calls);
    assert_handles_released();
}

/* -------------------------------------------------------------------------
 * UT-I2C-008 — i2c_lock / i2c_unlock round-trip does not deadlock
 * ------------------------------------------------------------------------- */
void test_lock_unlock_roundtrip(void)
{
    /* No-op in both builds today (the FreeRTOS mutex is still deferred);
     * validates that the calls return, and documents the intent. */
    i2c_lock();
    i2c_unlock();
    /* Reaching this line proves no deadlock occurred. */
    TEST_ASSERT_TRUE(true);
}

/* -------------------------------------------------------------------------
 * UT-I2C-009 — probing an absent device returns I2C_ERR_NACK
 *
 * i2c_master_probe() reports the NACK as ESP_ERR_NOT_FOUND. Before that code
 * was mapped (2026-05-17) it came back as I2C_ERR_BUS_BUSY, and lcd_init()
 * aborted on the unit without the optional PCA9633 at 0x60.
 * ------------------------------------------------------------------------- */
void test_i2c_probe_absent_device_returns_nack(void)
{
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, i2c_write(0x60, NULL, 0));

    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_PROBE, 0x60);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, x->result);
}

/* -------------------------------------------------------------------------
 * UT-I2C-010 — a probe that times out returns I2C_ERR_TIMEOUT
 * ------------------------------------------------------------------------- */
void test_i2c_probe_timeout_returns_timeout(void)
{
    mock_i2c_attach(0x68);
    mock_i2c_hold_scl(0x68, true);            /* slave stretches SCL forever */

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write(0x68, NULL, 0));

    const mock_i2c_xfer_t *x = only_xfer(MOCK_I2C_PROBE, 0x68);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, x->result);
}

/* -------------------------------------------------------------------------
 * UT-I2C-011 — i2c_scan never writes past max_count
 * ------------------------------------------------------------------------- */
void test_i2c_scan_stops_at_capacity(void)
{
    mock_i2c_attach(0x3E);
    mock_i2c_attach(0x60);
    mock_i2c_attach(0x68);

    uint8_t found[3] = { 0x00, 0xA5, 0xA5 };  /* [1] and [2] are guards */
    uint8_t n = i2c_scan(found, 1);

    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x3E, found[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, found[1]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, found[2]);
    /* The scan stops once full: nothing is probed after 0x3E. */
    TEST_ASSERT_EQUAL_HEX16(0x3E, mock_i2c_xfer(mock_i2c_xfer_count() - 1)->addr);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    /* UT-I2C-001 */
    RUN_TEST(test_i2c_init_returns_ok);

    /* UT-I2C-002 */
    RUN_TEST(test_i2c_write_sends_address_and_bytes);

    /* UT-I2C-003 */
    RUN_TEST(test_i2c_read_returns_preloaded_bytes);

    /* UT-I2C-004 */
    RUN_TEST(test_i2c_write_read_sequence);

    /* UT-I2C-005 */
    RUN_TEST(test_i2c_write_nack_returns_error);

    /* UT-I2C-006 */
    RUN_TEST(test_i2c_scan_finds_ack_addresses);

    /* UT-I2C-007 */
    RUN_TEST(test_i2c_write_zero_length_returns_ok);

    /* UT-I2C-008 */
    RUN_TEST(test_lock_unlock_roundtrip);

    /* UT-I2C-009 */
    RUN_TEST(test_i2c_probe_absent_device_returns_nack);

    /* UT-I2C-010 */
    RUN_TEST(test_i2c_probe_timeout_returns_timeout);

    /* UT-I2C-011 */
    RUN_TEST(test_i2c_scan_stops_at_capacity);

    return UNITY_END();
}
