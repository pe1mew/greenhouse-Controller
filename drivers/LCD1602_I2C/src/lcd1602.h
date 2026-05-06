/**
 * @file lcd1602.h
 * @brief HD44780 LCD1602 I2C driver — types and API for LIB-4.
 *
 * Drives a Waveshare LCD1602 module: HD44780 controller connected via an
 * AiP31068L I2C-to-parallel bridge at I2C address 0x3E.  All bus access goes
 * through the LIB-2 i2c_bus driver; callers must call i2c_init() before
 * lcd_init().
 *
 * AiP31068L write protocol (two bytes per transaction):
 * @code
 *   byte 0: control byte — 0x00 = command (RS=0), 0x40 = data (RS=1)
 *   byte 1: HD44780 command or character byte
 * @endcode
 * The chip handles the parallel HD44780 interface internally (8-bit bus).
 * Backlight is not software-controlled on this module.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Display geometry and I2C address
 * --------------------------------------------------------------------------- */

/** @brief 7-bit I2C address of the PCF8574A backpack on the Waveshare module. */
#define LCD_I2C_ADDR  0x3E

/** @brief Number of character columns (characters per row). */
#define LCD_COLS  16

/** @brief Number of rows. */
#define LCD_ROWS   2

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
 * @brief Turn the backlight on.
 *
 * Sets the module-level backlight flag; the backlight bit is applied to all
 * subsequent byte writes to the PCF8574A.
 *
 * @return @ref LCD_OK.
 */
lcd_status_t lcd_backlight_on(void);

/**
 * @brief Turn the backlight off.
 *
 * Clears the module-level backlight flag.
 *
 * @return @ref LCD_OK.
 */
lcd_status_t lcd_backlight_off(void);

/** @} */ /* end lcd_api */
