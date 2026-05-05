/**
 * @file ui_display.h
 * @brief T8 — UI / Display task declaration.
 *
 * Drives the 16×2 LCD (AiP31068L at I2C address 0x3E).  Implements the
 * menu FSM (max 4 key presses to any first-level setting), PIN session
 * management, and config change posting to Q4.  Receives key events from
 * Q2 and network status from Q5.
 *
 * Full implementation: Phase 7 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T8 — UI / Display task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_ui_display(void *pvParameters);
