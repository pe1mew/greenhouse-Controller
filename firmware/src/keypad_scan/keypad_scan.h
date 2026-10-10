/**
 * @file keypad_scan.h
 * @brief T7 — Keypad Scan task declaration.
 *
 * Scans the 4x4 membrane keypad every 20 ms, debounces key presses,
 * generates key-repeat events, and posts `key_event_t` records to Q2 for
 * consumption by T8 (LCD GUI / session manager).
 *
 * T7 is the sole producer for Q2 (depth 16). The downstream consumer (T8)
 * is expected to drain Q2 well within 100 ms — the key-repeat cadence — so
 * overflow is treated as a recoverable warning rather than a hard error.
 *
 * Full implementation: Phase 7 of firmwareImplementationPlan.md.
 *
 * @see  task_keypad_scan
 * @see  firmware/src/types/app_types.h (Q2, key_event_t)
 * @see  firmware/lib/keypad_matrix (LIB-5 row/col scanner with two-scan debounce)
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T7 — Keypad Scan task entry point.
 *
 * Initialises the LIB-5 keypad_matrix driver, subscribes to the FreeRTOS
 * task watchdog (gh#13), then loops forever at a 20 ms cadence emitting
 * `key_event_t` records to Q2:
 *
 *  - A newly observed key produces one event with `repeated = false`.
 *  - A key held continuously for more than 500 ms then produces
 *    `repeated = true` events every 100 ms until released.
 *  - `KP_NO_KEY` (i.e. nothing pressed) resets the internal repeat state.
 *  - gh#93 (2.17.0): when the key it was tracking goes up -- lifted, or
 *    replaced by another key -- one event with `released = true`. T8 times
 *    the LCD's M3 hold by it, and hands it to nothing else.
 *
 * This is a FreeRTOS task entry point — spawn it once via `xTaskCreate()` at
 * startup. The function never returns.
 *
 * @param pvParameters  Unused; pass NULL.
 *
 * @note    Q2 overflow on a first-press logs `ESP_LOGW`; overflow on a
 *          repeat logs `ESP_LOGD` only (repeats are best-effort).
 * @warning The 20 ms scan period must remain well under the 5 s task-watchdog
 *          window; do not add blocking calls to the loop body.
 * @see     task_lcd_gui (T8) — sole consumer of Q2.
 */
void task_keypad_scan(void *pvParameters);

#ifdef MODBUS_BENCH
#include <stdbool.h>
#include <stdint.h>
/**
 * @brief Bench builds only (gh#93): hold @p key on the matrix for @p ms.
 *
 * T7 reads the key in place of the matrix for that long, so the press, the
 * repeats and the release come from T7's own code, as from a finger. Used by
 * GET/POST /api/diag/key and bin/at_lcd_m3_hold.py.
 *
 * @return false when a held key has not been released yet, or the arguments
 *         are empty.
 */
bool keypad_bench_hold(char key, uint32_t ms);

/** @brief Bench builds only (gh#93): a key held by keypad_bench_hold() is still down. */
bool keypad_bench_busy(void);
#endif
