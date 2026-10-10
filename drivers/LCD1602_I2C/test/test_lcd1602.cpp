/**
 * LIB-4 LCD1602 I2C — unit tests (native build)
 *
 * Test IDs: UT-LCD-001 … UT-LCD-023
 *
 * lcd1602.cpp's ESP-IDF code runs unchanged (test/include_lib.cpp) against
 * simulated hardware (test/mock_i2c_bus.h): an I2C bus with bit timing, the
 * AiP31068L character controller at 0x3E, the PCA9633 backlight driver at
 * 0x60, and FreeRTOS delays on a simulated clock.
 *
 * AiP31068L write protocol: every transaction the driver sends is two bytes,
 *   0x00, instruction    (control byte Co=0, RS=0)
 *   0x40, character      (control byte Co=0, RS=1)
 *
 * UT-LCD-001…017 check what goes on the bus and what the chips end up
 * holding. UT-LCD-018…023 check that the driver leaves the controller the
 * execution time its datasheet (Table 3) lists before sending the next byte.
 * The serial interface cannot read the busy flag, so a byte sent too early
 * is lost.
 *
 * Run with:  pio test -e native
 */

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "../src/lcd1602.h"
#include "mock_i2c_bus.h"

/* lcd_init()'s instruction sequence, as lcd1602.cpp documents it. */
static const uint8_t k_init_instructions[] = {
    0x38,                    /* function set: 8-bit bus, 2 lines          */
    0x39,                    /* function set, IS=1                        */
    0x14, 0x70, 0x56, 0x6C,  /* OSC, contrast, power/icon, follower       */
    0x38,                    /* function set, IS=0                        */
    0x0C,                    /* display on, cursor off, blink off         */
    0x01,                    /* clear display                             */
    0x06                     /* entry mode: increment, no shift           */
};

static const uint8_t k_glyph_a[8] = { 0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0x00 };
static const uint8_t k_glyph_b[8] = { 0x04, 0x08, 0x1F, 0x08, 0x04, 0x00, 0x00, 0x00 };

static const char *const k_blank_row = "                ";

/* Power-cycle the module: both chips present, nothing initialised. */
static void power_cycle(void)
{
    mock_lcd_reset();
}

