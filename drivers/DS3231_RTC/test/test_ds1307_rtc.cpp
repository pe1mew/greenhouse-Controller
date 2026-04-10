/**
 * LIB-3 DS1307 RTC — unit tests (native build)
 *
 * Test IDs: UT-RTC-001 … UT-RTC-013
 *
 * DS1307 register map used here:
 *   0x00 seconds (bit 7 = CH)   0x01 minutes  0x02 hours   0x03 day-of-week
 *   0x04 day                    0x05 month    0x06 year
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include "../src/ds1307_rtc.h"
#include "mock_i2c_bus.h"

void setUp(void)
{
    mock_rtc_reset();
}

void tearDown(void) {}

/* -------------------------------------------------------------------------
 * UT-RTC-001 — rtc_init returns RTC_OK when mock ACKs
 * ------------------------------------------------------------------------- */
void test_rtc_init_ok(void)
{
    TEST_ASSERT_EQUAL(RTC_OK, rtc_init());
}

/* -------------------------------------------------------------------------
 * UT-RTC-002 — rtc_init returns RTC_ERR_NO_DEVICE on NACK
 * ------------------------------------------------------------------------- */
void test_rtc_init_nack(void)
{
    mock_nack_next = true;
    TEST_ASSERT_EQUAL(RTC_ERR_NO_DEVICE, rtc_init());
}

/* -------------------------------------------------------------------------
 * UT-RTC-003 — BCD decode seconds: reg 0x00 = 0x45 → second = 45
 * ------------------------------------------------------------------------- */
void test_bcd_decode_seconds(void)
{
    mock_rtc_set_register(0x00, 0x45); /* 0x45 BCD = 45 */
    mock_rtc_set_register(0x03, 0x01); /* day_of_week = 1 (valid) */
    mock_rtc_set_register(0x04, 0x01); /* day = 1 */
    mock_rtc_set_register(0x05, 0x01); /* month = 1 */

    rtc_datetime_t dt;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_get_time(&dt));
    TEST_ASSERT_EQUAL_UINT8(45, dt.second);
}

/* -------------------------------------------------------------------------
 * UT-RTC-004 — BCD decode minutes: reg 0x01 = 0x30 → minute = 30
 * ------------------------------------------------------------------------- */
void test_bcd_decode_minutes(void)
{
    mock_rtc_set_register(0x01, 0x30); /* 0x30 BCD = 30 */
    mock_rtc_set_register(0x03, 0x01);
    mock_rtc_set_register(0x04, 0x01);
    mock_rtc_set_register(0x05, 0x01);

    rtc_datetime_t dt;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_get_time(&dt));
    TEST_ASSERT_EQUAL_UINT8(30, dt.minute);
}

/* -------------------------------------------------------------------------
 * UT-RTC-005 — BCD decode hours: reg 0x02 = 0x23 → hour = 23
 * ------------------------------------------------------------------------- */
void test_bcd_decode_hours(void)
{
    mock_rtc_set_register(0x02, 0x23); /* 0x23 BCD = 23 */
    mock_rtc_set_register(0x03, 0x01);
    mock_rtc_set_register(0x04, 0x01);
    mock_rtc_set_register(0x05, 0x01);

    rtc_datetime_t dt;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_get_time(&dt));
    TEST_ASSERT_EQUAL_UINT8(23, dt.hour);
}

/* -------------------------------------------------------------------------
 * UT-RTC-006 — BCD decode full date (day, month, year) all fields correct
 * Pre-set: 2026-04-10, day_of_week = 5
 * ------------------------------------------------------------------------- */
void test_bcd_decode_full_date(void)
{
    mock_rtc_set_register(0x03, 0x05); /* day_of_week = 5 */
    mock_rtc_set_register(0x04, 0x10); /* day   = 0x10 BCD = 10 */
    mock_rtc_set_register(0x05, 0x04); /* month = 0x04 BCD = 4  */
    mock_rtc_set_register(0x06, 0x26); /* year  = 0x26 BCD = 26 → 2026 */

    /* Need valid day/month/dow for validation to pass */
    mock_rtc_set_register(0x03, 0x05);
    mock_rtc_set_register(0x04, 0x10);
    mock_rtc_set_register(0x05, 0x04);

    rtc_datetime_t dt;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_get_time(&dt));
    TEST_ASSERT_EQUAL_UINT8(5,    dt.day_of_week);
    TEST_ASSERT_EQUAL_UINT8(10,   dt.day);
    TEST_ASSERT_EQUAL_UINT8(4,    dt.month);
    TEST_ASSERT_EQUAL_UINT16(2026, dt.year);
}

