/**
 * S200 driver — unit tests (native build)
 *
 * Test IDs: UT-S200-001 … UT-S200-011
 *
 * Run with:  pio test -e native
 *
 * Tests verify:
 *   - Raw-to-float conversion (÷1000) for wind direction and speed
 *   - All six wind channels decoded in a single call
 *   - Error propagation from modbus layer → s200_status_t
 *   - NULL / address-0 parameter rejection
 *   - One FC04 request per measurement, the wind block (start=0x0008, count=12)
 */

#include <unity.h>
#include "mock_modbus.h"
#include "../../src/s200.h"

/* ---------------------------------------------------------------------------
 * Unity fixtures
 * --------------------------------------------------------------------------- */
void setUp(void)    { mock_modbus_reset(); }
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/**
 * Encode a signed int32 value into two uint16 registers (big-endian word order).
 * reg[0] = high word, reg[1] = low word.
 */
static void encode_int32(int32_t value, uint16_t *regs)
{
    regs[0] = (uint16_t)((uint32_t)value >> 16u);
    regs[1] = (uint16_t)(value & 0xFFFFu);
}

/**
 * Load the six wind channels (direction min/max/avg + speed min/max/avg)
 * into the register bank at 0x0008.  All values are raw (× 1000).
 */
static void set_wind_regs(int32_t dir_min, int32_t dir_max, int32_t dir_avg,
                           int32_t spd_min, int32_t spd_max, int32_t spd_avg)
{
    uint16_t regs[12];
    encode_int32(dir_min, &regs[0]);
    encode_int32(dir_max, &regs[2]);
    encode_int32(dir_avg, &regs[4]);
    encode_int32(spd_min, &regs[6]);
    encode_int32(spd_max, &regs[8]);
    encode_int32(spd_avg, &regs[10]);
    mock_modbus_set_registers(0x0008u, regs, 12u);
}

/* =========================================================================
 * UT-S200-001 — Wind direction decoded correctly
 *
 * Raw min direction = 180 000 → 180.000°
 * ========================================================================= */
void test_wind_direction_decoded(void)
{
    set_wind_regs(180000, 270000, 225000, 0, 0, 0);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 180.0f,  m.wind_dir_min_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 270.0f,  m.wind_dir_max_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 225.0f,  m.wind_dir_avg_deg);
}

/* =========================================================================
 * UT-S200-002 — Wind speed decoded correctly
 *
 * Raw avg speed = 5 000 → 5.000 m/s
 * ========================================================================= */
void test_wind_speed_decoded(void)
{
    set_wind_regs(0, 0, 0, 1000, 10000, 5000);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f,  m.wind_speed_min_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, m.wind_speed_max_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f,  m.wind_speed_avg_ms);
}

/* =========================================================================
 * UT-S200-003 — Zero wind speed and direction
 * ========================================================================= */
void test_zero_values(void)
{
    set_wind_regs(0, 0, 0, 0, 0, 0);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, m.wind_dir_min_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, m.wind_speed_avg_ms);
}

/* =========================================================================
 * UT-S200-004 — All six wind channels decoded in one call
 *
 * Distinct values for every channel to rule out cross-wiring.
 * ========================================================================= */
void test_all_wind_channels(void)
{
    set_wind_regs(10000, 350000, 180000, 500, 15000, 7500);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_OK, st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f,  m.wind_dir_min_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 350.0f, m.wind_dir_max_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 180.0f, m.wind_dir_avg_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f,   m.wind_speed_min_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f,  m.wind_speed_max_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.5f,   m.wind_speed_avg_ms);
}

/* =========================================================================
 * UT-S200-007 — Modbus timeout on wind read → S200_ERR_COMM
 * ========================================================================= */
void test_read_comm_error_timeout(void)
{
    mock_modbus_set_read_status(MODBUS_ERR_TIMEOUT);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_ERR_COMM, st);
}

/* =========================================================================
 * UT-S200-008 — Modbus CRC error → S200_ERR_COMM
 * ========================================================================= */
void test_read_comm_error_crc(void)
{
    mock_modbus_set_read_status(MODBUS_ERR_CRC);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    TEST_ASSERT_EQUAL_INT(S200_ERR_COMM, st);
}

/* =========================================================================
 * UT-S200-009 — NULL output pointer → S200_ERR_PARAM
 * ========================================================================= */
void test_null_output_pointer(void)
{
    s200_status_t st = s200_read_measurements(44u, nullptr);
    TEST_ASSERT_EQUAL_INT(S200_ERR_PARAM, st);
}

/* =========================================================================
 * UT-S200-010 — Slave address 0 → S200_ERR_PARAM
 * ========================================================================= */
void test_addr_zero(void)
{
    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(0u, &m);
    TEST_ASSERT_EQUAL_INT(S200_ERR_PARAM, st);
}

/* =========================================================================
 * UT-S200-011 — First FC04 read uses start register 0x0008, count 12
 *
 * Verifies the driver targets the correct register block for wind data.
 * After a successful call the mock records start_reg and count of the
 * only FC04 read.
 * ========================================================================= */
void test_wind_read_correct_register_block(void)
{
    set_wind_regs(0, 0, 0, 0, 0, 0);

    s200_measurement_t m;
    s200_status_t st = s200_read_measurements(44u, &m);

    /* The mock records the LAST FC04 call, so this is also the proof that the
     * heating temperature (0x001C) is no longer read after it. */
    TEST_ASSERT_EQUAL_INT(S200_OK, st);
    TEST_ASSERT_EQUAL_HEX16(0x0008u, mock_modbus_get_last_read_reg());
    TEST_ASSERT_EQUAL_INT(12, mock_modbus_get_last_read_count());
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    UNITY_BEGIN();

    /* UT-S200-001 */ RUN_TEST(test_wind_direction_decoded);
    /* UT-S200-002 */ RUN_TEST(test_wind_speed_decoded);
    /* UT-S200-003 */ RUN_TEST(test_zero_values);
    /* UT-S200-004 */ RUN_TEST(test_all_wind_channels);
    /* UT-S200-007 */ RUN_TEST(test_read_comm_error_timeout);
    /* UT-S200-008 */ RUN_TEST(test_read_comm_error_crc);
    /* UT-S200-009 */ RUN_TEST(test_null_output_pointer);
    /* UT-S200-010 */ RUN_TEST(test_addr_zero);
    /* UT-S200-011 */ RUN_TEST(test_wind_read_correct_register_block);

    return UNITY_END();
}