void setUp(void)
{
    power_cycle();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_init());
    mock_advance_ms(10);        /* real time passes before the firmware's next call */
    mock_lcd_clear_logs();
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static void expect_xfer(int i, uint8_t addr, const uint8_t *bytes, uint8_t len)
{
    char msg[40];
    snprintf(msg, sizeof(msg), "bus transaction #%d", i);
    const mock_xfer_t *x = mock_xfer(i);
    TEST_ASSERT_NOT_NULL_MESSAGE(x, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(addr, x->addr, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(len, x->len, msg);
    if (len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(bytes, x->data, len, msg);
    }
}

static void expect_probe(int i, uint8_t addr)
{
    expect_xfer(i, addr, NULL, 0);
}

static void expect_instruction(int i, uint8_t instruction)
{
    const uint8_t b[2] = { 0x00, instruction };
    expect_xfer(i, LCD_I2C_ADDR, b, 2);
}

static void expect_char(int i, uint8_t c)
{
    const uint8_t b[2] = { 0x40, c };
    expect_xfer(i, LCD_I2C_ADDR, b, 2);
}

/* The 16 visible characters of a row, as the glass shows them. */
static const char *row_text(uint8_t row)
{
    static char text[LCD_COLS + 1];
    for (uint8_t c = 0; c < LCD_COLS; c++) {
        text[c] = mock_aip_char_at(row, c);
    }
    text[LCD_COLS] = '\0';
    return text;
}

static const char *busy_name(uint16_t what)
{
    if (what == MOCK_AIP_BUSY_POWER_ON)  return "power-on";
    if (what == 0x01)                    return "Clear Display (0x01)";
    if (what == 0x02 || what == 0x03)    return "Return Home (0x02)";
    return (what & 0x100) ? "a data write" : "an instruction";
}

/* Nothing reached the AiP31068L while it was still executing. */
static void assert_no_lost_bytes(void)
{
    if (mock_aip_violation_count() == 0) {
        return;
    }
    const mock_aip_violation_t *v = mock_aip_violation(0);
    const bool power_on = (v->busy_with == MOCK_AIP_BUSY_POWER_ON);
    char msg[220];
    snprintf(msg, sizeof(msg),
             "%d byte(s) lost; first: %s 0x%02X sent %.3f ms after %s, "
             "which takes %.3f ms (%s)",
             mock_aip_violation_count(),
             v->is_data ? "data" : "instruction", v->byte,
             (double)(v->t_ns - v->busy_since_ns) / 1e6,
             busy_name(v->busy_with),
             (double)(v->busy_until_ns - v->busy_since_ns) / 1e6,
             power_on ? "power-on reset, per lcd1602.cpp"
                      : "AiP31068L datasheet Table 3");
    TEST_FAIL_MESSAGE(msg);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-001 — lcd_init sends the AiP31068L and PCA9633 init sequences
 * --------------------------------------------------------------------------- */
void test_lcd_init_sequence(void)
{
    power_cycle();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_init());

    int i = 0;
    expect_probe(i++, LCD_I2C_ADDR);
    for (size_t k = 0; k < sizeof(k_init_instructions); k++) {
        expect_instruction(i++, k_init_instructions[k]);
    }

    const uint8_t mode1[]  = { LCD_RGB_REG_MODE1,  0x00 };   /* wake the oscillator */
    const uint8_t mode2[]  = { LCD_RGB_REG_MODE2,  0x05 };
    const uint8_t grppwm[] = { LCD_RGB_REG_GRPPWM, 0xFF };
    const uint8_t ledout[] = { LCD_RGB_REG_LEDOUT, 0xFF };
    const uint8_t blue[]   = { LCD_RGB_AI_BIT | LCD_RGB_REG_PWM0, 0xFF, 0x00, 0x00, 0x00 };
    expect_probe(i++, LCD_RGB_I2C_ADDR);
    expect_xfer(i++, LCD_RGB_I2C_ADDR, mode1,  sizeof(mode1));
    expect_xfer(i++, LCD_RGB_I2C_ADDR, mode2,  sizeof(mode2));
    expect_xfer(i++, LCD_RGB_I2C_ADDR, grppwm, sizeof(grppwm));
    expect_xfer(i++, LCD_RGB_I2C_ADDR, ledout, sizeof(ledout));
    expect_xfer(i++, LCD_RGB_I2C_ADDR, blue,   sizeof(blue));
    TEST_ASSERT_EQUAL_INT(i, mock_xfer_count());

    /* What the two chips were left holding. */
    TEST_ASSERT_TRUE(mock_aip_display_on());
    TEST_ASSERT_FALSE(mock_aip_cursor_on());
    TEST_ASSERT_FALSE(mock_aip_blink_on());
    TEST_ASSERT_TRUE(mock_aip_two_line());
    TEST_ASSERT_TRUE(mock_aip_increment());
    TEST_ASSERT_FALSE(mock_aip_shift());
    TEST_ASSERT_EQUAL_STRING(k_blank_row, row_text(0));
    TEST_ASSERT_EQUAL_STRING(k_blank_row, row_text(1));
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_pca_reg(LCD_RGB_REG_MODE1) & 0x10);   /* SLEEP clear */
    TEST_ASSERT_EQUAL_HEX8(0xFF, mock_pca_reg(LCD_RGB_REG_PWM0));           /* blue on */
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_pca_reg(LCD_RGB_REG_PWM1));
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_pca_reg(LCD_RGB_REG_PWM2));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mock_pca_reg(LCD_RGB_REG_GRPPWM));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mock_pca_reg(LCD_RGB_REG_LEDOUT));
    TEST_ASSERT_EQUAL_INT(0, mock_pca_unmodelled_writes);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-002 — lcd_clear sends Clear Display (0x01) and blanks the screen
 * --------------------------------------------------------------------------- */
void test_lcd_clear(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(0, "old text"));
    mock_advance_ms(10);
    mock_lcd_clear_logs();

    TEST_ASSERT_EQUAL(LCD_OK, lcd_clear());

    expect_instruction(0, 0x01);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_EQUAL_STRING(k_blank_row, row_text(0));
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_aip_ac());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-003 — lcd_set_cursor(0, 0) → Set DDRAM address 0x00 (0x80)
 * --------------------------------------------------------------------------- */
void test_set_cursor_row0_col0(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_cursor(0, 0));

    expect_instruction(0, 0x80);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_FALSE(mock_aip_ac_in_cgram());
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_aip_ac());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-004 — lcd_set_cursor(1, 0) → 0xC0 (row 1 starts at DDRAM 0x40)
 * --------------------------------------------------------------------------- */
void test_set_cursor_row1_col0(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_cursor(1, 0));

    expect_instruction(0, 0xC0);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_EQUAL_HEX8(0x40, mock_aip_ac());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-005 — lcd_set_cursor(0, 5) → 0x85
 * --------------------------------------------------------------------------- */
