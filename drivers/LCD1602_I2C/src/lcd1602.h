/**
 * @file lcd1602.h
 * @brief HD44780 LCD1602 / LCD1602RGB I2C driver — types and API for LIB-4.
 *
 * Drives both the original Waveshare LCD1602 (AiP31068L only) and the
 * LCD1602RGB module (AiP31068L + PCA9633DP2 RGB backlight).  Detection is
 * automatic in lcd_init(): the PCA9633 is probed at 0x60 and, if present, the
 * RGB backlight is initialised to BLUE at full brightness; if absent (legacy
 * monochrome module), the RGB-control functions become no-ops and the
 * character display continues to work unchanged.
 *
 * All bus access goes through the LIB-2 i2c_bus driver; callers must call
 * i2c_init() before lcd_init().
 *
 * AiP31068L write protocol (two bytes per transaction):
 * @code
 *   byte 0: control byte — 0x00 = command (RS=0), 0x40 = data (RS=1)
 *   byte 1: HD44780 command or character byte
 * @endcode
 * The chip handles the parallel HD44780 interface internally (8-bit bus).
 *
 * PCA9633DP2 write protocol (auto-increment supported via the AI bit):
 * @code
 *   byte 0: control byte — register address; bit7 (AI) set = auto-increment
 *   byte 1+: register data
 * @endcode
 *
 * @author Greenhouse Controller project
 * @version 0.2.0
 */

#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Display geometry and I2C address
 * --------------------------------------------------------------------------- */

/** @brief 7-bit I2C address of the AiP31068L character controller. */
#define LCD_I2C_ADDR  0x3E

/** @brief Number of character columns (characters per row). */
#define LCD_COLS  16

/** @brief Number of rows. */
#define LCD_ROWS   2

/* ---------------------------------------------------------------------------
 * PCA9633DP2 RGB backlight (LCD1602RGB module only)
 *
 * 8-bit slave address 0xC0 = 7-bit 0x60. Wired channels on this Waveshare
 * LCD1602RGB PCB (verified empirically — does NOT match the Grove convention
 * where LED0=R/LED1=G/LED2=B):
 *   LED0 → BLUE
 *   LED1 → GREEN
 *   LED2 → RED
 *   LED3 → unused
 * lcd_backlight_color() handles the (r,g,b)-to-channel remap internally so
 * callers can keep using the natural (r, g, b) argument order.
 * --------------------------------------------------------------------------- */

/** @brief 7-bit I2C address of the PCA9633DP2 RGB backlight controller. */
#define LCD_RGB_I2C_ADDR    0x60

#define LCD_RGB_REG_MODE1   0x00  /**< MODE1: write 0x00 to clear SLEEP and wake oscillator. */
#define LCD_RGB_REG_MODE2   0x01  /**< MODE2: 0x05 = outputs change on STOP, group dimming, totem-pole. */
#define LCD_RGB_REG_PWM0    0x02  /**< channel 0 PWM duty — wired to BLUE on this PCB.  */
#define LCD_RGB_REG_PWM1    0x03  /**< channel 1 PWM duty — wired to GREEN. */
#define LCD_RGB_REG_PWM2    0x04  /**< channel 2 PWM duty — wired to RED on this PCB.   */
#define LCD_RGB_REG_PWM3    0x05  /**< channel 3 PWM duty — unused on this module. */
#define LCD_RGB_REG_GRPPWM  0x06  /**< Group brightness (0..255) applied to all channels. */
#define LCD_RGB_REG_LEDOUT  0x08  /**< Per-channel output mode: 0xFF = PWM × group dim, all 4. */

/** @brief Control-byte bit that enables auto-increment of the register pointer. */
#define LCD_RGB_AI_BIT      0x80

/* ---------------------------------------------------------------------------
 * @defgroup lcd_types LCD types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Return status for all LCD operations.
 */
typedef enum {
    LCD_OK           = 0, /**< Operation completed successfully. */
    LCD_ERR_NO_DEVICE = 1, /**< Module did not ACK its I2C address. */
    LCD_ERR_COMM      = 2  /**< I2C communication error (timeout/bus busy). */
} lcd_status_t;

/** @} */ /* end lcd_types */

/* ---------------------------------------------------------------------------
 * @defgroup lcd_api LCD API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the LCD module.
 *
 * Performs the HD44780 4-bit software-reset sequence then configures the
 * display (2-line mode, display on, cursor off, entry-mode increment).
 * Backlight is left on after init.
 * Must be called once before any other lcd_* function.
 *
 * @return @ref LCD_OK on success, @ref LCD_ERR_NO_DEVICE if the module
 *         does not respond.
 */
lcd_status_t lcd_init(void);

