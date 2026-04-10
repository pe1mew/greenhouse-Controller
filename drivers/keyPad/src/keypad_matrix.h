/**
 * @file keypad_matrix.h
 * @brief Keypad matrix driver — types and API for LIB-5.
 *
 * Scans a 4×4 membrane keypad and returns a single debounced key character
 * per press. Pin assignments are centralised in firmware/config/pin_config.h
 * and exposed here by inclusion so callers need only include this header.
 *
 * Key layout:
 * @verbatim
 *   Col:     1    2    3    4
 *   Row 1:  '1'  '2'  '3'  'A'
 *   Row 2:  '4'  '5'  '6'  'B'
 *   Row 3:  '7'  '8'  '9'  'C'
 *   Row 4:  '*'  '0'  '#'  'D'
 * @endverbatim
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include "pin_config.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * @defgroup keypad_constants Keypad constants
 * @{
 * --------------------------------------------------------------------------- */

/** @brief Value returned by keypad_scan() when no valid key is detected. */
#define KP_NO_KEY  '\0'

/** @} */

/* ---------------------------------------------------------------------------
 * @defgroup keypad_api Keypad API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise all keypad GPIO pins.
 *
 * Row pins (KP_ROW1–KP_ROW4) are configured as OUTPUT and driven HIGH (idle).
 * Column pins (KP_COL1–KP_COL4) are configured as INPUT_PULLUP.
 * Call once at startup before the first keypad_scan().
 */
void keypad_init(void);

/**
 * @brief Scan the keypad and return a debounced key character.
 *
 * Intended to be called at a regular ~20 ms interval (e.g. from a periodic
 * task). Each call drives each row LOW in turn, reads all four column inputs,
 * then restores the row HIGH before moving to the next row.
 *
 * Debounce: a key character is returned only after two consecutive calls
 * detect the same single key pressed. The first call after a new press always
 * returns KP_NO_KEY.
 *
 * Multi-press detection: if more than one key is pressed simultaneously
 * (same row or across rows), the input is discarded and KP_NO_KEY is returned.
 * The debounce counter is also reset so that releasing to a single key
 * requires two fresh scans before that key is reported.
 *
 * @return Key character ('0'–'9', 'A'–'D', '*', '#') on a confirmed single-key
 *         press, or @ref KP_NO_KEY when no valid key is detected.
 */
char keypad_scan(void);

/**
 * @brief Count how many keys are physically pressed right now.
 *
 * Performs one complete scan cycle (all four rows) and returns the total
 * number of column-LOW readings observed. Does NOT affect the debounce state,
 * so it can be called independently of keypad_scan().
 *
 * Typical use: hardware verification to confirm that a multi-press condition
 * is actually present on the hardware before testing that keypad_scan()
 * correctly discards it.
 *
 * @return Number of keys currently pressed (0, 1, or more than 1).
 */
int keypad_count_pressed(void);

/** @} */

#ifdef UNIT_TEST
/**
 * @brief Reset the driver's internal debounce state (unit-test builds only).
 *
 * Call from setUp() to guarantee test isolation between test cases.
 */
void keypad_test_reset_state(void);
#endif
