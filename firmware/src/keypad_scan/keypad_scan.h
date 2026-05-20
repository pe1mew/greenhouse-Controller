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