void test_set_cursor_row0_col5(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_cursor(0, 5));

    expect_instruction(0, 0x85);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_EQUAL_HEX8(0x05, mock_aip_ac());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-006 — lcd_print(0, 0, "Hi") positions, then sends 'H', 'i' as data
 * --------------------------------------------------------------------------- */
void test_lcd_print_data_rs(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_print(0, 0, "Hi"));

    expect_instruction(0, 0x80);
    expect_char(1, 'H');
    expect_char(2, 'i');
    TEST_ASSERT_EQUAL_INT(3, mock_xfer_count());
    TEST_ASSERT_EQUAL_STRING("Hi              ", row_text(0));
    assert_no_lost_bytes();
}

/* ---------------------------------------------------------------------------
 * UT-LCD-007 — lcd_backlight_color: one auto-increment burst from PWM0
 *
 * Waveshare LCD1602RGB wiring: LED0 = blue, LED1 = green, LED2 = red, so
 * the burst carries (b, g, r).
 * --------------------------------------------------------------------------- */
void test_backlight_color(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_backlight_color(0x11, 0x22, 0x33));

    const uint8_t burst[] = { LCD_RGB_AI_BIT | LCD_RGB_REG_PWM0, 0x33, 0x22, 0x11 };
    expect_xfer(0, LCD_RGB_I2C_ADDR, burst, sizeof(burst));
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_EQUAL_HEX8(0x33, mock_pca_reg(LCD_RGB_REG_PWM0));   /* blue  */
    TEST_ASSERT_EQUAL_HEX8(0x22, mock_pca_reg(LCD_RGB_REG_PWM1));   /* green */
    TEST_ASSERT_EQUAL_HEX8(0x11, mock_pca_reg(LCD_RGB_REG_PWM2));   /* red   */
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_pca_reg(LCD_RGB_REG_PWM3));   /* untouched */
    TEST_ASSERT_EQUAL_INT(0, mock_pca_unmodelled_writes);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-008 — lcd_backlight_lumination sets the group brightness only
 * --------------------------------------------------------------------------- */
void test_backlight_lumination(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_backlight_lumination(0x40));

    const uint8_t grppwm[] = { LCD_RGB_REG_GRPPWM, 0x40 };
    expect_xfer(0, LCD_RGB_I2C_ADDR, grppwm, sizeof(grppwm));
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_EQUAL_HEX8(0x40, mock_pca_reg(LCD_RGB_REG_GRPPWM));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mock_pca_reg(LCD_RGB_REG_PWM0));   /* colour kept */
}

/* ---------------------------------------------------------------------------
 * UT-LCD-009 — lcd_write_row pads a 3-character string to 16
 * --------------------------------------------------------------------------- */
void test_write_row_pads(void)
{
    const char *expected = "Hi!             ";

    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(0, "Hi!"));

    expect_instruction(0, 0x80);
    for (int k = 0; k < LCD_COLS; k++) {
        expect_char(1 + k, (uint8_t)expected[k]);
    }
    TEST_ASSERT_EQUAL_INT(1 + LCD_COLS, mock_xfer_count());
    TEST_ASSERT_EQUAL_STRING(expected, row_text(0));
    assert_no_lost_bytes();
}

/* ---------------------------------------------------------------------------
 * UT-LCD-010 — lcd_write_row truncates a 20-character string to 16
 * --------------------------------------------------------------------------- */
void test_write_row_truncates(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(0, "12345678901234567890"));

    TEST_ASSERT_EQUAL_INT(1 + LCD_COLS, mock_xfer_count());
    TEST_ASSERT_EQUAL_STRING("1234567890123456", row_text(0));
    /* Nothing written past column 15: the rest of line 1's DDRAM and all of
     * row 1 are still blank. */
    for (uint8_t a = LCD_COLS; a <= 0x27; a++) {
        TEST_ASSERT_EQUAL_HEX8(0x20, mock_aip_ddram(a));
    }
    TEST_ASSERT_EQUAL_STRING(k_blank_row, row_text(1));
    assert_no_lost_bytes();
}

/* ---------------------------------------------------------------------------
 * UT-LCD-011 — no AiP31068L → LCD_ERR_NO_DEVICE; only the probe went out
 * --------------------------------------------------------------------------- */
