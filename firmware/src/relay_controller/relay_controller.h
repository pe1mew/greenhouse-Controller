/**
 * @file relay_controller.h
 * @brief T2 — Relay Controller task declaration.
 *
 * T2 is the sole owner of the six relay GPIO outputs and implements the
 * per-channel window state machines for the three ventilation channels
 * (M1, M2, M3).  No other task may write to relay GPIO pins.
 *
 * ## Task-graph slot
 *  - Q1 (consumer)         — window_cmd_t from T3 (SRC_T3) and T6 (SRC_T6).
 *  - Q3 (producer)         — LOG_RELAY / LOG_ALARM / LOG_SYSTEM via log_post().
 *  - EG1 (set/clear)       — EG1_BIT_MOTOR_ALARM, EG1_BIT_CALIBRATING.
 *  - NVS (read/write)      — motor/travel_m{1..3}, motor/dwell_{open,close}_m{1..3},
 *                            motor/t2_st_ch{0..2} (gh#18 Phase 3 state recovery).
 *
 * ## Hardware caveats
 *  - Six relay outputs (PIN_RELAY_M{1..3}_{OPEN,CLOSE}) MUST never be
 *    driven by any other task — T2's FSM is the single writer.
 *  - GPIO42 (PIN_OPTO_INPUT) carries the RRK-3 motor-alarm contact via an
 *    active-low opto-coupler; the ISR is registered with IRAM_ATTR and is
 *    NOT suppressed during MOVING states (FR-MA01).
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
 *  - Persists terminal channel state (CLOSED/OPEN) to NVS so that a clean
 *    reboot with all channels in CLOSED can skip the up-to-171 s boot
 *    calibration (gh#18 Phase 3, since 1.17.36).
 *
 * @param pvParameters  Unused; pass NULL.
 * @warning Subscribes to the FreeRTOS task watchdog (esp_task_wdt_add).
 *          Long-running blocking sections (boot calibration, alarm guard) kick
 *          the WDT on a chunk boundary; do not extend those sections without
 *          preserving the chunked WDT reset pattern.
 * @see     t2_get_window_states(), task_safety_monitor() (T3 wind override
 *          source), task_climate_control() (T6 ventilation source).
 */
void task_relay_controller(void *pvParameters);

/**
 * @brief Thread-safe snapshot of all channel window states (T11 / web dashboard).
 *
 * Reads s_ch[].state under a portMUX spinlock.  Gap states
 * (CH_GAP_TO_OPEN / CH_GAP_TO_CLOSE) are mapped to the corresponding
 * MOVING state so callers see a stable five-value enum (matching
 * window_state_t in app_types.h).
 *
 * @param out  Caller-owned array of 3 window_state_t values; index 0 = M1,
 *             index 1 = M2, index 2 = M3.  Must not be NULL.
 * @note  Safe to call from any task (T11 dashboard, T12 MQTT publisher,
 *        T8 LCD) without coordinating with T2 — the portMUX read is
 *        wait-free on the producer side.
 */
void t2_get_window_states(window_state_t out[3]);