/**
 * @brief Clear the display and return the cursor to (0, 0).
 *
 * Sends HD44780 command 0x01.  The display remembers its backlight state.
 *
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_clear(void);

/**
 * @brief Return the cursor to position (0, 0) without clearing the display.
 *
 * Sends HD44780 command 0x02.
 *
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_home(void);

/**
 * @brief Move the cursor to the given row and column.
 *
 * @param row  Row index: 0 (top) or 1 (bottom).
 * @param col  Column index: 0–15.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_set_cursor(uint8_t row, uint8_t col);

/**
 * @brief Print a null-terminated string starting at (row, col).
 *
 * Characters are written until the null terminator.  The caller is
 * responsible for not exceeding the display boundary.
 *
 * @param row  Row index: 0 or 1.
 * @param col  Column index: 0–15.
 * @param str  Null-terminated string to display.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_print(uint8_t row, uint8_t col, const char *str);

/**
 * @brief Print a single character at (row, col).
 *
 * @param row  Row index: 0 or 1.
 * @param col  Column index: 0–15.
 * @param c    Character to display.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_print_char(uint8_t row, uint8_t col, char c);

/**
 * @brief Write exactly @ref LCD_COLS characters to the given row.
 *
 * Strings shorter than 16 characters are padded with trailing spaces.
 * Strings longer than 16 characters are silently truncated to 16.
 *
 * @param row   Row index: 0 or 1.
 * @param text  Null-terminated string.  NULL is treated as an empty string
 *              and fills the row with spaces.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_write_row(uint8_t row, const char *text);

/**
 * @brief Define a custom character in CGRAM.
 *
 * Writes an 8-byte pixel pattern to CGRAM slot @p slot (0–7), then returns
 * the display to DDRAM address 0 (home) so subsequent lcd_* calls write to
 * the visible display area.
 *
 * Custom characters are referenced in strings by their slot number (0x00–0x07).
 * Avoid slot 0 inside C strings — 0x00 is the null terminator and
 * lcd_write_row() would treat it as end-of-string.  Slots 1–7 are safe to
 * embed and pass to lcd_write_row().
 *
 * Must be called after lcd_init() and while the caller holds MX1.
 *
 * @param slot     CGRAM slot 0–7.
 * @param pattern  8-byte array; each byte encodes one pixel row (5 LSBs used).
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_create_char(uint8_t slot, const uint8_t pattern[8]);

/**
 * @brief Re-assert "display on" to wake the AiP31068L after I2C bus inactivity.
 *
 * Sends CMD_DISP_ON (0x0C) — display on, cursor off, blink off.  The command
 * is idempotent (no visible change when the display is already on) and has a
 * hardware busy time of only ~37 µs, which is safely covered by the I2C
 * transaction overhead (~70 µs).
 *
 * Use this as a preamble write before the first real lcd_write_row() call
 * that follows a period of I2C bus inactivity.  The AiP31068L silently drops
 * the first byte addressed to it after ~2.5 s of bus idle; sending this cheap,
 * safe command first absorbs that loss without corrupting display state.
 *
 * Must be called while the caller holds MX1.
 *
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_display_on(void);

/**
 * @brief Set the RGB backlight colour.
 *
 * Writes the three individual PCA9633 PWM channels (R/G/B intensities) using
 * an auto-increment burst from PWM0..PWM2.  The result is mixed with the
 * group-PWM master brightness (see lcd_backlight_lumination()) to produce the
 * final per-channel duty cycle.
 *
 * Silently returns @ref LCD_OK without bus traffic when the PCA9633 was not
 * detected at lcd_init() (i.e. on legacy LCD1602 hardware without RGB).
 *
 * Must be called while the caller holds MX1.
 *
 * @param r  Red   intensity 0..255.
 * @param g  Green intensity 0..255.
 * @param b  Blue  intensity 0..255.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_backlight_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set the RGB backlight master brightness.
 *
 * Writes the PCA9633 GRPPWM register (0..255).  Acts as a master multiplier
 * on the per-channel PWM values, so it dims the entire backlight without
 * changing the colour.
 *
 * Silently returns @ref LCD_OK without bus traffic when the PCA9633 was not
 * detected at lcd_init().
 *
 * Must be called while the caller holds MX1.
 *
 * @param level  Master brightness 0 (off) .. 255 (full).
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_backlight_lumination(uint8_t level);

/**
 * @brief Set the LCD character contrast (since 1.17.33 / gh#15-prep).
 *
 * Writes the AiP31068L's 6-bit contrast register via its extension
 * instruction set (IS=1). The contrast register is split: the low 4 bits
 * (C3..C0) go into the dedicated Contrast Set opcode, and the high 2 bits
 * (C5..C4) go into the Power/Icon/Contrast Set opcode alongside the
 * booster-on bit (Bon=1, matches the boot-init choice). After both writes
 * the controller returns to IS=0.
 *
 * lcd_init() configures contrast = 32 (binary 100000, ≈ 50 % of the 0–63
 * range) at boot. This call lets a higher-level task override that at
 * runtime. Useful band based on the LCD glass: ~16 (faded) to ~48 (bold);
 * < 16 makes characters fade out, > 55 makes dark blocks bleed.
 *
 * Must be called while the caller holds MX1.
 *
 * @param value  Contrast 0..63. Values > 63 are clamped to 63.
 * @return @ref lcd_status_t.
 */
lcd_status_t lcd_set_contrast(uint8_t value);

/** @} */ /* end lcd_api */