void test_lcd_init_nack(void)
{
    power_cycle();
    mock_lcd_set_present(LCD_I2C_ADDR, false);

    TEST_ASSERT_EQUAL(LCD_ERR_NO_DEVICE, lcd_init());

    expect_probe(0, LCD_I2C_ADDR);
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, mock_xfer(0)->result);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-012 — legacy LCD1602 without the PCA9633: init succeeds and the
 *              backlight calls are silent no-ops
 * --------------------------------------------------------------------------- */
void test_lcd_init_without_rgb_backlight(void)
{
    power_cycle();
    mock_lcd_set_present(LCD_RGB_I2C_ADDR, false);

    TEST_ASSERT_EQUAL(LCD_OK, lcd_init());
    const int last = mock_xfer_count() - 1;
    expect_probe(last, LCD_RGB_I2C_ADDR);              /* probed, NACKed, left alone */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, mock_xfer(last)->result);

    mock_lcd_clear_logs();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_backlight_color(0xFF, 0x00, 0x00));
    TEST_ASSERT_EQUAL(LCD_OK, lcd_backlight_lumination(0x10));
    TEST_ASSERT_EQUAL_INT(0, mock_xfer_count());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-013 — lcd_create_char stores the glyph and points back at DDRAM
 * --------------------------------------------------------------------------- */
void test_create_char(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_create_char(3, k_glyph_a));

    expect_instruction(0, 0x58);                      /* Set CGRAM address: slot 3 */
    for (int k = 0; k < 8; k++) {
        expect_char(1 + k, k_glyph_a[k]);
    }
    expect_instruction(9, 0x02);                      /* Return Home */
    TEST_ASSERT_EQUAL_INT(10, mock_xfer_count());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_glyph_a, mock_aip_cgram(3), 8);
    TEST_ASSERT_FALSE(mock_aip_ac_in_cgram());
    assert_no_lost_bytes();

    /* There is no slot 8: refused before any bus traffic. */
    mock_lcd_clear_logs();
    TEST_ASSERT_EQUAL(LCD_ERR_COMM, lcd_create_char(8, k_glyph_a));
    TEST_ASSERT_EQUAL_INT(0, mock_xfer_count());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-014 — lcd_set_contrast sends its documented sequence, clamped at 63
 *
 * This pins the bytes lcd1602.cpp documents (an ST7032-style extended
 * instruction set), not their effect: the AiP31068L datasheet in
 * documentation/ lists no extended set, and by its Table 3 the middle two
 * bytes are Set CGRAM address instructions.
 * --------------------------------------------------------------------------- */
void test_set_contrast(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_contrast(45));   /* 0b10'1101 */
    expect_instruction(0, 0x39);
    expect_instruction(1, 0x7D);                        /* C3..C0 = 1101 */
    expect_instruction(2, 0x56);                        /* Bon = 1, C5..C4 = 10 */
    expect_instruction(3, 0x38);
    TEST_ASSERT_EQUAL_INT(4, mock_xfer_count());

    mock_lcd_clear_logs();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_contrast(200));  /* clamped to 63 */
    expect_instruction(1, 0x7F);
    expect_instruction(2, 0x57);
    TEST_ASSERT_EQUAL_INT(4, mock_xfer_count());
    assert_no_lost_bytes();
}

/* ---------------------------------------------------------------------------
 * UT-LCD-015 — lcd_display_on: display on, cursor and blink off
 * --------------------------------------------------------------------------- */
void test_display_on(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_display_on());

    expect_instruction(0, 0x0C);
    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());
    TEST_ASSERT_TRUE(mock_aip_display_on());
    TEST_ASSERT_FALSE(mock_aip_cursor_on());
    TEST_ASSERT_FALSE(mock_aip_blink_on());
}

/* ---------------------------------------------------------------------------
 * UT-LCD-016 — lcd_write_row(row, NULL) blanks the row
 * --------------------------------------------------------------------------- */
void test_write_row_null_blanks(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(1, "something"));
    mock_advance_ms(10);
    mock_lcd_clear_logs();

    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(1, NULL));

    expect_instruction(0, 0xC0);
    TEST_ASSERT_EQUAL_INT(1 + LCD_COLS, mock_xfer_count());
    TEST_ASSERT_EQUAL_STRING(k_blank_row, row_text(1));
}

/* ---------------------------------------------------------------------------
 * UT-LCD-017 — a controller that stops answering ends the write at once
 * --------------------------------------------------------------------------- */
