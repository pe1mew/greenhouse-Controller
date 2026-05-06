#ifndef UNIT_TEST
  #include <Arduino.h>
  #include "i2c_bus.h"
#else
  #include "../test/mock_i2c_bus.h"
#endif

#include "lcd1602.h"

/* ---------------------------------------------------------------------------
 * AiP31068L I2C control bytes
 *
 * The Waveshare LCD1602 module uses the AiP31068L, a dedicated I2C-to-HD44780
 * bridge.  Each write transaction is exactly two bytes:
 *   byte 0: control byte — 0x00 for a command (RS=0), 0x40 for data (RS=1)
 *   byte 1: the HD44780 command or character byte
 * There is no nibble-splitting or EN pulsing; the chip handles the parallel
 * HD44780 interface internally using an 8-bit bus.
 * --------------------------------------------------------------------------- */
#define AIP_CTRL_CMD   0x00u  /**< Control byte: command follows (Co=0, RS=0). */
#define AIP_CTRL_DATA  0x40u  /**< Control byte: data follows   (Co=0, RS=1). */

/* ---------------------------------------------------------------------------
 * HD44780 command bytes
 * --------------------------------------------------------------------------- */
#define CMD_CLEAR       0x01u  /**< Clear display. */
#define CMD_HOME        0x02u  /**< Return home (cursor to 0,0; no clear). */
#define CMD_ENTRY_MODE  0x06u  /**< Entry mode: increment cursor, no shift. */
#define CMD_DISP_OFF    0x08u  /**< Display off. */
#define CMD_DISP_ON     0x0Cu  /**< Display on, cursor off, blink off. */
#define CMD_FUNC_SET    0x38u  /**< Function set: 8-bit, 2 lines, IS=0. */
#define CMD_FUNC_SET_EX 0x39u  /**< Function set: 8-bit, 2 lines, IS=1 (extension). */

/** @brief Row 1 DDRAM base address offset. */
#define ROW1_OFFSET  0x40u

/* ---------------------------------------------------------------------------
 * Portable millisecond delay (compiled away in the native/test build)
 * --------------------------------------------------------------------------- */
static inline void lcd_delay_ms(uint8_t ms)
{
#ifndef UNIT_TEST
    delay(ms);
#else
    (void)ms;
#endif
}

/* ---------------------------------------------------------------------------
 * Internal: map i2c_status_t → lcd_status_t
 * --------------------------------------------------------------------------- */
static lcd_status_t map_i2c(i2c_status_t st)
{
    switch (st) {
        case I2C_OK:       return LCD_OK;
        case I2C_ERR_NACK: return LCD_ERR_NO_DEVICE;
        default:           return LCD_ERR_COMM;
    }
}

/* ---------------------------------------------------------------------------
 * Internal: send one HD44780 command byte via AiP31068L.
 * --------------------------------------------------------------------------- */
static lcd_status_t aip_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { AIP_CTRL_CMD, cmd };
    return map_i2c(i2c_write(LCD_I2C_ADDR, buf, 2));
}

/* ---------------------------------------------------------------------------
 * Internal: send one character data byte via AiP31068L.
 * --------------------------------------------------------------------------- */
static lcd_status_t aip_data(uint8_t data)
{
    uint8_t buf[2] = { AIP_CTRL_DATA, data };
    return map_i2c(i2c_write(LCD_I2C_ADDR, buf, 2));
}

/* ---------------------------------------------------------------------------
 * API implementation
 * --------------------------------------------------------------------------- */

lcd_status_t lcd_init(void)
{
    /* Probe: confirm the AiP31068L is present before issuing any init bytes. */
    i2c_status_t probe = i2c_write(LCD_I2C_ADDR, NULL, 0);
    if (probe == I2C_ERR_NACK) return LCD_ERR_NO_DEVICE;
    if (probe != I2C_OK)       return LCD_ERR_COMM;

    /* AiP31068L requires ≥40 ms after VCC rises before accepting commands. */
    lcd_delay_ms(50);

    lcd_status_t r;

    /*
     * AiP31068L initialisation sequence (from datasheet):
     *  1. Function Set (IS=0): 8-bit bus, 2 lines.
     *  2. Function Set (IS=1): enter extension instruction set.
     *  3. Internal OSC frequency / bias select.
     *  4-6. Contrast, power/icon control, follower — allow 200 ms to settle.
     *  7. Function Set back to IS=0.
     *  8. Display ON.
     *  9. Clear display (busy time ≥1.52 ms).
     * 10. Entry mode.
     */
    r = aip_cmd(CMD_FUNC_SET);     if (r != LCD_OK) return r;  /* 8-bit, 2-line, IS=0 */
    lcd_delay_ms(1);
    r = aip_cmd(CMD_FUNC_SET_EX);  if (r != LCD_OK) return r;  /* extension IS=1 */
    r = aip_cmd(0x14u);            if (r != LCD_OK) return r;  /* internal OSC frequency */
    r = aip_cmd(0x70u);            if (r != LCD_OK) return r;  /* contrast low nibble */
    r = aip_cmd(0x56u);            if (r != LCD_OK) return r;  /* power/icon/contrast booster */
    r = aip_cmd(0x6Cu);            if (r != LCD_OK) return r;  /* follower control on */
    lcd_delay_ms(200);                                          /* follower amp stabilisation */
    r = aip_cmd(CMD_FUNC_SET);     if (r != LCD_OK) return r;  /* back to IS=0 */
    r = aip_cmd(CMD_DISP_ON);      if (r != LCD_OK) return r;  /* display on, cursor off */
    r = aip_cmd(CMD_CLEAR);        if (r != LCD_OK) return r;  /* clear display */
    lcd_delay_ms(2);                                            /* clear busy time ≥1.52 ms */
    r = aip_cmd(CMD_ENTRY_MODE);   if (r != LCD_OK) return r;  /* cursor increment, no shift */

    return LCD_OK;
}

