#ifndef UNIT_TEST
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
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
 * Portable millisecond delay (compiled away in the native/test build).
 * ESP-IDF migration (2.0.0-alpha.2.5): Arduino's delay() → vTaskDelay()
 * with pdMS_TO_TICKS conversion. Functionally identical for the LCD's use
 * — millisecond-scale waits between HD44780 power-on init steps.
 * --------------------------------------------------------------------------- */
static inline void lcd_delay_ms(uint8_t ms)
{
#ifndef UNIT_TEST
    vTaskDelay(pdMS_TO_TICKS(ms));
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
 * PCA9633DP2 RGB backlight — present on LCD1602RGB, absent on legacy LCD1602.
 *
 * `s_rgb_present` is set by lcd_init() after probing 0x60.  All public
 * lcd_backlight_*() functions early-return LCD_OK when this flag is false so
 * legacy hardware keeps working without producing bus errors.
 * --------------------------------------------------------------------------- */
static bool s_rgb_present = false;

/**
 * @brief Initialise the PCA9633: wake from sleep, configure dimming, set boot
 *        default to BLUE at full brightness.
 *
 * Returns LCD_OK in two cases:
 *   - PCA9633 not present (legacy LCD1602 module) — `s_rgb_present` stays false
 *   - PCA9633 present and successfully initialised — `s_rgb_present` set true
 *
 * Returns LCD_ERR_COMM if the device acks the probe but a subsequent register
 * write fails (genuine bus error mid-init).
 */
static lcd_status_t pca9633_init(void)
{
    /* Probe: zero-length write is acked by the chip if its address bytes
     * are recognised. NACK = device absent (legacy module). */
    i2c_status_t probe = i2c_write(LCD_RGB_I2C_ADDR, NULL, 0);
    if (probe == I2C_ERR_NACK) {
        s_rgb_present = false;
        return LCD_OK;
    }
    if (probe != I2C_OK) return LCD_ERR_COMM;

    /* Wake from sleep — chip powers up with MODE1.SLEEP set; clearing it
     * starts the internal oscillator. */
    {
        uint8_t buf[2] = { LCD_RGB_REG_MODE1, 0x00u };
        if (map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, 2)) != LCD_OK) return LCD_ERR_COMM;
    }
    /* MODE2: outputs change on STOP, group control = dimming (not blink),
     * totem-pole drivers, no inversion. */
    {
        uint8_t buf[2] = { LCD_RGB_REG_MODE2, 0x05u };
        if (map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, 2)) != LCD_OK) return LCD_ERR_COMM;
    }
    /* GRPPWM = 0xFF — full master brightness; per-channel PWM applied directly. */
    {
        uint8_t buf[2] = { LCD_RGB_REG_GRPPWM, 0xFFu };
        if (map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, 2)) != LCD_OK) return LCD_ERR_COMM;
    }
    /* LEDOUT = 0xFF — all four channels in mode 0b11 (individual PWM × group
     * dimming) so per-channel intensity and master brightness both take effect. */
    {
        uint8_t buf[2] = { LCD_RGB_REG_LEDOUT, 0xFFu };
        if (map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, 2)) != LCD_OK) return LCD_ERR_COMM;
    }
    /* Boot default: BLUE at full intensity (calm "OK" colour).
     * On this Waveshare LCD1602RGB PCB the channels are wired
     *   PWM0 = BLUE, PWM1 = GREEN, PWM2 = RED.
     * Boot blue = (PWM0=255, PWM1=0, PWM2=0, PWM3=0). The application layer
     * (T8) re-asserts the colour from EG1 status on the first tick anyway;
     * this just gives a sensible default for the gap between lcd_init() and
     * the first ui_display update. */
    {
        uint8_t buf[5] = {
            (uint8_t)(LCD_RGB_AI_BIT | LCD_RGB_REG_PWM0),
            0xFFu,  /* PWM0 = BLUE  → 255 */
            0x00u,  /* PWM1 = GREEN → 0   */
            0x00u,  /* PWM2 = RED   → 0   */
            0x00u   /* PWM3 unused        */
        };
        if (map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, sizeof buf)) != LCD_OK) return LCD_ERR_COMM;
    }

    s_rgb_present = true;
    return LCD_OK;
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

    /* Probe and initialise the PCA9633DP2 RGB backlight if this is an
     * LCD1602RGB module. On the legacy mono LCD1602 (no PCA9633) this returns
     * LCD_OK with s_rgb_present=false and the rest of the driver is unaffected. */
    lcd_status_t rgb = pca9633_init();
    if (rgb != LCD_OK) return rgb;

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

lcd_status_t lcd_backlight_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rgb_present) return LCD_OK;
    /* Auto-increment burst from PWM0..PWM2 in one I2C transaction.
     * On this Waveshare LCD1602RGB PCB the channels are wired LED0=BLUE,
     * LED1=GREEN, LED2=RED (R/B swapped vs Grove), so we remap here to keep
     * the public (r, g, b) API natural for callers. Channel 3 is unused and
     * was seeded to 0 by pca9633_init(); we don't touch it on every write. */
    uint8_t buf[4] = {
        (uint8_t)(LCD_RGB_AI_BIT | LCD_RGB_REG_PWM0),
        b,   /* PWM0 → BLUE  */
        g,   /* PWM1 → GREEN */
        r    /* PWM2 → RED   */
    };
    return map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, sizeof buf));
}

lcd_status_t lcd_backlight_lumination(uint8_t level)
{
    if (!s_rgb_present) return LCD_OK;
    uint8_t buf[2] = { LCD_RGB_REG_GRPPWM, level };
    return map_i2c(i2c_write(LCD_RGB_I2C_ADDR, buf, 2));
}

/* ---------------------------------------------------------------------------
 * lcd_set_contrast — runtime LCD contrast control (since 1.17.33).
 *
 * AiP31068L extended-instruction-set sequence; mirrors what lcd_init() does
 * once at boot, but parameterised. Bit layout:
 *
 *   Contrast register : C5 C4 C3 C2 C1 C0  (6 bits, range 0..63)
 *
 *   Low-nibble opcode  : 0111 C3 C2 C1 C0   →   0x70 | (value & 0x0F)
 *   High-bits opcode   : 0101 Ion Bon C5 C4 →   0x50 | (Ion<<3) | (Bon<<2)
 *                                              | ((value >> 4) & 0x03)
 *
 * Ion=0 (icon display off — this is a character LCD, no icon row),
 * Bon=1 (booster on — required by the AiP31068L for the contrast register
 * to take effect at all; matches the 0x56 sent in lcd_init()).
 * --------------------------------------------------------------------------- */
lcd_status_t lcd_set_contrast(uint8_t value)
{
    if (value > 63u) value = 63u;

    lcd_status_t r;
    r = aip_cmd(CMD_FUNC_SET_EX);                                       /* IS=1 */
    if (r != LCD_OK) return r;
    r = aip_cmd((uint8_t)(0x70u | (value & 0x0Fu)));                    /* C3..C0 */
    if (r != LCD_OK) return r;
    r = aip_cmd((uint8_t)(0x54u | ((value >> 4) & 0x03u)));             /* Bon=1, C5..C4 */
    if (r != LCD_OK) return r;
    r = aip_cmd(CMD_FUNC_SET);                                          /* back to IS=0 */
    return r;
}