void test_write_row_stops_on_nack(void)
{
    mock_lcd_set_present(LCD_I2C_ADDR, false);

    TEST_ASSERT_EQUAL(LCD_ERR_NO_DEVICE, lcd_write_row(0, "Hello"));

    TEST_ASSERT_EQUAL_INT(1, mock_xfer_count());      /* nothing after the failure */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, mock_xfer(0)->result);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-018 — lcd_init waits out the controller's power-on reset
 * --------------------------------------------------------------------------- */
void test_timing_init_after_power_on(void)
{
    power_cycle();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_init());

    for (int k = 0; k < mock_aip_violation_count(); k++) {
        TEST_ASSERT_TRUE_MESSAGE(mock_aip_violation(k)->busy_with != MOCK_AIP_BUSY_POWER_ON,
                                 "an instruction arrived during the power-on reset");
    }
    TEST_ASSERT_TRUE(mock_aip_first_instruction_ns() >= MOCK_AIP_T_POWER_ON_NS);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-019 — lcd_init gives Clear Display its 1.53 ms before Entry Mode Set
 *
 * The datasheet's own init flowchart (4.5): "Wait for more than 1.53 ms".
 * --------------------------------------------------------------------------- */
void test_timing_init_waits_after_clear(void)
{
    power_cycle();
    TEST_ASSERT_EQUAL(LCD_OK, lcd_init());

    assert_no_lost_bytes();
}

/* ---------------------------------------------------------------------------
 * UT-LCD-020 — lcd_clear returns only once Clear Display has executed
 *
 * The caller cannot know it has to wait; main.cpp's boot greeting prints
 * straight after lcd_clear().
 * --------------------------------------------------------------------------- */
void test_timing_clear_then_print(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_clear());
    TEST_ASSERT_EQUAL(LCD_OK, lcd_print(1, 0, "OK"));

    assert_no_lost_bytes();
    TEST_ASSERT_EQUAL_STRING("OK              ", row_text(1));
}

/* ---------------------------------------------------------------------------
 * UT-LCD-021 — back-to-back lcd_create_char calls both land
 *
 * ui_display.cpp (T8) defines its CGRAM glyphs this way at boot. Each call
 * ends with Return Home, a 1.53 ms instruction.
 * --------------------------------------------------------------------------- */
void test_timing_create_char_twice(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_create_char(1, k_glyph_a));
    TEST_ASSERT_EQUAL(LCD_OK, lcd_create_char(2, k_glyph_b));

    assert_no_lost_bytes();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_glyph_a, mock_aip_cgram(1), 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_glyph_b, mock_aip_cgram(2), 8);
}

/* ---------------------------------------------------------------------------
 * UT-LCD-022 — lcd_home returns only once Return Home has executed
 * --------------------------------------------------------------------------- */
void test_timing_home_then_print(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_home());
    TEST_ASSERT_EQUAL(LCD_OK, lcd_print_char(1, 0, '>'));

    assert_no_lost_bytes();
    TEST_ASSERT_EQUAL('>', mock_aip_char_at(1, 0));
}

/* ---------------------------------------------------------------------------
 * UT-LCD-023 — ordinary calls leave every instruction its 39 µs and every
 *              character its 43 µs
 * --------------------------------------------------------------------------- */
void test_timing_fast_writes(void)
{
    TEST_ASSERT_EQUAL(LCD_OK, lcd_display_on());
    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(0, "0123456789ABCDEF"));
    TEST_ASSERT_EQUAL(LCD_OK, lcd_write_row(1, "fedcba9876543210"));
    TEST_ASSERT_EQUAL(LCD_OK, lcd_set_contrast(32));

    assert_no_lost_bytes();
    TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", row_text(0));
    TEST_ASSERT_EQUAL_STRING("fedcba9876543210", row_text(1));
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
    RUN_TEST(test_backlight_color);
    RUN_TEST(test_backlight_lumination);
    RUN_TEST(test_write_row_pads);
    RUN_TEST(test_write_row_truncates);
    RUN_TEST(test_lcd_init_nack);
    RUN_TEST(test_lcd_init_without_rgb_backlight);
    RUN_TEST(test_create_char);
    RUN_TEST(test_set_contrast);
    RUN_TEST(test_display_on);
    RUN_TEST(test_write_row_null_blanks);
    RUN_TEST(test_write_row_stops_on_nack);

    RUN_TEST(test_timing_init_after_power_on);
    RUN_TEST(test_timing_init_waits_after_clear);
    RUN_TEST(test_timing_clear_then_print);
    RUN_TEST(test_timing_create_char_twice);
    RUN_TEST(test_timing_home_then_print);
    RUN_TEST(test_timing_fast_writes);

    return UNITY_END();
}