lcd_status_t lcd_clear(void)
{
    lcd_status_t r = aip_cmd(CMD_CLEAR);
    lcd_delay_ms(2);  /* CMD_CLEAR busy time ≥1.52 ms */
    return r;
}

lcd_status_t lcd_home(void)
{
    return aip_cmd(CMD_HOME);
}

lcd_status_t lcd_set_cursor(uint8_t row, uint8_t col)
{
    uint8_t addr = (row == 0u) ? col : (uint8_t)(ROW1_OFFSET + col);
    return aip_cmd((uint8_t)(0x80u | addr));
}

lcd_status_t lcd_print(uint8_t row, uint8_t col, const char *str)
{
    lcd_status_t r = lcd_set_cursor(row, col);
    if (r != LCD_OK || str == NULL) return r;
    while (*str != '\0') {
        r = aip_data((uint8_t)*str);
        if (r != LCD_OK) return r;
        str++;
    }
    return LCD_OK;
}

lcd_status_t lcd_print_char(uint8_t row, uint8_t col, char c)
{
    lcd_status_t r = lcd_set_cursor(row, col);
    if (r != LCD_OK) return r;
    return aip_data((uint8_t)c);
}

lcd_status_t lcd_write_row(uint8_t row, const char *text)
{
    lcd_status_t r = lcd_set_cursor(row, 0);
    if (r != LCD_OK) return r;

    /* Determine printable length: at most LCD_COLS characters. */
    uint8_t text_len = 0;
    if (text != NULL) {
        while (text_len < LCD_COLS && text[text_len] != '\0') {
            text_len++;
        }
    }

    for (uint8_t i = 0; i < LCD_COLS; i++) {
        char c = (i < text_len) ? text[i] : ' ';
        r = aip_data((uint8_t)c);
        if (r != LCD_OK) return r;
    }
    return LCD_OK;
}

lcd_status_t lcd_create_char(uint8_t slot, const uint8_t pattern[8])
{
    if (slot > 7u) return LCD_ERR_COMM;

    /* Set CGRAM address: 0x40 | (slot << 3) selects the first of the 8 rows
     * in the chosen slot.  After this command HD44780 auto-increments the
     * CGRAM address on every data write. */
    lcd_status_t r = aip_cmd((uint8_t)(0x40u | ((slot & 7u) << 3)));
    if (r != LCD_OK) return r;

    /* Write the 8 pixel rows (only the 5 LSBs of each byte are used). */
    for (uint8_t i = 0; i < 8u; i++) {
        r = aip_data(pattern[i]);
        if (r != LCD_OK) return r;
    }

    /* Return to DDRAM address 0 so subsequent writes go to the visible display. */
    return aip_cmd(CMD_HOME);
}

lcd_status_t lcd_display_on(void)
{
    /* CMD_DISP_ON (0x0Cu): display on, cursor off, blink off.
     * Busy time: ~37 µs — safely covered by I2C transaction overhead (~70 µs).
     * Idempotent: no visible side-effect when the display is already on.
     * Use as a sacrificial preamble write before row data when the I2C bus
     * has been idle for ~2.5 s — the AiP31068L silently drops the first
     * transaction after prolonged inactivity; this absorbs that loss. */
    return aip_cmd(CMD_DISP_ON);
}

lcd_status_t lcd_backlight_on(void)
{
    /* Backlight is not software-controlled on this module (AiP31068L). */
    return LCD_OK;
}

lcd_status_t lcd_backlight_off(void)
{
    /* Backlight is not software-controlled on this module (AiP31068L). */
    return LCD_OK;
}
