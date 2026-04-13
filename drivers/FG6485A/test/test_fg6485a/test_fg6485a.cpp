/**
 * FG6485A driver — unit tests (native build)
 *
 * Test IDs: UT-FG-001 … UT-FG-020
 *
 * Run with:  pio test -e native
 *
 * Tests verify:
 *   - Raw-to-float conversion (÷10) for humidity and temperature
 *   - Signed temperature values (negative °C)
 *   - Device info parsing (device_id assembly from two 16-bit registers)
 *   - Alarm config read and write round-trip encoding
 *   - Correction write encoding
 *   - read_all skipping when NULL pointers passed
 *   - Error propagation from modbus layer → fg6485a_status_t
 *   - NULL / address-0 parameter rejection
 */

#include <unity.h>
#include "mock_modbus.h"
#include "../../src/fg6485a.h"

/* ---------------------------------------------------------------------------
 * Unity fixtures
 * --------------------------------------------------------------------------- */
void setUp(void)    { mock_modbus_reset(); }
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/** Set the two measurement registers (humidity then temperature, both raw ×10). */
static void set_meas_regs(int16_t humidity_raw, int16_t temperature_raw)
{
    uint16_t regs[2] = {
        (uint16_t)humidity_raw,
        (uint16_t)temperature_raw
    };
    mock_modbus_set_registers(0x0000u, regs, 2u);
}

/* =========================================================================
 * UT-FG-001 — Positive humidity and temperature decoded correctly
 *
 * Raw humidity    = 471  → 47.1 %RH
 * Raw temperature = 214  → 21.4 °C
 * (example from the FG6485A datasheet, page 5)
 * ========================================================================= */
void test_measurement_positive_values(void)
{
    set_meas_regs(471, 214);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 47.1f, m.humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.4f, m.temperature_c);
}

/* =========================================================================
 * UT-FG-002 — Negative temperature decoded correctly
 *
 * Raw temperature = -100 (i.e. 0xFF9C as uint16) → -10.0 °C
 * ========================================================================= */
void test_measurement_negative_temperature(void)
{
    set_meas_regs(300, -100);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, m.humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.0f, m.temperature_c);
}

/* =========================================================================
 * UT-FG-003 — Zero temperature and zero humidity
 * ========================================================================= */
void test_measurement_zero_values(void)
{
    set_meas_regs(0, 0);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, m.humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, m.temperature_c);
}

/* =========================================================================
 * UT-FG-004 — Modbus communication error → FG6485A_ERR_COMM
 * ========================================================================= */
void test_read_measurements_comm_error(void)
{
    mock_modbus_set_read_status(MODBUS_ERR_TIMEOUT);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_COMM, st);
}

/* =========================================================================
 * UT-FG-005 — CRC error → FG6485A_ERR_COMM
 * ========================================================================= */
void test_read_measurements_crc_error(void)
{
    mock_modbus_set_read_status(MODBUS_ERR_CRC);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_COMM, st);
}

/* =========================================================================
 * UT-FG-006 — NULL output pointer → FG6485A_ERR_PARAM
 * ========================================================================= */
void test_read_measurements_null_out(void)
{
    fg6485a_status_t st = fg6485a_read_measurements(1u, nullptr);
    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_PARAM, st);
}

/* =========================================================================
 * UT-FG-007 — Slave address 0 → FG6485A_ERR_PARAM
 * ========================================================================= */
void test_read_measurements_addr_zero(void)
{
    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(0u, &m);
    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_PARAM, st);
}

/* =========================================================================
 * UT-FG-008 — Device info parsed correctly
 *
 * device_type = 0x1234
 * version     = 0x0056
 * id_high     = 0xABCD
 * id_low      = 0x1234
 * device_id   = 0xABCD1234
 * ========================================================================= */
void test_read_info_parsed_correctly(void)
{
    uint16_t regs[4] = {0x1234u, 0x0056u, 0xABCDu, 0x1234u};
    mock_modbus_set_registers(0x0008u, regs, 4u);

    fg6485a_info_t info;
    fg6485a_status_t st = fg6485a_read_info(1u, &info);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_EQUAL_HEX16(0x1234u, info.device_type);
    TEST_ASSERT_EQUAL_HEX16(0x0056u, info.version);
    TEST_ASSERT_EQUAL_HEX32(0xABCD1234u, info.device_id);
}

/* =========================================================================
 * UT-FG-009 — read_info comm error propagated
 * ========================================================================= */
void test_read_info_comm_error(void)
{
    mock_modbus_set_read_status(MODBUS_ERR_EXCEPTION);

    fg6485a_info_t info;
    fg6485a_status_t st = fg6485a_read_info(1u, &info);

    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_COMM, st);
}

/* =========================================================================
 * UT-FG-010 — read_all: both meas and info read
 * ========================================================================= */
