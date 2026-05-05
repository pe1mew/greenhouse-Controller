/**
 * @file relay_controller.h
 * @brief T2 — Relay Controller task declaration.
 *
 * T2 is the sole owner of the six relay GPIO outputs and implements the
 * per-channel window state machines.  No other task may write to relay
 * GPIO pins.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include "../types/app_types.h"  /* window_state_t */

/**
 * @brief T2 — Relay Controller task entry point.
 *
 * Responsibilities:
 *  - Sole owner of relay GPIO outputs (PIN_RELAY_M1_OPEN … PIN_RELAY_M3_CLOSE).
 *  - Per-channel window FSM (UNKNOWN → CLOSED ↔ MOVING_OPEN/MOVING_CLOSE ↔ OPEN).
 *  - 2 s inter-relay gap enforcement between complementary relay pairs.
 *  - Travel timer: energises relay for (travel_s + MOTOR_TRAVEL_MARGIN_S_DEFAULT) × 1000 ms.
 *  - Dwell timer: minimum rest period after reaching OPEN or CLOSED state.
 *  - Deferred-ISR on GPIO42 (PIN_OPTO_INPUT) with 75 ms debounce for
 *    RRK-3 motor alarm detection (NOT suppressed during MOVING states).
 *  - On alarm assertion: de-energises all 6 relays, sets EG1_BIT_MOTOR_ALARM,
 *    logs LOG_ALARM onset.
 *  - On alarm clearance: clears EG1_BIT_MOTOR_ALARM, runs synchronous
 *    CLOSE_ALL re-calibration, logs LOG_ALARM clearance, resumes AUTOMATIC.
 *  - Q1 consumer: window_cmd_t (CMD_OPEN / CMD_CLOSE / CMD_CLOSE_ALL /
 *    CMD_RESUME); SRC_T3 commands bypass the dwell timer; SRC_T6 commands
 *    respect it.
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_relay_controller(void *pvParameters);

/**
 * @brief Thread-safe snapshot of all channel window states (T11 / web dashboard).
 *
 * Reads s_ch[].state under a portMUX spinlock.  Gap states
 * (CH_GAP_TO_OPEN / CH_GAP_TO_CLOSE) are mapped to the corresponding
 * MOVING state so callers see a stable five-value enum.
 *
 * @param out  Array of 3 window_state_t values; index 0 = M1.
 */
void t2_get_window_states(window_state_t out[3]);
