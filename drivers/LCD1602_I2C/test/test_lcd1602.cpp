/**
 * LIB-4 LCD1602 I2C — unit tests (native build)
 *
 * Test IDs: UT-LCD-001 … UT-LCD-011
 *
 * PCF8574A byte layout (each nibble transfer = 2 bytes in the TX log):
 *   bits 7–4 : nibble value (DB7–DB4)
 *   bit  3   : backlight (1 = on)
 *   bit  2   : En  (1 = strobe high, 0 = strobe low / latch)
 *   bit  1   : RW  (always 0)
 *   bit  0   : RS  (0 = command, 1 = data)
 *
 * Expected bytes with backlight on (BL=1, BIT_BL=0x08):
 *   nibble N, RS=0: En=1 byte = (N<<4)|0x0C,  En=0 byte = (N<<4)|0x08
 *   nibble N, RS=1: En=1 byte = (N<<4)|0x0D,  En=0 byte = (N<<4)|0x09
 *
 * A full byte write produces 4 bytes:
 *   (hi_nibble with En=1), (hi_nibble with En=0),
 *   (lo_nibble with En=1), (lo_nibble with En=0)
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include "../src/lcd1602.h"
#include "mock_i2c_bus.h"

/* ---------------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------------- */

void setUp(void)
{
    mock_lcd_reset();
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * UT-LCD-001 — lcd_init sends the HD44780 4-bit init sequence
 *
 * Expected 28 bytes (backlight on throughout):
 *   [0..5]   : 3 × nibble 0x3 (function-set reset nibbles)  {0x3C, 0x38} each
 *   [6..7]   : nibble 0x2 (switch to 4-bit)                 {0x2C, 0x28}
 *   [8..11]  : byte 0x28 (function set)
 *   [12..15] : byte 0x08 (display off)
 *   [16..19] : byte 0x01 (clear)
 *   [20..23] : byte 0x06 (entry mode set)
 *   [24..27] : byte 0x0C (display on)
 * --------------------------------------------------------------------------- */
void test_lcd_init_sequence(void)
{
    lcd_backlight_on();
    lcd_init();

    uint8_t  log[32];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(28, len);

    /* Three function-set nibbles 0x3 (RS=0, BL=1). */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x3C, log[i * 2]);      /* En=1 */
        TEST_ASSERT_EQUAL_HEX8(0x38, log[i * 2 + 1]);  /* En=0 */
    }

    /* Entry mode set command 0x06 is at log[20..23]. */
    TEST_ASSERT_EQUAL_HEX8(0x0C, log[20]);  /* high nibble 0x0, En=1 */
    TEST_ASSERT_EQUAL_HEX8(0x08, log[21]);  /* high nibble 0x0, En=0 */
    TEST_ASSERT_EQUAL_HEX8(0x6C, log[22]);  /* low nibble  0x6, En=1 */
    TEST_ASSERT_EQUAL_HEX8(0x68, log[23]);  /* low nibble  0x6, En=0 */
}

/* ---------------------------------------------------------------------------
 * UT-LCD-002 — lcd_clear sends HD44780 command 0x01
 *
 * command 0x01 → hi nibble 0x0, lo nibble 0x1, RS=0, BL=1
 *   {0x0C, 0x08, 0x1C, 0x18}
 * --------------------------------------------------------------------------- */