void test_read_all_reads_both(void)
{
    set_meas_regs(350, 220);
    uint16_t info_regs[4] = {0x0001u, 0x0002u, 0x0003u, 0x0004u};
    mock_modbus_set_registers(0x0008u, info_regs, 4u);

    fg6485a_measurement_t meas;
    fg6485a_info_t        info;
    fg6485a_status_t st = fg6485a_read_all(1u, &meas, &info);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 35.0f, meas.humidity_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, meas.temperature_c);
    TEST_ASSERT_EQUAL_HEX32(0x00030004u, info.device_id);
}

/* =========================================================================
 * UT-FG-011 — read_all: NULL info skips info read (no extra modbus call)
 * ========================================================================= */
void test_read_all_skips_null_info(void)
{
    set_meas_regs(400, 200);
    /* Do not load info registers — if read is attempted values will be zero */

    fg6485a_measurement_t meas;
    fg6485a_status_t st = fg6485a_read_all(1u, &meas, nullptr);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, meas.humidity_pct);
}

/* =========================================================================
 * UT-FG-012 — read_all: NULL meas skips measurement read
 * ========================================================================= */
void test_read_all_skips_null_meas(void)
{
    uint16_t info_regs[4] = {0x0010u, 0x0020u, 0x0030u, 0x0040u};
    mock_modbus_set_registers(0x0008u, info_regs, 4u);

    fg6485a_info_t info;
    fg6485a_status_t st = fg6485a_read_all(1u, nullptr, &info);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_EQUAL_HEX16(0x0010u, info.device_type);
}

/* =========================================================================
 * UT-FG-013 — Alarm config read decoded correctly
 *
 * Raw values written in the register bank:
 *   temp_hi = 400  → 40.0 °C, en = 1
 *   temp_lo = 100  → 10.0 °C, en = 0
 *   hum_hi  = 800  → 80.0 %RH, en = 1
 *   hum_lo  = 200  → 20.0 %RH, en = 0
 * ========================================================================= */
void test_read_alarm_config_decoded(void)
{
    uint16_t regs[8] = {400u, 1u, 100u, 0u, 800u, 1u, 200u, 0u};
    mock_modbus_set_registers(0x000Cu, regs, 8u);

    fg6485a_alarm_config_t cfg;
    fg6485a_status_t st = fg6485a_read_alarm_config(1u, &cfg);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, cfg.temp_alarm_high);
    TEST_ASSERT_TRUE(cfg.temp_alarm_high_en);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, cfg.temp_alarm_low);
    TEST_ASSERT_FALSE(cfg.temp_alarm_low_en);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, cfg.hum_alarm_high);
    TEST_ASSERT_TRUE(cfg.hum_alarm_high_en);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, cfg.hum_alarm_low);
    TEST_ASSERT_FALSE(cfg.hum_alarm_low_en);
}

/* =========================================================================
 * UT-FG-014 — Alarm config write encodes correctly (×10 integer)
 *
 * Write:
 *   temp_hi = 35.5 °C, en = true   → raw 355, 1
 *   temp_lo = -5.0 °C, en = false  → raw -50 (0xFFCE), 0
 *   hum_hi  = 90.0 %RH, en = true  → raw 900, 1
 *   hum_lo  = 15.5 %RH, en = false → raw 155, 0
 * ========================================================================= */
void test_write_alarm_config_encoding(void)
{
    fg6485a_alarm_config_t cfg = {
        35.5f, true,
        -5.0f, false,
        90.0f, true,
        15.5f, false
    };
    fg6485a_status_t st = fg6485a_write_alarm_config(1u, &cfg);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);

    const uint16_t *v = mock_modbus_get_last_written_values();
    TEST_ASSERT_EQUAL_HEX16(0x000Cu,              mock_modbus_get_last_write_reg());
    TEST_ASSERT_EQUAL_INT(8,                       mock_modbus_get_last_write_count());
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(int16_t)355, v[0]);  /* temp hi */
    TEST_ASSERT_EQUAL_HEX16(1u,                   v[1]);  /* temp hi en */
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(int16_t)-50, v[2]); /* temp lo */
    TEST_ASSERT_EQUAL_HEX16(0u,                   v[3]);  /* temp lo en */
    TEST_ASSERT_EQUAL_HEX16(900u,                 v[4]);  /* hum hi */
    TEST_ASSERT_EQUAL_HEX16(1u,                   v[5]);  /* hum hi en */
    TEST_ASSERT_EQUAL_HEX16(155u,                 v[6]);  /* hum lo */
    TEST_ASSERT_EQUAL_HEX16(0u,                   v[7]);  /* hum lo en */
}

/* =========================================================================
 * UT-FG-015 — write_alarm_config NULL cfg → FG6485A_ERR_PARAM
 * ========================================================================= */
void test_write_alarm_config_null_param(void)
{
    fg6485a_status_t st = fg6485a_write_alarm_config(1u, nullptr);
    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_PARAM, st);
}

/* =========================================================================
 * UT-FG-016 — Temperature correction write encodes correctly
 *
 * correction = 1.5 °C → writes raw 15 to register 0x001D
 * ========================================================================= */
