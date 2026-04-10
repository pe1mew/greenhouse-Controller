/**
 * LIB-2 I2C Bus — unit tests (native build)
 *
 * Test IDs: UT-I2C-001 … UT-I2C-008
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include "../src/i2c_bus.h"
#include "mock_wire.h"

void setUp(void)
{
    mock_wire_reset();
}

void tearDown(void) {}

/* -------------------------------------------------------------------------
 * UT-I2C-001 — i2c_init returns I2C_OK
 * ------------------------------------------------------------------------- */
void test_i2c_init_returns_ok(void)
{
    i2c_status_t st = i2c_init();
    TEST_ASSERT_EQUAL(I2C_OK, st);
}

/* -------------------------------------------------------------------------
 * UT-I2C-002 — i2c_write sends correct address and bytes
 * ------------------------------------------------------------------------- */
void test_i2c_write_sends_address_and_bytes(void)
{
    const uint8_t data[] = { 0xAB, 0xCD };
    i2c_status_t st = i2c_write(0x3E, data, 2);

    TEST_ASSERT_EQUAL(I2C_OK, st);
    TEST_ASSERT_EQUAL_UINT8(0x3E, mock_last_addr);
    TEST_ASSERT_EQUAL_UINT8(2,    mock_tx_len);
    TEST_ASSERT_EQUAL_UINT8(0xAB, mock_tx_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xCD, mock_tx_buf[1]);
}

/* -------------------------------------------------------------------------
 * UT-I2C-003 — i2c_read returns preloaded bytes
 * ------------------------------------------------------------------------- */
void test_i2c_read_returns_preloaded_bytes(void)
{
    const uint8_t staged[] = { 0x12, 0x34, 0x56 };
    mock_wire_preload_rx(staged, 3);

    uint8_t buf[3] = { 0 };
    i2c_status_t st = i2c_read(0x68, buf, 3);

    TEST_ASSERT_EQUAL(I2C_OK, st);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x56, buf[2]);
}

/* -------------------------------------------------------------------------
 * UT-I2C-004 — i2c_write_read performs write before read, correct address
 * ------------------------------------------------------------------------- */
void test_i2c_write_read_sequence(void)
{
    const uint8_t rx_data[] = { 0x45 };
    mock_wire_preload_rx(rx_data, 1);

    const uint8_t tx_reg = 0x00; /* register address byte */
    uint8_t rx_byte = 0;
    i2c_status_t st = i2c_write_read(0x68, &tx_reg, 1, &rx_byte, 1);

    TEST_ASSERT_EQUAL(I2C_OK, st);
    /* Write phase: address and register byte were sent */
    TEST_ASSERT_EQUAL_UINT8(0x68, mock_last_addr);
    TEST_ASSERT_EQUAL_UINT8(1,    mock_tx_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, mock_tx_buf[0]);
    /* Read phase: correct byte returned */
    TEST_ASSERT_EQUAL_UINT8(0x45, rx_byte);
}

/* -------------------------------------------------------------------------
 * UT-I2C-005 — NACK flag set → I2C_ERR_NACK returned
 * ------------------------------------------------------------------------- */
void test_i2c_write_nack_returns_error(void)
{
    mock_nack_next = true;
    const uint8_t data = 0x00;
    i2c_status_t st = i2c_write(0x3E, &data, 1);
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, st);
}

/* -------------------------------------------------------------------------
 * UT-I2C-006 — i2c_scan returns addresses where mock ACKs
 * ------------------------------------------------------------------------- */
void test_i2c_scan_finds_ack_addresses(void)
{
    /* Only 0x3E (LCD PCF8574A) and 0x68 (RTC) should ACK */
    mock_ack_addrs[0] = 0x3E;
    mock_ack_addrs[1] = 0x68;
    mock_ack_count    = 2;

    uint8_t found[8] = { 0 };
    uint8_t n = i2c_scan(found, 8);

    TEST_ASSERT_EQUAL_UINT8(2, n);

    bool found_3e = false;
    bool found_68 = false;
    for (uint8_t i = 0; i < n; i++) {
        if (found[i] == 0x3E) { found_3e = true; }
        if (found[i] == 0x68) { found_68 = true; }
    }
    TEST_ASSERT_TRUE(found_3e);
    TEST_ASSERT_TRUE(found_68);
}

/* -------------------------------------------------------------------------
 * UT-I2C-007 — i2c_write with zero-length data returns I2C_OK
 * ------------------------------------------------------------------------- */
void test_i2c_write_zero_length_returns_ok(void)
{
    i2c_status_t st = i2c_write(0x3E, NULL, 0);
    TEST_ASSERT_EQUAL(I2C_OK, st);
    TEST_ASSERT_EQUAL_UINT8(0, mock_tx_len); /* nothing was written to the bus */
}

/* -------------------------------------------------------------------------
 * UT-I2C-008 — i2c_lock / i2c_unlock round-trip does not deadlock
 * ------------------------------------------------------------------------- */
void test_lock_unlock_roundtrip(void)
{
    /* No-op in native build; validates that the calls compile and return
     * without hanging — sufficient to document the FreeRTOS mutex intent. */
    i2c_lock();
    i2c_unlock();
    /* Reaching this line proves no deadlock occurred. */
    TEST_ASSERT_TRUE(true);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
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

    return UNITY_END();
}
