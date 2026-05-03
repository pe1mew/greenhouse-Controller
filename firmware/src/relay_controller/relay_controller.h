/**
 * @file relay_controller.h
 * @brief T2 — Relay Controller task declaration.
 *
 * Sole owner of all 6 relay GPIO pins.  Runs per-channel window state
 * machines, enforces mutual exclusion, manages travel/dwell timers, and
 * monitors the RRK-3 motor alarm input (GPIO42) via a deferred-ISR pattern.
 *
 * Full implementation: Phase 2 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T2 — Relay Controller task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_relay_controller(void *pvParameters);