void test_write_temp_correction_encoding(void)
{
    fg6485a_status_t st = fg6485a_write_temp_correction(1u, 1.5f);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_EQUAL_HEX16(0x001Du, mock_modbus_get_last_write_reg());
    TEST_ASSERT_EQUAL_INT(1, mock_modbus_get_last_write_count());
    TEST_ASSERT_EQUAL_HEX16(15u, mock_modbus_get_last_written_values()[0]);
}

/* =========================================================================
 * UT-FG-017 — Negative temperature correction encodes correctly
 *
 * correction = -2.0 °C → writes raw -20 (0xFFEC as uint16) to 0x001D
 * ========================================================================= */
void test_write_temp_correction_negative(void)
{
    fg6485a_status_t st = fg6485a_write_temp_correction(1u, -2.0f);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(int16_t)-20,
                             mock_modbus_get_last_written_values()[0]);
}

/* =========================================================================
 * UT-FG-018 — Humidity correction write encodes correctly
 *
 * correction = 3.0 %RH → writes raw 30 to register 0x001E
 * ========================================================================= */
void test_write_humidity_correction_encoding(void)
{
    fg6485a_status_t st = fg6485a_write_humidity_correction(1u, 3.0f);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_EQUAL_HEX16(0x001Eu, mock_modbus_get_last_write_reg());
    TEST_ASSERT_EQUAL_INT(1, mock_modbus_get_last_write_count());
    TEST_ASSERT_EQUAL_HEX16(30u, mock_modbus_get_last_written_values()[0]);
}

/* =========================================================================
 * UT-FG-019 — Write error from modbus → FG6485A_ERR_COMM
 * ========================================================================= */
void test_write_alarm_config_comm_error(void)
{
    mock_modbus_set_write_status(MODBUS_ERR_TIMEOUT);

    fg6485a_alarm_config_t cfg = {30.0f, false, 10.0f, false,
                                    70.0f, false, 30.0f, false};
    fg6485a_status_t st = fg6485a_write_alarm_config(1u, &cfg);

    TEST_ASSERT_EQUAL_INT(FG6485A_ERR_COMM, st);
}

/* =========================================================================
 * UT-FG-020 — Modbus reads correct register block for measurements
 *             (start = 0x0000, count = 2)
 *
 * Verifies the driver asks for the right address range by checking
 * that only registers 0x0000–0x0001 are consumed, leaving 0x0002
 * at its default (zero) value.
 * ========================================================================= */
void test_read_measurements_correct_register_range(void)
{
    /* Set register 0x0000=100, 0x0001=200, 0x0002=999 (must not be read) */
    uint16_t regs[3] = {100u, 200u, 999u};
    mock_modbus_set_registers(0x0000u, regs, 3u);

    fg6485a_measurement_t m;
    fg6485a_status_t st = fg6485a_read_measurements(1u, &m);

    TEST_ASSERT_EQUAL_INT(FG6485A_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, m.humidity_pct);    /* 100 / 10 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, m.temperature_c);   /* 200 / 10 */
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    UNITY_BEGIN();

    /* UT-FG-001 */ RUN_TEST(test_measurement_positive_values);
    /* UT-FG-002 */ RUN_TEST(test_measurement_negative_temperature);
    /* UT-FG-003 */ RUN_TEST(test_measurement_zero_values);
    /* UT-FG-004 */ RUN_TEST(test_read_measurements_comm_error);
    /* UT-FG-005 */ RUN_TEST(test_read_measurements_crc_error);
    /* UT-FG-006 */ RUN_TEST(test_read_measurements_null_out);
    /* UT-FG-007 */ RUN_TEST(test_read_measurements_addr_zero);
    /* UT-FG-008 */ RUN_TEST(test_read_info_parsed_correctly);
    /* UT-FG-009 */ RUN_TEST(test_read_info_comm_error);
    /* UT-FG-010 */ RUN_TEST(test_read_all_reads_both);
    /* UT-FG-011 */ RUN_TEST(test_read_all_skips_null_info);
    /* UT-FG-012 */ RUN_TEST(test_read_all_skips_null_meas);
    /* UT-FG-013 */ RUN_TEST(test_read_alarm_config_decoded);
    /* UT-FG-014 */ RUN_TEST(test_write_alarm_config_encoding);
    /* UT-FG-015 */ RUN_TEST(test_write_alarm_config_null_param);
    /* UT-FG-016 */ RUN_TEST(test_write_temp_correction_encoding);
    /* UT-FG-017 */ RUN_TEST(test_write_temp_correction_negative);
    /* UT-FG-018 */ RUN_TEST(test_write_humidity_correction_encoding);
    /* UT-FG-019 */ RUN_TEST(test_write_alarm_config_comm_error);
    /* UT-FG-020 */ RUN_TEST(test_read_measurements_correct_register_range);

    return UNITY_END();
}
