/**
 * @file m3_hold.h
 * @brief The LCD's tap-or-hold timing for M3's Open and Close keys (gh#93).
 *
 * The rule (operator, 2026-10-10):
 *  - released within M3H_TAP_MAX_MS (1 s): a tap -- a full stroke, on release;
 *  - released between 1 and 2 s: nothing, and the LCD says why;
 *  - held M3H_HOLD_MS (2 s) or more: M3 starts moving then, and stops when the
 *    key is released.
 *
 * Pure logic, by the rule of drivers/ventModel and drivers/m3Char: no FreeRTOS,
 * no ESP-IDF, no project headers. Times are milliseconds on any clock that only
 * moves forward and wraps at 2^32; T8 passes its tick count in milliseconds.
 *
 * T8 feeds it four events -- a first press, a repeat, a release (T7 posts one
 * since 2.17.0) and a tick every loop -- and each returns at most one action for
 * T8 to carry out, with the direction of the press it belongs to. T7 repeats a
 * held key every 100 ms once it has been held 500 ms, so a press that has gone
 * quiet for longer than that has ended without its release reaching T8: the
 * tick ends it at its last sign of life.
 *
 * Host tests: `pio test -e native` in drivers/m3Hold. Design: design/lcdM3HoldControl.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M3H_TAP_MAX_MS        1000u  /**< A release before this is a tap. */
#define M3H_HOLD_MS           2000u  /**< A hold this long starts the move. */
#define M3H_FIRST_REPEAT_MS    500u  /**< T7's first repeat comes this long after the press. */
#define M3H_LOST_MS            400u  /**< No sign of the key for this long past the expected
                                      *   one: it was released, and the release was lost. */

typedef enum {
    M3H_NOTHING = 0,    /**< Nothing to do. */
    M3H_FULL,           /**< A tap: a full stroke in `opening`'s direction. */
    M3H_MOVE_START,     /**< The hold reached 2 s: start moving in `opening`'s direction. */
    M3H_MOVE_STOP,      /**< A moving hold ended: stop. */
    M3H_MID,            /**< Released between 1 and 2 s, or at the 2 s mark before the
                         *   move started: nothing moves; say why. */
} m3h_action_t;

/** What T8 does, and for which press. */
typedef struct {
    m3h_action_t action;
    bool         opening;   /**< The direction of the press the action belongs to. */
    uint32_t     held_ms;   /**< How long that press had lasted. */
} m3h_out_t;

/** One press of the Open or Close key, as it is being timed. */
typedef struct {
    bool     active;        /**< A press is being timed. */
    bool     opening;       /**< Open (true) or Close. */
    char     key;           /**< The key, to match its repeats and its release. */
    bool     moving;        /**< M3H_MOVE_START has been returned for this press. */
    bool     repeated;      /**< A repeat of it has been seen. */
    uint32_t t0_ms;         /**< The press. */
    uint32_t last_ms;       /**< The press or its latest repeat: its last sign of life. */
} m3_hold_t;

/** Forget any press being timed. */
void m3h_reset(m3_hold_t *h);

/** A first press of the Open or Close key. A press still being timed has lost
 *  its release: it is ended at its last sign of life, and its action returned. */
m3h_out_t m3h_press(m3_hold_t *h, char key, bool opening, uint32_t now_ms);

/** A repeat of a held key. May start the move. */
m3h_out_t m3h_repeat(m3_hold_t *h, char key, uint32_t now_ms);

/** A release. Ends the press it belongs to: a tap, nothing, or the move's stop. */
m3h_out_t m3h_release(m3_hold_t *h, char key, uint32_t now_ms);

/** Every loop. Starts the move at 2 s, and ends a press whose release was lost. */
m3h_out_t m3h_tick(m3_hold_t *h, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
