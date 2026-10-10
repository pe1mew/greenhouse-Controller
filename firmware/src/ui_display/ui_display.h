/**
 * @file ui_display.h
 * @brief T8 — UI / Display task declaration.
 *
 * Drives the 16×2 LCD (AiP31068L at I2C address 0x3E). Implements the
 * menu FSM (max 4 key presses to any first-level setting), PIN session
 * management, and config change posting to Q4. Receives key events from
 * Q2 and network status from Q5.
 *
 * ## Subsystem ownership
 *  - **MX1**: all LCD bus access is protected here (shared with T4's RTC reads).
 *  - **Q2 consumer**: drains keypad events from T7.
 *  - **Q5 consumer**: drains network status snapshots from T10.
 *  - **Q4 producer**: posts `config_update_t` for setpoint edits via the LCD.
 *  - **EG1 reader**: status_colour_for_bits() mirrors EG1 onto the LCD backlight.
 *
 * ## Threading caveats
 *  - The menu FSM is single-task — no internal locking. Concurrent web edits
 *    (T11 → Q4) are serialised at T4 which is the single writer to NVS/MX4.
 *  - WDT subscribed at task entry; 100 ms tick cadence keeps a comfortable
 *    margin under the 5 s TWDT threshold.
 *
 * Full implementation: Phase 7 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T8 — UI / Display task entry point.
 *
 * Subscribes to TWDT, initialises the LCD under MX1, loads CGRAM glyphs,
 * shows the boot splash, then enters the 100 ms main loop (key dispatch,
 * status auto-rotate, session timeout, render-if-dirty).
 *
 * @param pvParameters  Unused; pass NULL.
 * @note Suggested xTaskCreatePinnedToCore: stack 4096 B, prio 4, core 1.
 */
void task_ui_display(void *pvParameters);

#ifdef MODBUS_BENCH
#include <stdbool.h>
#include <stdint.h>
/**
 * @brief Bench builds only (gh#93): what T8 shows and does, for GET /api/diag/key.
 *
 * A harness walks the LCD's menus with keys held by keypad_bench_hold(), and
 * judges the M3 hold by `last_*`. Read without a lock: diagnostics only.
 */
typedef struct {
    int      state;         /**< ui_state_t ordinal */
    uint8_t  status_page;   /**< 0..6; 5 = the windows page, whose # opens the motor menu */
    uint8_t  motor;         /**< the motor picked on the manual-motor menu, 0 if none */
    uint8_t  session;       /**< session_t: 0 none, 1 farmer, 2 admin */
    bool     msg;           /**< a transient message is up: keys are discarded */
    bool     suppress;      /**< the 600 ms after a message: keys are discarded */
    bool     hold_active;   /**< an M3 press is being timed */
    bool     hold_moving;   /**< ...and M3 was told to move until the release */
    bool     hold_opening;
    uint32_t last_seq;      /**< actions of the M3 hold since boot */
    uint8_t  last_action;   /**< m3h_action_t: 1 full, 2 move start, 3 move stop, 4 mid */
    bool     last_opening;
    uint32_t last_held_ms;  /**< how long that press had lasted */
    uint8_t  last_result;   /**< 0 carried out, 1 refused, 2 Q1 full */
    uint32_t move_at_ms;    /**< the latest hold's move start, ms into its press */
    char     row0[17];      /**< the LCD's two rows as last set (a message included) */
    char     row1[17];
    char     screen[16];    /**< "status", "pin", "menu_root", "menu_access",
                             *   "motor_pick", "motor_action" or "other" */
} ui_bench_state_t;

/** @brief Bench builds only (gh#93): fill @p out with T8's state. */
void ui_bench_state(ui_bench_state_t *out);
#endif

/**
 * @brief True if a Farmer/Admin PIN session is currently open on the LCD.
 *
 * Used by the ROTA quiet gate (R-P02) to defer an apply/reboot while an
 * operator is interacting with the physical keypad/LCD.
 */
bool ui_pin_session_active(void);