/* -------------------------------------------------------------------------
 * UT-RTC-007 — rtc_set_time encodes second = 45 → reg 0x00 = 0x45
 * ------------------------------------------------------------------------- */
void test_set_time_encodes_seconds(void)
{
    rtc_datetime_t dt = { 45, 0, 0, 1, 1, 1, 2026 };
    TEST_ASSERT_EQUAL(RTC_OK, rtc_set_time(&dt));
    TEST_ASSERT_EQUAL_HEX8(0x45, mock_rtc_regs[0x00]);
}

/* -------------------------------------------------------------------------
 * UT-RTC-008 — rtc_set_time encodes year 2026 correctly → reg 0x06 = 0x26
 * ------------------------------------------------------------------------- */
void test_set_time_encodes_year(void)
{
    rtc_datetime_t dt = { 0, 0, 0, 1, 1, 1, 2026 };
    TEST_ASSERT_EQUAL(RTC_OK, rtc_set_time(&dt));
    TEST_ASSERT_EQUAL_HEX8(0x26, mock_rtc_regs[0x06]); /* 26 decimal = 0x26 BCD */
}

/* -------------------------------------------------------------------------
 * UT-RTC-011 — CH bit 7 of seconds reg set → rtc_oscillator_stopped() true
 * ------------------------------------------------------------------------- */
void test_osf_set(void)
{
    mock_rtc_set_register(0x00, 0x80); /* bit 7 = CH (Clock Halt) */
    TEST_ASSERT_TRUE(rtc_oscillator_stopped());
}

/* -------------------------------------------------------------------------
 * UT-RTC-012 — CH bit 7 of seconds reg clear → rtc_oscillator_stopped() false
 * ------------------------------------------------------------------------- */
void test_osf_clear(void)
{
    mock_rtc_set_register(0x00, 0x00);
    TEST_ASSERT_FALSE(rtc_oscillator_stopped());
}

/* -------------------------------------------------------------------------
 * UT-RTC-013 — Invalid BCD (seconds reg = 0x60) → RTC_ERR_INVALID
 *   0x60 BCD decodes to 60 — out of valid range (0–59)
 * ------------------------------------------------------------------------- */
void test_invalid_bcd_seconds(void)
{
    mock_rtc_set_register(0x00, 0x60); /* 60 > 59 → invalid */
    mock_rtc_set_register(0x03, 0x01);
    mock_rtc_set_register(0x04, 0x01);
    mock_rtc_set_register(0x05, 0x01);

    rtc_datetime_t dt;
    TEST_ASSERT_EQUAL(RTC_ERR_INVALID, rtc_get_time(&dt));
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_rtc_init_ok);           /* UT-RTC-001 */
    RUN_TEST(test_rtc_init_nack);         /* UT-RTC-002 */
    RUN_TEST(test_bcd_decode_seconds);    /* UT-RTC-003 */
    RUN_TEST(test_bcd_decode_minutes);    /* UT-RTC-004 */
    RUN_TEST(test_bcd_decode_hours);      /* UT-RTC-005 */
    RUN_TEST(test_bcd_decode_full_date);  /* UT-RTC-006 */
    RUN_TEST(test_set_time_encodes_seconds); /* UT-RTC-007 */
    RUN_TEST(test_set_time_encodes_year); /* UT-RTC-008 */
    RUN_TEST(test_osf_set);               /* UT-RTC-011 */
    RUN_TEST(test_osf_clear);             /* UT-RTC-012 */
    RUN_TEST(test_invalid_bcd_seconds);   /* UT-RTC-013 */

    return UNITY_END();
}