void test_lcd_clear(void)
{
    lcd_backlight_on();
    lcd_clear();

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    TEST_ASSERT_EQUAL_HEX8(0x0C, log[0]);
    TEST_ASSERT_EQUAL_HEX8(0x08, log[1]);
    TEST_ASSERT_EQUAL_HEX8(0x1C, log[2]);
    TEST_ASSERT_EQUAL_HEX8(0x18, log[3]);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-003 — lcd_set_cursor(0, 0) → DDRAM address 0x80
 *
 * command 0x80 → hi 0x8, lo 0x0, RS=0, BL=1
 *   {0x8C, 0x88, 0x0C, 0x08}
 * --------------------------------------------------------------------------- */
void test_set_cursor_row0_col0(void)
{
    lcd_backlight_on();
    lcd_set_cursor(0, 0);

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    TEST_ASSERT_EQUAL_HEX8(0x8C, log[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88, log[1]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, log[2]);
    TEST_ASSERT_EQUAL_HEX8(0x08, log[3]);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-004 — lcd_set_cursor(1, 0) → DDRAM address 0xC0
 *
 * Row 1 base offset = 0x40; command = 0x80|0x40 = 0xC0
 * hi 0xC, lo 0x0, RS=0, BL=1
 *   {0xCC, 0xC8, 0x0C, 0x08}
 * --------------------------------------------------------------------------- */
void test_set_cursor_row1_col0(void)
{
    lcd_backlight_on();
    lcd_set_cursor(1, 0);

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    TEST_ASSERT_EQUAL_HEX8(0xCC, log[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC8, log[1]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, log[2]);
    TEST_ASSERT_EQUAL_HEX8(0x08, log[3]);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-005 — lcd_set_cursor(0, 5) → DDRAM address 0x85
 *
 * command = 0x80|0x05 = 0x85 → hi 0x8, lo 0x5, RS=0, BL=1
 *   {0x8C, 0x88, 0x5C, 0x58}
 * --------------------------------------------------------------------------- */
void test_set_cursor_row0_col5(void)
{
    lcd_backlight_on();
    lcd_set_cursor(0, 5);

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    TEST_ASSERT_EQUAL_HEX8(0x8C, log[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88, log[1]);
    TEST_ASSERT_EQUAL_HEX8(0x5C, log[2]);
    TEST_ASSERT_EQUAL_HEX8(0x58, log[3]);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-006 — lcd_print(0, 0, "Hi") sends 'H' then 'i' as data (RS=1)
 *
 * cursor(0,0) = command bytes RS=0; then 'H'=0x48, 'i'=0x69 with RS=1.
 *
 * 'H' (0x48): hi=4,RS=1 → {0x4D,0x49}; lo=8,RS=1 → {0x8D,0x89}
 * 'i' (0x69): hi=6,RS=1 → {0x6D,0x69}; lo=9,RS=1 → {0x9D,0x99}
 * --------------------------------------------------------------------------- */
void test_lcd_print_data_rs(void)
{
    lcd_backlight_on();
    lcd_print(0, 0, "Hi");

    uint8_t  log[16];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    /* 4 bytes cursor + 4 bytes 'H' + 4 bytes 'i' = 12 bytes total. */
    TEST_ASSERT_EQUAL_UINT16(12, len);

    /* Cursor bytes: RS=0 (bit 0 clear). */
    TEST_ASSERT_EQUAL_UINT8(0u, log[0] & 0x01u);
    TEST_ASSERT_EQUAL_UINT8(0u, log[3] & 0x01u);

    /* 'H' bytes [4..7]: RS=1. */
    TEST_ASSERT_EQUAL_HEX8(0x4D, log[4]);
    TEST_ASSERT_EQUAL_HEX8(0x49, log[5]);
    TEST_ASSERT_EQUAL_HEX8(0x8D, log[6]);
    TEST_ASSERT_EQUAL_HEX8(0x89, log[7]);

    /* 'i' bytes [8..11]: RS=1. */
    TEST_ASSERT_EQUAL_HEX8(0x6D, log[8]);
    TEST_ASSERT_EQUAL_HEX8(0x69, log[9]);
    TEST_ASSERT_EQUAL_HEX8(0x9D, log[10]);
    TEST_ASSERT_EQUAL_HEX8(0x99, log[11]);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-007 — lcd_backlight_on: bit 3 set in all subsequent bytes
 * --------------------------------------------------------------------------- */
void test_backlight_on(void)
{
    lcd_backlight_on();   /* sets flag; no bytes sent */
    lcd_clear();          /* 4 bytes, all with BL=1  */

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    for (uint16_t i = 0; i < len; i++) {
        TEST_ASSERT_NOT_EQUAL(0u, log[i] & 0x08u); /* BIT_BL must be set */
    }
}

/* ---------------------------------------------------------------------------
 * UT-LCD-008 — lcd_backlight_off: bit 3 cleared in all subsequent bytes
 * --------------------------------------------------------------------------- */
void test_backlight_off(void)
{
    lcd_backlight_off();  /* sets flag; no bytes sent */
    lcd_clear();          /* 4 bytes, all with BL=0  */

    uint8_t  log[8];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    TEST_ASSERT_EQUAL_UINT16(4, len);
    for (uint16_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, log[i] & 0x08u); /* BIT_BL must be clear */
    }
}

/* ---------------------------------------------------------------------------
 * UT-LCD-009 — lcd_write_row pads a 3-character string to 16 data bytes
 *
 * lcd_write_row(0, "Hi!") → cursor set (4 bytes) + 16 chars × 4 bytes = 68
 * Exactly 16 characters should be written as data (RS=1); count = 64 bytes.
 * --------------------------------------------------------------------------- */
void test_write_row_pads(void)
{
    lcd_backlight_on();
    lcd_write_row(0, "Hi!");

    uint8_t  log[80];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    /* 4 (cursor) + 16 chars × 4 bytes = 68 total. */
    TEST_ASSERT_EQUAL_UINT16(68, len);

    /* Count bytes with RS=1: should be exactly 16 chars × 4 bytes = 64. */
    uint16_t data_count = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (log[i] & 0x01u) data_count++;
    }
    TEST_ASSERT_EQUAL_UINT16(64, data_count);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-010 — lcd_write_row truncates a 20-character string to 16 bytes
 *
 * Only 16 characters must be written; no buffer overrun.
 * --------------------------------------------------------------------------- */
void test_write_row_truncates(void)
{
    lcd_backlight_on();
    lcd_write_row(0, "12345678901234567890"); /* 20 chars */

    uint8_t  log[80];
    uint16_t len = mock_lcd_get_transmitted_bytes(log, sizeof(log));

    /* Should be exactly the same length as the padding test: 68 bytes. */
    TEST_ASSERT_EQUAL_UINT16(68, len);

    /* Exactly 64 data bytes: no more than 16 characters written. */
    uint16_t data_count = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (log[i] & 0x01u) data_count++;
    }
    TEST_ASSERT_EQUAL_UINT16(64, data_count);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-011 — NACK on first write → LCD_ERR_NO_DEVICE
 * --------------------------------------------------------------------------- */
void test_lcd_init_nack(void)
{
    mock_nack_next = true;
    TEST_ASSERT_EQUAL(LCD_ERR_NO_DEVICE, lcd_init());
}

/* ---------------------------------------------------------------------------
 * main — Unity test runner
 * --------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_lcd_init_sequence);
    RUN_TEST(test_lcd_clear);
    RUN_TEST(test_set_cursor_row0_col0);
    RUN_TEST(test_set_cursor_row1_col0);
    RUN_TEST(test_set_cursor_row0_col5);
    RUN_TEST(test_lcd_print_data_rs);
    RUN_TEST(test_backlight_on);
    RUN_TEST(test_backlight_off);
    RUN_TEST(test_write_row_pads);
    RUN_TEST(test_write_row_truncates);
    RUN_TEST(test_lcd_init_nack);

    return UNITY_END();
}
