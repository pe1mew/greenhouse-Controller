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

/**
 * @brief True if a Farmer/Admin PIN session is currently open on the LCD.
 *
 * Used by the ROTA quiet gate (R-P02) to defer an apply/reboot while an
 * operator is interacting with the physical keypad/LCD.
 */
bool ui_pin_session_active(void);
